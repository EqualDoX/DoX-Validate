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
#include <exception>
#include <functional>
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

// A signature as the observer keeps it per operator: its text, length and whether it is one of the two patterns.
struct Signature {
    const char* text = nullptr;   // nullptr: the operator is not judged
    std::uint8_t length = 0;
    bool plus = false, star = false;   // "+", "*?"
};

bool operands_match(const Signature& sig, const std::vector<Operand>& a) {
    if (sig.plus || sig.star) {
        std::size_t n = a.size();
        if (sig.star && n > 0 && a.back().type == Operand::Name) --n;
        else if (sig.star && n == 0) return false;
        if (sig.plus && n == 0) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (!a[i].is_number()) return false;
        return true;
    }
    if (a.size() != sig.length) return false;
    for (std::size_t i = 0; i < sig.length; ++i) {
        const Operand& o = a[i];
        switch (sig.text[i]) {
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
            std::array<Signature, static_cast<std::size_t>(Op::Count)> t{};
            const auto set = [&](Op code, std::string_view sig) {
                t[static_cast<std::size_t>(code)] = Signature{sig.data(), static_cast<std::uint8_t>(sig.size()), sig == "+", sig == "*?"};
            };
            for (const auto& [name, sig] : signatures()) set(op_code(name), sig);
            set(Op::EI, "x");
            return t;
        }();
        const Signature& sig = table[static_cast<std::size_t>(op.code)];
        if (!sig.text) return;   // unknown operators are not judged (behaviour not established)
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

// --- Reading the content items page by page -----------------------------------------------------------
// The rules below read every content item, in the order of the items. They take what they need of a page's items
// when the page's content stream has ended (ItemTap, an observer), so the interpreter can give those items back
// (ContentModel::release_consumed) instead of keeping the document's content until the modules run.
//
// What is taken is small and is what the rules compute from the items alone. Everything that reads the PDF's
// objects (whether a structure element reaches the tree's root, a language, a font descriptor) and every report is
// left to run(), which works on what was taken, in the order of the items: the findings, their order, the order
// in which objects are first read (the order of qpdf's warnings) and the place where a failure stops the module
// are those of reading the items there.

// Consecutive content items of one stream instance and one marked-content sequence.
struct ItemRun {
    int stream = -1;
    int frame = -1;
    std::uint32_t first_op = 0;        // operator of its first item
    std::uint32_t first_text_op = 0;   // operator of its first text item, when it has one
    std::uint32_t count[5] = {0, 0, 0, 0, 0};   // its items per ContentItem::Kind
};

// What a text-contrast finding's detail is made from.
struct ContrastDetail {
    int stream = -1;
    std::uint32_t font = 0;       // index of the font whose weight decides the threshold (events EitherWay and UnlessBold)
    double ratio = 0, size = 0;
    float fg[3] = {0, 0, 0}, bg[3] = {0, 0, 0};
    std::string text;             // of the run, cut as in the detail
};

// One step of the text-contrast check, in the order of the text items.
struct ContrastEvent {
    enum Kind : std::uint8_t {
        AskBold,           // bold_font(fonts[a]) is asked here for the first time
        Large,             // a finding with detail a, large text (18 pt and more)
        Small,             // a finding with detail a, text below 14 pt
        EitherWay,         // a finding with detail a, 14 to 18 pt, below 3:1: large when its font is bold
        UnlessBold,        // 14 to 18 pt, between 3:1 and 4.5:1: a finding with detail a unless its font is bold
        CountCertain,      // findings that are only counted: the rule's record is full when they are reported
        CountUnlessBold,   // such findings unless fonts[a] is bold
    };
    Kind kind;
    std::uint32_t a;
    std::uint32_t n;       // how many times in a row
};

// A failure while a page was taken, thrown again where run() would have met it.
struct TakeFailure final : std::exception {
    std::string message;
    const char* what() const noexcept override { return message.c_str(); }
};

class ItemTap final : public ContentObserver {
public:
    const Document* doc = nullptr;
    std::size_t max_locations = 0;

    std::vector<ItemRun> runs;
    std::vector<ContrastEvent> events;
    std::vector<ContrastDetail> details;
    std::vector<const Font*> fonts;

    void on_stream_end(const ContentModel& cm, int si) override {
        if (cm.streams[static_cast<std::size_t>(si)].kind == StreamInstance::Kind::Page) take(cm);
    }

    // Takes the items that were not taken yet: those of the page that has ended, or, called from run(), what an
    // interpretation that stopped early left.
    void take(const ContentModel& cm) {
        const std::size_t begin = taken_, end = cm.items.size();
        taken_ = end;
        if (begin >= end) return;
        // A failure here (memory) is one run() would have had reading the same items: it is kept, the part that
        // failed takes nothing more, and run() throws it where that part's work is.
        if (!runs_failed_) guarded(runs_failed_, runs_what_, [&] { take_runs(cm, begin, end); });
        if (!contrast_failed_) guarded(contrast_failed_, contrast_what_, [&] { take_contrast(cm, begin, end); });
    }

    void rethrow_runs() const {
        if (runs_failed_) throw failure(runs_what_);
    }
    void rethrow_contrast() const {
        if (contrast_failed_) throw failure(contrast_what_);
    }

private:
    std::size_t taken_ = 0;
    bool runs_failed_ = false, contrast_failed_ = false;
    std::string runs_what_, contrast_what_;
    // text contrast: which details are kept
    std::unordered_map<const Font*, std::uint32_t> font_index_;
    std::unordered_map<std::string, std::uint32_t> detail_index_;                         // finding as it will be reported -> its detail
    std::unordered_set<std::size_t> certain_;                                             // findings whatever the fonts' weights
    std::unordered_map<const Font*, std::unordered_set<std::size_t>> by_font_;            // 14 to 18 pt findings per font
    bool record_full_ = false;   // from here on the rule's record is full whatever the fonts' weights

    static TakeFailure failure(const std::string& what) {
        TakeFailure f;
        f.message = what;
        return f;
    }
    template <class F>
    static void guarded(bool& failed, std::string& what, F&& work) {
        try {
            work();
        } catch (const std::exception& e) {
            failed = true;
            try {
                what = e.what();
            } catch (...) {
                what.clear();
            }
        }
    }

    void take_runs(const ContentModel& cm, std::size_t begin, std::size_t end) {
        bool fresh = true;   // items of different calls are of different pages: never one run
        for (std::size_t i = begin; i < end; ++i) {
            const ContentItem& it = cm.items[i];
            if (fresh || runs.back().stream != it.stream || runs.back().frame != it.frame) {
                ItemRun r;
                r.stream = it.stream;
                r.frame = it.frame;
                r.first_op = it.op_index;
                runs.push_back(r);
                fresh = false;
            }
            ItemRun& r = runs.back();
            if (it.kind == ContentItem::Kind::Text && r.count[static_cast<int>(ContentItem::Kind::Text)] == 0) r.first_text_op = it.op_index;
            ++r.count[static_cast<int>(it.kind)];
        }
    }

    // --- text contrast: the check of every text item up to its contrast ratio; what is left for run() is asking
    // the weight of a font (bold_font reads its descriptor) and reporting.
    void take_contrast(const ContentModel& cm, std::size_t begin, std::size_t end) {
        const auto& pages = doc->pages();
        static const float white[3] = {1, 1, 1};
        std::optional<Grid> grid;
        int grid_page = -1;
        for (std::size_t i = begin; i < end; ++i) {
            const ContentItem& it = cm.items[i];
            const int pg = cm.streams[static_cast<std::size_t>(it.stream)].page;
            if (pg < 0 || pg >= static_cast<int>(pages.size())) continue;
            if (!grid || pg != grid_page) {   // the items of a page follow each other
                grid.emplace();
                grid->box = pages[static_cast<std::size_t>(pg)].media_box;
                grid_page = pg;
            }
            const int idx = static_cast<int>(i);
            if (it.kind == ContentItem::Kind::Text) {
                take_text(cm, *grid, idx, white);
                continue;
            }
            bool painter = (it.kind == ContentItem::Kind::LineArt && (it.paint & 1)) ||
                           it.kind == ContentItem::Kind::InlineImage || it.kind == ContentItem::Kind::ImageXObject ||
                           it.kind == ContentItem::Kind::Shading;
            if (!painter) continue;
            Rect r = it.bbox_valid ? it.bbox : grid->box;   // shadings without a box cover the page
            grid->add(idx, r);
        }
    }

    void take_text(const ContentModel& cm, const Grid& grid, int idx, const float white[3]) {
        const ContentItem& it = cm.items[static_cast<std::size_t>(idx)];
        if (it.text < 0 || !it.bbox_valid) return;
        const TextRun& tr = cm.texts[static_cast<std::size_t>(it.text)];
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
                    const ContentItem& p = cm.items[static_cast<std::size_t>(*pos)];
                    Rect pr = p.bbox_valid ? p.bbox : grid.box;
                    if (pr.intersects(it.bbox)) {
                        best = *pos;
                        break;
                    }
                }
            }
        const float* bg = white;
        if (best >= 0) {
            const ContentItem& p = cm.items[static_cast<std::size_t>(best)];
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
        // large = size >= 18 || (size >= 14 && bold_font(font)), need = large ? 3.0 : 4.5, a finding when
        // ratio + 1e-9 < need: decided here as far as it does not depend on the font's weight.
        const bool big = tr.size_user >= 18.0;
        const bool asks_weight = !big && tr.size_user >= 14.0;
        const bool below3 = !(ratio + 1e-9 >= 3.0), below45 = !(ratio + 1e-9 >= 4.5);
        if (!asks_weight) {
            if (big ? below3 : below45) finding(cm, it, tr, big ? ContrastEvent::Large : ContrastEvent::Small, ratio, fg, bg);
            return;
        }
        font_of(tr.font);        // AskBold where the font's weight is asked first
        if (!below45) return;    // passes at either threshold
        finding(cm, it, tr, below3 ? ContrastEvent::EitherWay : ContrastEvent::UnlessBold, ratio, fg, bg);
    }

    std::uint32_t font_of(const Font* f) {
        auto [pos, fresh] = font_index_.emplace(f, static_cast<std::uint32_t>(fonts.size()));
        if (fresh) {
            fonts.push_back(f);
            events.push_back({ContrastEvent::AskBold, pos->second, 1});
        }
        return pos->second;
    }

    void event(ContrastEvent::Kind kind, std::uint32_t a) {
        if (!events.empty() && events.back().kind == kind && events.back().a == a && events.back().n != 0xffffffffu) {
            ++events.back().n;
            return;
        }
        events.push_back({kind, a, 1});
    }

    // A finding, or one that depends on its font's weight. Its detail is kept as long as the record of the rule can
    // still take a location when it is reported. That is certain not to be the case, whatever the fonts' weights are,
    // (1) once max_locations findings that do not depend on a weight, with different details, come before it: they
    // are reported before it and fill the record; (2) for a finding that depends on the weight of its font, once
    // max_locations findings of that font in the 14 to 18 pt range, with different details, come before it: if the
    // font is not bold they are all reported and fill the record, if it is bold this one is no finding. "Different
    // details" is judged on the detail without its threshold part: details that differ there differ as reported.
    void finding(const ContentModel& cm, const ContentItem& it, const TextRun& tr, ContrastEvent::Kind kind, double ratio,
                 const float fg[3], const float bg[3]) {
        const bool weighed = kind == ContrastEvent::EitherWay || kind == ContrastEvent::UnlessBold;
        const bool certain = kind != ContrastEvent::UnlessBold;
        if (record_full_) {
            if (certain) event(ContrastEvent::CountCertain, 0);
            else event(ContrastEvent::CountUnlessBold, font_index_.at(tr.font));
            return;
        }
        std::unordered_set<std::size_t>* of_font = weighed ? &by_font_[tr.font] : nullptr;
        if (!certain && of_font->size() >= max_locations) {
            event(ContrastEvent::CountUnlessBold, font_index_.at(tr.font));
            return;
        }
        const StreamInstance& s = cm.streams[static_cast<std::size_t>(it.stream)];
        std::string text = to_utf8(tr.text);
        if (text.size() > 40) text = text.substr(0, 40) + "...";
        char buf[200];
        std::snprintf(buf, sizeof buf, "|%.2f|%s|%s|%.1f|", ratio, hex(fg).c_str(), hex(bg).c_str(), tr.size_user);
        std::string key = std::to_string(page_number(s.page)) + '|' + s.ref.str() + buf + text;   // the detail without its threshold part
        const std::size_t skeleton = std::hash<std::string>()(key);
        key.push_back('\x01');
        key.push_back(static_cast<char>('0' + kind));
        if (weighed) key += std::to_string(font_index_.at(tr.font));   // the same detail when the same font decides
        auto [pos, fresh] = detail_index_.emplace(std::move(key), static_cast<std::uint32_t>(details.size()));
        if (fresh) {
            ContrastDetail d;
            d.stream = it.stream;
            d.font = weighed ? font_index_.at(tr.font) : 0;
            d.ratio = ratio;
            d.size = tr.size_user;
            for (int k = 0; k < 3; ++k) d.fg[k] = fg[k], d.bg[k] = bg[k];
            d.text = std::move(text);
            details.push_back(std::move(d));
        }
        event(kind, pos->second);
        if (certain) certain_.insert(skeleton);
        if (of_font) of_font->insert(skeleton);
        if (certain_.size() >= max_locations) {   // (1): no detail is kept from here on
            record_full_ = true;
            detail_index_ = {};
            certain_ = {};
            by_font_ = {};
        }
    }
};

class Module final : public RuleModule {
public:
    const char* name() const override { return "content"; }

    void prepare(ValidationContext& ctx, std::vector<ContentObserver*>& observers) override {
        observers.push_back(&syntax_);
        tap_.doc = &ctx.doc;
        tap_.max_locations = ctx.findings.max_locations();
        observers.push_back(&tap_);
    }

    void run(ValidationContext& ctx) override {
        tap_.take(ctx.content);   // what no page end delivered (the interpretation stopped inside a page)
        tagging(ctx);
        marked_content_language(ctx);
        xobjects(ctx);
        pua(ctx);
        pdf_syntax(ctx);
        text_contrast(ctx);
    }

private:
    SyntaxObserver syntax_;
    ItemTap tap_;

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
        // (The items were taken page by page as runs of one stream and frame, ItemTap: a run stands for its items.)
        tap_.rethrow_runs();
        int run_stream = -1, run_frame = -2;
        bool run_untagged = false;
        for (const ItemRun& it : tap_.runs) {
            if (skipped(ctx, it.stream)) continue;
            if (it.stream != run_stream || it.frame != run_frame) {
                run_stream = it.stream;
                run_frame = it.frame;
                run_untagged = !tagged(ctx, cm.item_struct_parent(it.stream, it.frame)) && !cm.parents_tags_contain(it.stream, it.frame, "Artifact");
            }
            if (!run_untagged) continue;
            Untagged& u = per_stream[it.stream];
            if (!u.any) u.first_op = it.first_op;
            u.any = true;
            for (int k = 0; k < 5; ++k) u.count[k] += it.count[k];
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
        // The language of a text item is that of its stream and frame: asked once for the text items of a run.
        for (const ItemRun& it : tap_.runs) {
            const std::uint32_t texts = it.count[static_cast<int>(ContentItem::Kind::Text)];
            if (texts == 0 || skipped(ctx, it.stream)) continue;
            if (cm.lang_value(ctx.st, it.stream, it.frame)) continue;
            auto& c = per_stream[it.stream];
            if (c.first == 0) c.second = it.first_text_op;
            c.first += texts;
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
            Obj at = e.get(StructElem::kActualText, "ActualText");
            if (at.isString() && has_pua(at.getStringValue()))
                ctx.report(r8432, -1, e.ref, "ActualText of " + e.S.value_or("?") + " contains private use characters");
            Obj alt = e.get(StructElem::kAlt, "Alt");
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

    // The contrast of every text item was measured when its page ended (ItemTap::take_contrast). Here, in the order
    // of the text items: the weight of a font is asked where the check asks it first, and the findings are reported.
    void text_contrast(ValidationContext& ctx) {
        static const RuleIndex rc = R("EQX-PAC-FailNotEnoughContrastForText");
        const ContentModel& cm = ctx.content;
        std::vector<signed char> bold(tap_.fonts.size(), -1);
        const auto is_bold = [&](std::uint32_t font) {
            if (bold[font] < 0) bold[font] = bold_font(tap_.fonts[font]) ? 1 : 0;
            return bold[font] == 1;
        };
        const auto report = [&](const ContrastDetail& d, bool large) {
            const StreamInstance& s = cm.streams[static_cast<std::size_t>(d.stream)];
            if (ctx.findings.saturated(rc)) {   // only counted: the record is full, the detail would be dropped
                ctx.report(rc, s.page, s.ref);
                return;
            }
            char buf[200];
            std::snprintf(buf, sizeof buf, "contrast %.2f:1 (text %s on %s, %.1f pt%s), needs %.1f:1", d.ratio,
                          hex(d.fg).c_str(), hex(d.bg).c_str(), d.size, large ? ", large" : "", large ? 3.0 : 4.5);
            ctx.report(rc, s.page, s.ref, std::string(buf) + (d.text.empty() ? "" : ": \"" + d.text + "\""));
        };
        // A finding whose detail was not kept: the record is full by now (ItemTap::finding), so it is only counted.
        const auto count = [&] {
            if (!ctx.findings.saturated(rc) && ctx.findings.max_locations() != 0)
                throw std::logic_error("text contrast: a finding without its detail meets a record that is not full");
            ctx.report(rc, -1, ObjRef{});
        };
        for (const ContrastEvent& e : tap_.events) {
            for (std::uint32_t n = 0; n < e.n; ++n) {
                switch (e.kind) {
                    case ContrastEvent::AskBold: (void)is_bold(e.a); break;
                    case ContrastEvent::Large: report(tap_.details[e.a], true); break;
                    case ContrastEvent::Small: report(tap_.details[e.a], false); break;
                    case ContrastEvent::EitherWay: report(tap_.details[e.a], is_bold(tap_.details[e.a].font)); break;
                    case ContrastEvent::UnlessBold:
                        if (!is_bold(tap_.details[e.a].font)) report(tap_.details[e.a], false);
                        break;
                    case ContrastEvent::CountCertain: count(); break;
                    case ContrastEvent::CountUnlessBold:
                        if (!is_bold(e.a)) count();
                        break;
                }
            }
        }
        tap_.rethrow_contrast();
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_content_module() { return std::make_unique<Module>(); }

}  // namespace eqx
