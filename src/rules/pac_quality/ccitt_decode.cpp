#include "rules/pac_quality/ccitt_decode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace eqx::ccitt {

namespace {

struct Code {
    std::uint16_t bits;
    std::uint8_t len;
    std::int16_t run;
};

// ITU-T T.4 tables 2, 3a and 3b
const Code kWhite[] = {
    {0x35, 8, 0},   {0x7, 6, 1},    {0x7, 4, 2},    {0x8, 4, 3},    {0xB, 4, 4},    {0xC, 4, 5},    {0xE, 4, 6},    {0xF, 4, 7},    {0x13, 5, 8},   {0x14, 5, 9},
    {0x7, 5, 10},   {0x8, 5, 11},   {0x8, 6, 12},   {0x3, 6, 13},   {0x34, 6, 14},  {0x35, 6, 15},  {0x2A, 6, 16},  {0x2B, 6, 17},  {0x27, 7, 18},  {0xC, 7, 19},
    {0x8, 7, 20},   {0x17, 7, 21},  {0x3, 7, 22},   {0x4, 7, 23},   {0x28, 7, 24},  {0x2B, 7, 25},  {0x13, 7, 26},  {0x24, 7, 27},  {0x18, 7, 28},  {0x2, 8, 29},
    {0x3, 8, 30},   {0x1A, 8, 31},  {0x1B, 8, 32},  {0x12, 8, 33},  {0x13, 8, 34},  {0x14, 8, 35},  {0x15, 8, 36},  {0x16, 8, 37},  {0x17, 8, 38},  {0x28, 8, 39},
    {0x29, 8, 40},  {0x2A, 8, 41},  {0x2B, 8, 42},  {0x2C, 8, 43},  {0x2D, 8, 44},  {0x4, 8, 45},   {0x5, 8, 46},   {0xA, 8, 47},   {0xB, 8, 48},   {0x52, 8, 49},
    {0x53, 8, 50},  {0x54, 8, 51},  {0x55, 8, 52},  {0x24, 8, 53},  {0x25, 8, 54},  {0x58, 8, 55},  {0x59, 8, 56},  {0x5A, 8, 57},  {0x5B, 8, 58},  {0x4A, 8, 59},
    {0x4B, 8, 60},  {0x32, 8, 61},  {0x33, 8, 62},  {0x34, 8, 63},
    {0x1B, 5, 64},  {0x12, 5, 128}, {0x17, 6, 192}, {0x37, 7, 256}, {0x36, 8, 320}, {0x37, 8, 384}, {0x64, 8, 448}, {0x65, 8, 512}, {0x68, 8, 576}, {0x67, 8, 640},
    {0xCC, 9, 704}, {0xCD, 9, 768}, {0xD2, 9, 832}, {0xD3, 9, 896}, {0xD4, 9, 960}, {0xD5, 9, 1024}, {0xD6, 9, 1088}, {0xD7, 9, 1152}, {0xD8, 9, 1216}, {0xD9, 9, 1280},
    {0xDA, 9, 1344}, {0xDB, 9, 1408}, {0x98, 9, 1472}, {0x99, 9, 1536}, {0x9A, 9, 1600}, {0x18, 6, 1664}, {0x9B, 9, 1728},
};
const Code kBlack[] = {
    {0x37, 10, 0},  {0x2, 3, 1},    {0x3, 2, 2},    {0x2, 2, 3},    {0x3, 3, 4},    {0x3, 4, 5},    {0x2, 4, 6},    {0x3, 5, 7},    {0x5, 6, 8},    {0x4, 6, 9},
    {0x4, 7, 10},   {0x5, 7, 11},   {0x7, 7, 12},   {0x4, 8, 13},   {0x7, 8, 14},   {0x18, 9, 15},  {0x17, 10, 16}, {0x18, 10, 17}, {0x8, 10, 18},  {0x67, 11, 19},
    {0x68, 11, 20}, {0x6C, 11, 21}, {0x37, 11, 22}, {0x28, 11, 23}, {0x17, 11, 24}, {0x18, 11, 25}, {0xCA, 12, 26}, {0xCB, 12, 27}, {0xCC, 12, 28}, {0xCD, 12, 29},
    {0x68, 12, 30}, {0x69, 12, 31}, {0x6A, 12, 32}, {0x6B, 12, 33}, {0xD2, 12, 34}, {0xD3, 12, 35}, {0xD4, 12, 36}, {0xD5, 12, 37}, {0xD6, 12, 38}, {0xD7, 12, 39},
    {0x6C, 12, 40}, {0x6D, 12, 41}, {0xDA, 12, 42}, {0xDB, 12, 43}, {0x54, 12, 44}, {0x55, 12, 45}, {0x56, 12, 46}, {0x57, 12, 47}, {0x64, 12, 48}, {0x65, 12, 49},
    {0x52, 12, 50}, {0x53, 12, 51}, {0x24, 12, 52}, {0x37, 12, 53}, {0x38, 12, 54}, {0x27, 12, 55}, {0x28, 12, 56}, {0x58, 12, 57}, {0x59, 12, 58}, {0x2B, 12, 59},
    {0x2C, 12, 60}, {0x5A, 12, 61}, {0x66, 12, 62}, {0x67, 12, 63},
    {0xF, 10, 64},  {0xC8, 12, 128}, {0xC9, 12, 192}, {0x5B, 12, 256}, {0x33, 12, 320}, {0x34, 12, 384}, {0x35, 12, 448}, {0x6C, 13, 512}, {0x6D, 13, 576}, {0x4A, 13, 640},
    {0x4B, 13, 704}, {0x4C, 13, 768}, {0x4D, 13, 832}, {0x72, 13, 896}, {0x73, 13, 960}, {0x74, 13, 1024}, {0x75, 13, 1088}, {0x76, 13, 1152}, {0x77, 13, 1216}, {0x52, 13, 1280},
    {0x53, 13, 1344}, {0x54, 13, 1408}, {0x55, 13, 1472}, {0x5A, 13, 1536}, {0x5B, 13, 1600}, {0x64, 13, 1664}, {0x65, 13, 1728},
};
const Code kExt[] = {
    {0x8, 11, 1792}, {0xC, 11, 1856}, {0xD, 11, 1920}, {0x12, 12, 1984}, {0x13, 12, 2048}, {0x14, 12, 2112}, {0x15, 12, 2176}, {0x16, 12, 2240}, {0x17, 12, 2304}, {0x1C, 12, 2368},
    {0x1D, 12, 2432}, {0x1E, 12, 2496}, {0x1F, 12, 2560},
};

// run lookup by the next 13 bits
struct Lut {
    std::vector<std::int16_t> run;
    std::vector<std::uint8_t> len;
    template <std::size_t N>
    explicit Lut(const Code (&tab)[N]) : run(1 << 13, -1), len(1 << 13, 0) {
        auto add = [&](const Code& c) {
            const int shift = 13 - c.len;
            for (int i = 0; i < (1 << shift); ++i) {
                const int at = (c.bits << shift) | i;
                run[std::size_t(at)] = c.run;
                len[std::size_t(at)] = c.len;
            }
        };
        for (const Code& c : tab) add(c);
        for (const Code& c : kExt) add(c);
    }
};

class Bits {
public:
    Bits(const unsigned char* d, std::size_t n) : d_(d), total_(n * 8) {}
    std::uint32_t peek(int n) const {
        std::uint32_t v = 0;
        std::size_t p = pos_;
        for (int i = 0; i < n; ++i, ++p) {
            v <<= 1;
            if (p < total_) v |= (d_[p >> 3] >> (7 - (p & 7))) & 1;
        }
        return v;
    }
    void skip(int n) { pos_ += std::size_t(n); }
    bool at_end() const { return pos_ >= total_; }
    void align() { pos_ = (pos_ + 7) & ~std::size_t(7); }

private:
    const unsigned char* d_;
    std::size_t total_, pos_ = 0;
};

const Lut& white_lut() {
    static const Lut l(kWhite);
    return l;
}
const Lut& black_lut() {
    static const Lut l(kBlack);
    return l;
}

// A whole run (make-up codes, then a terminating code); -1 on a bad code.
int read_run(Bits& b, bool black) {
    const Lut& lut = black ? black_lut() : white_lut();
    int total = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const std::uint32_t v = b.peek(13);
        const int r = lut.run[v];
        if (r < 0) return -1;
        b.skip(lut.len[v]);
        total += r;
        if (r < 64) return total;
    }
    return -1;
}

// Skips fill and one EOL; true when there was one.
bool skip_eol(Bits& b) {
    int zeros = 0;
    Bits t = b;
    while (!t.at_end() && t.peek(1) == 0 && zeros < 1 << 16) {
        t.skip(1);
        ++zeros;
    }
    if (zeros < 11 || t.at_end()) return false;
    t.skip(1);
    b = t;
    return true;
}

}  // namespace

int decode(const unsigned char* data, std::size_t size, const Params& p, const std::function<void(const unsigned char* row)>& row) {
    const int W = p.columns;
    if (W < 1 || W > 1 << 20) return 0;
    Bits b(data, size);
    std::vector<int> ref{W, W}, cur;
    std::vector<unsigned char> out((std::size_t(W) + 7) / 8);
    int y = 0;
    const int max_rows = p.rows > 0 ? p.rows : 1 << 24;
    while (y < max_rows && !b.at_end()) {
        if (p.byte_align) b.align();
        bool two_d = p.k < 0;
        if (p.k >= 0) {
            int eols = 0;
            while (skip_eol(b)) {
                ++eols;
                if (p.k > 0) {
                    two_d = b.peek(1) == 0;
                    b.skip(1);
                }
                if (eols >= 6) return y;
            }
        } else if (b.peek(24) == 0x001001u) {
            return y;   // EOFB
        }
        cur.clear();
        bool black = false, bad = false;
        if (!two_d) {
            int at = 0;
            while (at < W) {
                const int r = read_run(b, black);
                if (r < 0) {
                    bad = true;
                    break;
                }
                at = std::min(W, at + r);
                cur.push_back(at);
                black = !black;
            }
        } else {
            int a0 = -1;
            std::size_t bi = 0;
            while (a0 < W) {
                // b1: first changing element right of a0 of the colour opposite to the current one (even entries: white to black)
                bi = black ? 1 : 0;
                while (bi < ref.size() && ref[bi] <= a0) bi += 2;
                const int b1 = bi < ref.size() ? ref[bi] : W;
                const int b2 = bi + 1 < ref.size() ? ref[bi + 1] : W;
                int d = 100;
                if (b.peek(1) == 1) {
                    b.skip(1), d = 0;
                } else {
                    const std::uint32_t v = b.peek(7);
                    if ((v >> 4) == 0x3) b.skip(3), d = 1;
                    else if ((v >> 4) == 0x2) b.skip(3), d = -1;
                    else if ((v >> 4) == 0x1) {   // horizontal
                        b.skip(3);
                        const int r1 = read_run(b, black);
                        const int r2 = r1 < 0 ? -1 : read_run(b, !black);
                        if (r2 < 0) {
                            bad = true;
                            break;
                        }
                        const int s = std::max(a0, 0);
                        const int c1 = std::min(W, s + r1), c2 = std::min(W, c1 + r2);
                        cur.push_back(c1);
                        cur.push_back(c2);
                        a0 = c2;
                        if (a0 >= W) break;
                        continue;
                    } else if ((v >> 3) == 0x1) {   // pass
                        b.skip(4);
                        a0 = b2;
                        continue;
                    } else if ((v >> 1) == 0x3) b.skip(6), d = 2;
                    else if ((v >> 1) == 0x2) b.skip(6), d = -2;
                    else if (v == 0x3) b.skip(7), d = 3;
                    else if (v == 0x2) b.skip(7), d = -3;
                    else {
                        bad = true;
                        break;
                    }
                }
                const int a1 = std::clamp(b1 + d, std::max(a0, 0), W);
                cur.push_back(a1);
                a0 = a1;
                black = !black;
                if (a1 >= W) break;
            }
        }
        if (bad && cur.empty()) return y;
        // the row: runs alternate white, black, ...
        const unsigned char white = p.black_is_1 ? 0x00 : 0xFF;
        std::fill(out.begin(), out.end(), white);
        for (std::size_t i = 0; i < cur.size(); i += 2) {
            const int from = cur[i];
            const int to = i + 1 < cur.size() ? cur[i + 1] : W;
            for (int x = from; x < to && x < W; ++x) {
                if ((x & 7) == 0 && x + 8 <= to) {
                    out[std::size_t(x >> 3)] = static_cast<unsigned char>(~white);
                    x += 7;
                } else {
                    out[std::size_t(x >> 3)] ^= static_cast<unsigned char>(0x80 >> (x & 7));
                }
            }
        }
        row(out.data());
        ++y;
        if (bad) return y;
        cur.push_back(W);
        cur.push_back(W);
        ref.swap(cur);
    }
    return y;
}

}  // namespace eqx::ccitt
