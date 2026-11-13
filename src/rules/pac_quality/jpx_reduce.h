// Minimal JPEG 2000 Part 1 decoder (ISO/IEC 15444-1) for background sampling: it decodes only the resolution levels
// needed for a small image, so a page-sized JPX image costs little time and memory. Not a general image codec:
// code-block styles BYPASS, TERMALL and vertically causal context, POC, PPM, PPT and RGN are refused (the caller treats the
// image as undecodable). Input is untrusted: every length and index is checked.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eqx::jpx {

struct Image {
    int width = 0, height = 0;      // reduced size in pixels
    int components = 0;             // interleaved channels after palette expansion
    std::vector<std::uint8_t> samples;   // width * height * components, 8 bits per sample, row 0 = top
    // From the JP2 boxes (0 when absent): enumerated colour space (16 sRGB, 17 grey, 18 sYCC, 12 CMYK) and the channel
    // that holds opacity (cdef type 1), -1 when none.
    int enum_cs = 0;
    int alpha_channel = -1;
};

// Decodes a JP2 file or a raw codestream; the larger side of the result is at most max_side when the file has enough
// decomposition levels. False when it cannot be decoded (`why` tells).
bool decode_reduced(const std::uint8_t* data, std::size_t size, int max_side, Image& out, std::string* why = nullptr);

}  // namespace eqx::jpx
