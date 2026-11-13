#include "rules/pac_quality/wcag_color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "util/text.h"

namespace eqx::wcag {

namespace {

double channel(double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance8(const unsigned char* p) {
    return 0.2126 * channel(p[0] / 255.0) + 0.7152 * channel(p[1] / 255.0) + 0.0722 * channel(p[2] / 255.0);
}
unsigned char to8(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

std::string lower_name(const Font* f) {
    std::string n = f->base_font;
    if (n.size() > 7 && n[6] == '+') n.erase(0, 7);   // subset prefix
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return n;
}

std::string excerpt(std::string s) {
    if (s.size() > 40) {
        std::size_t cut = 40;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;   // not inside a UTF-8 sequence
        s = s.substr(0, cut) + "...";
    }
    return s;
}

// A colour array of /MK /BC or /BG: empty = no colour (transparent).
bool mk_colour(const Obj& arr, unsigned char* out) {
    std::vector<double> v;
    for (const Obj& o : cos::array_items(arr))
        if (auto n = cos::number(o)) v.push_back(std::clamp(*n, 0.0, 1.0));
    if (v.size() == 1) out[0] = out[1] = out[2] = to8(v[0]);
    else if (v.size() == 3) out[0] = to8(v[0]), out[1] = to8(v[1]), out[2] = to8(v[2]);
    else if (v.size() == 4)
        for (int k = 0; k < 3; ++k) out[k] = to8((1 - v[std::size_t(k)]) * (1 - v[3]));
    else return false;
    return true;
}

// Border width of an annotation: /BS /W, else the third /Border entry (both default to 1, ISO 32000-1 Tables 164, 166).
double border_width(const Obj& annot) {
    Obj bs = cos::get(annot, "BS");
    if (bs.isDictionary()) return cos::number(bs, "W").value_or(1);
    auto b = cos::array_items(cos::get(annot, "Border"));
    return b.size() >= 3 ? cos::number(b[2]).value_or(1) : 1;
}

bool is_painter(const ContentItem& it) {
    return (it.kind == ContentItem::Kind::LineArt && it.paint) || it.kind == ContentItem::Kind::InlineImage ||
           it.kind == ContentItem::Kind::ImageXObject || it.kind == ContentItem::Kind::Shading;
}

// The page colour under r: white where nothing painted touches r, the colour of one opaque filled rectangle covering r when it is the
// last paint there. False otherwise: the page content draws there itself (often the field's frame), which is not judged here.
bool flat_background(const ContentModel& cm, const std::vector<int>& painters, const Rect& r, unsigned char* out) {
    int last = -1;
    for (int i : painters)
        if (!cm.items[std::size_t(i)].bbox_valid || cm.items[std::size_t(i)].bbox.intersects(r)) last = i;
    if (last < 0) {
        out[0] = out[1] = out[2] = 255;
        return true;
    }
    const ContentItem& p = cm.items[std::size_t(last)];
    const bool covers = p.bbox_valid && p.bbox.llx <= r.llx + 0.5 && p.bbox.lly <= r.lly + 0.5 && p.bbox.urx >= r.urx - 0.5 &&
                        p.bbox.ury >= r.ury - 0.5;
    if (p.kind != ContentItem::Kind::LineArt || p.paint != 1 || !p.exact_rect || !p.fill.known() || p.fill_alpha < 1.0f || !covers)
        return false;
    for (int k = 0; k < 3; ++k) out[k] = to8(p.fill.rgb[k]);
    return true;
}

bool letter(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= 0xC0 && !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0xE000 && c <= 0xF8FF) && c != 0xD7 && c != 0xF7);
}

}  // namespace

double contrast8(const unsigned char* a, const unsigned char* b) {
    double la = luminance8(a), lb = luminance8(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

std::string ratio_str(double r) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f:1", std::floor(r * 100) / 100);
    return b;
}

std::string hex(const unsigned char* c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c[0], c[1], c[2]);
    return b;
}

std::string box_str(const Rect& r) {
    char b[96];
    std::snprintf(b, sizeof b, "box [%.0f %.0f %.0f %.0f]", r.llx, r.lly, r.urx, r.ury);
    return b;
}

int bold_state(const Font* f, int tr) {
    if (!f) return -1;
    if (tr == 2 || tr == 6) return -1;   // fill and stroke thickens the strokes
    const std::string n = lower_name(f);
    const bool semi = n.find("semibold") != std::string::npos || n.find("demibold") != std::string::npos ||
                      n.find("semi-bold") != std::string::npos || n.find("demi-bold") != std::string::npos ||
                      n.find("semibd") != std::string::npos || n.find("demibd") != std::string::npos;
    if (semi) return -1;
    const bool named = n.find("bold") != std::string::npos || n.find("black") != std::string::npos || n.find("heavy") != std::string::npos;
    Obj fd = cos::get(f->dict, "FontDescriptor");
    if (!fd.isDictionary()) {
        Obj desc = cos::get(f->dict, "DescendantFonts");
        if (desc.isArray() && desc.getArrayNItems() > 0) fd = cos::get(desc.getArrayItem(0), "FontDescriptor");
    }
    if (auto w = cos::number(fd, "FontWeight")) {
        if (*w >= 700) return 1;
        if (*w <= 500) return named ? -1 : 0;
        return -1;
    }
    if (named) return 1;
    if (auto fl = cos::integer(fd, "Flags"); fl && (*fl & (1 << 18))) return 1;   // ForceBold
    return 0;
}

// WCAG 2.x 1.4.3: 4.5:1, large text (at least 18 pt, or 14 pt bold) 3:1. No rounding of the ratio; the size is taken at the 0.1 pt
// the detail shows (a scaled 18 pt font is 17.999 pt). A weight that cannot be told decides nothing between 3:1 and 4.5:1.
TextJudgement judge_text(const TextFacts& f) {
    TextJudgement j;
    if (f.ratio >= 4.5) return j;
    const bool large = f.size >= 17.95 || (f.size >= 13.95 && f.bold == 1);
    const bool normal = f.size < 17.95 && (f.size < 13.95 || f.bold == 0);
    if (large && f.ratio >= 3.0) return j;
    j.required = large ? "3:1 (large text)" : normal ? "4.5:1" : "4.5:1, or 3:1 if it counts as large text";
    std::vector<const char*> why;
    if (!large && !normal && f.ratio >= 3.0) why.push_back("whether it is large text (18 pt, or 14 pt bold) cannot be told");
    if (f.ratio < 1.05) why.push_back("the text has the colour of its background (hidden text?)");
    if (f.rendered) why.push_back("background measured on the rendering (image, gradient or overlapping paint)");
    if (f.device_colour) why.push_back("device-dependent colour (CMYK or spot colour)");
    if (f.transparency) why.push_back("soft mask or blend mode");
    if (f.clipped) why.push_back("a clip may hide the text");
    if (f.stroked) why.push_back("outlined text");
    if (f.artifact) why.push_back("marked as artifact (exempt if pure decoration)");
    if (f.in_figure) why.push_back("inside a Figure (exempt if part of a picture or a logotype)");
    for (const char* w : why) j.reasons += std::string(j.reasons.empty() ? "" : "; ") + w;
    j.verdict = why.empty() ? Verdict::Violation : Verdict::Review;
    return j;
}

std::string text_detail(const TextFacts& f, const TextJudgement& j, const Rect& box, const std::string& text) {
    char size[32];
    std::snprintf(size, sizeof size, "%.1f pt", f.size);
    std::string d = std::string("WCAG 1.4.3 ") + (j.verdict == Verdict::Violation ? "confirmed" : "manual review") + ": contrast " +
                    ratio_str(f.ratio) + " (text " + hex(f.fg) + " on " + (f.rendered ? "rendered " : "") + hex(f.bg) + ", " + size +
                    "), required " + j.required;
    if (!j.reasons.empty()) d += "; to check: " + j.reasons;
    d += "; " + box_str(box);
    if (!text.empty()) d += ": \"" + excerpt(text) + "\"";
    return d;
}

// WCAG 1.4.11 for form fields: the visual information that identifies the field (its border or background, ISO 32000-1 12.5.6.19
// /MK) against the page around it. Read-only fields are inactive components (exempt); hidden fields are not shown. Only fields on a
// page area the content leaves empty (or covers with one flat rectangle) are judged: elsewhere the page draws the frame itself. The
// appearance stream decides what a viewer shows, /MK is what it is regenerated from, so a finding is always for review.
void check_form_fields(ValidationContext& ctx) {
    static const RuleIndex rule = R("EQX-WCAG-1.4.11-NonTextContrastReview");
    const ContentModel& cm = ctx.content;
    const auto& pages = ctx.doc.pages();
    struct Field {
        std::size_t page;
        Obj annot;
        Rect r;
        const char* kind;
        std::string label;
        bool border, fill;
        unsigned char bc[3], bg[3];
    };
    std::vector<Field> fields;
    for (std::size_t pi = 0; pi < pages.size(); ++pi) {
        for (const Obj& a : cos::array_items(cos::get(pages[pi].dict, "Annots"))) {
            if (!cos::name_is(a, "Subtype", "Widget")) continue;
            if (cos::integer(a, "F").value_or(0) & (1 | 2 | 32)) continue;   // Invisible, Hidden, NoView
            auto r = cos::rect(cos::get(a, "Rect"));
            if (!r || r->width() < 2 || r->height() < 2) continue;
            std::optional<std::string> ft, label;
            std::int64_t ff = -1;
            Obj f = a;
            for (int guard = 0; f.isDictionary() && guard < 64; ++guard) {
                if (!ft) ft = cos::name(f, "FT");
                if (ff < 0) ff = cos::integer(f, "Ff").value_or(-1);
                if (!label) label = cos::text(f, "TU");
                if (!label) label = cos::text(f, "T");
                f = cos::get(f, "Parent");
            }
            if (!ft || (*ft != "Tx" && *ft != "Btn" && *ft != "Ch")) continue;
            if (ff > 0 && (ff & 1)) continue;   // ReadOnly
            Field fd{pi, a, *r, "", label.value_or(""), false, false, {}, {}};
            Obj mk = cos::get(a, "MK");
            fd.border = mk_colour(cos::get(mk, "BC"), fd.bc) && border_width(a) > 0;
            fd.fill = mk_colour(cos::get(mk, "BG"), fd.bg);
            if (!fd.border && !fd.fill) continue;   // no indicator of its own (a label or the page may identify it)
            const std::int64_t flags = std::max<std::int64_t>(ff, 0);
            fd.kind = *ft == "Tx" ? "text field" : *ft == "Ch" ? "choice field"
                      : (flags & (1 << 16)) ? "push button" : (flags & (1 << 15)) ? "radio button" : "check box";
            fields.push_back(std::move(fd));
        }
    }
    if (fields.empty()) return;
    // the painters of the pages that hold such fields
    std::vector<std::vector<int>> painters(pages.size());
    std::vector<char> wanted(pages.size(), 0);
    for (const Field& f : fields) wanted[f.page] = 1;
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
        const StreamInstance& s = cm.streams[std::size_t(cm.items[i].stream)];
        if (s.page >= 0 && std::size_t(s.page) < pages.size() && wanted[std::size_t(s.page)] && s.semantic && is_painter(cm.items[i]))
            painters[std::size_t(s.page)].push_back(int(i));
    }
    for (const Field& f : fields) {
        unsigned char page_bg[3];
        if (!flat_background(cm, painters[f.page], f.r, page_bg)) continue;
        const double rb = f.border ? contrast8(f.bc, page_bg) : 0, rf = f.fill ? contrast8(f.bg, page_bg) : 0;
        if (std::max(rb, rf) >= 3.0) continue;
        std::string d = "WCAG 1.4.11 manual review: form field";
        if (!f.label.empty()) d += " \"" + excerpt(f.label) + "\"";
        d += std::string(" (") + f.kind + "): its boundary has " + ratio_str(std::max(rb, rf)) + " against the page " + hex(page_bg) + " (";
        if (f.border) d += "border " + hex(f.bc) + " " + ratio_str(rb);
        if (f.fill) d += std::string(f.border ? ", " : "") + "background " + hex(f.bg) + " " + ratio_str(rf);
        d += "), required 3:1 unless something else identifies the field; colours from /MK, the appearance stream decides what is shown; " +
             box_str(f.r);
        ctx.report(rule, int(f.page), ref_of(f.annot), d);
    }
}

// WCAG 1.4.1: text set apart from the text next to it on the same line by colour alone, the way links commonly are (failure F73).
// Same font and size, no underline below it, and the two colours less than 3:1 apart (Understanding 1.4.1: a lightness difference
// of 3:1 is a distinction of its own). The reference is the page's usual colour for that font and size, so only the text that differs
// from it is reported. Whether the colour carries information needs a reviewer, so every finding is for review. Page by page: items
// of one page are contiguous in the content model, so only one page's runs are held.
namespace {

struct ColourRun {
    int item;
    double base, x0, x1, size;
    int font;
    unsigned char rgb[3];
    int letters;
};

std::uint32_t colour_key(const unsigned char* c) { return std::uint32_t(c[0]) << 16 | std::uint32_t(c[1]) << 8 | c[2]; }

void colour_page(ValidationContext& ctx, std::size_t pi, std::vector<ColourRun>& rs, const std::vector<int>& thin) {
    static const RuleIndex rule = R("EQX-WCAG-1.4.1-UseOfColorReview");
    const ContentModel& cm = ctx.content;
    // the usual colour per font and size (letters), and the links of the page
    std::map<std::pair<int, long>, std::map<std::uint32_t, int>> usage;
    auto style = [](const ColourRun& r) { return std::make_pair(r.font, std::lround(r.size * 10)); };
    for (const ColourRun& r : rs) usage[style(r)][colour_key(r.rgb)] += r.letters;
    std::map<std::pair<int, long>, std::uint32_t> usual;
    for (const auto& [k, m] : usage)
        usual[k] = std::max_element(m.begin(), m.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
    struct Link {
        Rect r;
        bool framed;
    };
    std::vector<Link> links;
    for (const Obj& a : cos::array_items(cos::get(ctx.doc.pages()[pi].dict, "Annots"))) {
        if (!cos::name_is(a, "Subtype", "Link")) continue;
        if (auto r = cos::rect(cos::get(a, "Rect"))) links.push_back({*r, border_width(a) > 0 && !cos::array_items(cos::get(a, "C")).empty()});
    }
    std::sort(rs.begin(), rs.end(), [](const ColourRun& a, const ColourRun& b) { return a.base != b.base ? a.base < b.base : a.x0 < b.x0; });
    std::vector<const ColourRun*> line;
    for (std::size_t a = 0; a < rs.size();) {
        std::size_t b = a + 1;
        while (b < rs.size() && rs[b].base - rs[a].base <= 0.15 * rs[a].size) ++b;
        line.clear();
        for (std::size_t k = a; k < b; ++k) line.push_back(&rs[k]);
        a = b;
        if (line.size() < 2) continue;
        std::sort(line.begin(), line.end(), [](const ColourRun* x, const ColourRun* y) { return x->x0 < y->x0; });
        // phrases: neighbouring runs of one colour, font and size, at most 1 em apart
        struct Phrase {
            std::size_t first, last;
            int letters;
        };
        std::vector<Phrase> ph;
        for (std::size_t k = 0; k < line.size(); ++k) {
            const ColourRun& r = *line[k];
            if (!ph.empty()) {
                const ColourRun& p = *line[ph.back().last];
                if (colour_key(p.rgb) == colour_key(r.rgb) && style(p) == style(r) && r.x0 - p.x1 <= p.size) {
                    ph.back().last = k;
                    ph.back().letters += r.letters;
                    continue;
                }
            }
            ph.push_back({k, k, r.letters});
        }
        for (std::size_t k = 0; k < ph.size(); ++k) {
            const ColourRun& f = *line[ph[k].first];
            const ColourRun& l = *line[ph[k].last];
            const std::uint32_t ref = usual[style(f)];
            if (colour_key(f.rgb) == ref || ph[k].letters < 2) continue;
            const ColourRun* other = nullptr;
            const Phrase* other_phrase = nullptr;
            for (int side : {-1, 1}) {
                if ((side < 0 && k == 0) || (side > 0 && k + 1 == ph.size())) continue;
                const Phrase& n = ph[std::size_t(std::ptrdiff_t(k) + side)];
                const ColourRun& nr = *line[side < 0 ? n.last : n.first];
                const double gap = side < 0 ? f.x0 - nr.x1 : nr.x0 - l.x1;
                if (colour_key(nr.rgb) != ref || style(nr) != style(f) || gap > f.size) continue;
                const int d0 = f.rgb[0] - nr.rgb[0], d1 = f.rgb[1] - nr.rgb[1], d2 = f.rgb[2] - nr.rgb[2];
                if (d0 * d0 + d1 * d1 + d2 * d2 < 64 * 64 || contrast8(f.rgb, nr.rgb) >= 3.0) continue;   // hardly visible, or lightness
                other = &nr;
                other_phrase = &n;
                break;
            }
            if (!other) continue;
            const double x0 = f.x0, x1 = l.x1, base = f.base, size = f.size;
            // Underlined: the thin paths at the baseline cover at least 60 % of the phrase together (a phrase of several
            // links has one underline per link, with gaps between them).
            auto underlined = [&](double a0, double a1) {
                std::vector<std::pair<double, double>> cover;
                for (int li : thin) {
                    const Rect& u = cm.items[std::size_t(li)].bbox;
                    if (u.height() > std::max(1.5, 0.15 * size) || u.ury < base - 0.4 * size || u.lly > base + 0.1 * size) continue;
                    const double c0 = std::max(u.llx, a0), c1 = std::min(u.urx, a1);
                    if (c1 > c0) cover.emplace_back(c0, c1);
                }
                std::sort(cover.begin(), cover.end());
                double covered = 0, end = a0;
                for (const auto& [c0, c1] : cover) {
                    if (c1 <= end) continue;
                    covered += c1 - std::max(c0, end);
                    end = c1;
                }
                return covered >= 0.6 * (a1 - a0);
            };
            // The two phrases differ by more than colour when either is underlined or framed as a link: the deviating
            // phrase can be the body text when the page's usual colour for this font and size is that of its links.
            const double o0 = line[other_phrase->first]->x0, o1 = line[other_phrase->last]->x1;
            if (underlined(x0, x1) || underlined(o0, o1)) continue;
            const double cx = (x0 + x1) / 2, cy = base + 0.3 * size, ocx = (o0 + o1) / 2;
            bool link = false, framed = false;
            for (const Link& lk : links) {
                if (lk.r.contains(cx, cy)) link = true, framed = framed || lk.framed;
                if (lk.r.contains(ocx, cy) && lk.framed) framed = true;
            }
            if (framed) continue;   // the link border is a visual cue of its own
            std::string text;
            for (std::size_t q = ph[k].first; q <= ph[k].last; ++q)
                text += to_utf8(cm.texts[std::size_t(cm.items[std::size_t(line[q]->item)].text)].text);
            char size_s[32];
            std::snprintf(size_s, sizeof size_s, "%.1f pt", size);
            const std::string d = "WCAG 1.4.1 manual review: \"" + excerpt(text) + "\" (" + hex(f.rgb) + ") is set apart from the adjacent text (" +
                                  hex(other->rgb) + ") by colour alone: same font and size (" + size_s + "), no underline, the two colours " +
                                  ratio_str(contrast8(f.rgb, other->rgb)) + " apart (3:1 or another visual cue needed if the colour carries meaning)" +
                                  (link ? "; it is a link" : "") + "; " + box_str(Rect{x0, base - 0.2 * size, x1, base + 0.8 * size});
            const StreamInstance& s = cm.streams[std::size_t(cm.items[std::size_t(f.item)].stream)];
            ctx.report(rule, s.page, s.ref, d);
        }
    }
}

}  // namespace

void check_use_of_color(ValidationContext& ctx) {
    const ContentModel& cm = ctx.content;
    const std::size_t npages = ctx.doc.pages().size();
    std::vector<ColourRun> runs;
    std::vector<int> thin;   // painted paths: underline candidates
    std::map<std::string, int> font_ids;
    std::map<const Font*, int> font_of;
    int page = -1;
    auto flush = [&]() {
        if (page >= 0 && runs.size() >= 2) colour_page(ctx, std::size_t(page), runs, thin);
        runs.clear();
        thin.clear();
    };
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
        const ContentItem& it = cm.items[i];
        const StreamInstance& s = cm.streams[std::size_t(it.stream)];
        if (!s.semantic || s.page < 0 || std::size_t(s.page) >= npages || !it.bbox_valid) continue;
        if (s.page != page) {
            flush();
            page = s.page;
        }
        if (it.kind == ContentItem::Kind::LineArt) {
            if (it.paint) thin.push_back(int(i));
            continue;
        }
        if (it.kind != ContentItem::Kind::Text || it.text < 0) continue;
        const TextRun& tr = cm.texts[std::size_t(it.text)];
        if (tr.glyphs == 0 || !tr.font || !tr.fill.known() || tr.size_user < 2) continue;
        if (tr.tr == 1 || tr.tr == 3 || tr.tr == 5 || tr.tr == 7) continue;   // invisible or outline only
        const double h = it.bbox.height();
        if (h < 0.9 * tr.size_user || h > 1.1 * tr.size_user) continue;    // upright, horizontal, one line
        if (s.inherited_artifact || cm.parents_tags_contain(it.stream, it.frame, "Artifact")) continue;
        int letters = 0;
        for (char32_t c : tr.text) letters += letter(c) ? 1 : 0;
        if (letters == 0) continue;
        auto fo = font_of.find(tr.font);
        if (fo == font_of.end()) fo = font_of.emplace(tr.font, font_ids.emplace(lower_name(tr.font), int(font_ids.size())).first->second).first;
        ColourRun r{int(i), it.bbox.lly + 0.2 * tr.size_user, it.bbox.llx, it.bbox.urx, tr.size_user, fo->second, {}, letters};
        for (int k = 0; k < 3; ++k) r.rgb[k] = to8(tr.fill.rgb[k]);
        runs.push_back(r);
    }
    flush();
}

}  // namespace eqx::wcag
