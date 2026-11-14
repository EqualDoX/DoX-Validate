// Module "content": tagging of page content, marked-content language, Form XObjects, PUA in alternative
// texts, PAC's PDF syntax and text-contrast checks.
//
// veraPDF semantics reproduced (1.30.2, validation-model):
//   7.1-1 / 7.1-2   SEMarkedContent: tag, isTaggedContent (structure parent reaches the StructTreeRoot via
//                   /P), parentsTags (stream-inherited tags + frame chain + own tag)       GFSEMarkedContent
//   7.1-3 / 8.2.2-1 SESimpleContentItem: isTaggedContent || parentsTags contains Artifact  GFSESimpleContentItem
//   7.2-30..32      SEMarkedContent with tag Span and ActualText / Alt / E, no Lang, no inherited Lang
//   7.2-34          SETextItem: catalog /Lang string or a Lang value (own, inherited, default)  GFSETextItem
//   7.20-1          PDXForm containsRef (every Form XObject instance, incl. annotation appearances)
//   7.20-2          PDXForm isUniqueSemanticParent: a Form XObject with /StructParents met a second time
//   8.4.3-2 / -3    CosActualText / CosAlt of structure elements and of marked content in real content
// SE content items exist only in semantic streams, and only inside top-level marked-content sequences that
// are closed (GFPDSemanticContentStream.getContentItem); the interpreter already applies that split.
//
// PAC 26.1 conditions:
//   PDFParsable     PAC's parse-error condition: content-stream syntax errors (lexical errors,
//                   operators with operands of the wrong number or type, undecodable content streams).
//                   File-level damage that qpdf repairs is not reported (PAC behaviour not established).
//   ContrastOfText  WCAG 1.4.3: text colour against the colour painted underneath, reported only when both are
//                   known exactly (device colours, a single axis-aligned filled rectangle or the white page).
//                   Text over images, shadings or non-rectangular fills is not decided (no finding).
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "rules/module.h"
#include "rules/pac_quality/wcag_color.h"
#include "util/text.h"

namespace eqx {

namespace {

// --- PDF syntax: operand signatures ----------------------------------------------------------------
// n number, s string, N name, a array, x name or dictionary, '*' any number of numbers, '+' one or more
// numbers, '?' an optional trailing name (SCN/scn)
const std::unordered_map<std::string_view, std::string_view>& signatures() {
    static const std::unordered_map<std::string_view, std::string_view> m = {
        {"b", ""}, {"B", ""}, {"b*", ""}, {"B*", ""}, {"BT", ""}, {"BX", ""}, {"EMC", ""}, {"ET", ""},
        {"EX", ""}, {"f", ""}, {"F", ""}, {"f*", ""}, {"h", ""}, {"n", ""}, {"q", ""}, {"Q", ""},
        {"s", ""}, {"S", ""}, {"T*", ""}, {"W", ""}, {"W*", ""},
        {"BDC", "Nx"}, {"BMC", "N"}, {"DP", "Nx"}, {"MP", "N"},
        {"cm", "nnnnnn"}, {"Tm", "nnnnnn"}, {"d1", "nnnnnn"}, {"c", "nnnnnn"},
        {"CS", "N"}, {"cs", "N"}, {"Do", "N"}, {"gs", "N"}, {"ri", "N"}, {"sh", "N"},
        {"d", "an"}, {"d0", "nn"}, {"G", "n"}, {"g", "n"}, {"i", "n"}, {"j", "n"}, {"J", "n"},
        {"K", "nnnn"}, {"k", "nnnn"}, {"l", "nn"}, {"m", "nn"}, {"Td", "nn"}, {"TD", "nn"},
        {"M", "n"}, {"Tc", "n"}, {"TL", "n"}, {"Ts", "n"}, {"Tw", "n"}, {"Tz", "n"}, {"w", "n"}, {"Tr", "n"},
        {"re", "nnnn"}, {"v", "nnnn"}, {"y", "nnnn"}, {"RG", "nnn"}, {"rg", "nnn"},
        {"Tf", "Nn"}, {"Tj", "s"}, {"'", "s"}, {"\"", "nns"}, {"TJ", "a"},
        {"SC", "+"}, {"sc", "+"}, {"SCN", "*?"}, {"scn", "*?"}, {"EI", "x"}};
    return m;
}

bool operands_match(std::string_view sig, const std::vector<Operand>& a) {
    if (sig == "+" || sig == "*?") {
        std::size_t n = a.size();
        if (sig == "*?" && n > 0 && a.back().type == Operand::Name) --n;
        else if (sig == "*?" && n == 0) return false;
        if (sig == "+" && n == 0) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (!a[i].is_number()) return false;
        return true;
    }
    if (a.size() != sig.size()) return false;
    for (std::size_t i = 0; i < sig.size(); ++i) {
        const Operand& o = a[i];
        switch (sig[i]) {
            case 'n': if (!o.is_number()) return false; break;
            case 's': if (o.type != Operand::String) return false; break;
            case 'N': if (o.type != Operand::Name) return false; break;
            case 'a': if (o.type != Operand::Array) return false; break;
            case 'x': if (o.type != Operand::Name && o.type != Operand::Dict) return false; break;
            default: return false;
        }
    }
    return true;
}

std::string describe(const std::vector<Operand>& a) {
    std::string out;
    for (const Operand& o : a) {
        if (!out.empty()) out += ' ';
        switch (o.type) {
            case Operand::Null: out += "null"; break;
            case Operand::Bool: out += "bool"; break;
            case Operand::Int: case Operand::Real: out += "number"; break;
            case Operand::Name: out += "name"; break;
            case Operand::String: out += "string"; break;
            case Operand::Array: out += "array"; break;
            case Operand::Dict: out += "dict"; break;
        }
    }
    return out.empty() ? "none" : out;
}

struct SyntaxObserver final : ContentObserver {
    struct Problem {
        int stream;
        std::string detail;
    };
    std::vector<Problem> problems;
    void on_operator(const ContentModel&, int stream, std::uint32_t, const RawOp& op, const GState&) override {
        if (problems.size() > 10000) return;
        static const auto table = [] {
            std::array<const char*, static_cast<std::size_t>(Op::Count)> t{};
            for (const auto& [name, sig] : signatures()) t[static_cast<std::size_t>(op_code(name))] = sig.data();
            t[static_cast<std::size_t>(Op::EI)] = "x";
            return t;
        }();
        const char* sig = table[static_cast<std::size_t>(op.code)];
        if (!sig) return;   // unknown operators are not judged (behaviour not established)
        if (!operands_match(sig, *op.operands)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "operator %.*s at byte %zu has operands (%s)", int(op.op.size()),
                          op.op.data(), op.offset, describe(*op.operands).c_str());
            problems.push_back({stream, buf});
        }
    }
};

// --- WCAG 2.x contrast -------------------------------------------------------------------------------
// The channel value is wcag::srgb_channel(c) = c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4), which keeps
// the results of recent components: a document shows the same few colours on every page.
double luminance(const float rgb[3]) {
    return 0.2126 * wcag::srgb_channel(rgb[0]) + 0.7152 * wcag::srgb_channel(rgb[1]) + 0.0722 * wcag::srgb_channel(rgb[2]);
}
double contrast(const float a[3], const float b[3]) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

bool covers(const Rect& outer, const Rect& inner, double tol) {
    return outer.llx <= inner.llx + tol && outer.lly <= inner.lly + tol && outer.urx >= inner.urx - tol &&
           outer.ury >= inner.ury - tol;
}

bool bold_font(const Font* f) {
    if (!f) return false;
    std::string n = f->base_font;
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (n.find("bold") != std::string::npos || n.find("black") != std::string::npos ||
        n.find("heavy") != std::string::npos)
        return true;
    Obj fd = cos::get(f->dict, "FontDescriptor");
    if (!fd.isDictionary()) {
        Obj desc = cos::get(f->dict, "DescendantFonts");
        if (desc.isArray() && desc.getArrayNItems() > 0) fd = cos::get(desc.getArrayItem(0), "FontDescriptor");
    }
    if (auto w = cos::number(fd, "FontWeight"); w && *w >= 700) return true;
    if (auto fl = cos::integer(fd, "Flags"); fl && (*fl & (1 << 18))) return true;   // ForceBold
    return false;
}

std::string hex(const float rgb[3]) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", int(std::lround(rgb[0] * 255)), int(std::lround(rgb[1] * 255)),
                  int(std::lround(rgb[2] * 255)));
    return b;
}

// Uniform grid over a page for the background search (painting candidates in paint order).
struct Grid {
    static constexpr int N = 24;
    Rect box;
    std::vector<std::vector<int>> cells{N * N};
    void cell_range(const Rect& r, int& x0, int& y0, int& x1, int& y1) const {
        double w = std::max(1.0, box.width()), h = std::max(1.0, box.height());
        auto cx = [&](double x) { return std::clamp(int((x - box.llx) / w * N), 0, N - 1); };
        auto cy = [&](double y) { return std::clamp(int((y - box.lly) / h * N), 0, N - 1); };
        x0 = cx(r.llx); x1 = cx(r.urx); y0 = cy(r.lly); y1 = cy(r.ury);
    }
    void add(int idx, const Rect& r) {
        int x0, y0, x1, y1;
        cell_range(r, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells[y * N + x].push_back(idx);
    }
};

class Module final : public RuleModule {
public:
    const char* name() const override { return "content"; }

    void prepare(ValidationContext&, std::vector<ContentObserver*>& observers) override {
        observers.push_back(&syntax_);
    }

    void run(ValidationContext& ctx) override {
        tagging(ctx);
        marked_content_language(ctx);
        xobjects(ctx);
        pua(ctx);
        pdf_syntax(ctx);
        text_contrast(ctx);
    }

private:
    SyntaxObserver syntax_;

    static bool in_se_tree(const ContentModel& cm, int frame) {
        const McFrame& f = cm.frames[frame];
        if (!cm.streams[f.stream].semantic) return false;
        int root = frame;
        while (cm.frames[root].parent >= 0) root = cm.frames[root].parent;
        return cm.frames[root].end_op != 0xffffffffu;
    }

    // --reference-dedup: veraPDF validates a reused content stream only at its first use.
    static bool skipped(const ValidationContext& ctx, int stream) {
        return ctx.reference_dedup && ctx.content.streams[stream].repeat;
    }

    static Obj frame_struct_parent(const ContentModel& cm, int frame) {
        const McFrame& f = cm.frames[frame];
        return f.struct_elem.isDictionary() ? f.struct_elem : cm.streams[f.stream].parent_struct_elem;
    }

    void tagging(ValidationContext& ctx) {
        static const RuleIndex r711 = R("EQX-UA1-7.1-1"), r712 = R("EQX-UA1-7.1-2"), r713 = R("EQX-UA1-7.1-3"),
                               r822 = R("EQX-UA2-8.2.2-1");
        const ContentModel& cm = ctx.content;
        for (std::size_t i = 0; i < cm.frames.size(); ++i) {
            if (!in_se_tree(cm, static_cast<int>(i))) continue;
            const McFrame& f = cm.frames[i];
            if (skipped(ctx, f.stream)) continue;
            if (!tagged(ctx, frame_struct_parent(cm, static_cast<int>(i)))) continue;
            const StreamInstance& s = cm.streams[f.stream];
            std::string where = "/" + f.tag + (f.mcid ? " MCID " + std::to_string(*f.mcid) : std::string()) +
                                " (operator " + std::to_string(f.begin_op) + ")";
            if (f.tag == "Artifact") ctx.report(r711, s.page, s.ref, "Artifact sequence inside tagged content " + where);
            if (cm.parents_tags_contain(f.stream, static_cast<int>(i), "Artifact"))
                ctx.report(r712, s.page, s.ref, "tagged content inside an Artifact " + where);
        }
        // One location per stream instance: the number of untagged items per kind and the first operator.
        struct Untagged {
            std::uint32_t count[5] = {0, 0, 0, 0, 0};
            std::uint32_t first_op = 0;
            bool any = false;
        };
        std::vector<Untagged> per_stream(cm.streams.size());
        // Whether an item is tagged or in an artifact depends on its stream and frame only, and items come in runs of
        // one frame: decided when the frame changes.
        int run_stream = -1, run_frame = -2;
        bool run_untagged = false;
        for (const ContentItem& it : cm.items) {
            if (skipped(ctx, it.stream)) continue;
            if (it.stream != run_stream || it.frame != run_frame) {
                run_stream = it.stream;
                run_frame = it.frame;
                run_untagged = !tagged(ctx, cm.item_struct_parent(it)) && !cm.parents_tags_contain(it.stream, it.frame, "Artifact");
            }
            if (!run_untagged) continue;
            Untagged& u = per_stream[it.stream];
            if (!u.any) u.first_op = it.op_index;
            u.any = true;
            ++u.count[static_cast<int>(it.kind)];
        }
        static const char* kinds[] = {"text", "path", "shading", "inline image", "image"};
        for (std::size_t i = 0; i < per_stream.size(); ++i) {
            const Untagged& u = per_stream[i];
            if (!u.any) continue;
            std::string d;
            for (int k = 0; k < 5; ++k)
                if (u.count[k]) d += (d.empty() ? "" : ", ") + std::to_string(u.count[k]) + " " + kinds[k];
            d += " item(s) neither tagged nor an artifact (first at operator " + std::to_string(u.first_op) + ")";
            const StreamInstance& s = cm.streams[i];
            ctx.report(r713, s.page, s.ref, d);
            ctx.report(r822, s.page, s.ref, d);
        }
    }

    // GFSEGroupedContent.isTaggedContent: the /P chain of the structure parent reaches the StructTreeRoot
    // (StructTree::reaches_root keeps the answer per structure element).
    static bool tagged(const ValidationContext& ctx, const Obj& parent) {
        if (!parent.isDictionary()) return false;
        return ctx.st.reaches_root(parent);
    }

    // GFOpMarkedContent.getInheritedLang + GFSEMarkedContent.getinheritedLang (stream default language).
    static std::optional<std::string> inherited_lang(const ValidationContext& ctx, int frame) {
        const ContentModel& cm = ctx.content;
        const McFrame& f = cm.frames[frame];
        if (f.bdc && f.own_struct_elem.isDictionary()) {
            Obj lang = cos::get(f.own_struct_elem, "Lang");
            if (lang.isString()) return cos::text(lang);
            if (auto pl = ctx.st.parent_lang(f.own_struct_elem)) return pl;
        }
        for (int p = f.parent; p >= 0; p = cm.frames[p].parent) {
            const McFrame& pf = cm.frames[p];
            if (pf.lang) return pdf_text_to_utf8(*pf.lang);
            if (pf.bdc && pf.own_struct_elem.isDictionary()) {
                Obj lang = cos::get(pf.own_struct_elem, "Lang");
                if (lang.isString()) return cos::text(lang);
                if (auto pl = ctx.st.parent_lang(pf.own_struct_elem)) return pl;
            }
        }
        return cm.streams[f.stream].default_lang;
    }

    void marked_content_language(ValidationContext& ctx) {
        static const RuleIndex r730 = R("EQX-UA1-7.2-30"), r731 = R("EQX-UA1-7.2-31"), r732 = R("EQX-UA1-7.2-32"),
                               r734 = R("EQX-UA1-7.2-34");
        const ContentModel& cm = ctx.content;
        const bool catalog_lang = cos::get(ctx.doc.catalog(), "Lang").isString();   // gContainsCatalogLang
        if (catalog_lang) return;
        for (std::size_t i = 0; i < cm.frames.size(); ++i) {
            const McFrame& f = cm.frames[i];
            if (f.tag != "Span" || !in_se_tree(cm, static_cast<int>(i)) || skipped(ctx, f.stream)) continue;
            if (!f.actual_text && !f.alt && !f.e) continue;
            if (f.lang || inherited_lang(ctx, static_cast<int>(i))) continue;
            const StreamInstance& s = cm.streams[f.stream];
            std::string where = "Span marked content (operator " + std::to_string(f.begin_op) + ")";
            if (f.actual_text) ctx.report(r730, s.page, s.ref, where + " has ActualText without a language");
            if (f.alt) ctx.report(r731, s.page, s.ref, where + " has Alt without a language");
            if (f.e) ctx.report(r732, s.page, s.ref, where + " has E without a language");
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> per_stream(cm.streams.size(), {0, 0});   // count, first op
        for (const ContentItem& it : cm.items) {
            if (it.kind != ContentItem::Kind::Text || skipped(ctx, it.stream)) continue;
            if (cm.lang_value(ctx.st, it.stream, it.frame)) continue;
            auto& c = per_stream[it.stream];
            if (c.first == 0) c.second = it.op_index;
            ++c.first;
        }
        for (std::size_t i = 0; i < per_stream.size(); ++i) {
            if (per_stream[i].first == 0) continue;
            const StreamInstance& s = cm.streams[i];
            ctx.report(r734, s.page, s.ref,
                       std::to_string(per_stream[i].first) + " text item(s) without a language (first at operator " +
                           std::to_string(per_stream[i].second) + "); the catalog has no /Lang");
        }
    }

    void xobjects(ValidationContext& ctx) {
        static const RuleIndex r7201 = R("EQX-UA1-7.20-1"), r7202 = R("EQX-UA1-7.20-2");
        const ContentModel& cm = ctx.content;
        std::unordered_set<std::uint64_t> seen;   // GFPDXForm.isUniqueSemanticParent (StaticContainers set)
        for (const StreamInstance& s : cm.streams) {
            bool form = s.kind == StreamInstance::Kind::Form;
            bool appearance = s.kind == StreamInstance::Kind::Annotation;
            if (!form && !appearance) continue;
            if (form && skipped(ctx, s.parent)) continue;   // the invoking Do was never visited
            int page = s.page;
            ObjRef where = form ? cm.streams[s.parent].ref : s.annot;
            std::string what = std::string(form ? "Form XObject " : "appearance stream ") + s.ref.str();
            if (cos::has(s.stream, "Ref") || (s.stream.isStream() && cos::dict_of(s.stream).hasKey("/Ref")))
                ctx.report(r7201, page, where, what + " is a reference XObject (/Ref)");
            if (cos::dict_of(s.stream).hasKey("/StructParents") && s.ref.valid()) {
                if (!seen.insert(s.ref.key()).second)
                    ctx.report(r7202, page, where, what + " with /StructParents is painted more than once");
            }
        }
    }

    void pua(ValidationContext& ctx) {
        static const RuleIndex r8432 = R("EQX-UA2-8.4.3-2"), r8433 = R("EQX-UA2-8.4.3-3");
        auto has_pua = [](const std::string& raw) {
            for (char32_t c : pdf_text_to_u32(raw))
                if (is_pua(c)) return true;
            return false;
        };
        for (const StructElem& e : ctx.st.elems()) {
            Obj at = cos::get(e.dict, "ActualText");
            if (at.isString() && has_pua(at.getStringValue()))
                ctx.report(r8432, -1, e.ref, "ActualText of " + e.S.value_or("?") + " contains private use characters");
            Obj alt = cos::get(e.dict, "Alt");
            if (alt.isString() && has_pua(alt.getStringValue()))
                ctx.report(r8433, -1, e.ref, "Alt of " + e.S.value_or("?") + " contains private use characters");
        }
        const ContentModel& cm = ctx.content;
        for (const McFrame& f : cm.frames) {
            const StreamInstance& s = cm.streams[f.stream];
            if (!s.semantic || skipped(ctx, f.stream)) continue;   // only real content in PDF/UA flavours
            if (f.actual_text && has_pua(*f.actual_text))
                ctx.report(r8432, s.page, s.ref, "ActualText of /" + f.tag + " marked content contains private use characters");
            if (f.alt && has_pua(*f.alt))
                ctx.report(r8433, s.page, s.ref, "Alt of /" + f.tag + " marked content contains private use characters");
        }
    }

    void pdf_syntax(ValidationContext& ctx) {
        static const RuleIndex rp = R("EQX-PAC-FailPDFParsable");
        const ContentModel& cm = ctx.content;
        for (const auto& p : syntax_.problems) {
            const StreamInstance& s = cm.streams[p.stream];
            ctx.report(rp, s.page, s.ref, "content stream: " + p.detail);
        }
        for (const StreamInstance& s : cm.streams) {
            for (const LexError& e : s.errors)
                ctx.report(rp, s.page, s.ref, "content stream: " + e.message + " at byte " + std::to_string(e.offset));
            if (!s.decode_error.empty()) ctx.report(rp, s.page, s.ref, "content stream cannot be decoded: " + s.decode_error);
        }
        // File structure: qpdf had to rebuild the cross-reference table (startxref points to no xref section,
        // ISO 32000-1 7.5.4 / 7.5.5). PAC 26.1 ("The PDF document is corrupted ... Cross reference section not
        // found") and veraPDF 1.30.2 ("can not locate xref table") refuse such files, so neither reports anything;
        // eqx validates the reconstruction and reports the damage under PAC's parse-error condition
        // PDFParsable (which covers xref syntax).
        if (!ctx.pac_applicable) return;
        for (const std::string& w : ctx.doc.warnings())
            if (w.find("reconstruct cross-reference table") != std::string::npos) {
                ctx.report(rp, -1, ObjRef{}, "file structure: cross-reference table not found at startxref, rebuilt "
                                             "by qpdf (PAC 26.1 and veraPDF 1.30.2 refuse to open the file)");
                break;
            }
    }

    void text_contrast(ValidationContext& ctx) {
        static const RuleIndex rc = R("EQX-PAC-FailNotEnoughContrastForText");
        const ContentModel& cm = ctx.content;
        const auto& pages = ctx.doc.pages();
        std::vector<std::vector<int>> per_page(pages.size());
        for (std::size_t i = 0; i < cm.items.size(); ++i) {
            int pg = cm.streams[cm.items[i].stream].page;
            if (pg >= 0 && pg < static_cast<int>(pages.size())) per_page[pg].push_back(static_cast<int>(i));
        }
        static const float white[3] = {1, 1, 1};
        for (std::size_t pg = 0; pg < per_page.size(); ++pg) {
            Grid grid;
            grid.box = pages[pg].media_box;
            for (int idx : per_page[pg]) {
                const ContentItem& it = cm.items[idx];
                if (it.kind == ContentItem::Kind::Text) {
                    check_text(ctx, grid, idx, white, rc);
                    continue;
                }
                bool painter = (it.kind == ContentItem::Kind::LineArt && (it.paint & 1)) ||
                               it.kind == ContentItem::Kind::InlineImage || it.kind == ContentItem::Kind::ImageXObject ||
                               it.kind == ContentItem::Kind::Shading;
                if (!painter) continue;
                Rect r = it.bbox_valid ? it.bbox : grid.box;   // shadings without a box cover the page
                grid.add(idx, r);
            }
        }
    }

    void check_text(ValidationContext& ctx, const Grid& grid, int idx, const float white[3], RuleIndex rc) {
        const ContentModel& cm = ctx.content;
        const ContentItem& it = cm.items[idx];
        if (it.text < 0 || !it.bbox_valid) return;
        const TextRun& tr = cm.texts[it.text];
        if (tr.glyphs == 0) return;
        if (tr.tr == 3 || tr.tr == 7) return;                              // invisible text
        if (cm.parents_tags_contain(it.stream, it.frame, "Artifact")) return;   // incidental content
        if (tr.unmapped == 0 && is_whitespace_only(tr.text)) return;       // white space excluded (PAC 24.4.3)
        const bool stroke_only = tr.tr == 1 || tr.tr == 5;
        const Color& tc = stroke_only ? tr.stroke : tr.fill;
        if (!tc.known()) return;
        // Background: the last painter before the text that intersects it.
        int x0, y0, x1, y1;
        grid.cell_range(it.bbox, x0, y0, x1, y1);
        int best = -1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const auto& v = grid.cells[y * Grid::N + x];
                auto pos = std::lower_bound(v.begin(), v.end(), idx);
                while (pos != v.begin()) {
                    --pos;
                    if (*pos <= best) break;
                    const ContentItem& p = cm.items[*pos];
                    Rect pr = p.bbox_valid ? p.bbox : grid.box;
                    if (pr.intersects(it.bbox)) {
                        best = *pos;
                        break;
                    }
                }
            }
        const float* bg = white;
        if (best >= 0) {
            const ContentItem& p = cm.items[best];
            bool exact = p.kind == ContentItem::Kind::LineArt && p.exact_rect && p.fill.known() && p.fill_alpha >= 1.0f &&
                         covers(p.bbox, it.bbox, 0.5);
            if (!exact) return;   // background not determinable without rendering
            bg = p.fill.rgb;
        }
        float fg[3] = {tc.rgb[0], tc.rgb[1], tc.rgb[2]};
        double a = std::clamp(tr.fill_alpha, 0.0, 1.0);
        if (a < 1.0 && !stroke_only)
            for (int k = 0; k < 3; ++k) fg[k] = static_cast<float>(a * fg[k] + (1 - a) * bg[k]);
        double ratio = contrast(fg, bg);
        bool large = tr.size_user >= 18.0 || (tr.size_user >= 14.0 && bold_font(tr.font));
        double need = large ? 3.0 : 4.5;
        if (ratio + 1e-9 >= need) return;
        const StreamInstance& s = cm.streams[it.stream];
        char buf[200];
        std::snprintf(buf, sizeof buf, "contrast %.2f:1 (text %s on %s, %.1f pt%s), needs %.1f:1", ratio,
                      hex(fg).c_str(), hex(bg).c_str(), tr.size_user, large ? ", large" : "", need);
        std::string text = to_utf8(tr.text);
        if (text.size() > 40) text = text.substr(0, 40) + "...";
        ctx.report(rc, s.page, s.ref, std::string(buf) + (text.empty() ? "" : ": \"" + text + "\""));
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_content_module() { return std::make_unique<Module>(); }

}  // namespace eqx
