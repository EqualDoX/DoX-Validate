// PAC 26.1 text contrast check (NotEnoughContrastForText; WCAG 2.x 1.4.3).
//
// Text is judged per PAC's text piece against the finished page (later paint included). The simple cases need no rendering (nothing
// painted under the text, or one exact opaque rectangle covering it); everything else (images, gradients, partial or
// non-rectangular paint) comes from a small software rendering. PAC renders too: it decodes images of every filter, and a glyph
// is judged on the colour prevailing in its box (measured with single-condition PAC runs).
//
// Memory stays low by construction: page geometry is flattened into float arrays and dropped at the end of the page,
// rendering happens only on pages that have such a text run, one canvas per page at about 1 px/pt, images are
// reduced while they are decoded.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rules/module.h"

namespace eqx::pacc {

struct ContrastFinding {
    int page = -1;
    ObjRef obj;
    std::string detail;
};

// One text piece below the contrast PAC asks for: what was measured, kept as numbers. The detail text of the finding is
// made from them when the finding is reported (detail), and not at all for a finding that is only counted.
struct ContrastMeasure {
    int page = -1;
    ObjRef obj;
    double ratio = 0;                   // the contrast measured
    double size_user = 0;               // the text size in pt
    double need = 0;                    // the contrast asked for (3.0 for large text, else 4.5)
    // The text of the run as the detail shows it (contrast_text).
    static constexpr std::uint32_t kNoText = 0xffffffffu;
    std::uint32_t text = kNoText;
    unsigned char fg[3] = {0, 0, 0}, bg[3] = {0, 0, 0};
    bool rendered = false;              // the background was measured on the rendering
    bool large = false;
    std::string detail(const std::string& run_text) const;
};

// True when the box lies entirely inside the top or the bottom 14.8 % of the page as displayed (/Rotate).
// Evidence (boxes of -0.2 .. 0.8 em, 11 pt text; PAC runs of the a_*, b_*, d_* probes): A4 top: box bottom 124.2 pt not
// flagged, 126.2 pt flagged; A4 bottom: box top 120.8 pt not flagged, 124.8 pt flagged; landscape (595 pt) bottom: box top
// 88.8 pt flagged. All fit one share of the page height between 14.75 % and 14.82 %.
inline bool in_margin_band(const Page& pg, const Rect& b) {
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

// Text in an /Artifact sequence (PAC's text pieces, see pac_contrast.cpp) that lies in the page body: on a page of the
// document, not entirely inside a margin band, inside the crop box (artifact_in_body). Only those are kept: the
// pieces of running headers and footers, most of all artifact text, are never reported.
struct ArtifactTextPiece {
    int page = -1;
    ObjRef obj;
    Rect box;
    std::string text;
};

class ContrastEngine;

// Observer owned by the pac_quality module.
std::unique_ptr<ContentObserver> make_contrast_observer(ValidationContext& ctx);
// The findings gathered during interpretation (the observer made by make_contrast_observer).
const std::vector<ContrastMeasure>& contrast_findings(const ContentObserver& observer);
// The text for the detail of one of them; std::logic_error when it was not kept (ContrastMeasure::text).
const std::string& contrast_text(const ContentObserver& observer, const ContrastMeasure& m);
// Findings after those of contrast_findings that are only counted: the record of the rule is full when they are
// reported (after the others), so nothing of them but their number is kept.
std::uint64_t contrast_findings_counted(const ContentObserver& observer);
// Whether an artifact text piece with this box on this page (0-based) is in the page body.
bool artifact_in_body(const std::vector<Page>& pages, int page, const Rect& box);
const std::vector<ArtifactTextPiece>& artifact_text_pieces(const ContentObserver& observer);
// Operators naming a resource that does not exist.
const std::vector<ContrastFinding>& missing_resources(const ContentObserver& observer);
// A page exceeded the budgets and was not judged.
bool contrast_gave_up(const ContentObserver& observer);

// Pure helpers (unit-tested).
double wcag_contrast(const float a[3], const float b[3]);

// (sum + count / 2) / count, the rounded mean of `count` samples (count >= 1): what the image reduction stores per
// reduced pixel and channel. For the sums and counts that occur there (count <= 255, sum below 2^16) one multiplication
// by ceil(2^32 / count) replaces the division: with s = sum + count / 2 < 2^17 and e = count * ceil(2^32 / count) - 2^32
// < count, floor(s * ceil(2^32 / count) / 2^32) = floor(s / count) because s * e < 2^32.
inline unsigned rounded_mean(unsigned sum, unsigned count) {
    static constexpr auto reciprocal = [] {
        std::array<std::uint64_t, 256> t{};
        for (unsigned c = 1; c < 256; ++c) t[c] = ((std::uint64_t(1) << 32) + c - 1) / c;
        return t;
    }();
    if (count >= 256 || sum >= (1u << 16)) return (sum + count / 2) / count;
    return static_cast<unsigned>(((std::uint64_t(sum) + count / 2) * reciprocal[count]) >> 32);
}

// Reduced image: rgb (3 bytes per pixel, row 0 = top of the image), optional alpha.
struct ReducedImage {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    std::vector<unsigned char> alpha;
    bool stencil = false;    // alpha only (image mask): painted with the current fill colour
};
// Decodes an image XObject to at most max_side pixels per side, streaming (never holds the full image).
// False when it cannot be decoded (JBIG2Decode, damaged data).
bool reduce_image(const Obj& image, int max_side, ReducedImage& out, std::string* why = nullptr);

}  // namespace eqx::pacc
