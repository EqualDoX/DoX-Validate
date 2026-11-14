// EqualDoX's own WCAG 2.x colour checks (catalogue family wcag.*, tool "EqualDoX" in catalog/rules.json):
//
//   EQX-WCAG-1.4.3-TextContrast            text below 4.5:1 (3:1 for large text), measured on exact colours, no exception in reach
//   EQX-WCAG-1.4.3-TextContrastReview      the same measurement where something can change the verdict: manual review
//   EQX-WCAG-1.4.11-NonTextContrastReview  graphical parts of figures, form-field boundaries below 3:1: manual review
//   EQX-WCAG-1.4.1-UseOfColorReview        text set apart from the adjacent text by colour alone: manual review
//
// Text and the graphics of figures are measured by the contrast engine (pac_contrast.cpp) on the paint records and the rendering it
// makes for PAC's text contrast anyway; form fields and colour-only text need neither and run after the interpretation. Only the
// first rule is a violation: an uncertain measurement or an exception that may apply is never reported as one.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rules/module.h"

namespace eqx::wcag {

// What the contrast engine measured for one text run (or one of PAC's text pieces on a rendered background).
struct TextFacts {
    double ratio = 21;                  // text colour, its own transparency composed, against the background
    double size = 0;                    // effective size in pt
    int bold = 0;                       // bold_state
    unsigned char fg[3] = {0, 0, 0}, bg[3] = {255, 255, 255};
    bool rendered = false;              // background from the rendering (image, gradient, overlapping paint)
    bool device_colour = false;         // CMYK, spot or other colour whose appearance depends on the output device
    bool transparency = false;          // soft mask of unknown value or a blend mode
    bool clipped = false;               // the clip may hide part of the text
    bool stroked = false;               // outline or fill-and-stroke rendering mode
    bool artifact = false;              // marked as artifact: exempt if pure decoration
    bool in_figure = false;             // inside a Figure: exempt if part of a picture or a logotype
};

enum class Verdict : std::uint8_t { Pass, Violation, Review };

struct TextJudgement {
    Verdict verdict = Verdict::Pass;
    std::string required;               // the threshold in words
    std::string reasons;                // what a reviewer has to decide; empty for a violation
};

TextJudgement judge_text(const TextFacts& f);
// 1 bold, 0 not bold, -1 cannot tell (semibold, fill and stroke, a descriptor that contradicts the name).
int bold_state(const Font* font, int render_mode);
std::string text_detail(const TextFacts& f, const TextJudgement& j, const Rect& box, const std::string& text);
std::string box_str(const Rect& r);
std::string hex(const unsigned char* c);
double contrast8(const unsigned char* a, const unsigned char* b);
// The linear value of an sRGB component for the relative luminance of WCAG 2.x (c <= 0.03928 ? c / 12.92 :
// ((c + 0.055) / 1.055) ^ 2.4, computed in double on the float's value), with the results of recent components kept.
double srgb_channel(float c);
std::string ratio_str(double r);   // two decimals, cut (never rounded up to a threshold)

// std::sort(rs.begin(), rs.end(), before) with before(a, b) = a.base != b.base ? a.base < b.base : a.x0 < b.x0, the order
// of a page's text runs for the WCAG 1.4.1 check, mostly without the sort. The runs of a page come in content order: line
// after line down the page, each line left to right, column after column. By (base, x0) a column is its lines in reverse
// order. So the runs are cut into stretches that strictly ascend (the lines); stretches of which each lies wholly below
// the one before it form a block (a column), which is in order once its stretches are reversed; the few blocks are
// merged. If the result strictly ascends, every key differs from every other, there is only one sorted order, and this
// is it: what std::sort gives. Anything else (equal keys, a NaN coordinate, for which the comparison is no ordering,
// more than a few blocks) is sorted by std::sort as before. `scratch` is working space.
template <class Run>
void sort_by_base_x0(std::vector<Run>& rs, std::vector<Run>& scratch) {
    const auto before = [](const Run& a, const Run& b) { return a.base != b.base ? a.base < b.base : a.x0 < b.x0; };
    const auto sort_all = [&] { std::sort(rs.begin(), rs.end(), before); };
    constexpr std::size_t kMaxBlocks = 8;
    const std::size_t n = rs.size();
    if (n < 2) return;
    std::vector<std::size_t> starts{0};   // where each stretch starts
    for (std::size_t i = 0; i < n; ++i) {
        if (rs[i].base != rs[i].base || rs[i].x0 != rs[i].x0) return sort_all();   // NaN
        if (i == 0 || before(rs[i - 1], rs[i])) continue;                            // the stretch goes on
        if (!before(rs[i], rs[i - 1])) return sort_all();                           // equal keys
        starts.push_back(i);
    }
    if (starts.size() == 1) return;   // one stretch: in order already
    const auto stretch_end = [&](std::size_t t) { return t + 1 < starts.size() ? starts[t + 1] : n; };
    scratch.clear();
    scratch.reserve(n);
    std::vector<std::size_t> block_ends;   // in scratch
    for (std::size_t j = 0; j < starts.size();) {
        std::size_t k = j;   // the block is the stretches j .. k: the largest key of each (its last) below the smallest of the one before
        while (k + 1 < starts.size() && before(rs[stretch_end(k + 1) - 1], rs[starts[k]])) ++k;
        if (block_ends.size() == kMaxBlocks) return sort_all();
        for (std::size_t t = k + 1; t-- > j;)   // its stretches from the last to the first, each in its own order
            scratch.insert(scratch.end(), rs.begin() + std::ptrdiff_t(starts[t]), rs.begin() + std::ptrdiff_t(stretch_end(t)));
        block_ends.push_back(scratch.size());
        j = k + 1;
    }
    for (std::size_t b = 1; b < block_ends.size(); ++b)
        std::inplace_merge(scratch.begin(), scratch.begin() + std::ptrdiff_t(block_ends[b - 1]), scratch.begin() + std::ptrdiff_t(block_ends[b]), before);
    for (std::size_t i = 1; i < n; ++i)
        if (!before(scratch[i - 1], scratch[i])) return sort_all();   // equal keys in different blocks
    rs.swap(scratch);
}

// The keys alone, for the unit test of sort_by_base_x0.
struct RunKey {
    double base = 0, x0 = 0;
    int id = 0;
};
inline bool run_key_before(const RunKey& a, const RunKey& b) { return a.base != b.base ? a.base < b.base : a.x0 < b.x0; }
inline void sort_run_keys(std::vector<RunKey>& keys) {
    std::vector<RunKey> scratch;
    sort_by_base_x0(keys, scratch);
}

// What the two checks below need of the content items. It is taken page by page, when a page's content stream has
// ended, so that the content model need not keep the items until the checks run: the painted items of the pages that
// may hold a form field, and, for the use-of-colour check, the phrases it would report if no link frame decides
// otherwise. Only the content model is read (and annotation dictionaries the interpreter has read already); the
// PDF's objects are read and the findings reported by the checks, as before.
class ContentColours {
public:
    ContentColours();
    ~ContentColours();
    // Takes the items [begin, end) of `cm`: those of one page in one call, in the order of the items.
    void take(const ContentModel& cm, const Document& doc, std::size_t begin, std::size_t end);

    struct State;
    std::unique_ptr<State> state;
};

// Run after the interpretation (no rendering), on what was taken of the whole content.
void check_form_fields(ValidationContext& ctx, const ContentColours& taken);
void check_use_of_color(ValidationContext& ctx, const ContentColours& taken);

}  // namespace eqx::wcag
