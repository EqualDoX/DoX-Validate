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

// Text in an /Artifact sequence (PAC's text pieces, see pac_contrast.cpp).
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
const std::vector<ContrastFinding>& contrast_findings(const ContentObserver& observer);
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
