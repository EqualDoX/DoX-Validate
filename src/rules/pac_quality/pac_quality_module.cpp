// PAC 26.1 Quality-tab recommendations (EQX-PACQ-*) and the PDF/UA-tab language checks of alternative,
// actual and expansion texts (EQX-PAC-Fail*HasLanguage).
//
// PAC is closed source. The semantics below reproduce its observable behaviour: the condition texts and the
// evaluation tables of PAC's documentation (pac.pdf-accessibility.org/en/resources/quality-checks/...) and
// the object counts of stored PAC detail-view read-outs of synthetic development fixtures. Where neither
// decides a question, the most literal and conservative reading is used (marked "open" in the comments).
//
// Structure types are compared by their standard type with PDF 1.7 semantics (PAC evaluates every file,
// also PDF 2.0, as PDF/UA-1).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rules/module.h"
#include "rules/pac_quality/pac_contrast.h"
#include "rules/pac_quality/wcag_color.h"
#include "rules/pac_quality/pac_quality_text.h"
#include "util/text.h"

namespace eqx {

namespace {

using pacq::whitespace_only;

bool is_heading(const std::string& t) {
    return t == "H" || (t.size() == 2 && t[0] == 'H' && t[1] >= '1' && t[1] <= '6');
}

// TextTagContainsOnlyWhitespace: H, H1-H6, P, Caption, Table, L, Lbl, Quote, BlockQuote, Note,
// Reference, BibEntry, Code, Annot (PAC documentation).
bool whitespace_checked_type(const std::string& t) {
    static const std::unordered_set<std::string> s = {"H",     "H1",    "H2",        "H3",   "H4",        "H5",
                                                      "H6",    "P",     "Caption",   "Table", "L",        "Lbl",
                                                      "Quote", "BlockQuote", "Note", "Reference", "BibEntry", "Code",
                                                      "Annot"};
    return s.count(t) > 0;
}

// TextTagHasAltText: H, H1-H6, P, Lbl, Quote, Note, Reference, BibEntry, Code, Annot, and Span (the documentation does not name it; probes tb_alt_span*, 120 on
// a real-world sample).
bool text_alt_checked_type(const std::string& t) {
    static const std::unordered_set<std::string> s = {"H",     "H1",   "H2",        "H3",       "H4",   "H5", "H6",
                                                      "P",     "Lbl",  "Quote",     "Note",     "Reference",
                                                      "BibEntry", "Code", "Annot", "Span"};
    return s.count(t) > 0;
}

// PAC's type of an element: a standard /S counts as itself even when the RoleMap remaps it (probe ua_rm_unused_std_std: /H1 -> /P, the H1 still is a
// heading for DocumentHasHeadings; same rule as pac_structure), a non-standard /S through the RoleMap.
std::string std17(const StructElem& e) {
    if (e.S && StructTree::is_standard17(*e.S)) return *e.S;
    return e.std17.value_or(std::string());
}

// Text accumulation over text runs in content order: a space is inserted between runs on different
// lines or separated by a visible gap, so words of separate lines/operators do not merge.
struct TextAcc {
    std::u32string text;
    std::uint32_t unmapped = 0;
    std::uint32_t glyphs = 0;
    Rect last;
    bool has_last = false;

    void add(const TextRun& r) {
        glyphs += r.glyphs;
        unmapped += r.unmapped;
        if (r.text.empty()) return;
        if (has_last && !text.empty() && !pacq::is_whitespace(text.back()) && !pacq::is_whitespace(r.text.front())) {
            double h = std::max(1.0, std::min(last.height(), r.bbox.height()));
            bool new_line = std::fabs(r.bbox.lly - last.lly) > 0.5 * h;
            bool gap = r.bbox.llx - last.urx > 0.25 * h;
            if (new_line || gap) text.push_back(U' ');
        }
        text += r.text;
        last = r.bbox;
        has_last = true;
    }
};

void unite(Rect& acc, bool& valid, const Rect& r) {
    if (!valid) {
        acc = r;
        valid = true;
        return;
    }
    acc.llx = std::min(acc.llx, r.llx);
    acc.lly = std::min(acc.lly, r.lly);
    acc.urx = std::max(acc.urx, r.urx);
    acc.ury = std::max(acc.ury, r.ury);
}

// Content of one marked-content sequence with an MCID.
// Ascent of a font in em (FontDescriptor /Ascent / 1000; Type 0: of the descendant), 0.8 (the glyph box top
// used everywhere else) when the descriptor gives none or an implausible value. Used for the heading top that
// PAC's TOCI destination check measures from (Phase 3 calibration, see check_toc_destination).
double ascent_em(const Font* f) {
    if (!f) return 0.8;
    Obj d = f->dict;
    if (cos::name_is(d, "Subtype", "Type0")) {
        Obj desc = cos::get(d, "DescendantFonts");
        d = desc.isArray() && desc.getArrayNItems() > 0 ? desc.getArrayItem(0) : Obj();
    }
    std::optional<double> a = cos::number(cos::get(cos::get(d, "FontDescriptor"), "Ascent"));
    return a && *a > 0 && *a < 2500 ? *a / 1000.0 : 0.8;
}

// ascent_em per font: the font dictionary is read once per font, not once per text item.
struct AscentCache {
    std::unordered_map<const Font*, double> by_font;
    const Font* last = nullptr;
    double last_value = 0;
    bool have_last = false;
    double of(const Font* f) {
        if (have_last && f == last) return last_value;
        auto pos = by_font.find(f);
        if (pos == by_font.end()) pos = by_font.emplace(f, ascent_em(f)).first;
        last = f;
        last_value = pos->second;
        have_last = true;
        return last_value;
    }
};

struct McContent {
    int frame = -1;
    int page = -1;
    double ascent_top = 0;        // highest baseline + Ascent * size of its text items (upright text)
    bool ascent_top_valid = false;
    TextAcc acc;
    bool nontext = false;     // images, paths, shadings
    Rect box;                 // union of all item boxes on the page
    bool box_valid = false;

    void add_item(const ContentItem& it, const TextRun* run, AscentCache& ascents) {
        if (run) {
            acc.add(*run);
            if (it.bbox_valid && run->size_user > 0) {
                // the item box spans -0.2 .. +0.8 em around the baseline
                const double top = it.bbox.ury - 0.8 * run->size_user + ascents.of(run->font) * run->size_user;
                if (!ascent_top_valid || top > ascent_top) ascent_top = top;
                ascent_top_valid = true;
            }
        } else {
            nontext = true;
        }
        if (it.bbox_valid) unite(box, box_valid, it.bbox);
    }
};

McContent make_mc(int frame, int page) {
    McContent m;
    m.frame = frame;
    m.page = page;
    return m;
}

// Own content of a structure element: the marked-content sequences its MCID / MCR kids reference (in /K
// order), plus content attributed to it only through a Form XObject's /StructParent.
struct ElemContent {
    TextAcc acc;
    bool nontext = false;
    bool has_items = false;
    int page = -1;            // page of the first own content
    Rect box;                 // union of own content boxes on `page`
    bool box_valid = false;
    double ascent_top = 0;    // highest ascent top of the text on `page` (McContent::ascent_top)
    bool ascent_top_valid = false;

    void add(const McContent& mc) {
        if (!mc.acc.text.empty() && !acc.text.empty() && !pacq::is_whitespace(acc.text.back()) &&
            !pacq::is_whitespace(mc.acc.text.front()))
            acc.text.push_back(U' ');
        acc.text += mc.acc.text;
        acc.unmapped += mc.acc.unmapped;
        acc.glyphs += mc.acc.glyphs;
        nontext = nontext || mc.nontext;
        if (!has_items) page = mc.page;
        has_items = true;
        if (mc.box_valid && mc.page == page) unite(box, box_valid, mc.box);
        if (mc.ascent_top_valid && mc.page == page && (!ascent_top_valid || mc.ascent_top > ascent_top)) {
            ascent_top = mc.ascent_top;
            ascent_top_valid = true;
        }
    }
};

// Explicit destination of a link (ISO 32000-1 12.3.2.2).
struct ExplicitDest {
    int page = -1;
    std::string type;
    std::vector<std::optional<double>> params;
};

enum class DestStatus { None, NotGoTo, Explicit, NameObject, NameMissing, Malformed };

struct LinkDest {
    DestStatus status = DestStatus::None;
    ExplicitDest dest;
    Obj sd;                   // GoTo /SD structure destination (PDF 2.0)
    std::string name;
};

// Character codes without any mapping (ISO 14289-1 7.2, Matterhorn 10-001): a simple font without /ToUnicode, in WinAnsi or MacRoman, showing a code that encoding does not define.
// PAC 26.1 reports unmappable characters per text operator (probes tu_em_01, tu_ne_01, tu_mac_7f, tu_mac_f0); veraPDF asks the font program and passes them.
// Not imitated: PAC also fails WinAnsi 0x7F 0x81 0x8D 0x8F 0x90 0x9D (ISO 32000-1 Annex D: unused codes above 0o40 map to the bullet) and WinAnsi 0xA0, MacRoman 0xCA (the same
// annex: a second code of space), recorded in catalog/pac_rule_evidence.json.
struct UnmappedCodes final : ContentObserver {
    struct Hit {
        int stream;
        std::uint32_t op;
        std::string detail;
    };
    std::vector<Hit> hits;
    std::uint64_t last = ~std::uint64_t(0);
    std::unordered_map<const Font*, int> kind;   // 0 not applicable, 1 WinAnsi, 2 MacRoman
    const Font* last_font = nullptr;             // the font of the last glyph and its kind (glyphs come in runs of one font)
    int last_kind = 0;
    void on_glyph(const ContentModel&, const GlyphEvent& g) override {
        if (g.font == last_font && last_kind == 0) return;   // no font, or the font of the glyph before, which is not applicable
        if (!g.font || !g.real_content || hits.size() >= 10000) return;
        if (g.font != last_font) {
            auto [it, fresh] = kind.emplace(g.font, 0);
            if (fresh) {
                const Obj& d = g.font->dict;
                const auto sub = cos::name(d, "Subtype").value_or("");
                if ((sub == "TrueType" || sub == "Type1" || sub == "MMType1") && !cos::has(d, "ToUnicode")) {
                    Obj enc = cos::get(d, "Encoding");
                    if (enc.isDictionary()) enc = cos::get(enc, "BaseEncoding");
                    const auto n = cos::name(enc).value_or("");
                    it->second = n == "WinAnsiEncoding" ? 1 : n == "MacRomanEncoding" ? 2 : 0;
                    // a /Differences array may define the code: the fonts module decides those
                    if (cos::get(cos::get(d, "Encoding"), "Differences").isArray()) it->second = 0;
                }
            }
            last_font = g.font;
            last_kind = it->second;
        }
        const int font_kind = last_kind;
        if (font_kind == 0) return;
        const std::uint32_t c = std::uint32_t(g.code.code);
        const bool undefined = c < 0x20 || (font_kind == 2 && (c == 0x7F || c == 0xF0));
        if (!undefined) return;
        const std::uint64_t key = (std::uint64_t(std::uint32_t(g.stream)) << 32) | g.op_index;
        if (key == last) return;
        last = key;
        char buf[120];
        std::snprintf(buf, sizeof buf, "PAC: character code 0x%02X has no Unicode mapping (no ToUnicode, not defined by %s)", c, font_kind == 1 ? "WinAnsiEncoding" : "MacRomanEncoding");
        hits.push_back({g.stream, g.op_index, buf});
    }
};

// Date string of ISO 32000-1 7.9.4, as PAC reads it: D:YYYY, then optional MM DD HH mm SS O HH 'mm with real calendar values (feb 30 fails); O is Z, + or -, the
// offset may stop after its hours or lack the closing apostrophe; no space before or after (probes dt_mod_*).
bool valid_pdf_date(const std::string& s) {
    if (s.size() < 6 || s[0] != 'D' || s[1] != ':') return false;
    std::size_t at = 2;
    auto two = [&](int& out, int lo, int hi) {
        if (at + 2 > s.size() || !std::isdigit((unsigned char)s[at]) || !std::isdigit((unsigned char)s[at + 1])) return false;
        out = (s[at] - '0') * 10 + (s[at + 1] - '0');
        at += 2;
        return out >= lo && out <= hi;
    };
    int y1 = 0, y2 = 0, mo = 1, d = 1, v = 0;
    if (!two(y1, 0, 99) || !two(y2, 0, 99)) return false;
    const int year = y1 * 100 + y2;
    if (at == s.size()) return true;
    if (!two(mo, 1, 12)) return false;
    if (at == s.size()) return true;
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (!two(d, 1, days[mo - 1] + (mo == 2 && leap ? 1 : 0))) return false;
    if (at == s.size()) return true;
    if (!two(v, 0, 23)) return false;
    if (at == s.size()) return true;
    if (!two(v, 0, 59)) return false;
    if (at == s.size()) return true;
    if (!two(v, 0, 59)) return false;
    if (at == s.size()) return true;
    const char o = s[at++];
    if (o != 'Z' && o != '+' && o != '-') return false;
    if (at == s.size()) return true;
    if (!two(v, 0, 23)) return false;
    if (at == s.size()) return true;
    if (s[at++] != '\'') return false;
    if (at == s.size()) return true;
    if (!two(v, 0, 59)) return false;
    if (at == s.size()) return true;
    if (s[at++] != '\'') return false;
    return at == s.size();
}

// Operators in a position ISO 32000-1 Figure 9 does not allow (through ISO 14289-1 5). PAC 26.1 reports such an operator with a reference to ISO 32000-2 Figure 9.
// under PDF syntax (probes gv_*; a real-world Canva export: re /gs2 gs f). veraPDF does not look at it. One finding per operator; PAC counts each several times.
struct OperatorStates final : ContentObserver {
    enum State : unsigned char { Page, Text, Path, Clip };
    struct Hit {
        int stream;
        std::string detail;
    };
    std::vector<Hit> hits;
    std::vector<State> state;
    static bool general(Op c) {
        switch (c) {
            case Op::w: case Op::J: case Op::j: case Op::M: case Op::d: case Op::ri: case Op::i: case Op::gs:
            case Op::CS: case Op::cs: case Op::SC: case Op::SCN: case Op::sc: case Op::scn: case Op::G: case Op::g: case Op::RG: case Op::rg: case Op::K: case Op::k:
            case Op::Tc: case Op::Tw: case Op::Tz: case Op::TL: case Op::Tf: case Op::Tr: case Op::Ts:
            case Op::MP: case Op::DP: case Op::BMC: case Op::BDC: case Op::EMC: case Op::BX: case Op::EX: case Op::d0: case Op::d1:
                return true;
            default:
                return false;
        }
    }
    static bool paint(Op c) {
        switch (c) {
            case Op::S: case Op::s: case Op::f: case Op::F: case Op::f_star: case Op::B: case Op::B_star: case Op::b: case Op::b_star: case Op::n:
                return true;
            default:
                return false;
        }
    }
    static bool path_part(Op c) { return c == Op::m || c == Op::l || c == Op::c || c == Op::v || c == Op::y || c == Op::h || c == Op::re; }
    static bool text_show(Op c) {
        return c == Op::Tj || c == Op::TJ || c == Op::quote || c == Op::dquote || c == Op::Td || c == Op::TD || c == Op::Tm || c == Op::T_star;
    }
    std::vector<int> bx, mc_depth;
    void on_operator(const ContentModel& cm, int stream, std::uint32_t, const RawOp& op, const GState&) override {
        if (stream < 0) return;
        if (std::size_t(stream) >= state.size()) state.resize(std::size_t(stream) + 1, Page), bx.resize(std::size_t(stream) + 1, 0);
        if (op.code == Op::BX) ++bx[std::size_t(stream)];
        if (op.code == Op::EX && bx[std::size_t(stream)] > 0) --bx[std::size_t(stream)];
        // An unknown operator outside BX ... EX (ISO 32000-1 7.8.2): PAC closes the document with a content-stream error (sx_unknown_op). It closes it inside BX too (sx_unknown_op_in_bx), which
        // the standard allows: not imitated.
        if (op.code == Op::Unknown) {
            if (bx[std::size_t(stream)] == 0 && hits.size() < 10000)
                hits.push_back({stream, "PAC: unknown operator " + std::string(op.op) + " outside BX ... EX (PAC closes the document)"});
            return;
        }
        // Tr takes 0 to 7 (PAC closes the document, sx_tr_bad).
        if (op.code == Op::Tr && !op.operands->empty() && op.operands->back().is_number()) {
            const double v = op.operands->back().num();
            if ((v < 0 || v > 7) && hits.size() < 10000) hits.push_back({stream, "PAC: text rendering mode out of range (PAC closes the document)"});
        }
        State& st = state[std::size_t(stream)];
        bool ok = true;
        const Op c = op.code;
        if (c == Op::BX || c == Op::EX) return;   // compatibility operators are allowed anywhere (tp_path_BX, tp_clip_BX)
        // EMC without an open sequence: PAC closes the document (ct_emc_surplus).
        if (std::size_t(stream) >= mc_depth.size()) mc_depth.resize(std::size_t(stream) + 1, 0);
        if (c == Op::BMC || c == Op::BDC) ++mc_depth[std::size_t(stream)];
        if (c == Op::EMC) {
            if (mc_depth[std::size_t(stream)] == 0) {
                if (hits.size() < 10000 && cm.streams[std::size_t(stream)].kind == StreamInstance::Kind::Page)
                    hits.push_back({stream, "PAC: EMC without an open marked-content sequence (PAC closes the document)"});
            } else {
                --mc_depth[std::size_t(stream)];
            }
        }
        // Appearance streams: PAC does not look (a real-world sample: f S in a Caret appearance passes).
        // Patterns and Type 3 glyph descriptions neither (tq2_pattern_rg_in_path, tq2_type3_rg_in_path), forms do (tq2_form_rg_in_path).
        const StreamInstance::Kind kind = cm.streams[std::size_t(stream)].kind;
        if (kind == StreamInstance::Kind::Annotation || kind == StreamInstance::Kind::Pattern || kind == StreamInstance::Kind::Type3Glyph) return;
        // d0, d1 belong to a Type 3 glyph description: PAC closes a page holding one (content-stream error, sx_d0_in_page, sx_d1_in_page).
        if ((c == Op::d0 || c == Op::d1) && cm.streams[std::size_t(stream)].kind != StreamInstance::Kind::Type3Glyph && hits.size() < 10000) {
            hits.push_back({stream, "PAC: operator " + std::string(op.op) + " outside a Type 3 glyph description (PAC closes the document)"});
            return;
        }
        switch (st) {
            case Page:
                if (c == Op::BT) st = Text;
                else if (c == Op::m || c == Op::re) st = Path;
                // a lone n: PAC passes it (two real-world samples); nothing is painted
                else if (c == Op::n) ok = true;
                else ok = general(c) || c == Op::q || c == Op::Q || c == Op::cm || c == Op::sh || c == Op::BI || c == Op::EI || c == Op::ID || c == Op::Do;
                break;
            case Text:
                if (c == Op::ET) st = Page;
                else ok = general(c) || text_show(c);
                break;
            case Path:
                if (c == Op::W || c == Op::W_star) st = Clip;
                else if (paint(c)) st = Page;
                else ok = path_part(c);
                break;
            case Clip:
                if (paint(c)) st = Page;
                else ok = false;
                break;
        }
        if (!ok && hits.size() < 10000) {
            hits.push_back({stream, "PAC: operator " + std::string(op.op) + " is not allowed " +
                                        (st == Page ? "at page description level" : st == Text ? "in a text object" : st == Path ? "in a path object" : "after a clipping operator") + " at byte " +
                                        std::to_string(op.offset) + " (ISO 32000-1 Figure 9)"});
        }
    }
};

class Module final : public RuleModule {
public:
    const char* name() const override { return "pac_quality"; }

    void prepare(ValidationContext& ctx, std::vector<ContentObserver*>& observers) override {
        // The contrast engine also measures EqualDoX's WCAG colour rules (wcag_color.h), which apply to every PDF version; on PDF 2.0
        // the engine removes the PAC findings it makes as well.
        contrast_ = pacc::make_contrast_observer(ctx);
        observers.push_back(contrast_.get());
        // A rule selection of WCAG rules only (dox-validate --rules EQX-WCAG-): the contrast engine and the WCAG checks, nothing of PAC's.
        wcag_only_ = !ctx.selected.empty();
        for (RuleIndex r : owned_rules(name()))
            if (ctx.selects(r) && std::string_view(Catalog::get().rule(r).id).rfind("EQX-WCAG-", 0) != 0) wcag_only_ = false;
        if (!ctx.pac_applicable || wcag_only_) return;
        observers.push_back(&unmapped_);
        observers.push_back(&states_);
    }

    void run(ValidationContext& ctx) override {
        ctx_ = &ctx;
        wcag::check_form_fields(ctx);
        wcag::check_use_of_color(ctx);
        if (wcag_only_) {   // the contrast engine reported the WCAG text and figure findings while the content was interpreted
            contrast_.reset();
            return;
        }
        // Text contrast: this module's judgement replaces the content module's first approximation (which sees one opaque
        // rectangle at most, composes the text's own alpha, counts operators), see pac_contrast.h.
        if (contrast_) {
            static const RuleIndex rc = R("EQX-PAC-FailNotEnoughContrastForText");
            if (!pacc::contrast_gave_up(*contrast_)) ctx.findings.erase(rc);   // else its findings keep the rule reported
            for (const pacc::ContrastFinding& f : pacc::contrast_findings(*contrast_)) ctx.report(rc, f.page, f.obj, f.detail);
        }
        static const RuleIndex ru = R("EQX-UA1-7.21.7-1");
        for (const UnmappedCodes::Hit& h : unmapped_.hits) {
            const StreamInstance& si = ctx.content.streams[h.stream];
            ctx.report(ru, si.page, si.ref, h.detail);
        }
        static const RuleIndex rp = R("EQX-PAC-FailPDFParsable");
        for (const OperatorStates::Hit& h : states_.hits) {
            const StreamInstance& si = ctx.content.streams[h.stream];
            ctx.report(rp, si.page, si.ref, h.detail);
        }
        if (contrast_)
            for (const pacc::ContrastFinding& f : pacc::missing_resources(*contrast_)) ctx.report(rp, f.page, f.obj, f.detail);
        // /ModDate of the Info dictionary (PDF syntax in PAC, dt_mod_*; /CreationDate and annotation dates are not read there: dt_cre_*, dt_annot_*).
        {
            Obj md = cos::get(ctx.doc.info(), "ModDate");
            if (md.isString() && !valid_pdf_date(md.getStringValue())) ctx.report(rp, -1, ObjRef{}, "PAC: Info /ModDate is not a date as in ISO 32000-1 7.9.4");
        }
        // The file's frame: PAC refuses a file without %%EOF ("corrupted PDF document", sb_no_eof; a few bytes after the marker pass, sb_eof_trailing_junk) and an unknown version
        // (unsupported-version error, sb_header_version_bad). qpdf reads both silently.
        {
            std::FILE* f = std::fopen(ctx.doc.path().c_str(), "rb");
            if (f) {
                char tail[1024];
                const long n = long(std::min<std::uint64_t>(sizeof tail, ctx.doc.file_size()));
                std::string_view view;
                if (std::fseek(f, -n, SEEK_END) == 0) view = std::string_view(tail, std::fread(tail, 1, std::size_t(n), f));
                std::fclose(f);
                if (!view.empty() && view.find("%%EOF") == std::string_view::npos)
                    ctx.report(rp, -1, ObjRef{}, "PAC: no %%EOF in the last 1024 bytes of the file (ISO 32000-1 7.5.5; PAC refuses the file)");
            }
            const std::string& v = ctx.doc.header_version();
            if (!v.empty() && !(v.size() == 3 && v[1] == '.' && ((v[0] == '1' && v[2] >= '0' && v[2] <= '7') || (v[0] == '2' && v[2] == '0'))))
                ctx.report(rp, -1, ObjRef{}, "PAC: the header names PDF version " + v + " (PAC refuses the file)");
        }
        // /Title, /ModDate that are no strings (PAC stops: sy_title_int, sy_date_int), a page box that is no rectangle (sy_page_mediabox_short, sy_page_mediabox_zero),
        // an MCID used twice in one page's content (PAC closes the document, "MCID n already present", ow_mcid_twice_on_page; ISO 32000-1 14.7.4.3).
        for (const char* k : {"Title", "ModDate"}) {
            Obj v = cos::get(ctx.doc.info(), k);
            if (!v.isNull() && !v.isString()) ctx.report(rp, -1, ObjRef{}, std::string("PAC: Info /") + k + " is not a string (PAC stops)");
        }
        for (std::size_t pi = 0; pi < ctx.doc.pages().size(); ++pi) {
            auto box = cos::array_items(cos::get(ctx.doc.pages()[pi].dict, "MediaBox"));
            if (!box.empty() && box.size() != 4) ctx.report(rp, int(pi), ctx.doc.pages()[pi].ref, "PAC: /MediaBox is not a rectangle (PAC stops)");
            else if (ctx.doc.pages()[pi].media_box.empty()) ctx.report(rp, int(pi), ctx.doc.pages()[pi].ref, "PAC: empty /MediaBox (PAC stops)");
        }
        {
            std::set<std::pair<int, std::int64_t>> used;
            for (const McFrame& f : ctx.content.frames) {
                if (!f.mcid || f.stream < 0) continue;
                const StreamInstance& si = ctx.content.streams[std::size_t(f.stream)];
                if (si.kind != StreamInstance::Kind::Page || !si.semantic || *f.mcid < 0) continue;
                if (!used.insert({si.page, *f.mcid}).second)
                    ctx.report(rp, si.page, si.ref, "PAC: MCID " + std::to_string(*f.mcid) + " is used twice in the page's content (PAC closes the document)");
            }
        }
        // Marked content open at the end of a page's content (PAC closes the document with an unterminated marked-content error; probes sx_comment_only, sx_inline_no_ei).
        {
            std::vector<char> flagged(ctx.content.streams.size(), 0);
            for (const McFrame& f : ctx.content.frames) {
                if (f.end_op != 0xffffffffu || f.stream < 0 || flagged[std::size_t(f.stream)]) continue;
                const StreamInstance& si = ctx.content.streams[std::size_t(f.stream)];
                if (si.kind != StreamInstance::Kind::Page) continue;
                flagged[std::size_t(f.stream)] = 1;
                ctx.report(rp, si.page, si.ref, "PAC: marked content /" + f.tag + " is not closed at the end of the page's content (PAC closes the document)");
            }
        }
        // A form field whose Rect has no area is not outside the crop box and not hidden, so ISO 14289-1 7.18.1 asks for its name; veraPDF's test counts it as outside. PAC fails it
        // (signed Office files: a real-world sample; probes fz_sig_zero_*, fz_tx_zero_*).
        {
            static const RuleIndex rtu = R("EQX-UA1-7.18.1-3");
            for (std::size_t pi = 0; pi < ctx.doc.pages().size(); ++pi)
                for (const Obj& a : cos::array_items(cos::get(ctx.doc.pages()[pi].dict, "Annots"))) {
                    if (!cos::name_is(a, "Subtype", "Widget")) continue;
                    auto r = cos::rect(cos::get(a, "Rect"));
                    if (!r || r->width() != 0 || r->height() != 0) continue;
                    if (cos::integer(a, "F").value_or(0) & 2) continue;
                    bool named = false;
                    Obj f = a;
                    for (int guard = 0; f.isDictionary() && guard < 64 && !named; ++guard) {
                        named = cos::get(f, "TU").isString();
                        f = cos::get(f, "Parent");
                    }
                    if (auto sp = cos::integer(a, "StructParent")) named = named || cos::get(ctx.st.parent_tree_entry(*sp), "Alt").isString();
                    if (!named) ctx.report(rtu, int(pi), ref_of(a), "PAC: form field with an empty /Rect and no /TU");
                }
        }
        gather_content();
        catalog_lang_ = cos::get(ctx.doc.catalog(), "Lang").isString();
        check_document();
        check_elements();
        check_marked_content();
        check_language();
        contrast_.reset();
    }

private:
    ValidationContext* ctx_ = nullptr;
    bool wcag_only_ = false;
    std::unique_ptr<ContentObserver> contrast_;
    UnmappedCodes unmapped_;
    OperatorStates states_;
    std::vector<ElemContent> elem_content_;
    std::vector<McContent> mcid_mc_;       // tagged marked content (own MCID)
    std::map<int, McContent> plain_mc_;    // frame -> content of a marked-content sequence without MCID, not an artifact
    std::vector<int> mc_owner_;            // per mcid_mc_: element whose MCID/MCR kid names it (-1 none)
    bool catalog_lang_ = false;
    bool dest_names_built_ = false;
    std::unordered_map<std::string, Obj> dest_names_;

    // --- content gathering ------------------------------------------------------------------------

    // Innermost frame with an own MCID along the frame chain, continued into the invoking stream of a Form
    // XObject; -1 when none.
    int mcid_frame(int stream, int frame) const {
        const ContentModel& cm = ctx_->content;
        for (int guard = 0; stream >= 0 && guard < 256; ++guard) {
            for (int f = frame; f >= 0; f = cm.frames[f].parent)
                if (cm.frames[f].mcid) return f;
            frame = cm.streams[stream].parent_frame;
            stream = cm.streams[stream].parent;
        }
        return -1;
    }
    // Outermost frame tagged /Artifact along the same chain; -1 when the content is not in an artifact.
    int artifact_frame(int stream, int frame) const {
        const ContentModel& cm = ctx_->content;
        int found = -1;
        for (int guard = 0; stream >= 0 && guard < 256; ++guard) {
            for (int f = frame; f >= 0; f = cm.frames[f].parent)
                if (cm.frames[f].tag == "Artifact") found = f;
            frame = cm.streams[stream].parent_frame;
            stream = cm.streams[stream].parent;
        }
        return found;
    }

    void gather_content() {
        const ContentModel& cm = ctx_->content;
        const StructTree& st = ctx_->st;
        elem_content_.assign(st.elems().size(), ElemContent{});
        std::unordered_map<int, int> mcid_index;
        std::unordered_map<int, McContent> stream_parent_content;   // element -> content via /StructParent only
        std::vector<int> stream_parent_order;
        AscentCache ascents;
        // Content items come in runs of one marked-content sequence: what depends only on (stream, frame) or on the
        // stream is looked up when that changes, not for every item.
        int run_stream = -1, run_frame = -2;   // the (stream, frame) the values below belong to
        int mf = -1;                            // mcid_frame of it
        bool in_artifact = false;               // mf < 0 and inside an /Artifact sequence
        int mc_at = -1;                         // index in mcid_mc_ of mf (-1: not looked up yet)
        McContent* plain = nullptr;             // entry of plain_mc_ for the frame (nullptr: not looked up yet)
        int sp_stream = -1;                     // the stream the two values below belong to
        int sp_elem = -1;                       // element named by the stream's structure parent (-1 none)
        McContent* sp_content = nullptr;        // its entry in stream_parent_content
        for (const ContentItem& it : cm.items) {
            if (it.stream < 0 || !cm.streams[it.stream].semantic) continue;
            std::optional<TextRun> run_view;   // cm.texts[i] is a view, built on access
            if (it.kind == ContentItem::Kind::Text && it.text >= 0) run_view.emplace(cm.texts[it.text]);
            const TextRun* run = run_view ? &*run_view : nullptr;
            int page = cm.page_of(it.stream);
            if (it.stream != run_stream || it.frame != run_frame) {
                run_stream = it.stream;
                run_frame = it.frame;
                mf = mcid_frame(it.stream, it.frame);
                in_artifact = mf < 0 && artifact_frame(it.stream, it.frame) >= 0;
                mc_at = -1;
                plain = nullptr;
            }
            // Artifact content belongs to no element, unless the sequence has an MCID: Word writes /Artifact <</MCID n>> BDC
            // for table rules owned by a role-mapped element (a real-world sample: PAC 7 white-space elements, not
            // the 44 elements holding only such paths).
            if (in_artifact) continue;
            if (mf >= 0) {
                if (mc_at < 0) {
                    auto [pos, fresh] = mcid_index.emplace(mf, int(mcid_mc_.size()));
                    if (fresh) mcid_mc_.push_back(make_mc(mf, page));
                    mc_at = pos->second;
                }
                mcid_mc_[mc_at].add_item(it, run, ascents);
                // A Form XObject painted inside the sequence is not looked into for white space (probe ws_form: not flagged).
                if (cm.frames[mf].stream != it.stream) mcid_mc_[mc_at].nontext = true;
                continue;
            }
            // No marked-content identifier: judged only for "outside the page" (a real-world sample: /Span BDC without MCID).
            if (it.frame >= 0) {
                if (!plain) {
                    auto pos = plain_mc_.find(it.frame);
                    if (pos == plain_mc_.end()) pos = plain_mc_.emplace(it.frame, make_mc(it.frame, page)).first;
                    plain = &pos->second;   // map nodes stay where they are
                }
                plain->add_item(it, run, ascents);
            }
            // A Form XObject whose own /StructParent names the element.
            if (it.stream != sp_stream) {
                sp_stream = it.stream;
                const Obj& se = cm.streams[it.stream].parent_struct_elem;
                sp_elem = se.isDictionary() ? st.find(ref_of(se)) : -1;
                sp_content = nullptr;
                if (sp_elem >= 0) {
                    auto [pos, fresh] = stream_parent_content.emplace(sp_elem, make_mc(-1, page));
                    if (fresh) stream_parent_order.push_back(sp_elem);
                    sp_content = &pos->second;   // references into an unordered_map survive rehashing
                }
            }
            if (sp_elem < 0) continue;
            sp_content->add_item(it, run, ascents);
        }

        // Structure -> content: an MCID kid names the sequence with that MCID on its page (/Pg), an MCR with
        // /Stm the one in that content stream. This does not depend on the ParentTree (U1-X03 has none,
        // U1-X01 a page without /StructParents; PAC still sees their elements' text).
        std::map<std::pair<std::uint64_t, std::int64_t>, std::vector<int>> by_key;
        for (std::size_t m = 0; m < mcid_mc_.size(); ++m) {
            const McFrame& fr = cm.frames[mcid_mc_[m].frame];
            const StreamInstance& s = cm.streams[fr.stream];
            const std::uint64_t page_key = (std::uint64_t(1) << 63) | std::uint64_t(std::uint32_t(s.page));
            if (s.kind == StreamInstance::Kind::Page) {
                by_key[{page_key, *fr.mcid}].push_back(int(m));
                continue;
            }
            by_key[{s.ref.key(), *fr.mcid}].push_back(int(m));
            // MCIDs inside a Form XObject without its own /StructParents are not this element's content for PAC, although the
            // form shares the page's MCID space: a real-world Canva export (all text in one form):
            // PAC finds all 21 P and H1 "white space only" and no linkable text.
        }
        mc_owner_.assign(mcid_mc_.size(), -1);
        for (const StructElem& e : st.elems()) {
            ElemContent& ec = elem_content_[e.index];
            for (const StructKid& kid : e.kids) {
                if ((kid.kind != StructKid::Kind::MCID && kid.kind != StructKid::Kind::MCR) || kid.mcid < 0) continue;
                std::uint64_t key;
                if (kid.stm.valid()) key = kid.stm.key();
                else {
                    int p = kid.page.valid() ? ctx_->doc.page_index(kid.page) : -1;
                    if (p < 0) continue;
                    key = (std::uint64_t(1) << 63) | std::uint64_t(p);
                }
                auto f = by_key.find({key, kid.mcid});
                if (f == by_key.end()) continue;
                for (int m : f->second) {
                    ec.add(mcid_mc_[m]);
                    if (mc_owner_[m] < 0) mc_owner_[m] = e.index;
                }
            }
        }
        for (int ei : stream_parent_order) elem_content_[ei].add(stream_parent_content[ei]);
    }

    // Text of an element and its descendants (own content first, then the children in /K order).
    TextAcc full_text(int root) const {
        const StructTree& st = ctx_->st;
        TextAcc out;
        std::vector<int> stack{root};
        std::size_t budget = 100000;
        while (!stack.empty() && budget-- > 0) {
            int i = stack.back();
            stack.pop_back();
            const TextAcc& a = elem_content_[i].acc;
            if (!a.text.empty() && !out.text.empty() && !pacq::is_whitespace(out.text.back())) out.text.push_back(U' ');
            out.text += a.text;
            out.unmapped += a.unmapped;
            out.glyphs += a.glyphs;
            const auto& kids = st.elem(i).elems;
            for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back(*k);
        }
        return out;
    }

    static std::string snippet(const std::u32string& s) {
        std::string u = to_utf8(s.size() > 60 ? s.substr(0, 60) : s);
        return s.size() > 60 ? u + "..." : u;
    }

    std::string elem_label(int i) const {
        const StructElem& e = ctx_->st.elem(i);
        std::string s = e.S.value_or("?");
        std::string t = std17(e);
        if (!t.empty() && t != s) s += " (" + t + ")";
        return s;
    }

    int elem_page(int i) const {
        if (elem_content_[i].page >= 0) return elem_content_[i].page;
        if (int p = first_content(i).page; p >= 0) return p;
        const StructElem& e = ctx_->st.elem(i);
        return e.inherited_page.valid() ? ctx_->doc.page_index(e.inherited_page) : -1;
    }

    // --- document-level checks -------------------------------------------------------------------

    void check_document() {
        static const RuleIndex kNoHeadings = R("EQX-PACQ-WarnDocumentHasHeadings");
        static const RuleIndex kNoOutline = R("EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings");
        bool headings = false;
        for (const StructElem& e : ctx_->st.elems())
            if (is_heading(std17(e))) {
                headings = true;
                break;
            }
        // Bookmarks: an outline dictionary with at least one item.
        Obj outlines = cos::get(ctx_->doc.catalog(), "Outlines");
        bool bookmarks = outlines.isDictionary() && cos::get(outlines, "First").isDictionary();
        ObjRef cat = ref_of(ctx_->doc.catalog());
        // DocumentHasHeadings: WARN when the document contains no heading tag (also untagged files).
        if (!headings) ctx_->report(kNoHeadings, -1, cat, "no H or H1-H6 structure element");
        if (headings && !bookmarks) ctx_->report(kNoOutline, -1, cat, "headings present, no document outline items");
        // CheckFailOutlineIsPresentWithoutHeadings exists in PAC's strings, but bookmarks without headings pass (PAC documentation;
        // measured on probe outline_noheadings), so eqx does not report it either.
    }

    // --- structure-element checks ----------------------------------------------------------------

    void check_elements() {
        static const RuleIndex kLI = R("EQX-PACQ-FailLIContainsExactlyOneBody");
        static const RuleIndex kWs = R("EQX-PACQ-WarnTextTagContainsOnlyWhitespace");
        static const RuleIndex kWsApprox = R("EQX-PACQ-WarnTextTagContainsOnlyWhitespaceApproximation");
        static const RuleIndex kTextAlt = R("EQX-PACQ-WarnTextTagHasAltText");
        static const RuleIndex kGenAlt = R("EQX-PACQ-WarningAltTextsAreNotGenerated");
        static const RuleIndex kNoteLbl = R("EQX-PACQ-WarnNoteContainsLbl");
        static const RuleIndex kNoteInP = R("EQX-PACQ-WarnNoteTagInsidePTag");
        const StructTree& st = ctx_->st;
        const auto& elems = st.elems();
        const std::size_t n = elems.size();

        // Whitespace-only, bottom-up (preorder indices: children follow their parent). An element qualifies when its own text is
        // white space only (or absent), it has no other own content (no images/paths, no object references: an Annot holding only
        // its OBJR is not reported, U1-A01) and all child elements qualify, whatever their type. Only the listed types are
        // reported. (PAC's documentation says a child of another type makes it SKIP; measured instead: P > Span " " and
        // L > LI > LBody and Table > TR > TH > P with white space are flagged: real-world samples with 4, 13
        // and 61 instances, all reproduced.) `ws_box` tells whether the subtree's whitespace content has a location.
        std::vector<char> ws(n, 0), ws_located(n, 0), ws_content(n, 0);
        for (std::size_t k = n; k-- > 0;) {
            const StructElem& e = elems[k];
            const ElemContent& ec = elem_content_[k];
            // U+0000 counts as no text (tu_tu_zero: flagged). Glyphs without any mapping stay text (U1-F02 passes), except that PAC also flags a simple font without an encoding
            // (tu_noenc_ne_41): not reproduced, recorded.
            std::u32string own = ec.acc.text;
            own.erase(std::remove(own.begin(), own.end(), U'\0'), own.end());
            if (ec.nontext || ec.acc.unmapped > 0 || !whitespace_only(own)) continue;
            bool ok = true, located = ec.box_valid, content = ec.has_items;
            for (const StructKid& kid : e.kids) {
                if (kid.kind == StructKid::Kind::OBJR) ok = false;
                if (kid.kind == StructKid::Kind::Elem) {
                    if (kid.elem < 0 || !ws[kid.elem]) ok = false;
                    else {
                        located = located || ws_located[kid.elem];
                        content = content || ws_content[kid.elem];
                    }
                }
            }
            if (!ok) continue;
            ws[k] = 1;
            ws_located[k] = located;
            ws_content[k] = content;
        }

        // Text below an element (own or in descendants): TextTagHasAltText judges only elements with text: PAC leaves an empty one, one with white space, a path or
        // only an annotation alone (probes ta_p_empty_alt, ta_p_space_alt, ta_p_path_alt, ta_p_form_alt, ta_p_link_alt; a real-world sample: 4 P /Alt without content).
        std::vector<char> has_text(n, 0);
        for (std::size_t k = n; k-- > 0;) {
            const ElemContent& ec = elem_content_[k];
            if (ec.acc.unmapped > 0 || !whitespace_only(ec.acc.text)) has_text[k] = 1;
            for (int c : elems[k].elems)
                if (c >= 0 && has_text[c]) has_text[k] = 1;
        }

        for (std::size_t k = 0; k < n; ++k) {
            const StructElem& e = elems[k];
            const std::string t = std17(e);
            const int i = int(k);

            if (ws[k] && whitespace_checked_type(t)) {
                // Open: the readouts cannot tell the two variants apart (PAC truncates both labels to the
                // same prefix). Whitespace content without any measurable box is
                // reported as the approximated-location variant; empty elements (U1-S03's empty H) and
                // located whitespace (U1-Q01's " " paragraph) as the plain variant.
                bool approx = ws_content[k] && !ws_located[k];
                ctx_->report(approx ? kWsApprox : kWs, elem_page(i), e.ref,
                             elem_label(i) + (ws_content[k] ? ": white space only" : ": no content"));
            }

            Obj alt = cos::get(e.dict, "Alt");
            if (alt.isString()) {
                std::string a = cos::text(alt).value_or("");
                // TextTagHasAltText: WARN when the tag has alt text set; a value of white space and NUL only is
                // not (a real-world sample: Annot with /Alt "    \0" unflagged).
                std::u32string alt32 = utf8_to_u32(a);
                alt32.erase(std::remove(alt32.begin(), alt32.end(), U'\0'), alt32.end());
                if (text_alt_checked_type(t) && has_text[k] && !whitespace_only(alt32)) ctx_->report(kTextAlt, elem_page(i), e.ref, elem_label(i) + " /Alt \"" + snippet(utf8_to_u32(a)) + "\"");
                // AltTextsAreNotGenerated: every structure element /Alt (U1-Q01 counts the Figure
                // and the P; marked-content /Alt is not counted, U1-D04); empty -> SKIPPED.
                if (pacq::looks_like_path_or_file_name(a))
                    ctx_->report(kGenAlt, elem_page(i), e.ref, elem_label(i) + " /Alt \"" + snippet(utf8_to_u32(a)) + "\"");
            }

            if (t == "LI") {
                // "LI element must contain exactly one LBody": LBody elements among the descendants, not
                // descending into an LBody or a nested list (open: PAC may count direct children only).
                int bodies = count_descendants(i, "LBody", {"LBody", "L"});
                if (bodies != 1)
                    ctx_->report(kLI, elem_page(i), e.ref, elem_label(i) + ": " + std::to_string(bodies) + " LBody");
            }

            if (t == "Note") {
                // Open: direct child or descendant is undocumented; any descendant Lbl counts (conservative).
                if (count_descendants(i, "Lbl", {}) == 0)
                    ctx_->report(kNoteLbl, elem_page(i), e.ref, elem_label(i) + " without Lbl");
                // NoteTagInsidePTag: the direct parent's standard type.
                if (e.parent >= 0 && std17(elems[e.parent]) == "P")
                    ctx_->report(kNoteInP, elem_page(i), e.ref, elem_label(i) + " in " + elem_label(e.parent));
            }
        }

        check_notes_referenced();
        check_toc_items();
    }

    // Number of descendants with standard type `type`; the walk does not enter elements whose type is in
    // `stop` (a matching element is counted before the stop test).
    int count_descendants(int root, const std::string& type, std::initializer_list<const char*> stop) const {
        const StructTree& st = ctx_->st;
        int count = 0;
        std::vector<int> stack(st.elem(root).elems.rbegin(), st.elem(root).elems.rend());
        std::size_t budget = 100000;
        while (!stack.empty() && budget-- > 0) {
            int c = stack.back();
            stack.pop_back();
            std::string t = std17(st.elem(c));
            if (t == type) ++count;
            bool halt = false;
            for (const char* s : stop)
                if (t == s) halt = true;
            if (halt) continue;
            const auto& kids = st.elem(c).elems;
            for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back(*k);
        }
        return count;
    }

    // --- destinations ------------------------------------------------------------------------------

    const std::unordered_map<std::string, Obj>& dest_names() {
        if (!dest_names_built_) {
            dest_names_built_ = true;
            Obj names = cos::get(ctx_->doc.catalog(), "Names");
            Obj tree = cos::get(names, "Dests");
            if (tree.isDictionary())
                for (auto& [k, v] : cos::name_tree(tree)) dest_names_.emplace(k, v);
        }
        return dest_names_;
    }

    bool parse_explicit(const Obj& arr, ExplicitDest& out) const {
        if (!arr.isArray() || arr.getArrayNItems() < 2) return false;
        Obj pg = arr.getArrayItem(0);
        if (pg.isDictionary()) out.page = ctx_->doc.page_index(ref_of(pg));
        else if (pg.isInteger()) {
            long long p = pg.getIntValue();
            out.page = (p >= 0 && p < (long long)ctx_->doc.pages().size()) ? int(p) : -1;
        }
        auto type = cos::name(arr.getArrayItem(1));
        if (!type) return false;
        out.type = *type;
        for (int k = 2; k < arr.getArrayNItems(); ++k) out.params.push_back(cos::number(arr.getArrayItem(k)));
        return true;
    }

    // Destination of a link annotation: /Dest, else a GoTo action's /D (and /SD).
    LinkDest link_dest(const Obj& annot) {
        LinkDest out;
        Obj d = cos::get(annot, "Dest");
        if (d.isNull()) {
            Obj a = cos::get(annot, "A");
            if (!a.isDictionary()) return out;
            if (!cos::name_is(a, "S", "GoTo")) {
                out.status = DestStatus::NotGoTo;
                return out;
            }
            d = cos::get(a, "D");
            out.sd = cos::get(a, "SD");
        }
        if (d.isName()) {
            out.status = DestStatus::NameObject;
            out.name = *cos::name(d);
            return out;
        }
        if (d.isString()) {
            out.name = cos::bytes(d).value_or("");
            auto it = dest_names().find(out.name);
            if (it == dest_names().end()) {
                out.status = DestStatus::NameMissing;
                return out;
            }
            d = it->second;
            if (d.isDictionary()) d = cos::get(d, "D");
        }
        if (d.isNull() && out.sd.isArray()) {
            out.status = DestStatus::Malformed;
            return out;
        }
        out.status = parse_explicit(d, out.dest) ? DestStatus::Explicit : DestStatus::Malformed;
        return out;
    }

    static bool whole_page(const ExplicitDest& d) { return d.type == "Fit" || d.type == "FitB"; }

    // Vertical target coordinate of a destination (nullopt when the destination keeps the position).
    static std::optional<double> dest_top(const ExplicitDest& d) {
        auto p = [&](std::size_t k) { return k < d.params.size() ? d.params[k] : std::nullopt; };
        if (d.type == "XYZ") return p(1);
        if (d.type == "FitH" || d.type == "FitBH") return p(0);
        if (d.type == "FitR") return p(3);
        return std::nullopt;
    }

    // --- Note references ----------------------------------------------------------------------------

    void check_notes_referenced() {
        static const RuleIndex kRef = R("EQX-PACQ-WarningNoteHasReferringLinkOnPage");
        const StructTree& st = ctx_->st;
        std::vector<int> notes;
        for (const StructElem& e : st.elems())
            if (std17(e) == "Note") notes.push_back(e.index);
        if (notes.empty()) return;
        // Referenced: a link on any page whose destination lies at the Note (pq_otherpage: the link on page 1, the Note on page 2,
        // passes, whatever PAC's condition key "NoteHasReferringLinkOnPage" suggests). Single-condition runs (probes pn_*, pm_*, pq_*; a 9 pt Note with box 72-140 x 98-107): an
        // /XYZ point inside the box passes, also up to about 5 pt outside (x 70 and y 112 pass; x 65 and y 116 fail); /FitR over the
        // box passes; /FitH, /Fit, a point elsewhere on the page and a structure destination (/SD) next to an /XYZ away from the
        // Note do not count. A destination name works like the array it names.
        constexpr double kTolerance = 5.0;
        struct Target {
            Rect r;
        };
        std::unordered_map<int, std::vector<Target>> by_page;
        const auto& all_pages = ctx_->doc.pages();
        for (std::size_t pi = 0; pi < all_pages.size(); ++pi) {
            for (const Obj& an : cos::array_items(cos::get(all_pages[pi].dict, "Annots"))) {
                if (!cos::name_is(an, "Subtype", "Link")) continue;
                LinkDest ld = link_dest(an);
                if (ld.status != DestStatus::Explicit || ld.dest.page < 0) continue;
                const auto& q = ld.dest.params;
                auto v = [&](std::size_t k) { return k < q.size() ? q[k] : std::nullopt; };
                if (ld.dest.type == "XYZ" && v(0) && v(1)) by_page[ld.dest.page].push_back({Rect{*v(0), *v(1), *v(0), *v(1)}});
                else if (ld.dest.type == "FitR" && v(0) && v(1) && v(2) && v(3)) by_page[ld.dest.page].push_back({Rect{*v(0), *v(1), *v(2), *v(3)}});
            }
        }
        for (int n : notes) {
            const int page = elem_page(n);
            Rect box;
            bool valid = false;
            // union of the Note's content on its page, descendants included
            std::vector<int> stack{n};
            std::size_t budget = 100000;
            while (!stack.empty() && budget-- > 0) {
                int c = stack.back();
                stack.pop_back();
                const ElemContent& ec = elem_content_[c];
                if (ec.box_valid && ec.page == page) unite(box, valid, ec.box);
                for (int k : st.elem(c).elems) stack.push_back(k);
            }
            bool referenced = false;
            if (valid) {
                auto it = by_page.find(page);
                if (it != by_page.end()) {
                    const Rect in{box.llx - kTolerance, box.lly - kTolerance, box.urx + kTolerance, box.ury + kTolerance};
                    for (const Target& t : it->second)
                        if (t.r.llx <= in.urx && t.r.urx >= in.llx && t.r.lly <= in.ury && t.r.ury >= in.lly) referenced = true;
                }
            }
            // a Note without content is not judged (wl_note_empty; a real-world sample)
            if (valid && !referenced) ctx_->report(kRef, page, st.elem(n).ref, elem_label(n) + " not referenced by a link");
        }
    }

    // --- TOCI checks -------------------------------------------------------------------------------

    void check_toc_items() {
        static const RuleIndex kNotLinked = R("EQX-PACQ-FailTOCINotLinked");
        static const RuleIndex kMultiple = R("EQX-PACQ-FailTOCILinkedMultiple");
        const StructTree& st = ctx_->st;
        std::vector<int> headings;
        bool any_toci = false;
        for (const StructElem& e : st.elems()) {
            if (is_heading(std17(e))) headings.push_back(e.index);
            if (std17(e) == "TOCI") any_toci = true;
        }
        if (!any_toci) return;
        std::vector<TextAcc> heading_text;
        heading_text.reserve(headings.size());
        for (int h : headings) heading_text.push_back(full_text(h));
        std::vector<pacq::TocText> heading_toc;   // filled by the first entry that is matched against the headings

        for (const StructElem& e : st.elems()) {
            if (std17(e) != "TOCI") continue;
            // Link elements among the descendants, excluding nested TOC elements and their subtrees (PAC).
            std::vector<int> links;
            std::vector<int> stack(e.elems.rbegin(), e.elems.rend());
            std::size_t budget = 100000;
            while (!stack.empty() && budget-- > 0) {
                int c = stack.back();
                stack.pop_back();
                std::string t = std17(st.elem(c));
                if (t == "TOC") continue;
                if (t == "Link") {
                    links.push_back(c);
                    continue;
                }
                const auto& kids = st.elem(c).elems;
                for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back(*k);
            }
            const int pg = elem_page(e.index);
            if (links.empty()) ctx_->report(kNotLinked, pg, e.ref, elem_label(e.index) + " without Link");
            else if (links.size() > 1)
                ctx_->report(kMultiple, pg, e.ref, elem_label(e.index) + ": " + std::to_string(links.size()) + " Link elements");
            // CheckFailTOCIWithLinkedDescendants exists in PAC's strings, but one Link anywhere below the TOCI passes (PAC
            // documentation; measured on probe toc_desc: TOCI > P > Link, and on
            // a real-world sample: 23 x TOCI > Reference > Link). PAC 26.1 was never seen reporting the condition.
            if (!links.empty()) check_toc_destination(e.index, links, headings, heading_text, heading_toc);
        }
    }

    void check_toc_destination(int toci, const std::vector<int>& links, const std::vector<int>& headings,
                               const std::vector<TextAcc>& heading_text, std::vector<pacq::TocText>& heading_toc) {
        static const RuleIndex kIncorrect = R("EQX-PACQ-WarnTOCILinkPointsToIncorrectDestination");
        static const RuleIndex kNameTable = R("EQX-PACQ-WarnTOCILinkHasNoCorrespondingNameTableEntry");
        static const RuleIndex kNamed = R("EQX-PACQ-WarnTOCILinkIsNamed");
        const StructTree& st = ctx_->st;
        // The first Link element (tree order) with a link annotation that has a destination (Word tags the
        // entry title as a Link without OBJR and the page number as a second Link with the annotation).
        LinkDest ld;
        int annot_page = -1, link = -1;
        ObjRef annot_ref;
        for (int l : links) {
            for (const StructKid& kid : st.elem(l).kids) {
                if (kid.kind != StructKid::Kind::OBJR || !cos::name_is(kid.obj, "Subtype", "Link")) continue;
                LinkDest d = link_dest(kid.obj);
                if (d.status == DestStatus::None || d.status == DestStatus::NotGoTo) continue;
                ld = d;
                link = l;
                annot_ref = ref_of(kid.obj);
                annot_page = kid.page.valid() ? ctx_->doc.page_index(kid.page) : -1;
                break;
            }
            if (link >= 0) break;
        }
        // A TOCI whose Link has no GoTo action -> SKIPPED.
        if (ld.status == DestStatus::None || ld.status == DestStatus::NotGoTo) return;
        const ObjRef toci_ref = st.elem(toci).ref;
        const std::string where = elem_label(toci) + " link " + (annot_ref.valid() ? annot_ref.str() : "?");
        // Open (PAC experiment needed): a destination given as a name object (PDF 1.1 /Dests dictionary)
        // is taken as the "unsupported PDF syntax" condition, a string destination missing from the
        // /Names /Dests name tree as the "not formally correct" (name table) condition.
        if (ld.status == DestStatus::NameObject) {
            ctx_->report(kNamed, annot_page, toci_ref, where + ": named destination /" + ld.name);
            return;
        }
        if (ld.status == DestStatus::NameMissing) {
            ctx_->report(kNameTable, annot_page, toci_ref, where + ": destination (" + ld.name + ") not in the name tree");
            return;
        }
        // A structure destination to a heading is a correct target.
        if (ld.sd.isArray() && ld.sd.getArrayNItems() > 0) {
            int t = st.find(ref_of(ld.sd.getArrayItem(0)));
            if (t >= 0 && is_heading(std17(st.elem(t)))) return;
        }
        if (ld.status == DestStatus::Malformed || ld.dest.page < 0) {
            ctx_->report(kIncorrect, annot_page, toci_ref, where + ": destination page not found");
            return;
        }
        // A Link that points to the whole page instead of the heading (U1-Q01: /Fit to the page; reported
        // although no heading text matches the entry, so this test precedes the heading match).
        if (whole_page(ld.dest)) {
            ctx_->report(kIncorrect, annot_page, toci_ref, where + ": /" + ld.dest.type + " destination (whole page)");
            return;
        }
        // A destination without a vertical coordinate (/XYZ null null null) is not judged at all, with or without a matching
        // heading (probes toc3_*; a real-world sample, 23 entries: the check is "not applicable").
        if ((ld.dest.type == "XYZ" || ld.dest.type == "FitH" || ld.dest.type == "FitBH" || ld.dest.type == "FitR") && !dest_top(ld.dest)) return;
        // Matching heading: its text matches the Link element's text; the whole TOCI text is accepted too
        // (open, conservative: Word's page-number Link holds only the number, the title is a sibling).
        const TextAcc link_text = full_text(link), entry_text = full_text(toci);
        const TextAcc& text = whitespace_only(link_text.text) ? entry_text : link_text;
        if (whitespace_only(entry_text.text)) {
            if (entry_text.unmapped > 0) ctx_->mark_incomplete(kIncorrect, "TOC entry text has no Unicode mapping");
            else ctx_->report(kIncorrect, annot_page, toci_ref, where + ": entry has no text");
            return;
        }
        std::vector<std::size_t> matches;
        bool unknown_heading = false;
        // The texts as toc_text_matches compares them: of every heading once for the document, of this entry once.
        if (heading_toc.size() != headings.size()) {
            heading_toc.clear();
            heading_toc.reserve(headings.size());
            for (const TextAcc& t : heading_text) heading_toc.push_back(pacq::toc_text(t.text));
        }
        const pacq::TocText link_toc = pacq::toc_text(link_text.text), entry_toc = pacq::toc_text(entry_text.text);
        for (std::size_t h = 0; h < headings.size(); ++h) {
            if (pacq::toc_text_matches(link_toc, heading_toc[h]) || pacq::toc_text_matches(entry_toc, heading_toc[h]))
                matches.push_back(h);
            else if (heading_text[h].unmapped > 0)
                unknown_heading = true;
        }
        if (matches.empty() && (link_text.unmapped > 0 || entry_text.unmapped > 0)) unknown_heading = true;
        if (matches.empty()) {
            // PAC reports an entry no heading matches as linking to an incorrect destination (Phase 3 probes pacal_mismatch: destination
            // at a heading with other text; pacal_untagged: at an untagged P with the entry's text;
            // a real-world sample; probe toc_nohead: a document without any heading).
            // PAC was never seen reporting TOCILinkHasNoCorrespondingHeading.
            if (unknown_heading) ctx_->mark_incomplete(kIncorrect, "heading text has no Unicode mapping");
            else ctx_->report(kIncorrect, annot_page, toci_ref, where + ": no heading matches \"" + snippet(text.text) + "\"");
            return;
        }
        // Correct position: a matching heading on the destination page at the destination's vertical coordinate.
        // PAC's tolerance, calibrated in Phase 3 (probes pacal_above: 12 pt, pacal_above24: 24 pt, pacal_below;
        // a real-world sample): measured from the heading's top at baseline + font Ascent
        // (FontDescriptor) x size, a destination up to 19.5 pt above passes and 21.2 pt fails for both sizes
        // (measured from the 0.8 em glyph box top the limit would differ per size: 21.4-23.6 pt at 12 pt,
        // 26-30 pt at 24 pt); below the heading, 1.6 pt under the glyph box bottom (-0.2 em) passes and 5.6 pt
        // fails (12 pt only). The midpoints are used.
        constexpr double kAbove = 20.4, kBelow = 3.6;
        std::optional<double> top = dest_top(ld.dest);
        for (std::size_t h : matches) {
            const ElemContent& hc = first_content(headings[h]);
            if (hc.page != ld.dest.page) continue;
            if (!top || !hc.box_valid) return;
            const double head_top = hc.ascent_top_valid ? std::max(hc.ascent_top, hc.box.lly) : hc.box.ury;
            if (*top >= hc.box.lly - kBelow && *top <= head_top + kAbove) return;
        }
        ctx_->report(kIncorrect, annot_page, toci_ref,
                     where + ": destination page " + std::to_string(ld.dest.page + 1) + " /" + ld.dest.type +
                         " is not at the heading \"" + snippet(heading_text[matches.front()].text) + "\"");
    }

    // Location of an element: its own content, else that of its first descendant with content.
    const ElemContent& first_content(int h) const {
        if (elem_content_[h].has_items) return elem_content_[h];
        const StructTree& st = ctx_->st;
        std::vector<int> stack(st.elem(h).elems.rbegin(), st.elem(h).elems.rend());
        std::size_t budget = 10000;
        while (!stack.empty() && budget-- > 0) {
            int c = stack.back();
            stack.pop_back();
            if (elem_content_[c].has_items) return elem_content_[c];
            const auto& kids = st.elem(c).elems;
            for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back(*k);
        }
        return elem_content_[h];
    }

    // --- marked-content checks ---------------------------------------------------------------------

    void check_marked_content() {
        static const RuleIndex kOutside = R("EQX-PACQ-WarnTaggedContentFoundOutsidePage");
        static const RuleIndex kMissingLink = R("EQX-PACQ-FailMissingLinkTags");
        static const RuleIndex kArtifact = R("EQX-PACQ-WarnArtifactedBodyParagraphs");
        const ContentModel& cm = ctx_->content;
        const StructTree& st = ctx_->st;
        const auto& pages = ctx_->doc.pages();

        for (const auto& [frame, mc] : plain_mc_) {
            const McFrame& fr = cm.frames[frame];
            if (mc.page >= 0 && mc.page < int(pages.size()) && mc.box_valid && !mc.box.intersects(pages[mc.page].crop_box))
                ctx_->report(kOutside, mc.page, cm.streams[fr.stream].ref, "/" + fr.tag + " outside the page");
        }
        for (std::size_t m = 0; m < mcid_mc_.size(); ++m) {
            const McContent& mc = mcid_mc_[m];
            const McFrame& fr = cm.frames[mc.frame];
            ObjRef stream_ref = cm.streams[fr.stream].ref;
            std::string mcid = "MCID " + std::to_string(*fr.mcid);
            // Marked content entirely outside the page boundaries (partly inside -> SKIP). The page
            // boundary is the crop box (the visible page).
            if (mc.page >= 0 && mc.page < int(pages.size()) && mc.box_valid && !mc.box.intersects(pages[mc.page].crop_box))
                ctx_->report(kOutside, mc.page, stream_ref, mcid + " /" + fr.tag + " outside the page");
            // Marked content with a linkable string outside any Link element.
            // The owning element comes from the structure (MCID/MCR kid), else from the ParentTree; marked
            // content no element claims is not checked (open: PAC may treat it as outside any Link).
            int owner = mc_owner_[m];
            const StreamInstance& mc_stream = cm.streams[fr.stream];
            const bool form_shared = mc_stream.kind == StreamInstance::Kind::Form && !mc_stream.struct_parents;
            if (owner < 0 && !form_shared && fr.struct_elem.isDictionary()) owner = st.find(ref_of(fr.struct_elem));
            // Without a structure tree no marked content can be in a Link (a real-world sample: MCIDs, no tree).
            if (owner >= 0 || !st.present()) {
                if (auto url = pacq::find_linkable(mc.acc.text)) {
                    bool in_link = false;
                    for (int a = owner; a >= 0 && !in_link; a = st.elem(a).parent)
                        if (std17(st.elem(a)) == "Link") in_link = true;
                    if (!in_link)
                        ctx_->report(kMissingLink, mc.page, stream_ref, mcid + ": \"" + *url + "\" without Link element");
                }
            }
        }

        // ArtifactedBodyParagraphs: one instance per text piece of an /Artifact sequence (a string, or
        // the strings of a TJ array between two numbers) that contains a letter (a_digit_tj, a_punct_tj: digits and punctuation alone are not counted) and whose box does not lie
        // entirely inside the top or bottom margin band. Artifact /Type does not matter; an MCID inside or around the sequence
        // changes nothing (ai_*, ag_*; it is also a 7.1-2 failure), a Form XObject painted inside the artifact is not looked into.
        // Evidence (PAC): 3 Tj -> 3, one Tj in the band and one in the body -> 1 (a_band_mixed), TJ pieces counted singly and
        // punctuation-only pieces not (a real-world sample: 19 + 9 pieces of two lines minus the 4 made of /, . = 24;
        // the lines above them are inside the band), RW-04 / art_form_inside / art_form_wm: text of a form in an artifact is
        // not flagged, art_form_own: an artifact inside the form is. Band: see in_margin_band.
        if (contrast_) {
            for (const pacc::ArtifactTextPiece& a : pacc::artifact_text_pieces(*contrast_)) {
                if (a.page < 0 || a.page >= int(pages.size()) || in_margin_band(pages[a.page], a.box)) continue;
                // outside the crop box there is no page body (a real-world sample: a spread cropped to one
                // page, 99 artifact pieces on the invisible half, PAC: none)
                if (!a.box.intersects(pages[a.page].crop_box)) continue;
                ctx_->report(kArtifact, a.page, a.obj, "artifact text \"" + a.text + "\" in the page body");
            }
        }
    }

    // True when the box lies entirely inside the top or the bottom 14.8 % of the page as displayed (/Rotate).
    // Evidence (boxes of -0.2 .. 0.8 em, 11 pt text; PAC runs of the a_*, b_*, d_* probes): A4 top: box bottom 124.2 pt not
    // flagged, 126.2 pt flagged; A4 bottom: box top 120.8 pt not flagged, 124.8 pt flagged; landscape (595 pt) bottom: box top
    // 88.8 pt flagged. All fit one share of the page height between 14.75 % and 14.82 %.
    static bool in_margin_band(const Page& pg, const Rect& b) {
        const Rect& c = pg.crop_box;
        double from_top_min, from_top_max, extent;   // distances from the displayed top edge
        int rot = ((pg.rotate % 360) + 360) % 360;
        if (rot == 90) {
            extent = c.width();
            from_top_min = b.llx - c.llx;
            from_top_max = b.urx - c.llx;
        } else if (rot == 180) {
            extent = c.height();
            from_top_min = b.lly - c.lly;
            from_top_max = b.ury - c.lly;
        } else if (rot == 270) {
            extent = c.width();
            from_top_min = c.urx - b.urx;
            from_top_max = c.urx - b.llx;
        } else {
            extent = c.height();
            from_top_min = c.ury - b.ury;
            from_top_max = c.ury - b.lly;
        }
        if (extent <= 0) return false;
        const double band = 0.148 * extent;
        return from_top_max <= band || from_top_min >= extent - band;   // entirely inside a band
    }

    // --- language of alternative, actual and expansion texts (PDF/UA tab) ---------------------------

    void check_language() {
        static const RuleIndex kAlt = R("EQX-PAC-FailAltTextHasLanguage");
        static const RuleIndex kActual = R("EQX-PAC-FailActualTextHasLanguage");
        static const RuleIndex kE = R("EQX-PAC-FailExpansionTexHasLanguage");
        const StructTree& st = ctx_->st;
        const ContentModel& cm = ctx_->content;
        // A language is determined by a /Lang string on the object, an ancestor (structure element chain;
        // for marked content also the enclosing sequences and their structure elements) or the catalog.
        // U1-D04 counts structure elements and marked-content sequences (2 per kind). Empty values are not
        // alternative texts and are skipped.
        for (const StructElem& e : st.elems()) {
            struct K {
                const char* key;
                RuleIndex rule;
            };
            for (K k : {K{"Alt", kAlt}, K{"ActualText", kActual}, K{"E", kE}}) {
                Obj v = cos::get(e.dict, k.key);
                if (!v.isString() || cos::bytes(v).value_or("").empty()) continue;
                if (lang_determined(e.dict)) continue;
                ctx_->report(k.rule, elem_page(e.index), e.ref,
                             elem_label(e.index) + " /" + k.key + " \"" + snippet(utf8_to_u32(cos::text(v).value_or(""))) + "\" without language");
            }
        }
        for (std::size_t f = 0; f < cm.frames.size(); ++f) {
            const McFrame& fr = cm.frames[f];
            if (fr.stream < 0 || !cm.streams[fr.stream].semantic) continue;
            struct K {
                const OptString* value;
                const char* key;
                RuleIndex rule;
            };
            for (K k : {K{&fr.alt, "Alt", kAlt}, K{&fr.actual_text, "ActualText", kActual}, K{&fr.e, "E", kE}}) {
                if (!*k.value || k.value->value().empty()) continue;
                if (catalog_lang_ || mc_lang(fr.stream, int(f))) continue;
                ctx_->report(k.rule, cm.page_of(fr.stream), cm.streams[fr.stream].ref,
                             "/" + fr.tag + " marked content /" + k.key + " \"" + snippet(pdf_text_to_u32(k.value->value())) +
                                 "\" without language");
            }
        }
    }

    bool lang_determined(const Obj& elem_dict) const {
        if (catalog_lang_) return true;
        if (cos::get(elem_dict, "Lang").isString()) return true;
        return ctx_->st.parent_lang(elem_dict).has_value();
    }

    // Language of a marked-content sequence: own or enclosing /Lang, the structure element of an MCID
    // (and its ancestors), continued into the invoking stream of a Form XObject.
    bool mc_lang(int stream, int frame) const {
        const ContentModel& cm = ctx_->content;
        for (int guard = 0; stream >= 0 && guard < 256; ++guard) {
            if (cm.lang_value(ctx_->st, stream, frame)) return true;
            const StreamInstance& s = cm.streams[stream];
            if (s.parent_struct_elem.isDictionary() && lang_determined(s.parent_struct_elem)) return true;
            frame = s.parent_frame;
            stream = s.parent;
        }
        return false;
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_pac_quality_module() { return std::make_unique<Module>(); }

}  // namespace eqx
