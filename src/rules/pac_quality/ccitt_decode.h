// Bounded CCITT G3/G4 decoder (ISO 32000-1 7.4.6, ITU-T T.4, T.6) for the contrast engine: rows are handed over one at a time and never kept,
// so memory stays two arrays of changing elements per image.
#pragma once

#include <cstddef>
#include <functional>

namespace eqx::ccitt {

struct Params {
    int k = 0;                  // < 0: T.6; 0: T.4 1-D; > 0: T.4 mixed
    int columns = 1728;
    int rows = 0;               // upper bound of the rows produced (the image height); 0: until the data ends
    bool black_is_1 = false;
    bool byte_align = false;
    bool end_of_block = true;
};

// `row` = packed 1 bit per pixel, MSB first, polarity as BlackIs1 says. Returns the number of rows decoded (damage ends the decoding: what came before stays valid).
int decode(const unsigned char* data, std::size_t size, const Params& p, const std::function<void(const unsigned char* row)>& row);

}  // namespace eqx::ccitt
