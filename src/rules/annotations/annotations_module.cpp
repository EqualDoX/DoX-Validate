// Module "annotations": annotations, form fields, links, media clips and page tab order.
//
// veraPDF 1.30.2 semantics reproduced (validation-model, parser):
//   Visited annotations   PDPage.getAnnotations: the COS_DICT entries of the page's /Annots array, pages in
//                         document order; an indirect annotation is visited once (model object ID), on the
//                         first page that lists it. GFPDAnnot.createAnnot picks the model type by /Subtype.
//   F                     getIntegerKey: integer, truncated real, or the byte length of a string; null -> 0
//                         in the JavaScript bit tests.
//   isOutsideCropBox      raw /Rect (TypeConverter.getRealArray, >= 4 numbers) against PDPage.getCropBox
//                         (inherited CropBox clipped to the inherited MediaBox, else the MediaBox).
//   struct parent         /StructParent (getIntegerKey) looked up in the ParentTree with the parser's number
//                         tree search (Limits, Nums before Kids); structParentType = its /S name (entry must
//                         be a dictionary), structParentStandardType = standard type of that element in the
//                         profile's flavour (PDF 1.7 role map for ua1, ISO 32000-2 namespaces for ua2/wt).
//   Alt / containsLang    /Alt string / presence of a /Lang string on that ParentTree entry (not inherited).
//   isArtifact            the entry or any /P ancestor has the standard type Artifact (ISO 32000-2).
//   Contents              getStringKey: a string (names do not count).
//   GFPDWidgetAnnot       isFieldWidget = own /T or a dictionary /Parent; TU = own /TU when the widget has /T,
//                         else the /Parent's /TU; containsLbl = a structure child of the struct parent with
//                         standard type Lbl and at least one kid.
//   GFPDLinkAnnot         differentTargetAnnotObjectKey: links visited earlier with the same (indirect) struct
//                         parent but another structure destination (GoTo /SD or /D, /Dest, named destinations;
//                         a destination dictionary's /SD or an array whose first element has /S).
//   PDMediaClip           every media clip reachable through the model graph (Rendition actions of page,
//                         annotation, field, catalog and outline actions and their /Next chains).
//   PDFormField           /AcroForm /Fields and their /Kids that have /T; PDTextField = inherited /FT /Tx.
//   SEForm                structure elements of the traversal whose standard type is Form.
//   8.10.3.5-1            SEGraphicContentItem.isSignature can never be true in veraPDF 1.30.2: the flag is
//                         only passed to annotation appearance XObjects (GFPDAnnot.addAppearance; GFOp_Do passes
//                         false), and GFPDXForm.parseContentStream builds a non-semantic content stream for
//                         annotations (isAnnotation), so no SE content item of a signature appearance exists:
//                         veraPDF never fails it (--reference-dedup keeps that). The default mode evaluates the
//                         clause itself (ISO 14289-2 8.10.3.5): an image, inline image, shading or curved path
//                         painted by a signature field's appearance needs alternative text. Text is no graphic and
//                         straight rules (a border) are not reported.
//
// PAC 26.1 conditions merged into veraPDF rules (union): their observed behaviour on the fixtures is
// covered by the veraPDF definitions. PAC-only "AnnotationHasAltText" (inferred from the readouts, see
// pac_alt_text below): annotations other than Widget, Popup, PrinterMark and TrapNet that are not hidden and
// not outside the crop box need a non-whitespace /Contents or /Alt on their struct parent; missing on a Link
// annotation and whitespace-only /Contents are warnings, otherwise an error. Evaluated on tagged documents only.
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "rules/annotations/annot_helpers.h"
#include "rules/module.h"
#include "util/text.h"

namespace eqx {

namespace {

using namespace annot;

struct Rules {
    RuleIndex pac_alt = R("EQX-PAC-FailAnnotationHasAltText");
    RuleIndex pac_ws = R("EQX-PAC-WarnContentsEntryOnAnnotationIsWhiteSpace");
    RuleIndex ua1_annot_tag = R("EQX-UA1-7.18.1-1");
    RuleIndex ua1_annot_desc = R("EQX-UA1-7.18.1-2");
    RuleIndex ua1_field_desc = R("EQX-UA1-7.18.1-3");
    RuleIndex ua1_trapnet = R("EQX-UA1-7.18.2-1");
    RuleIndex ua1_tabs = R("EQX-UA1-7.18.3-1");
    RuleIndex ua1_widget_form = R("EQX-UA1-7.18.4-1");
    RuleIndex ua1_form_child = R("EQX-UA1-7.18.4-2");
    RuleIndex ua1_link_tag = R("EQX-UA1-7.18.5-1");
    RuleIndex ua1_link_contents = R("EQX-UA1-7.18.5-2");
    RuleIndex ua1_clip_ct = R("EQX-UA1-7.18.6.2-1");
    RuleIndex ua1_clip_alt = R("EQX-UA1-7.18.6.2-2");
    RuleIndex ua1_printermark = R("EQX-UA1-7.18.8-1");
    RuleIndex ua1_lang_contents = R("EQX-UA1-7.2-24");
    RuleIndex ua1_lang_tu = R("EQX-UA1-7.2-25");
    RuleIndex ua2_widget_form = R("EQX-UA2-8.10.1-1");
    RuleIndex ua2_form_widgets = R("EQX-UA2-8.10.1-2");
    RuleIndex ua2_widget_label = R("EQX-UA2-8.10.2.3-1");
    RuleIndex ua2_widget_aa = R("EQX-UA2-8.10.2.3-2");
    RuleIndex ua2_text_rv = R("EQX-UA2-8.10.3.3-1");
    RuleIndex ua2_signature_graphic = R("EQX-UA2-8.10.3.5-1");
    RuleIndex ua2_link_tag = R("EQX-UA2-8.2.5.20-1");
    RuleIndex ua2_link_targets = R("EQX-UA2-8.2.5.20-2");
    RuleIndex ua2_invisible = R("EQX-UA2-8.9.2.2-1");
    RuleIndex ua2_noview = R("EQX-UA2-8.9.2.2-2");
    RuleIndex ua2_markup_tag = R("EQX-UA2-8.9.2.3-1");
    RuleIndex ua2_markup_rc = R("EQX-UA2-8.9.2.3-2");
    RuleIndex ua2_attachment = R("EQX-UA2-8.9.2.4.10-1");
    RuleIndex ua2_sound = R("EQX-UA2-8.9.2.4.11-1");
    RuleIndex ua2_movie = R("EQX-UA2-8.9.2.4.11-2");
    RuleIndex ua2_screen = R("EQX-UA2-8.9.2.4.12-1");
    RuleIndex ua2_widget_zero = R("EQX-UA2-8.9.2.4.13-1");
    RuleIndex ua2_printermark = R("EQX-UA2-8.9.2.4.14-1");
    RuleIndex ua2_trapnet = R("EQX-UA2-8.9.2.4.15-1");
    RuleIndex ua2_watermark = R("EQX-UA2-8.9.2.4.16-1");
    RuleIndex ua2_3d = R("EQX-UA2-8.9.2.4.19-1");
    RuleIndex ua2_richmedia = R("EQX-UA2-8.9.2.4.19-2");
    RuleIndex ua2_stamp = R("EQX-UA2-8.9.2.4.7-1");
    RuleIndex ua2_ink = R("EQX-UA2-8.9.2.4.8-1");
    RuleIndex ua2_popup = R("EQX-UA2-8.9.2.4.9-1");
    RuleIndex ua2_tabs = R("EQX-UA2-8.9.3.3-1");
    RuleIndex ua2_alt_contents = R("EQX-UA2-8.9.4.2-1");
};

// GFPDAnnot.createAnnot: subtypes modelled as PDMarkupAnnot (incl. the subclasses FileAttachment, Ink, Stamp).
bool is_markup(const std::string& s) {
    static const std::unordered_set<std::string> k = {
        "Caret", "Circle", "FreeText", "Highlight", "Line", "Polygon", "PolyLine", "Redact", "StrikeOut",
        "Square", "Squiggly", "Text", "Underline", "FileAttachment", "Ink", "Stamp"};
    return k.count(s) > 0;
}

bool non_empty(const std::optional<std::string>& s) { return s && !s->empty(); }

bool whitespace_only(const std::string& s) { return is_whitespace_only(utf8_to_u32(s)); }

class Module final : public RuleModule {
public:
    const char* name() const override { return "annotations"; }
    void run(ValidationContext& ctx) override;
};

// One evaluation pass over a document.
class Pass {
public:
    explicit Pass(ValidationContext& ctx) : ctx_(ctx), doc_(ctx.doc), st_(ctx.st) {}
    void run();

private:
    struct Annot {
        Obj dict;
        int page = -1;
        ObjRef loc;                              // the annotation, or its page when the annotation is direct
        std::string subtype;                     // "" when /Subtype is not a name
        bool has_subtype = false;
        std::optional<std::int64_t> flags;
        std::optional<std::vector<double>> rect;
        std::optional<bool> outside;
        Obj entry;                               // ParentTree entry (any type)
        Obj parent;                              // entry when it is a dictionary (getParentDictionary)
        std::optional<std::string> parent_s;     // structParentType
        std::optional<std::string> std17, std20; // structParentStandardType per flavour
        bool artifact20 = false;
        std::optional<std::string> contents, alt;
        bool has_lang = false;
        bool flag(std::int64_t bit) const { return flags && (*flags & bit) == bit; }
        bool ua1_exempt() const { return (outside && *outside) || flag(2); }
    };

    // --- veraPDF helpers ---------------------------------------------------------------------------
    std::optional<std::string> std_type(const Obj& d, Flavor f) const;
    bool is_artifact20(const Obj& parent) const;
    Obj parent_tree_entry(const Obj& owner);
    bool contains_lbl(const Obj& parent) const;
    std::optional<std::uint64_t> structure_destination_key(const Annot& a);
    Obj resolve_named_destination(const Obj& dest, bool* resolved);

    // --- passes ------------------------------------------------------------------------------------
    void annotations();
    void evaluate(Annot& a);
    void page_rules(const Page& p);
    void forms();
    void fields();
    void field(const Obj& f, ObjRef ctx_ref);
    void media_clips();
    void action(const Obj& a, int page, ObjRef ctx_ref);
    void signature_graphics();
    void additional_actions(const Obj& aa, std::initializer_list<const char*> keys, int page, ObjRef ctx_ref);
    void clip(const Obj& c, int page, ObjRef ctx_ref);
    void pac_alt_text(const Annot& a);

    ValidationContext& ctx_;
    Document& doc_;
    StructTree& st_;
    Rules r_;
    Obj struct_root_;
    std::optional<NumberTree> parent_tree_;
    std::optional<NameTree> dests_tree_;
    bool dests_tree_init_ = false;
    bool catalog_lang_ = false;
    bool tagged_ = false;
    std::unordered_set<std::uint64_t> seen_annots_, seen_actions_, seen_clips_, seen_aa_;
    std::unordered_set<std::uint64_t> seen_fields_;
    std::unordered_map<std::uint64_t, std::optional<std::string>> field_ft_;
    // StaticContainers.getDestinationToStructParentsMap (destination key -> struct parent keys), kept inverted:
    // struct parent key -> destination keys of the links visited so far.
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> parent_dests_;
};

std::optional<std::string> Pass::std_type(const Obj& d, Flavor f) const {
    if (!d.isDictionary()) return std::nullopt;
    if (f == Flavor::Pdf17) return st_.standard_of(d, f);
    // PDStructElem.getStructureElementStandardType: the default type's name (also for MathML types).
    auto t = st_.stype_of(d);
    if (!t) return std::nullopt;
    auto def = st_.default_type20(*t);
    if (!def) return std::nullopt;
    return def->type;
}

bool Pass::is_artifact20(const Obj& parent) const {
    // GFPDAnnot.getisArtifact: the parent and its /P chain (loops end the walk; veraPDF would not stop).
    std::unordered_set<std::uint64_t> seen;
    Obj e = parent;
    while (!e.isNull()) {
        if (e.isIndirect() && !seen.insert(ref_of(e).key()).second) break;
        auto t = std_type(e, Flavor::Pdf20);
        if (t && *t == "Artifact") return true;
        e = cos::get(e, "P");
    }
    return false;
}

Obj Pass::parent_tree_entry(const Obj& owner) {
    // GFPDAnnot.getParentDictionary / getAlt / getLang, GFPDFormField.getLang.
    if (!parent_tree_) return Obj::newNull();
    auto sp = vera_integer(cos::get(owner, "StructParent"));
    if (!sp) return Obj::newNull();
    return parent_tree_->get(*sp);
}

bool Pass::contains_lbl(const Obj& parent) const {
    // GFPDWidgetAnnot.getcontainsLbl: getStructChildren (dictionaries with no /Type or /Type /StructElem),
    // standard type Lbl (ISO 32000-2) and getChildren() not empty.
    auto is_struct_elem = [](const Obj& o) {
        if (!o.isDictionary()) return false;
        Obj t = cos::get(o, "Type");
        return !t.isName() || t.getName() == "/StructElem";
    };
    auto has_children = [&](const Obj& e) {
        Obj k = cos::get(e, "K");
        auto counts = [&](const Obj& o) {
            if (is_struct_elem(o) || o.isInteger()) return true;
            if (o.isDictionary() || o.isStream()) {
                Obj t = cos::get(o, "Type");
                return t.isName() && (t.getName() == "/MCR" || t.getName() == "/OBJR");
            }
            return false;
        };
        if (k.isArray()) {
            for (const Obj& o : cos::array_items(k))
                if (counts(o)) return true;
            return false;
        }
        return counts(k);
    };
    Obj k = cos::get(parent, "K");
    std::vector<Obj> kids;
    if (is_struct_elem(k)) kids.push_back(k);
    else if (k.isArray()) {
        for (const Obj& o : cos::array_items(k))
            if (is_struct_elem(o)) kids.push_back(o);
    }
    for (const Obj& c : kids) {
        auto t = std_type(c, Flavor::Pdf20);
        if (t && *t == "Lbl" && has_children(c)) return true;
    }
    return false;
}

Obj Pass::resolve_named_destination(const Obj& dest, bool* resolved) {
    *resolved = true;
    Obj catalog = doc_.catalog();
    if (dest.isString()) {
        Obj names = cos::get(catalog, "Names");
        if (!names.isDictionary()) {
            *resolved = false;
            return Obj::newNull();
        }
        if (!dests_tree_init_) {
            dests_tree_init_ = true;
            Obj d = cos::get(names, "Dests");
            if (d.isDictionary() || d.isStream()) dests_tree_.emplace(d);
        }
        if (!dests_tree_) return dest;   // no Dests tree: the string stays the destination
        Obj v = dests_tree_->get(pdf_text_to_utf8(dest.getStringValue()));
        if (v.isNull()) *resolved = false;
        return v;
    }
    if (dest.isName()) {
        Obj dests = cos::get(catalog, "Dests");
        if (dests.isNull()) return dest;
        Obj v = cos::get(dests, dest.getName().substr(1));
        if (v.isNull()) *resolved = false;
        return v;
    }
    return dest;
}

std::optional<std::uint64_t> Pass::structure_destination_key(const Annot& a) {
    // GFPDLinkAnnot.getStructureDestinationObject.
    Obj dest = a.dict;
    if (cos::has(a.dict, "A")) {
        Obj act = cos::get(a.dict, "A");
        if (act.isDictionary() && cos::name_is(act, "S", "GoTo"))
            dest = cos::has(act, "SD") ? cos::get(act, "SD") : cos::get(act, "D");
    } else if (cos::has(a.dict, "Dest")) {
        dest = cos::get(a.dict, "Dest");
    }
    bool ok = true;
    dest = resolve_named_destination(dest, &ok);
    if (!ok || dest.isNull()) return std::nullopt;
    Obj target;
    if (dest.isDictionary()) {
        target = cos::get(dest, "SD");
    } else if (dest.isArray() && dest.getArrayNItems() > 0) {
        Obj first = dest.getArrayItem(0);
        if (first.isDictionary() && cos::has(first, "S")) target = first;
    }
    if (!target || target.isNull() || !target.isIndirect()) return std::nullopt;   // getKey() == null
    return ref_of(target).key();
}

// --- annotations ------------------------------------------------------------------------------------

void Pass::annotations() {
    const auto& pages = doc_.pages();
    std::unordered_set<std::uint64_t> seen_pages;
    for (const Page& p : pages) {
        if (p.ref.valid() && !seen_pages.insert(p.ref.key()).second) continue;
        page_rules(p);
        Obj annots = cos::get(p.dict, "Annots");
        if (!annots.isArray()) continue;
        std::optional<Box> crop;
        bool crop_done = false;
        for (const Obj& item : cos::array_items(annots)) {
            if (!item.isDictionary()) continue;
            if (item.isIndirect() && !seen_annots_.insert(ref_of(item).key()).second) continue;
            if (!crop_done) {
                crop = vera_crop_box(p.dict);
                crop_done = true;
            }
            Annot a;
            a.dict = item;
            a.page = p.index;
            a.loc = item.isIndirect() ? ref_of(item) : p.ref;
            Obj st = cos::get(item, "Subtype");
            if (st.isName()) {
                a.subtype = st.getName().substr(1);
                a.has_subtype = true;
            }
            a.flags = vera_integer(cos::get(item, "F"));
            a.rect = vera_real_array(cos::get(item, "Rect"));
            a.outside = outside_crop_box(crop, a.rect);
            a.entry = parent_tree_entry(item);
            if (a.entry.isDictionary()) {
                a.parent = a.entry;
                a.parent_s = cos::name(a.parent, "S");
                a.std17 = std_type(a.parent, Flavor::Pdf17);
                a.std20 = std_type(a.parent, Flavor::Pdf20);
                a.artifact20 = is_artifact20(a.parent);
            }
            a.contents = vera_string_key(item, "Contents");
            if (a.entry.isDictionary() || a.entry.isStream()) {
                Obj alt = cos::get(a.entry, "Alt");
                if (alt.isString()) a.alt = pdf_text_to_utf8(alt.getStringValue());
                a.has_lang = cos::get(a.entry, "Lang").isString();
            }
            evaluate(a);
        }
    }
}

void Pass::page_rules(const Page& p) {
    // GFPDPage.getcontainsAnnotations: at least one dictionary in /Annots. getTabs: a name, not inherited.
    Obj annots = cos::get(p.dict, "Annots");
    bool has = false;
    for (const Obj& item : cos::array_items(annots)) {
        if (item.isDictionary()) {
            has = true;
            break;
        }
    }
    if (!has) return;
    auto tabs = cos::name(p.dict, "Tabs");
    std::string detail = tabs ? "Tabs /" + *tabs : (cos::has(p.dict, "Tabs") ? "Tabs is not a name" : "no Tabs");
    if (!tabs || *tabs != "S") ctx_.report(r_.ua1_tabs, p.index, p.ref, detail);
    if (!tabs || (*tabs != "S" && *tabs != "A" && *tabs != "W")) ctx_.report(r_.ua2_tabs, p.index, p.ref, detail);
}

void Pass::evaluate(Annot& a) {
    const std::string& s = a.subtype;
    const std::string what = a.has_subtype ? s + " annotation" : "annotation without Subtype";
    auto report = [&](RuleIndex r, const std::string& d) { ctx_.report(r, a.page, a.loc, d); };
    const bool tagged20 = a.parent_s.has_value() && !a.artifact20;   // structParentType != null && !isArtifact
    auto std17_is = [&](const char* t) { return a.std17 && *a.std17 == t; };
    auto std20_is = [&](const char* t) { return a.std20 && *a.std20 == t; };
    std::string parent_desc = a.parent_s ? "struct parent /" + *a.parent_s : "no struct parent";

    // ---- PDF/UA-1 (PDAnnot and subtypes)
    if (s != "Widget" && s != "PrinterMark" && s != "Link" && !a.ua1_exempt() && !std17_is("Annot"))
        report(r_.ua1_annot_tag, what + ", " + parent_desc);
    if (s != "Widget" && !a.ua1_exempt() && !non_empty(a.contents) && !non_empty(a.alt))
        report(r_.ua1_annot_desc, what + " without Contents and without Alt on its struct parent");
    if (s == "TrapNet" && !a.ua1_exempt()) report(r_.ua1_trapnet, what);
    if (s == "PrinterMark" && !a.ua1_exempt() && a.parent_s) report(r_.ua1_printermark, what + ", " + parent_desc);
    if (s == "Link" && !a.ua1_exempt()) {
        if (!std17_is("Link")) report(r_.ua1_link_tag, what + ", " + parent_desc);
        if (!non_empty(a.contents)) report(r_.ua1_link_contents, what + " without Contents");
    }
    if (a.contents && !a.has_lang && !catalog_lang_)
        report(r_.ua1_lang_contents, what + ": Contents without Lang on the struct parent and no catalog Lang");

    // ---- PDF/UA-2, WTPDF (PDAnnot)
    if (tagged20 && a.flag(1)) report(r_.ua2_invisible, what + " with the Invisible flag, " + parent_desc);
    if (tagged20 && a.flag(32) && !a.flag(256))
        report(r_.ua2_noview, what + " with NoView and without ToggleNoView, " + parent_desc);
    if (a.contents && a.alt && *a.contents != *a.alt)
        report(r_.ua2_alt_contents, what + ": Contents differs from the struct parent's Alt");

    if (a.has_subtype && is_markup(s)) {
        if (tagged20 && !std20_is("Annot")) report(r_.ua2_markup_tag, what + ", " + parent_desc);
        if (cos::has(a.dict, "RC") && a.contents) {
            auto rc = rich_text_entry(a.dict, "RC");
            if (!rc || *rc != *a.contents) report(r_.ua2_markup_rc, what + ": RC and Contents differ");
        }
    }
    if (s == "FileAttachment" && cos::has(a.dict, "FS")) {
        Obj fs = cos::get(a.dict, "FS");
        if (!(fs.isDictionary() && cos::name(fs, "AFRelationship")))
            report(r_.ua2_attachment, what + ": file specification without AFRelationship");
    }
    if (s == "Sound") report(r_.ua2_sound, what);
    if (s == "Movie") report(r_.ua2_movie, what);
    if (s == "TrapNet") report(r_.ua2_trapnet, what);
    if (s == "Screen" && !a.contents) report(r_.ua2_screen, what + " without Contents");
    if (s == "3D" && !a.contents) report(r_.ua2_3d, what + " without Contents");
    if (s == "RichMedia" && !a.contents) report(r_.ua2_richmedia, what + " without Contents");
    if (s == "Ink" && !a.contents) report(r_.ua2_ink, what + " without Contents");
    if (s == "Stamp" && !cos::name(a.dict, "Name") && !a.contents) report(r_.ua2_stamp, what + " without Name and Contents");
    if (s == "Popup" && a.parent_s) report(r_.ua2_popup, what + " in the structure tree, " + parent_desc);
    if (s == "PrinterMark" && tagged20) report(r_.ua2_printermark, what + ", " + parent_desc);
    if (s == "Watermark" && tagged20 && !std20_is("Annot")) report(r_.ua2_watermark, what + ", " + parent_desc);

    // ---- Link annotations
    if (s == "Link") {
        if (tagged20 && !std20_is("Link") && !std20_is("Reference")) report(r_.ua2_link_tag, what + ", " + parent_desc);
        // GFPDLinkAnnot.calculateStructDestinationProperties (constructor, document order).
        if (a.parent.isDictionary() && a.parent.isIndirect()) {
            if (auto dest = structure_destination_key(a)) {
                // Same result as scanning every destination entry for this parent, in O(1).
                std::uint64_t pk = ref_of(a.parent).key();
                auto& dests = parent_dests_[pk];
                bool different = dests.size() > 1 || (dests.size() == 1 && !dests.count(*dest));
                dests.insert(*dest);
                if (different)
                    report(r_.ua2_link_targets, what + ": its structure element " + ref_of(a.parent).str() +
                                                    " holds links to different targets");
            }
        }
    }

    // ---- Widget annotations
    if (s == "Widget") {
        bool is_field = cos::has(a.dict, "T");
        Obj fparent = cos::get(a.dict, "Parent");
        bool field_widget = is_field || fparent.isDictionary();
        std::optional<std::string> tu = is_field ? vera_string_key(a.dict, "TU")
                                                 : (fparent.isDictionary() ? vera_string_key(fparent, "TU")
                                                                           : std::nullopt);
        if (!a.ua1_exempt()) {
            if (!std17_is("Form")) report(r_.ua1_widget_form, what + ", " + parent_desc);
            if (!non_empty(tu) && !non_empty(a.alt))
                report(r_.ua1_field_desc, what + " without TU and without Alt on its struct parent");
        }
        if (field_widget && tagged20) {
            if (!std20_is("Form")) report(r_.ua2_widget_form, what + ", " + parent_desc);
            if (!a.contents && !contains_lbl(a.parent))
                report(r_.ua2_widget_label, what + " without Contents and without a Lbl in its Form element");
            if (!a.contents && cos::has(a.dict, "AA")) report(r_.ua2_widget_aa, what + " with AA and without Contents");
        }
        // GFPDAnnot.getwidth / getheight: Rect[2]-Rect[0], Rect[3]-Rect[1] (null when too short).
        if (tagged20 && a.rect && a.rect->size() > 3 && (*a.rect)[2] - (*a.rect)[0] == 0 &&
            (*a.rect)[3] - (*a.rect)[1] == 0)
            report(r_.ua2_widget_zero, what + " of zero width and height, " + parent_desc);
    }

    pac_alt_text(a);
}

void Pass::pac_alt_text(const Annot& a) {
    // PAC "AnnotationHasAltText" (Logical Structure > Alternative Descriptions). Applicability inferred from
    // the U1-A01 readout counts (6 annotations checked: Widget, Popup, PrinterMark and TrapNet are not), the
    // check inventory ("not hidden") and Matterhorn 28-004 (not outside the crop box). It also runs in untagged
    // documents: PAC warns for the two untagged Link annotations without Contents of an untagged
    // real-world sample (spot check; no fixture is an
    // untagged document with annotations).
    const std::string& s = a.subtype;
    if (s == "Widget" || s == "Popup" || s == "PrinterMark" || s == "TrapNet") return;
    if (a.ua1_exempt()) return;
    if (a.contents && !whitespace_only(*a.contents)) return;
    if (a.alt && !whitespace_only(*a.alt)) return;
    const std::string what = a.has_subtype ? s + " annotation" : "annotation without Subtype";
    if (a.contents) {
        ctx_.report(r_.pac_ws, a.page, a.loc, what + ": Contents is whitespace only");
    } else if (s == "Link") {
        // U1-A01: the tagged Link annotation without Contents is PAC's warned case.
        ctx_.report(r_.pac_ws, a.page, a.loc, what + " without Contents");
    } else {
        ctx_.report(r_.pac_alt, a.page, a.loc, what + " without Contents and without Alt");
    }
}

// --- Form structure elements ---------------------------------------------------------------------

void Pass::forms() {
    // GFSEForm: roleAttribute (AttributeHelper.getRole: /A, then /C via the ClassMap, owner PrintField),
    // hasOneInteractiveChild and widgetAnnotsCount over PDStructTreeNode.getChildren().
    Obj class_map = cos::get(struct_root_, "ClassMap");
    auto attr_role = [&](const Obj& attrs) -> bool {
        auto one = [](const Obj& o) {
            return o.isDictionary() && cos::name_is(o, "O", "PrintField") && cos::get(o, "Role").isName();
        };
        if (attrs.isArray()) {
            for (const Obj& o : cos::array_items(attrs))
                if (one(o)) return true;
            return false;
        }
        return one(attrs);
    };
    auto has_role = [&](const Obj& e) -> bool {
        if (attr_role(cos::get(e, "A"))) return true;
        Obj c = cos::get(e, "C");
        if (!class_map.isDictionary()) return false;
        if (c.isName()) return attr_role(cos::get(class_map, c.getName().substr(1)));
        if (c.isArray()) {
            for (const Obj& n : cos::array_items(c)) {
                if (n.isName() && attr_role(cos::get(class_map, n.getName().substr(1)))) return true;
            }
        }
        return false;
    };
    auto children = [](const Obj& e, int* total, int* widgets, bool* single_widget) {
        *total = 0;
        *widgets = 0;
        *single_widget = false;
        Obj k = cos::get(e, "K");
        std::vector<Obj> items;
        if (k.isArray()) items = cos::array_items(k);
        else if (!k.isNull()) items.push_back(k);
        bool first_widget = false;
        for (const Obj& o : items) {
            // TaggedPDFHelper.getChildren: struct elem (dict, no /Type or /Type /StructElem), MCR, integer, OBJR.
            bool counted = false, widget = false;
            Obj t = cos::get(o, "Type");
            if (o.isDictionary() && (!t.isName() || t.getName() == "/StructElem")) counted = true;
            else if ((o.isDictionary() || o.isStream()) && t.isName() && t.getName() == "/MCR") counted = true;
            else if (o.isInteger()) counted = true;
            else if ((o.isDictionary() || o.isStream()) && t.isName() && t.getName() == "/OBJR") {
                counted = true;
                Obj ref = cos::get(o, "Obj");
                widget = ref.isDictionary() && cos::name_is(ref, "Subtype", "Widget");
            }
            if (!counted) continue;
            if (*total == 0) first_widget = widget;
            ++*total;
            if (widget) ++*widgets;
        }
        *single_widget = *total == 1 && first_widget;
    };
    const auto& elems = st_.elems();
    for (const StructElem& e : elems) {
        bool form17 = e.standard(Flavor::Pdf17) == "Form";
        bool form20 = !e.is_math(Flavor::Pdf20) && e.std20 && e.std20->type == "Form";
        if (!form17 && !form20) continue;
        ObjRef loc = e.ref;
        for (int p = e.parent; !loc.valid() && p >= 0; p = elems[p].parent) loc = elems[p].ref;
        int total = 0, widgets = 0;
        bool single = false;
        children(e.dict, &total, &widgets, &single);
        if (form17 && !single && !has_role(e.dict))
            ctx_.report(r_.ua1_form_child, -1, loc,
                        "Form element without Role attribute with " + std::to_string(total) + " children");
        if (form20 && widgets > 1)
            ctx_.report(r_.ua2_form_widgets, -1, loc, "Form element with " + std::to_string(widgets) + " widgets");
    }
}

// --- form fields --------------------------------------------------------------------------------------

void Pass::fields() {
    Obj acro = cos::get(doc_.catalog(), "AcroForm");
    if (!(acro.isDictionary() || acro.isStream())) return;
    Obj list = cos::get(acro, "Fields");
    if (!list.isArray()) return;
    ObjRef acro_ref = ref_of(acro);
    for (const Obj& f : cos::array_items(list)) {
        if (!(f.isDictionary() || f.isStream())) continue;
        field(f, acro_ref);
    }
}

void Pass::field(const Obj& root, ObjRef root_ref) {
    // Depth first in /Kids order on an explicit stack: fields nest without a limit.
    struct Level {
        std::uint64_t key;     // 0: direct field
        ObjRef ref;
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    std::vector<Level> stack;
    std::unordered_set<std::uint64_t> path;
    auto visit = [&](const Obj& f, ObjRef ctx_ref) {
        // PDFormField.getFT: inherited /FT (first present value, a name or nothing), memoised along the chain.
        std::optional<std::string> ft;
        {
            std::vector<std::uint64_t> chain;
            std::unordered_set<std::uint64_t> seen;
            Obj cur = f;
            while (cur.isDictionary() || cur.isStream()) {
                if (cur.isIndirect()) {
                    const std::uint64_t k = ref_of(cur).key();
                    auto known = field_ft_.find(k);
                    if (known != field_ft_.end()) {
                        ft = known->second;
                        break;
                    }
                    if (!seen.insert(k).second) break;
                    chain.push_back(k);
                }
                Obj v = cos::get(cur, "FT");
                if (!v.isNull()) {
                    if (v.isName()) ft = v.getName().substr(1);
                    break;
                }
                Obj parent = cos::get(cur, "Parent");
                if (!(parent.isDictionary() || parent.isStream())) break;
                cur = parent;
            }
            for (std::uint64_t k : chain) field_ft_.emplace(k, ft);
        }
        const int type_class = ft && *ft == "Sig" ? 1 : (ft && *ft == "Tx" ? 2 : 0);
        std::uint64_t key = 0;
        if (f.isIndirect()) {
            key = ref_of(f).key();
            // PDFormField: LoopedException for a field already on the path (veraPDF aborts); the branch ends here.
            if (path.count(key)) return;
            // Model object ID "N G obj <type>": visited once per type.
            if (!seen_fields_.insert(key * 4 + static_cast<std::uint64_t>(type_class)).second) return;
            ctx_ref = ref_of(f);
        }
        // Media clips of the field's additional actions (K, F, V, C) are handled in media_clips().
        auto tu = vera_string_key(f, "TU");
        if (tu) {
            Obj entry = parent_tree_entry(f);
            bool lang = (entry.isDictionary() || entry.isStream()) && cos::get(entry, "Lang").isString();
            if (!lang && !catalog_lang_)
                ctx_.report(r_.ua1_lang_tu, -1, ctx_ref, "form field TU without Lang on its struct parent and no catalog Lang");
        }
        if (type_class == 2 && cos::has(f, "RV")) {
            // GFPDTextField: V (string or text stream, own entry) and the plain text of RV.
            auto v = text_or_stream_entry(f, "V");
            auto rv = rich_text_entry(f, "RV");
            if (!v || !rv || *v != *rv)
                ctx_.report(r_.ua2_text_rv, -1, ctx_ref, v ? "RV and V are not textually equal" : "RV without V");
        }
        Obj kids = cos::get(f, "Kids");
        if (!kids.isArray()) return;
        if (key) path.insert(key);
        stack.push_back({key, ctx_ref, cos::array_items(kids), 0});
    };
    visit(root, root_ref);
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            if (top.key) path.erase(top.key);
            stack.pop_back();
            continue;
        }
        Obj kid = top.kids[top.next++];
        if ((kid.isDictionary() || kid.isStream()) && cos::has(kid, "T")) visit(kid, ObjRef(top.ref));
    }
}

// --- media clips ----------------------------------------------------------------------------------------

void Pass::clip(const Obj& c, int page, ObjRef ctx_ref) {
    if (c.isIndirect()) {
        if (!seen_clips_.insert(ref_of(c).key()).second) return;
        ctx_ref = ref_of(c);
    }
    // PDMediaClip.getContentType: getStringKey(CT).
    if (!vera_string_key(c, "CT")) ctx_.report(r_.ua1_clip_ct, page, ctx_ref, "media clip without CT");
    if (!media_clip_has_correct_alt(c)) ctx_.report(r_.ua1_clip_alt, page, ctx_ref, "media clip without a correct Alt");
}

void Pass::action(const Obj& first, int page, ObjRef first_ref) {
    // Depth first through /Next on an explicit stack: chains have no length limit; every indirect action once.
    std::vector<std::pair<Obj, ObjRef>> stack;
    stack.push_back({first, first_ref});
    while (!stack.empty()) {
        auto [a, ctx_ref] = stack.back();
        stack.pop_back();
        if (!a.isDictionary()) continue;
        if (a.isIndirect()) {
            if (!seen_actions_.insert(ref_of(a).key()).second) continue;
            ctx_ref = ref_of(a);
        }
        if (cos::name_is(a, "S", "Rendition")) {
            // GFPDRenditionAction.getC: /R's /C when it is a dictionary.
            Obj c = cos::get(cos::get(a, "R"), "C");
            if (c.isDictionary()) clip(c, page, ctx_ref);
        }
        Obj next = cos::get(a, "Next");
        if (next.isDictionary()) {
            stack.push_back({next, ctx_ref});
        } else if (next.isArray()) {
            auto items = cos::array_items(next);
            for (auto it = items.rbegin(); it != items.rend(); ++it) stack.push_back({*it, ctx_ref});
        }
    }
}

void Pass::additional_actions(const Obj& aa, std::initializer_list<const char*> keys, int page, ObjRef ctx_ref) {
    if (aa.isIndirect()) {
        if (!seen_aa_.insert(ref_of(aa).key()).second) return;
        ctx_ref = ref_of(aa);
    }
    for (const char* k : keys) action(cos::get(aa, k), page, ctx_ref);
}

void Pass::media_clips() {
    // Model-graph order (depth first): pages (page AA, annotations: A, AA), AcroForm fields (AA, Kids),
    // catalog AA, OpenAction, outlines. The first visit of an indirect object decides its location.
    std::unordered_set<std::uint64_t> seen_pages, seen_annots;
    for (const Page& p : doc_.pages()) {
        if (p.ref.valid() && !seen_pages.insert(p.ref.key()).second) continue;
        Obj paa = cos::get(p.dict, "AA");
        if (paa.isDictionary()) additional_actions(paa, {"O", "C"}, p.index, p.ref);
        for (const Obj& item : cos::array_items(cos::get(p.dict, "Annots"))) {
            if (!item.isDictionary()) continue;
            if (item.isIndirect() && !seen_annots.insert(ref_of(item).key()).second) continue;
            ObjRef loc = item.isIndirect() ? ref_of(item) : p.ref;
            action(cos::get(item, "A"), p.index, loc);
            Obj aa = cos::get(item, "AA");
            if (aa.isDictionary())
                additional_actions(aa, {"E", "X", "D", "U", "Fo", "Bl", "PO", "PC", "PV", "PI"}, p.index, loc);
        }
    }
    Obj catalog = doc_.catalog();
    Obj acro = cos::get(catalog, "AcroForm");
    if (acro.isDictionary() || acro.isStream()) {
        std::unordered_set<std::uint64_t> seen_f;
        std::vector<std::pair<Obj, ObjRef>> stack;
        for (const Obj& f : cos::array_items(cos::get(acro, "Fields")))
            if (f.isDictionary() || f.isStream()) stack.push_back({f, ref_of(acro)});
        std::reverse(stack.begin(), stack.end());
        while (!stack.empty()) {
            auto [f, ref] = stack.back();
            stack.pop_back();
            if (f.isIndirect()) {
                if (!seen_f.insert(ref_of(f).key()).second) continue;
                ref = ref_of(f);
            }
            Obj aa = cos::get(f, "AA");
            if (aa.isDictionary() || aa.isStream()) additional_actions(aa, {"K", "F", "V", "C"}, -1, ref);
            auto kids = cos::array_items(cos::get(f, "Kids"));
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                if ((it->isDictionary() || it->isStream()) && cos::has(*it, "T")) stack.push_back({*it, ref});
        }
    }
    Obj caa = cos::get(catalog, "AA");
    if (caa.isDictionary()) additional_actions(caa, {"WC", "WS", "DS", "WP", "DP"}, -1, ObjRef{});
    action(cos::get(catalog, "OpenAction"), -1, ObjRef{});
    Obj outlines = cos::get(catalog, "Outlines");
    if (outlines.isDictionary() || outlines.isStream()) {
        // OutlinesHelper: every item reachable through /First and /Next, once.
        std::unordered_set<std::uint64_t> seen_items;
        std::vector<Obj> stack;
        Obj first = cos::get(outlines, "First");
        if (first.isDictionary()) stack.push_back(first);
        while (!stack.empty()) {
            Obj item = stack.back();
            stack.pop_back();
            if (item.isIndirect() && !seen_items.insert(ref_of(item).key()).second) continue;
            action(cos::get(item, "A"), -1, ref_of(item));
            Obj next = cos::get(item, "Next"), child = cos::get(item, "First");
            if (next.isDictionary()) stack.push_back(next);
            if (child.isDictionary()) stack.push_back(child);
        }
    }
}

void Pass::run() {
    Obj catalog = doc_.catalog();
    // PDCatalog.getStructTreeRoot: a dictionary; PDStructTreeRoot.getParentTree: dictionary based.
    struct_root_ = cos::get(catalog, "StructTreeRoot");
    if (struct_root_.isDictionary()) {
        Obj pt = cos::get(struct_root_, "ParentTree");
        if (pt.isDictionary() || pt.isStream()) parent_tree_.emplace(pt);
    } else {
        struct_root_ = Obj::newNull();
    }
    // Variable gContainsCatalogLang (PDDocument.containsLang): catalog /Lang is a string.
    catalog_lang_ = cos::get(catalog, "Lang").isString();
    // PAC evaluates its structure checks on tagged documents only.
    tagged_ = struct_root_.isDictionary() && cos::boolean(cos::get(catalog, "MarkInfo"), "Marked").value_or(false);

    annotations();
    forms();
    fields();
    media_clips();
    if (!ctx_.reference_dedup) signature_graphics();
}

// ISO 14289-2 8.10.3.5 (see the file header). Alternative text: /Alt or /ActualText of marked content around the
// graphic, also around the Do that invokes its form; the annotation's /Contents; /Alt or /ActualText of its Form element.
void Pass::signature_graphics() {
    const ContentModel& cm = ctx_.content;
    std::unordered_set<std::uint64_t> seen;
    for (const SignatureGraphic& g : cm.signature_graphics) {
        const StreamInstance& origin = cm.streams[g.stream];
        bool described = false;
        for (int s = g.stream, f = g.frame; s >= 0 && !described; f = cm.streams[s].parent_frame, s = cm.streams[s].parent)
            described = f >= 0 && (cm.inherited_alt(f) || cm.inherited_actual_text(f));
        if (!described && non_empty(vera_string_key(origin.annot_dict, "Contents"))) described = true;
        if (!described) {
            Obj e = parent_tree_entry(origin.annot_dict);
            if (e.isDictionary())
                described = cos::get(e, "Alt").isString() || cos::get(e, "ActualText").isString();
        }
        if (described || !seen.insert(origin.annot.valid() ? origin.annot.key() : static_cast<std::uint64_t>(g.stream)).second)
            continue;
        ctx_.report(r_.ua2_signature_graphic, origin.page, origin.annot,
                    std::string("signature appearance paints ") + (g.what[0] == 'i' ? "an " : "a ") + g.what + " without alternative text");
    }
}

void Module::run(ValidationContext& ctx) {
    Pass pass(ctx);
    pass.run();
}

}  // namespace

std::unique_ptr<RuleModule> make_annotations_module() { return std::make_unique<Module>(); }

}  // namespace eqx
