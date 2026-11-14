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

#include <cstdint>
#include <string>

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

// Run after the interpretation (no rendering).
void check_form_fields(ValidationContext& ctx);
void check_use_of_color(ValidationContext& ctx);

}  // namespace eqx::wcag
