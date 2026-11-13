// Module "fonts": PDF/UA-1 7.21.x, PDF/UA-2 / WTPDF 8.4.5.x and 8.4.3-1 with veraPDF 1.30.2 semantics
// (validation-model GFPDFont / GFPDSimpleFont / GFPDTrueTypeFont / GFPDType1Font / GFPDType0Font /
// GFPDCIDFont / GFPDCmap / GFPDReferencedCMap / GFCMapFile / GFTrueTypeFontProgram / GFGlyph) plus the merged
// PAC 26.1 conditions (union semantics, PDF <= 1.7 only).
//
// Objects veraPDF reaches: fonts only through text-showing operators (OpTextShow link "font"), glyphs through
// "usedGlyphs" of the same operators (strings of the FIRST operand: arguments.get(0)). A font object is
// validated once (GFIDGenerator id = font dictionary identity): its rendering-mode dependent properties come
// from the first text-showing operator that uses it, in content order. Glyph rules are evaluated for every
// shown code (veraPDF caches identical glyph objects; only presence matters for findings).
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "fonts/model.h"
#include "rules/module.h"

namespace eqx {

namespace {

using fontimpl::GlyphFacts;
using fontimpl::VeraFont;
using fontimpl::VeraKind;

struct Rules {
    RuleIndex embedded1 = R("EQX-UA1-7.21.4.1-1"), embedded2 = R("EQX-UA2-8.4.5.5.1-1");
    RuleIndex glyph1 = R("EQX-UA1-7.21.4.1-2"), glyph2 = R("EQX-UA2-8.4.5.5.1-2");
    RuleIndex charset = R("EQX-UA1-7.21.4.2-1"), cidset = R("EQX-UA1-7.21.4.2-2");
    RuleIndex width1 = R("EQX-UA1-7.21.5-1"), width2 = R("EQX-UA2-8.4.5.6-1");
    RuleIndex ttcmap1 = R("EQX-UA1-7.21.6-1"), ttcmap2 = R("EQX-UA2-8.4.5.7-1");
    RuleIndex ttenc1 = R("EQX-UA1-7.21.6-2"), ttenc2 = R("EQX-UA2-8.4.5.7-2");
    RuleIndex ttsymenc1 = R("EQX-UA1-7.21.6-3"), ttsymenc2 = R("EQX-UA2-8.4.5.7-3");
    RuleIndex ttsymcmap1 = R("EQX-UA1-7.21.6-4"), ttsymcmap2 = R("EQX-UA2-8.4.5.7-4");
    RuleIndex unicode1 = R("EQX-UA1-7.21.7-1"), unicode2 = R("EQX-UA2-8.4.5.8-1");
    RuleIndex badunicode1 = R("EQX-UA1-7.21.7-2"), badunicode2 = R("EQX-UA2-8.4.5.8-2");
    RuleIndex notdef1 = R("EQX-UA1-7.21.8-1"), notdef2 = R("EQX-UA2-8.4.5.9-1");
    RuleIndex pua = R("EQX-UA2-8.4.3-1");
    RuleIndex csi1 = R("EQX-UA1-7.21.3.1-1"), csi2 = R("EQX-UA2-8.4.5.3.1-1");
    RuleIndex c2g1 = R("EQX-UA1-7.21.3.2-1"), c2g2 = R("EQX-UA2-8.4.5.3.2-1");
    RuleIndex cmapemb1 = R("EQX-UA1-7.21.3.3-1"), cmapemb2 = R("EQX-UA2-8.4.5.4-1");
    RuleIndex wmode1 = R("EQX-UA1-7.21.3.3-2"), wmode2 = R("EQX-UA2-8.4.5.4-2");
    RuleIndex usecmap1 = R("EQX-UA1-7.21.3.3-3"), usecmap2 = R("EQX-UA2-8.4.5.4-3");
};

const Rules& rules() {
    static const Rules r;
    return r;
}

std::string hex_code(std::uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", c);
    return b;
}

// fontName.search(/[A-Z]{6}\+/) == 0
bool subset_prefix(const std::optional<std::string>& n) {
    if (!n || n->size() < 7) return false;
    for (int i = 0; i < 6; ++i)
        if ((*n)[static_cast<std::size_t>(i)] < 'A' || (*n)[static_cast<std::size_t>(i)] > 'Z') return false;
    return (*n)[6] == '+';
}

struct Use {
    const VeraFont* font = nullptr;
    int first_tr = 0;
    bool any_rendered = false;   // a use with Tr != 3 (the fonts PAC counts as rendered)
    int stream = -1;
};

struct GlyphKey {
    const void* font;
    std::uint32_t code;
    std::int32_t stream;
    std::int32_t extra;   // rendering-mode class / frame
    bool operator==(const GlyphKey& o) const {
        return font == o.font && code == o.code && stream == o.stream && extra == o.extra;
    }
};
struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& k) const {
        std::size_t h = std::hash<const void*>()(k.font);
        h ^= (static_cast<std::size_t>(k.code) * 0x9E3779B97F4A7C15ull) + (h << 6) + (h >> 2);
        h ^= (static_cast<std::size_t>(static_cast<std::uint32_t>(k.stream)) << 20) ^ static_cast<std::size_t>(k.extra);
        return h;
    }
};

class Module final : public RuleModule, public ContentObserver {
public:
    const char* name() const override { return "fonts"; }

    void prepare(ValidationContext& ctx, std::vector<ContentObserver*>& obs) override {
        ctx_ = &ctx;
        obs.push_back(this);
    }

    void on_operator(const ContentModel& m, int si, std::uint32_t, const RawOp& op, const GState& gs) override {
        if (si < 0) return;
        if (ctx_->reference_dedup && m.streams[static_cast<std::size_t>(si)].repeat) return;
        if (static_cast<std::size_t>(si) >= frames_.size()) frames_.resize(static_cast<std::size_t>(si) + 1);
        const std::string_view o = op.op;
        if (o == "BMC" || o == "BDC") {
            // the interpreter creates exactly one frame per BMC/BDC, right after this callback
            frames_[static_cast<std::size_t>(si)].push_back(static_cast<int>(m.frames.size()));
            return;
        }
        if (o == "EMC") {
            auto& st = frames_[static_cast<std::size_t>(si)];
            if (!st.empty()) st.pop_back();
            return;
        }
        if (o != "Tj" && o != "TJ" && o != "'" && o != "\"") return;
        const auto* font = dynamic_cast<const VeraFont*>(gs.font);
        if (!font) return;
        if (font->vkind == VeraKind::None) {
            if (font->kind == FontKind::Type0) type0_without_encoding_ = true;
            return;
        }
        register_use(font, gs.tr, si);
        const bool raw_ok = !ctx_->reference_dedup || raw_font_parsed(m, si, font, gs.tr);
        // GFOpTextShow.getStrings: the first operand only
        const std::vector<Operand>& a = *op.operands;
        if (a.empty()) return;
        // The " operator's string is its last operand: veraPDF's first-operand reading misses that text
        // (--reference-dedup keeps it), so text shown this way gets no glyph check there.
        const Operand& first = !ctx_->reference_dedup && o == "\"" ? a.back() : a.front();
        const StreamInstance& s = m.streams[static_cast<std::size_t>(si)];
        const auto& fst = frames_[static_cast<std::size_t>(si)];
        int frame = fst.empty() ? -1 : fst.back();
        auto show = [&](const std::string& bytes) {
            codes_.clear();
            font->vera_codes(bytes, codes_);
            for (const CharCode& c : codes_) glyph(m, s, si, frame, font, c.code, gs.tr, raw_ok);
        };
        if (first.type == Operand::String) show(first.s);
        else if (first.type == Operand::Array)
            for (const Operand& e : first.items)
                if (e.type == Operand::String) show(e.s);
    }

    void on_stream_end(const ContentModel&, int si) override {
        if (si >= 0 && static_cast<std::size_t>(si) < frames_.size()) frames_[static_cast<std::size_t>(si)].clear();
    }

    void run(ValidationContext& ctx) override {
        ctx_ = &ctx;
        const Rules& r = rules();
        for (const Use& u : uses_) evaluate_font(ctx, u);
        if (type0_without_encoding_) {
            for (RuleIndex x : {r.csi1, r.csi2, r.cmapemb1, r.cmapemb2})
                ctx.mark_incomplete(x, "a Type 0 font has no /Encoding (veraPDF 1.30.2 cannot build the font object)");
        }
        for (RuleIndex x : charset_incomplete_) ctx.mark_incomplete(x, "the Type 1 font's CFF program is CID-keyed");
    }

private:
    static std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
        return a * 0x9E3779B97F4A7C15ull ^ (b + 0x7F4A7C15ull + (a << 6) + (a >> 2));
    }

    // veraPDF's PDResources instance a stream's fonts come from: one per page (also with a shared /Resources
    // object), one per annotation appearance, one per form / pattern / Type 3 glyph with own /Resources (else the
    // invoking stream's).
    static std::uint64_t resource_context(const ContentModel& m, int si) {
        for (int guard = 0; guard < 64 && si >= 0; ++guard) {
            const StreamInstance& s = m.streams[static_cast<std::size_t>(si)];
            if (s.kind == StreamInstance::Kind::Page) return (1ull << 60) | static_cast<std::uint64_t>(s.page);
            if (s.kind == StreamInstance::Kind::Annotation || s.parent < 0 ||
                cos::get(s.stream, "Resources").isDictionary())
                return (2ull << 60) | static_cast<std::uint64_t>(si);
            si = s.parent;
        }
        return 3ull << 60;
    }

    // The successfullyParsed flag of veraPDF's raw font for this text-showing operator (see raw_parsed_).
    bool raw_font_parsed(const ContentModel& m, int si, const VeraFont* f, int tr) {
        if (f->vkind == VeraKind::Type3) return true;   // GFGlyph: Type 3 glyphs never depend on the flag
        const std::uint64_t fk = reinterpret_cast<std::uintptr_t>(f);
        const std::uint64_t rk = mix(resource_context(m, si), fk);
        if (gf_fonts_.insert(mix(fk, static_cast<std::uint64_t>(tr) + 1)).second)
            raw_parsed_[rk] = f->program && f->program_parsed;
        auto it = raw_parsed_.find(rk);
        return it != raw_parsed_.end() && it->second;
    }

    // GFPDReferencedCMap (the /UseCMap of a CMap) extends GFPDCmap: besides its own rule (7.21.3.3-3 /
    // 8.4.5.4-3: only Table 118 CMaps may be referenced) it is a PDCMap (7.21.3.3-1 / 8.4.5.4-1: predefined or
    // embedded), its stream is a CMapFile (7.21.3.3-2 / 8.4.5.4-2: WMode) and its own /UseCMap is linked again.
    // Once per referenced object. (veraPDF corpus 7.21.3.3-t03-fail-a / 8.4.5.4-t03-fail-a: a /UseCMap naming
    // Adobe-Korea1-2 fails -1 and -3.)
    void referenced_cmap(ValidationContext& ctx, const Obj& use, const std::string& parent_name, ObjRef parent_loc,
                         int page) {
        const Rules& r = rules();
        std::string parent = parent_name;
      for (Obj cur_use = use;;) {   // the /UseCMap chain: every indirect object once, so it ends
        const Obj& use = cur_use;
        const std::string& parent_name = parent;
        if (use.isNull()) return;
        ObjRef ur = ref_of(use);
        if (ur.valid() && !refcmaps_seen_.insert(ur.key()).second) return;
        if (ur.valid()) parent_loc = ur;
        std::string un;
        if (use.isName()) un = *cos::name(use);
        else if (use.isStream() || use.isDictionary()) {
            Obj n = cos::get(use, "CMapName");
            if (n.isName()) un = *cos::name(n);
            else if (n.isString()) un = cos::text(n).value_or("");
        }
        const ObjRef ul = ur.valid() ? ur : parent_loc;
        const bool predefined = fontimpl::is_table118_cmap(un);
        if (!predefined) {
            std::string d = "CMap " + parent_name + " uses the non-predefined CMap " + un;
            ctx.report(r.usecmap1, page, ul, d);
            ctx.report(r.usecmap2, page, ul, d);
        }
        if (!predefined && !use.isStream()) {
            std::string d = "CMap " + un + " (UseCMap of " + parent_name + ") is neither predefined nor embedded";
            ctx.report(r.cmapemb1, page, ul, d);
            ctx.report(r.cmapemb2, page, ul, d);
        }
        if (use.isStream()) {
            int wmode = 0;
            if (auto data = cos::stream_data(use))
                if (auto file = fontimpl::parse_cmap_program(*data)) wmode = file->wmode;
            Obj dw = cos::get(use, "WMode");
            std::int64_t dict_wmode = dw.isInteger() ? dw.getIntValue() : dw.isReal() ? static_cast<std::int64_t>(dw.getNumericValue()) : 0;
            if (wmode != dict_wmode) {
                std::string d = "CMap " + un + " (UseCMap of " + parent_name + "): WMode " + std::to_string(wmode) +
                                " in the data, " + std::to_string(dict_wmode) + " in the dictionary";
                ctx.report(r.wmode1, page, ul, d);
                ctx.report(r.wmode2, page, ul, d);
            }
            Obj next = cos::get(use, "UseCMap");
            parent = un;
            cur_use = next;
            continue;
        }
        return;
      }
    }

    void register_use(const VeraFont* f, int tr, int si) {
        f->ensure_program();   // veraPDF creates (and caches) font programs at the first text-showing operator
        auto it = use_index_.find(f);
        if (it == use_index_.end()) {
            use_index_.emplace(f, uses_.size());
            uses_.push_back(Use{f, tr, tr != 3, si});
        } else if (tr != 3) {
            uses_[it->second].any_rendered = true;
        }
    }

    void report_glyph(RuleIndex a, RuleIndex b, const StreamInstance& s, const VeraFont* f, std::uint32_t code,
                      const char* what) {
        std::string d = std::string(what) + " font " + f->font_name.value_or(f->base_font) + " code " + hex_code(code);
        ctx_->report(a, s.page, s.ref, d);
        if (b != kNoRule) ctx_->report(b, s.page, s.ref, d);
    }

    // Dedup key classes (GlyphKey::extra): 0 / 1 rendering mode not 3 / 3, 4 program facts, 16 + frame PUA.
    void glyph(const ContentModel& m, const StreamInstance& s, int si, int frame, const VeraFont* f,
               std::uint32_t code, int tr, bool raw_ok) {
        const Rules& r = rules();
        const bool tr3 = tr == 3;
        const bool real = s.semantic;
        const GlyphFacts& g = f->glyph(code);
        // --reference-dedup: GFGlyph computes presence and widths only when its raw font instance was marked
        // successfully parsed when the glyph object was created (glyph ids: font, code, rendering mode, marked
        // content, stream); a reused glyph object keeps that state.
        bool facts = true;
        if (ctx_->reference_dedup)
            facts = glyph_objects_.emplace(GlyphKey{f, code, frame >= 0 ? -1 : si, frame >= 0 ? frame * 8 + tr : tr},
                                           raw_ok).first->second;
        if (!tr3 && facts && seen_.insert(GlyphKey{f, code, si, 4}).second) {
            if (g.present && !*g.present) report_glyph(r.glyph1, r.glyph2, s, f, code, "glyph missing:");
            // The rule is |a - b| <= 1: a NaN width (a broken unitsPerEm) does not satisfy it.
            if (g.width_program && g.width_dictionary && !(std::fabs(*g.width_program - *g.width_dictionary) <= 1))
                report_glyph(r.width1, r.width2, s, f, code, "width mismatch:");
        }
        if (seen_.insert(GlyphKey{f, code, si, tr3 ? 1 : 0}).second) {
            if (!g.has_unicode) report_glyph(r.unicode1, kNoRule, s, f, code, "no Unicode mapping:");
            else if (g.unicode_invalid) report_glyph(r.badunicode1, kNoRule, s, f, code, "Unicode 0/FFFE/FEFF:");
            if (!g.has_unicode20) report_glyph(r.unicode2, kNoRule, s, f, code, "no Unicode mapping:");
            else if (g.unicode_invalid20) report_glyph(r.badunicode2, kNoRule, s, f, code, "Unicode 0/FFFE/FEFF:");
            if (g.name == GlyphFacts::Name::Notdef) report_glyph(r.notdef1, r.notdef2, s, f, code, ".notdef glyph:");
        }
        if (real && g.has_unicode20 && g.unicode_pua20 && seen_.insert(GlyphKey{f, code, si, 16 + frame}).second) {
            if (!actual_or_alt(m, s, frame)) report_glyph(r.pua, kNoRule, s, f, code, "PUA without ActualText/Alt:");
        }
    }

    // MarkedContentHelper.containsStringKey(ActualText) || (Alt)
    bool actual_or_alt(const ContentModel& m, const StreamInstance& s, int frame) const {
        if (frame < 0) return false;
        if (m.inherited_actual_text(frame) || m.inherited_alt(frame)) return true;
        if (!ctx_->st.present() || !ctx_->st.has_parent_tree()) return false;
        std::optional<std::int64_t> mcid = m.inherited_mcid(frame);
        Obj e = ctx_->st.struct_elem_for_mcid(s.struct_parent, s.struct_parents, mcid);
        if (!e.isDictionary()) return false;
        return cos::get(e, "ActualText").isString() || cos::get(e, "Alt").isString();
    }

    // Whether the embedding rules apply to the font: veraPDF validates a font object once, with the rendering
    // mode of its first text-showing operator (--reference-dedup reproduces that); by default every use counts,
    // so a font first used invisibly (Tr 3) and later for rendered text must be embedded (ISO 14289-1 7.21.4.1,
    // ISO 14289-2 8.4.5.5.1: fonts used for rendering).
    static bool renders(const ValidationContext& ctx, const Use& u) {
        return ctx.reference_dedup ? u.first_tr != 3 : u.any_rendered;
    }

    // The same for a CIDFont shared by several Type 0 fonts (veraPDF: the first Type 0 font's first use).
    bool cid_renders(const ValidationContext& ctx, const VeraFont& f) const {
        if (ctx.reference_dedup) return use_index_.count(&f) && uses_[use_index_.at(&f)].first_tr != 3;
        for (const Use& o : uses_)
            if (o.any_rendered && o.font->cid.present &&
                (o.font == &f || (f.cid.ref.valid() && o.font->cid.ref == f.cid.ref)))
                return true;
        return false;
    }

    void evaluate_font(ValidationContext& ctx, const Use& u) {
        const Rules& r = rules();
        const VeraFont& f = *u.font;
        const StreamInstance& s = ctx.content.streams[static_cast<std::size_t>(u.stream)];
        const int page = s.page;
        const ObjRef loc = s.ref;
        const std::string fname = f.font_name.value_or(f.base_font);
        const bool pac = ctx.pac_applicable;
        auto both = [&](RuleIndex a, RuleIndex b, const std::string& d) {
            ctx.report(a, page, loc, d);
            ctx.report(b, page, loc, d);
        };
        f.ensure_program();
        const bool contains_font_file = f.program && f.program_parsed;

        if (f.vkind == VeraKind::Type0) {
            evaluate_type0(ctx, u, page, loc, fname);
            return;
        }
        // PDFont (Type1 / MMType1 / TrueType / Type3)
        if (f.vkind != VeraKind::Type3 && renders(ctx, u) && (!contains_font_file || f.program_stream_damaged))
            both(r.embedded1, r.embedded2, "font " + fname + " is not embedded");
        else if (pac && f.vkind != VeraKind::Type3 && u.any_rendered && !f.has_font_file_entry)
            ctx.report(r.embedded1, page, loc, "PAC: font " + fname + " is not embedded (rendered text)");

        if (f.vkind == VeraKind::Type1) {
            if (contains_font_file && subset_prefix(f.font_name) && f.charset) {
                bool undecidable = false;
                bool ok = f.charset_lists_all_glyphs(&undecidable);
                if (undecidable) charset_incomplete_.insert(r.charset);
                else if (!ok) ctx.report(r.charset, page, loc, "font " + fname + ": CharSet does not list the program's glyphs");
            }
        }
        if (f.vkind == VeraKind::TrueType) {
            const std::string enc = f.encoding_prop.value_or("");
            bool enc_ok = f.encoding_prop && (enc == "MacRomanEncoding" || enc == "WinAnsiEncoding");
            if (!f.symbolic && !(enc_ok && (!f.contains_differences || f.differences_unicode_compliant())))
                both(r.ttenc1, r.ttenc2, "non-symbolic TrueType font " + fname + ": Encoding " + (f.encoding_prop ? enc : "null"));
            else if (pac && !f.symbolic && f.contains_differences && !f.all_differences_in_agl())
                ctx.report(r.ttenc1, page, loc, "PAC: non-symbolic TrueType font " + fname + ": Differences glyph name not in the AGL");
            if (f.symbolic && f.encoding_prop) both(r.ttsymenc1, r.ttsymenc2, "symbolic TrueType font " + fname + " has an Encoding");
            else if (pac && f.symbolic && !cos::get(f.dict, "Encoding").isNull())
                ctx.report(r.ttsymenc1, page, loc, "PAC: symbolic TrueType font " + fname + " has an Encoding entry");
            // TrueTypeFontProgram (linked only when the program was parsed)
            if (contains_font_file) {
                if (const fontimpl::TrueTypeTables* t = fontimpl::truetype_tables(f.program.get())) {
                    std::size_t nr = t->has_cmap ? t->cmaps.size() : 0;
                    bool c30 = t->cmap(3, 0), c31 = t->cmap(3, 1), c10 = t->cmap(1, 0);
                    std::string d = "TrueType program of " + fname + ": " + std::to_string(nr) + " cmap subtable(s)";
                    if (!f.symbolic && !(c30 ? nr > 1 : nr > 0)) ctx.report(r.ttcmap1, page, loc, d);
                    if (f.symbolic && !(nr == 1 || c30)) ctx.report(r.ttsymcmap1, page, loc, d);
                    if (!f.symbolic && !(c31 || c10)) ctx.report(r.ttcmap2, page, loc, d);
                    if (f.symbolic && !(c30 || c10)) ctx.report(r.ttsymcmap2, page, loc, d);
                }
            }
        }
    }

    void evaluate_type0(ValidationContext& ctx, const Use& u, int page, ObjRef loc, const std::string& fname) {
        const Rules& r = rules();
        const VeraFont& f = *u.font;
        fontimpl::PdCMap& cm = *f.cmap;
        const std::string cmap_name = cm.cmap_name();
        // PDType0Font: CIDSystemInfo compatibility (flavour-dependent data for predefined CMap names)
        for (int fl = 0; fl < 2; ++fl) {
            bool pdf20 = fl == 1;
            if (cmap_name == "Identity-H" || cmap_name == "Identity-V") break;
            auto reg = cm.registry(pdf20);
            auto ord = cm.ordering(pdf20);
            std::int64_t sup = cm.supplement(pdf20).value_or(0);
            bool ok = f.cidfont_ordering && ord && *f.cidfont_ordering == *ord && f.cidfont_registry && reg &&
                      *f.cidfont_registry == *reg && f.cidfont_supplement <= sup;
            if (!ok) {
                std::string d = "Type 0 font " + fname + ": CIDFont " + f.cidfont_registry.value_or("null") + "-" +
                                f.cidfont_ordering.value_or("null") + "-" + std::to_string(f.cidfont_supplement) +
                                ", CMap " + reg.value_or("null") + "-" + ord.value_or("null") + "-" + std::to_string(sup);
                ctx.report(pdf20 ? r.csi2 : r.csi1, page, loc, d);
            }
        }
        // PDCMap (Encoding), once per CMap object
        bool cmap_new = !cm.ref.valid() || cmaps_seen_.insert(cm.ref.key()).second;
        ObjRef cmap_loc = cm.ref.valid() ? cm.ref : loc;
        if (cmap_new) {
            if (!cm.is_stream() && !fontimpl::is_table118_cmap(cmap_name)) {
                std::string d = "CMap " + cmap_name + " is neither predefined nor embedded";
                ctx.report(r.cmapemb1, page, cmap_loc, d);
                ctx.report(r.cmapemb2, page, cmap_loc, d);
            }
            if (cm.is_stream()) {   // CMapFile
                auto file = cm.file();
                int wmode = 0;
                if (file) {
                    // the CMap program's own WMode (CMapFile parses the stream without the /UseCMap merge)
                    wmode = file->wmode;
                }
                Obj dw = cos::get(cm.obj, "WMode");
                std::int64_t dict_wmode = dw.isInteger() ? dw.getIntValue() : dw.isReal() ? static_cast<std::int64_t>(dw.getNumericValue()) : 0;
                if (wmode != dict_wmode) {
                    std::string d = "CMap " + cmap_name + ": WMode " + std::to_string(wmode) + " in the data, " +
                                    std::to_string(dict_wmode) + " in the dictionary";
                    ctx.report(r.wmode1, page, cmap_loc, d);
                    ctx.report(r.wmode2, page, cmap_loc, d);
                }
            }
            referenced_cmap(ctx, cm.use_cmap(), cmap_name, cmap_loc, page);
        }
        // PDCIDFont (descendant), once per CIDFont dictionary
        if (!f.cid.present) return;
        const void* key = f.cid.ref.valid() ? reinterpret_cast<const void*>(static_cast<std::uintptr_t>(f.cid.ref.key()))
                                            : static_cast<const void*>(&f);
        if (!cidfonts_seen_.insert(key).second) return;
        const auto& c = f.cid;
        const std::string cname = c.font_name.value_or("");
        bool cff = c.contains_font_file;
        if (cid_renders(ctx, f) && (!cff || f.program_stream_damaged)) {
            ctx.report(r.embedded1, page, loc, "CIDFont " + cname + " is not embedded");
            ctx.report(r.embedded2, page, loc, "CIDFont " + cname + " is not embedded");
        } else if (ctx.pac_applicable && u.any_rendered && !f.has_font_file_entry) {
            ctx.report(r.embedded1, page, loc, "PAC: CIDFont " + cname + " is not embedded (rendered text)");
        }
        if (c.subtype == "CIDFontType2" && !c.cid_to_gid_map && cff) {
            ctx.report(r.c2g1, page, loc, "CIDFontType2 " + cname + " without a valid CIDToGIDMap");
            ctx.report(r.c2g2, page, loc, "CIDFontType2 " + cname + " without a valid CIDToGIDMap");
        }
        if (cff && subset_prefix(c.font_name) && c.contains_cid_set && !f.cid_set_lists_all_glyphs(ctx.reference_dedup))
            ctx.report(r.cidset, page, loc, "CIDFont " + cname + ": CIDSet does not identify all CIDs of the program");
    }

    ValidationContext* ctx_ = nullptr;
    std::vector<std::vector<int>> frames_;
    std::vector<Use> uses_;
    std::unordered_map<const VeraFont*, std::size_t> use_index_;
    std::unordered_set<GlyphKey, GlyphKeyHash> seen_;
    // --reference-dedup: veraPDF's raw font objects. PDResources builds one org.verapdf.pd.font.PDFont per
    // resource dictionary instance and name; FontFactory caches the validation font object per (font
    // dictionary, rendering mode) and only its constructor marks the raw font it was built from as successfully
    // parsed. A raw font of a later resource context (another page, a form with own /Resources, an annotation
    // appearance) therefore stays "not parsed" unless it is the first to use a (font, rendering mode) pair, and
    // the glyphs it shows have no presence / width facts (probes fq_page2_*, fq_form_*, fq_annot_first).
    std::unordered_set<std::uint64_t> gf_fonts_;                       // (font, rendering mode) with a GF object
    std::unordered_map<std::uint64_t, bool> raw_parsed_;              // (resource context, font) -> parsed flag
    std::unordered_map<GlyphKey, bool, GlyphKeyHash> glyph_objects_;  // glyph ids -> facts computed
    std::unordered_set<std::uint64_t> cmaps_seen_, refcmaps_seen_;
    std::unordered_set<const void*> cidfonts_seen_;
    std::unordered_set<RuleIndex> charset_incomplete_;
    std::vector<CharCode> codes_;
    bool type0_without_encoding_ = false;
};

}  // namespace

std::unique_ptr<RuleModule> make_fonts_module() { return std::make_unique<Module>(); }

}  // namespace eqx
