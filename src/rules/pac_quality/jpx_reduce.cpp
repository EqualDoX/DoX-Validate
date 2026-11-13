// Portions of this file (the MQ arithmetic decoder and the code-block decoder with its context-label tables,
// between the markers "[Apache-2.0 portion" and "end of Apache-2.0 portion]") are a C++ translation of pdf.js
// (https://github.com/mozilla/pdf.js, v4.0.379: src/core/arithmetic_decoder.js and the BitModel class of
// src/core/jpx.js), Copyright 2012 Mozilla Foundation, licensed under the Apache License, Version 2.0
// (LICENSES/Apache-2.0.txt). They were modified: translated to C++ and adapted to this decoder. The rest of the
// file is EqualDoX's own work.
#include "rules/pac_quality/jpx_reduce.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace eqx::jpx {

namespace {

using u8 = std::uint8_t;

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr std::size_t kMaxCodeBlocks = 1'000'000;
constexpr std::size_t kMaxPrecincts = 2'000'000;
constexpr std::size_t kMaxPixels = 16'000'000;   // samples of the reduced image

inline int ceil_div(std::int64_t a, std::int64_t b) { return int((a + b - 1) / b); }
inline int ceil_shift(std::int64_t a, int s) { return int((a + (std::int64_t(1) << s) - 1) >> s); }
inline int floor_shift(std::int64_t a, int s) { return int(a >> s); }

// --- bytes ------------------------------------------------------------------------------------------------------

struct Reader {
    const u8* p;
    std::size_t n;
    std::size_t at = 0;
    bool has(std::size_t k) const { return at <= n && n - at >= k; }
    unsigned u8v() {
        if (!has(1)) throw Bad("truncated");
        return p[at++];
    }
    unsigned u16() {
        if (!has(2)) throw Bad("truncated");
        unsigned v = (unsigned(p[at]) << 8) | p[at + 1];
        at += 2;
        return v;
    }
    std::uint32_t u32() {
        if (!has(4)) throw Bad("truncated");
        std::uint32_t v = (std::uint32_t(p[at]) << 24) | (std::uint32_t(p[at + 1]) << 16) | (std::uint32_t(p[at + 2]) << 8) | p[at + 3];
        at += 4;
        return v;
    }
};

// --- parameters ---------------------------------------------------------------------------------------------------

struct CompSiz {
    int prec = 8;
    bool sgn = false;
    int dx = 1, dy = 1;
};

struct Coding {
    int levels = 0;
    int xcb = 6, ycb = 6;
    int style = 0;
    bool reversible = true;
    std::vector<std::pair<int, int>> precincts;   // per resolution (PPx, PPy); empty = maximal
    int prec_x(int r) const { return r < int(precincts.size()) ? precincts[std::size_t(r)].first : 15; }
    int prec_y(int r) const { return r < int(precincts.size()) ? precincts[std::size_t(r)].second : 15; }
};

struct Quant {
    int style = 0;      // 0 none, 1 scalar derived, 2 scalar expounded
    int guard = 0;
    std::vector<std::pair<int, int>> steps;   // (exponent, mantissa)
};

struct TileParams {
    int progression = 0, layers = 1;
    bool sop = false, eph = false, mct = false;
    std::vector<Coding> coding;
    std::vector<Quant> quant;
    std::vector<char> coc_set, qcc_set;   // set by a COC/QCC of this scope (a main COD must not override)
};

void read_coding_body(Reader& r, Coding& c, bool user_precincts) {
    c.levels = int(r.u8v());
    c.xcb = int(r.u8v()) + 2;
    c.ycb = int(r.u8v()) + 2;
    c.style = int(r.u8v());
    int transform = int(r.u8v());
    c.reversible = transform == 1;
    if (transform > 1) throw Bad("wavelet transform");
    if (c.levels > 32 || c.xcb > 10 || c.ycb > 10 || c.xcb + c.ycb > 12) throw Bad("code block size");
    if (c.style & (0x01 | 0x04 | 0x08)) throw Bad("code block style");
    c.precincts.clear();
    if (user_precincts)
        for (int i = 0; i <= c.levels; ++i) {
            unsigned v = r.u8v();
            c.precincts.push_back({int(v & 15), int(v >> 4)});
        }
}

void read_quant_body(Reader& r, std::size_t end, Quant& q) {
    unsigned s = r.u8v();
    q.style = int(s & 31);
    q.guard = int(s >> 5);
    q.steps.clear();
    if (q.style == 0) {
        while (r.at < end) q.steps.push_back({int(r.u8v() >> 3), 0});
    } else if (q.style == 1 || q.style == 2) {
        while (r.at + 1 < end) {
            unsigned v = r.u16();
            q.steps.push_back({int(v >> 11), int(v & 0x7ff)});
        }
    } else {
        throw Bad("quantization style");
    }
    if (q.steps.empty()) throw Bad("quantization table");
}

// --- bit reader of packet headers -------------------------------------------------------------------------------

class BitReader {
public:
    BitReader(const u8* p, std::size_t n) : p_(p), n_(n) {}
    int bit() {
        if (ct_ == 0) {
            if (at_ >= n_) throw Bad("packet header truncated");
            buf_ = ((buf_ << 8) & 0xffff) | p_[at_++];
            ct_ = (buf_ & 0xff00) == 0xff00 ? 7 : 8;
        }
        --ct_;
        return int((buf_ >> ct_) & 1);
    }
    unsigned bits(int k) {
        unsigned v = 0;
        for (int i = 0; i < k; ++i) v = (v << 1) | unsigned(bit());
        return v;
    }
    // Byte alignment at the end of a packet header (ISO/IEC 15444-1 B.10.1: a final 0xFF is followed by a stuffed byte).
    std::size_t finish() {
        if ((buf_ & 0xff) == 0xff) {
            if (at_ >= n_) throw Bad("packet header truncated");
            ++at_;
        }
        ct_ = 0;
        return at_;
    }

private:
    const u8* p_;
    std::size_t n_, at_ = 0;
    unsigned buf_ = 0;
    int ct_ = 0;
};

// --- tag tree ------------------------------------------------------------------------------------------------------

class TagTree {
public:
    void init(int w, int h) {
        nodes_.clear();
        leaves_w_ = w;
        struct Level {
            int w, h, first;
        };
        std::vector<Level> lv;
        int count = 0, cw = w, ch = h;
        while (true) {
            lv.push_back({cw, ch, count});
            count += cw * ch;
            if (cw <= 1 && ch <= 1) break;
            cw = (cw + 1) / 2;
            ch = (ch + 1) / 2;
        }
        nodes_.resize(std::size_t(count));
        for (std::size_t l = 0; l + 1 < lv.size(); ++l)
            for (int y = 0; y < lv[l].h; ++y)
                for (int x = 0; x < lv[l].w; ++x)
                    nodes_[std::size_t(lv[l].first + y * lv[l].w + x)].parent = lv[l + 1].first + (y / 2) * lv[l + 1].w + x / 2;
    }
    // True when the leaf's value is known to be below the threshold.
    bool decode(BitReader& br, int leaf, int threshold) {
        int path[32];
        int depth = 0;
        for (int n = leaf; n >= 0 && depth < 32; n = nodes_[std::size_t(n)].parent) path[depth++] = n;
        int low = 0;
        for (int i = depth - 1; i >= 0; --i) {
            Node& node = nodes_[std::size_t(path[i])];
            if (low > node.low) node.low = low;
            else low = node.low;
            while (low < threshold && low < node.value) {
                if (br.bit()) node.value = low;
                else ++low;
            }
            node.low = low;
        }
        return nodes_[std::size_t(leaf)].value < threshold;
    }

private:
    struct Node {
        int parent = -1;
        int value = std::numeric_limits<int>::max();
        int low = 0;
    };
    std::vector<Node> nodes_;
    int leaves_w_ = 0;
};

// [Apache-2.0 portion (pdf.js, Copyright 2012 Mozilla Foundation, modified)]
// --- MQ decoder ---------------------------------------------------------------------------------------------------

struct QeEntry {
    std::uint16_t qe;
    std::uint8_t nmps, nlps, sw;
};
const QeEntry kQe[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},  {0x0221, 38, 33, 0},
    {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0}, {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0},
    {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1}, {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0},
    {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0}, {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0},
    {0x1C01, 25, 22, 0}, {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0}, {0x02A1, 36, 33, 0},
    {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0}, {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0},
    {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0}, {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}};

class MQ {
public:
    MQ(const u8* d, std::size_t n) : d_(d), n_(n) {
        chigh_ = byte_at(0);
        clow_ = 0;
        byte_in();
        chigh_ = ((chigh_ << 7) & 0xffff) | ((clow_ >> 9) & 0x7f);
        clow_ = (clow_ << 7) & 0xffff;
        ct_ -= 7;
        a_ = 0x8000;
    }
    int read(std::uint8_t& cx) {
        unsigned idx = cx >> 1, mps = cx & 1;
        const QeEntry& e = kQe[idx];
        const unsigned qe = e.qe;
        unsigned a = a_ - qe;
        int d;
        if (chigh_ < qe) {
            if (a < qe) {
                a = qe;
                d = int(mps);
                idx = e.nmps;
            } else {
                a = qe;
                d = int(1 ^ mps);
                if (e.sw) mps = unsigned(d);
                idx = e.nlps;
            }
        } else {
            chigh_ -= qe;
            if (a & 0x8000) {
                a_ = a;
                return int(mps);
            }
            if (a < qe) {
                d = int(1 ^ mps);
                if (e.sw) mps = unsigned(d);
                idx = e.nlps;
            } else {
                d = int(mps);
                idx = e.nmps;
            }
        }
        do {
            if (ct_ == 0) byte_in();
            a <<= 1;
            chigh_ = ((chigh_ << 1) & 0xffff) | ((clow_ >> 15) & 1);
            clow_ = (clow_ << 1) & 0xffff;
            --ct_;
        } while (!(a & 0x8000));
        a_ = a;
        cx = std::uint8_t((idx << 1) | mps);
        return d;
    }

private:
    unsigned byte_at(std::size_t i) const { return i < n_ ? d_[i] : 0xff; }
    void byte_in() {
        if (byte_at(bp_) == 0xff) {
            if (byte_at(bp_ + 1) > 0x8f) {
                clow_ += 0xff00;
                ct_ = 8;
            } else {
                ++bp_;
                clow_ += byte_at(bp_) << 9;
                ct_ = 7;
            }
        } else {
            ++bp_;
            clow_ += bp_ < n_ ? unsigned(d_[bp_]) << 8 : 0xff00;
            ct_ = 8;
        }
        if (clow_ > 0xffff) {
            chigh_ += clow_ >> 16;
            clow_ &= 0xffff;
        }
    }
    const u8* d_;
    std::size_t n_;
    std::size_t bp_ = 0;
    unsigned chigh_ = 0, clow_ = 0, a_ = 0;
    int ct_ = 0;
};

// --- code block decoding (ISO/IEC 15444-1 Annex D) -------------------------------------------------------------------

const u8 kLabelLLLH[75] = {0, 5, 8, 0, 3, 7, 8, 0, 4, 7, 8, 0, 0, 0, 0, 0, 1, 6, 8, 0, 3, 7, 8, 0, 4, 7, 8, 0, 0, 0, 0, 0, 2, 6, 8, 0, 3, 7, 8,
                           0, 4, 7, 8, 0, 0, 0, 0, 0, 2, 6, 8, 0, 3, 7, 8, 0, 4, 7, 8, 0, 0, 0, 0, 0, 2, 6, 8, 0, 3, 7, 8, 0, 4, 7, 8};
const u8 kLabelHL[75] = {0, 3, 4, 0, 5, 7, 7, 0, 8, 8, 8, 0, 0, 0, 0, 0, 1, 3, 4, 0, 6, 7, 7, 0, 8, 8, 8, 0, 0, 0, 0, 0, 2, 3, 4, 0, 6, 7, 7,
                         0, 8, 8, 8, 0, 0, 0, 0, 0, 2, 3, 4, 0, 6, 7, 7, 0, 8, 8, 8, 0, 0, 0, 0, 0, 2, 3, 4, 0, 6, 7, 7, 0, 8, 8, 8};
const u8 kLabelHH[75] = {0, 1, 2, 0, 1, 2, 2, 0, 2, 2, 2, 0, 0, 0, 0, 0, 3, 4, 5, 0, 4, 5, 5, 0, 5, 5, 5, 0, 0, 0, 0, 0, 6, 7, 7, 0, 7, 7, 7,
                         0, 7, 7, 7, 0, 0, 0, 0, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 0, 0, 0, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8};

class BlockDecoder {
public:
    BlockDecoder(int w, int h, const u8* labels, int zero_bitplanes, MQ& mq)
        : w_(w), h_(h), labels_(labels), mq_(mq) {
        const std::size_t n = std::size_t(w) * std::size_t(h);
        nsig_.assign(n, 0);
        flags_.assign(n, 0);
        sign_.assign(n, 0);
        mag_.assign(n, 0);
        bits_.assign(n, std::uint8_t(std::min(zero_bitplanes, 255)));
        reset();
    }
    void reset() {
        cx_.fill(0);
        cx_[0] = (4 << 1);
        cx_[17] = (46 << 1);
        cx_[18] = (3 << 1);
    }
    void significance() {
        for (int i0 = 0; i0 < h_; i0 += 4)
            for (int j = 0; j < w_; ++j) {
                std::size_t index = std::size_t(i0) * std::size_t(w_) + std::size_t(j);
                for (int i1 = 0; i1 < 4 && i0 + i1 < h_; ++i1, index += std::size_t(w_)) {
                    flags_[index] &= u8(~1);
                    if (mag_[index] || !nsig_[index]) continue;
                    if (mq_.read(cx_[labels_[nsig_[index] & 127]])) become_significant(i0 + i1, j, index);
                    ++bits_[index];
                    flags_[index] |= 1;
                }
            }
    }
    void refinement() {
        for (int i0 = 0; i0 < h_; i0 += 4)
            for (int j = 0; j < w_; ++j) {
                std::size_t index = std::size_t(i0) * std::size_t(w_) + std::size_t(j);
                for (int i1 = 0; i1 < 4 && i0 + i1 < h_; ++i1, index += std::size_t(w_)) {
                    if (!mag_[index] || (flags_[index] & 1)) continue;
                    int label = 16;
                    if (flags_[index] & 2) {
                        flags_[index] ^= 2;
                        label = (nsig_[index] & 127) == 0 ? 14 : 15;
                    }
                    mag_[index] = (mag_[index] << 1) | unsigned(mq_.read(cx_[std::size_t(label)]));
                    ++bits_[index];
                    flags_[index] |= 1;
                }
            }
    }
    void cleanup(bool segmentation_symbols) {
        const std::size_t w = std::size_t(w_);
        for (int i0 = 0; i0 < h_; i0 += 4) {
            const int next = std::min(i0 + 4, h_);
            for (int j = 0; j < w_; ++j) {
                const std::size_t index0 = std::size_t(i0) * w + std::size_t(j);
                const bool all_empty = i0 + 3 < h_ && !flags_[index0] && !flags_[index0 + w] && !flags_[index0 + 2 * w] && !flags_[index0 + 3 * w] &&
                                       !nsig_[index0] && !nsig_[index0 + w] && !nsig_[index0 + 2 * w] && !nsig_[index0 + 3 * w];
                int i1 = 0;
                std::size_t index = index0;
                if (all_empty) {
                    if (!mq_.read(cx_[18])) {
                        ++bits_[index0], ++bits_[index0 + w], ++bits_[index0 + 2 * w], ++bits_[index0 + 3 * w];
                        continue;
                    }
                    i1 = (mq_.read(cx_[17]) << 1);
                    i1 |= mq_.read(cx_[17]);
                    const int i = i0 + i1;
                    index += std::size_t(i1) * w;
                    become_significant(i, j, index);
                    index = index0;
                    for (int i2 = i0; i2 <= i; ++i2, index += w) ++bits_[index];
                    ++i1;
                }
                for (int i = i0 + i1; i < next; ++i, index += w) {
                    if (mag_[index] || (flags_[index] & 1)) continue;
                    if (mq_.read(cx_[labels_[nsig_[index] & 127]])) become_significant(i, j, index);
                    ++bits_[index];
                }
            }
        }
        if (segmentation_symbols)
            for (int k = 0; k < 4; ++k) mq_.read(cx_[17]);
    }
    unsigned mag(std::size_t i) const { return mag_[i]; }
    int bits(std::size_t i) const { return bits_[i]; }
    bool negative(std::size_t i) const { return sign_[i] != 0; }

private:
    void become_significant(int row, int col, std::size_t index) {
        sign_[index] = decode_sign(row, col, index);
        mag_[index] = 1;
        const std::size_t w = std::size_t(w_);
        const bool left = col > 0, right = col + 1 < w_;
        if (row > 0) {
            std::size_t i = index - w;
            if (left) nsig_[i - 1] = u8(nsig_[i - 1] + 0x10);
            if (right) nsig_[i + 1] = u8(nsig_[i + 1] + 0x10);
            nsig_[i] = u8(nsig_[i] + 0x04);
        }
        if (row + 1 < h_) {
            std::size_t i = index + w;
            if (left) nsig_[i - 1] = u8(nsig_[i - 1] + 0x10);
            if (right) nsig_[i + 1] = u8(nsig_[i + 1] + 0x10);
            nsig_[i] = u8(nsig_[i] + 0x04);
        }
        if (left) nsig_[index - 1] = u8(nsig_[index - 1] + 1);
        if (right) nsig_[index + 1] = u8(nsig_[index + 1] + 1);
        nsig_[index] |= 0x80;
        flags_[index] |= 2;
    }
    u8 decode_sign(int row, int col, std::size_t index) {
        const std::size_t w = std::size_t(w_);
        auto contrib = [&](bool a, std::size_t ia, bool b, std::size_t ib) {
            const bool sa = a && mag_[ia], sb = b && mag_[ib];
            if (sa && sb) return 1 - int(sign_[ib]) - int(sign_[ia]);
            if (sa) return 1 - 2 * int(sign_[ia]);
            if (sb) return 1 - 2 * int(sign_[ib]);
            return 0;
        };
        const int h = contrib(col > 0, index - 1, col + 1 < w_, index + 1);
        const int v = contrib(row > 0, index - w, row + 1 < h_, index + w);
        const int c = 3 * h + v;
        if (c >= 0) return u8(mq_.read(cx_[std::size_t(9 + c)]));
        return u8(mq_.read(cx_[std::size_t(9 - c)]) ^ 1);
    }
    int w_, h_;
    const u8* labels_;
    MQ& mq_;
    std::array<std::uint8_t, 19> cx_{};
    std::vector<u8> nsig_, flags_, sign_;
    std::vector<unsigned> mag_;
    std::vector<std::uint8_t> bits_;
};

// [end of Apache-2.0 portion]

// --- tile structure -------------------------------------------------------------------------------------------------

struct Span {
    const u8* p;
    std::size_t n;
};

struct CodeBlock {
    int x0, y0, x1, y1;
    int lblock = 3;
    bool included = false;
    int zero_bitplanes = 0;
    int passes = 0;
    std::vector<Span> data;
};

struct PrecinctBand {
    int cw = 0, ch = 0;
    TagTree inclusion, zero_bits;
    std::vector<CodeBlock> blocks;
};

struct Band {
    int kind = 0;    // 0 LL, 1 HL, 2 LH, 3 HH
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int epsilon = 0, mu = 0;
    std::vector<PrecinctBand> precincts;
    std::vector<float> coeffs;   // decoded, kept resolutions only
};

struct Resolution {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int pw = 0, ph = 0;     // precincts wide, high
    int ppx = 15, ppy = 15;
    std::vector<Band> bands;
};

struct TileComp {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Coding coding;
    Quant quant;
    std::vector<Resolution> res;
    std::vector<float> samples;   // after the inverse transform at the kept resolution
    int sw = 0, sh = 0, sx0 = 0, sy0 = 0;
};

// --- inverse wavelet ------------------------------------------------------------------------------------------------

inline int mirror(int i, int lo, int hi) {   // [lo, hi)
    const int span = hi - lo - 1;
    if (span <= 0) return lo;
    int k = (i - lo) % (2 * span);
    if (k < 0) k += 2 * span;
    return lo + (k <= span ? k : 2 * span - k);
}

// One line in place; `a` holds index `i0`.. of the interleaved signal.
void synth_53(float* a, int i0, int i1) {
    const int n = i1 - i0;
    if (n == 1) {
        if (i0 & 1) a[0] = std::floor(a[0] / 2);
        return;
    }
    auto at = [&](int i) -> float& { return a[mirror(i, i0, i1) - i0]; };
    for (int i = (i0 & 1) ? i0 + 1 : i0; i < i1; i += 2) a[i - i0] -= std::floor((at(i - 1) + at(i + 1) + 2) / 4);
    for (int i = (i0 & 1) ? i0 : i0 + 1; i < i1; i += 2) a[i - i0] += std::floor((at(i - 1) + at(i + 1)) / 2);
}

void synth_97(float* a, int i0, int i1) {
    constexpr float alpha = -1.586134342059924f, beta = -0.052980118572961f, gamma = 0.882911075530934f, delta = 0.443506852043971f;
    constexpr float K = 1.230174104914001f;
    const int n = i1 - i0;
    if (n == 1) {
        if (i0 & 1) a[0] /= 2;
        return;
    }
    auto at = [&](int i) -> float { return a[mirror(i, i0, i1) - i0]; };
    const int even = (i0 & 1) ? i0 + 1 : i0, odd = (i0 & 1) ? i0 : i0 + 1;
    for (int i = even; i < i1; i += 2) a[i - i0] *= K;
    for (int i = odd; i < i1; i += 2) a[i - i0] /= K;
    for (int i = even; i < i1; i += 2) a[i - i0] -= delta * (at(i - 1) + at(i + 1));
    for (int i = odd; i < i1; i += 2) a[i - i0] -= gamma * (at(i - 1) + at(i + 1));
    for (int i = even; i < i1; i += 2) a[i - i0] -= beta * (at(i - 1) + at(i + 1));
    for (int i = odd; i < i1; i += 2) a[i - i0] -= alpha * (at(i - 1) + at(i + 1));
}

// --- the decoder ------------------------------------------------------------------------------------------------------

class Decoder {
public:
    Decoder(const u8* d, std::size_t n, int max_side) : d_(d), n_(n), max_side_(max_side) {}

    void run(std::vector<std::vector<float>>& planes, std::vector<CompSiz>& comps, int& w, int& h) {
        parse();
        for (std::size_t t = 0; t < tile_parts_.size(); ++t)
            if (!tile_parts_[t].empty()) decode_tile(int(t));
        comps = comps_;
        w = out_w_;
        h = out_h_;
        planes = std::move(planes_);
    }

private:
    void parse() {
        Reader r{d_, n_};
        if (r.u16() != 0xff4f) throw Bad("no SOC");
        bool siz = false;
        while (r.has(2)) {
            unsigned marker = r.u16();
            if (marker == 0xffd9) break;
            if (marker == 0xff90) {   // SOT
                std::size_t sot_at = r.at - 2;
                unsigned len = r.u16();
                if (len != 10) throw Bad("SOT");
                int tile = int(r.u16());
                std::uint32_t psot = r.u32();
                r.u8v();
                r.u8v();
                if (!siz || tile >= num_tiles_) throw Bad("tile index");
                if (tile_params_[std::size_t(tile)].coding.empty()) {
                    // priority: tile COC > tile COD > main COC > main COD (A.6.1): a tile COD overrides a main COC
                    tile_params_[std::size_t(tile)] = main_;
                    std::fill(tile_params_[std::size_t(tile)].coc_set.begin(), tile_params_[std::size_t(tile)].coc_set.end(), 0);
                    std::fill(tile_params_[std::size_t(tile)].qcc_set.begin(), tile_params_[std::size_t(tile)].qcc_set.end(), 0);
                }
                TileParams& tp = tile_params_[std::size_t(tile)];
                // tile-part header until SOD
                while (true) {
                    unsigned m = r.u16();
                    if (m == 0xff93) break;
                    read_marker(r, m, tp, true);
                }
                std::size_t end = psot ? sot_at + psot : n_;
                if (end > n_ || end < r.at) end = n_;
                if (end > r.at) tile_parts_[std::size_t(tile)].push_back({d_ + r.at, end - r.at});
                r.at = end;
                continue;
            }
            if (marker == 0xff51) {
                read_siz(r);
                siz = true;
                continue;
            }
            if (!siz) throw Bad("no SIZ");
            read_marker(r, marker, main_, false);
        }
        if (!siz) throw Bad("no SIZ");
    }

    void read_siz(Reader& r) {
        std::size_t start = r.at;
        unsigned len = r.u16();
        r.u16();
        xsiz_ = r.u32(), ysiz_ = r.u32(), xosiz_ = r.u32(), yosiz_ = r.u32();
        xtsiz_ = r.u32(), ytsiz_ = r.u32(), xtosiz_ = r.u32(), ytosiz_ = r.u32();
        unsigned csiz = r.u16();
        if (!csiz || csiz > 64 || xsiz_ <= xosiz_ || ysiz_ <= yosiz_ || !xtsiz_ || !ytsiz_ || xsiz_ > (1u << 30) || ysiz_ > (1u << 30))
            throw Bad("SIZ");
        for (unsigned c = 0; c < csiz; ++c) {
            CompSiz s;
            unsigned v = r.u8v();
            s.sgn = (v & 0x80) != 0;
            s.prec = int(v & 0x7f) + 1;
            s.dx = int(r.u8v());
            s.dy = int(r.u8v());
            if (!s.dx || !s.dy || s.prec > 31) throw Bad("SIZ component");
            comps_.push_back(s);
        }
        r.at = start + len;
        std::int64_t tx = ceil_div(std::int64_t(xsiz_) - xtosiz_, xtsiz_), ty = ceil_div(std::int64_t(ysiz_) - ytosiz_, ytsiz_);
        if (tx < 1 || ty < 1 || tx * ty > 65535) throw Bad("tile count");
        tiles_x_ = int(tx), tiles_y_ = int(ty);
        num_tiles_ = tiles_x_ * tiles_y_;
        tile_params_.resize(std::size_t(num_tiles_));
        tile_parts_.resize(std::size_t(num_tiles_));
        main_.coding.resize(csiz);
        main_.quant.resize(csiz);
        main_.coc_set.assign(csiz, 0);
        main_.qcc_set.assign(csiz, 0);
    }

    void read_marker(Reader& r, unsigned marker, TileParams& tp, bool in_tile) {
        std::size_t start = r.at;
        unsigned len = r.u16();
        if (len < 2 || !r.has(len - 2)) throw Bad("marker length");
        const std::size_t end = start + len;
        const int csiz = int(comps_.size());
        switch (marker) {
            case 0xff52: {   // COD
                unsigned scod = r.u8v();
                tp.progression = int(r.u8v());
                tp.layers = int(r.u16());
                tp.mct = r.u8v() != 0;
                tp.sop = scod & 2;
                tp.eph = scod & 4;
                if (tp.progression > 4 || !tp.layers) throw Bad("COD");
                Coding c;
                read_coding_body(r, c, scod & 1);
                for (int k = 0; k < csiz; ++k)
                    if (!tp.coc_set[std::size_t(k)]) tp.coding[std::size_t(k)] = c;
                break;
            }
            case 0xff53: {   // COC
                int comp = csiz < 257 ? int(r.u8v()) : int(r.u16());
                unsigned scoc = r.u8v();
                if (comp >= csiz) throw Bad("COC");
                read_coding_body(r, tp.coding[std::size_t(comp)], scoc & 1);
                tp.coc_set[std::size_t(comp)] = 1;
                break;
            }
            case 0xff5c: {   // QCD
                Quant q;
                read_quant_body(r, end, q);
                for (int k = 0; k < csiz; ++k)
                    if (!tp.qcc_set[std::size_t(k)]) tp.quant[std::size_t(k)] = q;
                break;
            }
            case 0xff5d: {   // QCC
                int comp = csiz < 257 ? int(r.u8v()) : int(r.u16());
                if (comp >= csiz) throw Bad("QCC");
                read_quant_body(r, end, tp.quant[std::size_t(comp)]);
                tp.qcc_set[std::size_t(comp)] = 1;
                break;
            }
            case 0xff5e: throw Bad("RGN");
            case 0xff5f: throw Bad("POC");
            case 0xff60: throw Bad("PPM");
            case 0xff61: throw Bad("PPT");
            default:     // TLM, PLM, PLT, CRG, COM
                break;
        }
        (void)in_tile;
        r.at = end;
    }

    // Levels discarded from the top so the larger side fits; never more than any component's decomposition levels.
    int choose_discard(const TileParams& tp) const {
        int levels = 32;
        for (const Coding& c : tp.coding) levels = std::min(levels, c.levels);
        std::int64_t side = std::max<std::int64_t>(std::int64_t(xsiz_) - xosiz_, std::int64_t(ysiz_) - yosiz_);
        int d = 0;
        while (d < levels && (side >> d) > max_side_) ++d;
        return d;
    }

    void ensure_output(int discard) {
        if (!planes_.empty()) return;
        discard_ = discard;
        const int k = int(comps_.size());
        out_w_ = ceil_shift(xsiz_, discard) - ceil_shift(xosiz_, discard);
        out_h_ = ceil_shift(ysiz_, discard) - ceil_shift(yosiz_, discard);
        if (out_w_ < 1 || out_h_ < 1 || std::size_t(out_w_) * std::size_t(out_h_) * std::size_t(k) > kMaxPixels) throw Bad("too many pixels");
        planes_.assign(std::size_t(k), std::vector<float>(std::size_t(out_w_) * std::size_t(out_h_), 0.0f));
    }

    void build_tile_comp(TileComp& tc, const CompSiz& cs, int tx0, int ty0, int tx1, int ty1) {
        tc.x0 = ceil_div(tx0, cs.dx), tc.y0 = ceil_div(ty0, cs.dy);
        tc.x1 = ceil_div(tx1, cs.dx), tc.y1 = ceil_div(ty1, cs.dy);
        const int NL = tc.coding.levels;
        tc.res.resize(std::size_t(NL + 1));
        for (int r = 0; r <= NL; ++r) {
            Resolution& rs = tc.res[std::size_t(r)];
            const int nd = NL - r;
            rs.x0 = ceil_shift(tc.x0, nd), rs.y0 = ceil_shift(tc.y0, nd);
            rs.x1 = ceil_shift(tc.x1, nd), rs.y1 = ceil_shift(tc.y1, nd);
            rs.ppx = tc.coding.prec_x(r), rs.ppy = tc.coding.prec_y(r);
            rs.pw = rs.x1 > rs.x0 ? ceil_shift(rs.x1, rs.ppx) - floor_shift(rs.x0, rs.ppx) : 0;
            rs.ph = rs.y1 > rs.y0 ? ceil_shift(rs.y1, rs.ppy) - floor_shift(rs.y0, rs.ppy) : 0;
            const int nb = r == 0 ? NL : NL - r + 1;
            rs.bands.resize(r == 0 ? 1 : 3);
            for (std::size_t b = 0; b < rs.bands.size(); ++b) {
                Band& band = rs.bands[b];
                band.kind = r == 0 ? 0 : int(b) + 1;
                const int xob = band.kind & 1, yob = band.kind >> 1;
                if (r == 0) {
                    band.x0 = rs.x0, band.y0 = rs.y0, band.x1 = rs.x1, band.y1 = rs.y1;
                } else {
                    const std::int64_t ox = std::int64_t(1) << (nb - 1), oy = ox;
                    band.x0 = ceil_shift(tc.x0 - ox * xob, nb), band.y0 = ceil_shift(tc.y0 - oy * yob, nb);
                    band.x1 = ceil_shift(tc.x1 - ox * xob, nb), band.y1 = ceil_shift(tc.y1 - oy * yob, nb);
                }
                // quantization
                const Quant& q = tc.quant;
                const std::size_t qi = r == 0 ? 0 : 3 * std::size_t(r - 1) + 1 + b;
                if (q.style == 1) {
                    band.epsilon = q.steps[0].first + (r > 0 ? 1 - r : 0);
                    band.mu = q.steps[0].second;
                } else {
                    if (qi >= q.steps.size()) throw Bad("quantization table too short");
                    band.epsilon = q.steps[qi].first;
                    band.mu = q.steps[qi].second;
                }
                // precincts and code blocks
                const int ppbx = r == 0 ? rs.ppx : rs.ppx - 1, ppby = r == 0 ? rs.ppy : rs.ppy - 1;
                if (ppbx < 0 || ppby < 0) throw Bad("precinct size");
                const int xcb = std::min(tc.coding.xcb, ppbx), ycb = std::min(tc.coding.ycb, ppby);
                precinct_count_ += std::size_t(rs.pw) * std::size_t(rs.ph);
                if (precinct_count_ > kMaxPrecincts) throw Bad("too many precincts");
                band.precincts.resize(std::size_t(rs.pw) * std::size_t(rs.ph));
                const int px_start = floor_shift(rs.x0, rs.ppx), py_start = floor_shift(rs.y0, rs.ppy);
                for (int py = 0; py < rs.ph; ++py)
                    for (int px = 0; px < rs.pw; ++px) {
                        PrecinctBand& pb = band.precincts[std::size_t(py) * std::size_t(rs.pw) + std::size_t(px)];
                        const int bx0 = std::max(band.x0, (px_start + px) << ppbx), by0 = std::max(band.y0, (py_start + py) << ppby);
                        const int bx1 = std::min(band.x1, (px_start + px + 1) << ppbx), by1 = std::min(band.y1, (py_start + py + 1) << ppby);
                        if (bx1 <= bx0 || by1 <= by0) continue;
                        const int cx0 = floor_shift(bx0, xcb), cy0 = floor_shift(by0, ycb);
                        pb.cw = ceil_shift(bx1, xcb) - cx0;
                        pb.ch = ceil_shift(by1, ycb) - cy0;
                        block_count_ += std::size_t(pb.cw) * std::size_t(pb.ch);
                        if (block_count_ > kMaxCodeBlocks) throw Bad("too many code blocks");
                        pb.blocks.resize(std::size_t(pb.cw) * std::size_t(pb.ch));
                        pb.inclusion.init(pb.cw, pb.ch);
                        pb.zero_bits.init(pb.cw, pb.ch);
                        for (int cy = 0; cy < pb.ch; ++cy)
                            for (int cx = 0; cx < pb.cw; ++cx) {
                                CodeBlock& cb = pb.blocks[std::size_t(cy) * std::size_t(pb.cw) + std::size_t(cx)];
                                cb.x0 = std::max(bx0, (cx0 + cx) << xcb);
                                cb.y0 = std::max(by0, (cy0 + cy) << ycb);
                                cb.x1 = std::min(bx1, (cx0 + cx + 1) << xcb);
                                cb.y1 = std::min(by1, (cy0 + cy + 1) << ycb);
                            }
                    }
            }
        }
    }

    void decode_tile(int t) {
        const TileParams& tp = tile_params_[std::size_t(t)];
        const int p = t % tiles_x_, q = t / tiles_x_;
        const int tx0 = int(std::max<std::int64_t>(std::int64_t(xtosiz_) + std::int64_t(p) * xtsiz_, xosiz_));
        const int ty0 = int(std::max<std::int64_t>(std::int64_t(ytosiz_) + std::int64_t(q) * ytsiz_, yosiz_));
        const int tx1 = int(std::min<std::int64_t>(std::int64_t(xtosiz_) + std::int64_t(p + 1) * xtsiz_, xsiz_));
        const int ty1 = int(std::min<std::int64_t>(std::int64_t(ytosiz_) + std::int64_t(q + 1) * ytsiz_, ysiz_));
        if (tx1 <= tx0 || ty1 <= ty0) return;

        std::vector<TileComp> tcs(comps_.size());
        block_count_ = 0;
        precinct_count_ = 0;
        for (std::size_t c = 0; c < comps_.size(); ++c) {
            tcs[c].coding = tp.coding[c];
            tcs[c].quant = tp.quant[c];
            if (tcs[c].quant.steps.empty()) throw Bad("no quantization");
            build_tile_comp(tcs[c], comps_[c], tx0, ty0, tx1, ty1);
        }
        ensure_output(choose_discard(tp));
        read_packets(t, tp, tcs, tx0, ty0, tx1, ty1);
        for (std::size_t c = 0; c < tcs.size(); ++c) reconstruct(tcs[c], comps_[c]);
        // inverse multiple component transform (ISO/IEC 15444-1 Annex G)
        if (tp.mct && tcs.size() >= 3 && tcs[0].samples.size() == tcs[1].samples.size() && tcs[0].samples.size() == tcs[2].samples.size()) {
            auto& y0 = tcs[0].samples;
            auto& y1 = tcs[1].samples;
            auto& y2 = tcs[2].samples;
            if (tcs[0].coding.reversible) {
                for (std::size_t i = 0; i < y0.size(); ++i) {
                    float g = y0[i] - std::floor((y2[i] + y1[i]) / 4);
                    float r = y2[i] + g, b = y1[i] + g;
                    y0[i] = r, y1[i] = g, y2[i] = b;
                }
            } else {
                for (std::size_t i = 0; i < y0.size(); ++i) {
                    float r = y0[i] + 1.402f * y2[i];
                    float g = y0[i] - 0.34413f * y1[i] - 0.71414f * y2[i];
                    float b = y0[i] + 1.772f * y1[i];
                    y0[i] = r, y1[i] = g, y2[i] = b;
                }
            }
        }
        // copy to the output planes
        const int ox0 = ceil_shift(xosiz_, discard_), oy0 = ceil_shift(yosiz_, discard_);
        for (std::size_t c = 0; c < tcs.size(); ++c) {
            const TileComp& tc = tcs[c];
            const CompSiz& cs = comps_[c];
            if (tc.sw <= 0 || tc.sh <= 0) continue;
            std::vector<float>& plane = planes_[c];
            for (int y = 0; y < tc.sh; ++y) {
                const int yy = ((tc.sy0 + y) * cs.dy) - oy0;
                for (int k = 0; k < cs.dy; ++k) {
                    const int row = yy + k;
                    if (row < 0 || row >= out_h_) continue;
                    for (int x = 0; x < tc.sw; ++x) {
                        const int xx = ((tc.sx0 + x) * cs.dx) - ox0;
                        for (int m = 0; m < cs.dx; ++m) {
                            const int col = xx + m;
                            if (col < 0 || col >= out_w_) continue;
                            plane[std::size_t(row) * std::size_t(out_w_) + std::size_t(col)] = tc.samples[std::size_t(y) * std::size_t(tc.sw) + std::size_t(x)];
                        }
                    }
                }
            }
        }
    }

    // --- tier 2 ------------------------------------------------------------------------------------------------

    void read_packets(int t, const TileParams& tp, std::vector<TileComp>& tcs, int tx0, int ty0, int tx1, int ty1) {
        // Tile parts of a tile form one packet sequence.
        const std::vector<Span>& parts = tile_parts_[std::size_t(t)];
        std::vector<u8> joined;
        const u8* data;
        std::size_t size;
        if (parts.size() == 1) {
            data = parts[0].p;
            size = parts[0].n;
        } else {
            std::size_t total = 0;
            for (const Span& s : parts) total += s.n;
            joined.resize(total);
            std::size_t at = 0;
            for (const Span& s : parts) {
                std::memcpy(joined.data() + at, s.p, s.n);
                at += s.n;
            }
            joined_store_.push_back(std::move(joined));
            data = joined_store_.back().data();
            size = total;
        }
        std::size_t at = 0;
        int max_res = 0;
        for (const TileComp& tc : tcs) max_res = std::max(max_res, tc.coding.levels);
        const int nc = int(tcs.size());
        auto packet = [&](int l, int r, int c, int k) -> bool {
            TileComp& tc = tcs[std::size_t(c)];
            if (r > tc.coding.levels) return true;
            Resolution& rs = tc.res[std::size_t(r)];
            if (k >= rs.pw * rs.ph) return true;
            return read_packet(data, size, at, tp, tc, rs, k, l);
        };
        const auto steps = [&]() {
            std::pair<std::int64_t, std::int64_t> s{std::int64_t(1) << 40, std::int64_t(1) << 40};
            for (int c = 0; c < nc; ++c) {
                const TileComp& tc = tcs[std::size_t(c)];
                for (int r = 0; r <= tc.coding.levels; ++r) {
                    const Resolution& rs = tc.res[std::size_t(r)];
                    s.first = std::min(s.first, std::int64_t(comps_[std::size_t(c)].dx) << (rs.ppx + tc.coding.levels - r));
                    s.second = std::min(s.second, std::int64_t(comps_[std::size_t(c)].dy) << (rs.ppy + tc.coding.levels - r));
                }
            }
            return s;
        }();
        // precinct index for position (x, y), -1 when no precinct starts there (B.12)
        auto precinct_at = [&](int c, int r, std::int64_t x, std::int64_t y) -> int {
            const TileComp& tc = tcs[std::size_t(c)];
            if (r > tc.coding.levels) return -1;
            const Resolution& rs = tc.res[std::size_t(r)];
            if (rs.pw == 0 || rs.ph == 0) return -1;
            const CompSiz& cs = comps_[std::size_t(c)];
            const int nd = tc.coding.levels - r;
            const std::int64_t sx = std::int64_t(cs.dx) << (rs.ppx + nd), sy = std::int64_t(cs.dy) << (rs.ppy + nd);
            const bool oky = y % sy == 0 || (y == ty0 && ((std::int64_t(rs.y0) << nd) % (std::int64_t(1) << (rs.ppy + nd))) != 0);
            const bool okx = x % sx == 0 || (x == tx0 && ((std::int64_t(rs.x0) << nd) % (std::int64_t(1) << (rs.ppx + nd))) != 0);
            if (!okx || !oky) return -1;
            const int px = floor_shift(ceil_div(x, std::int64_t(cs.dx) << nd), rs.ppx) - floor_shift(rs.x0, rs.ppx);
            const int py = floor_shift(ceil_div(y, std::int64_t(cs.dy) << nd), rs.ppy) - floor_shift(rs.y0, rs.ppy);
            if (px < 0 || py < 0 || px >= rs.pw || py >= rs.ph) return -1;
            return px + py * rs.pw;
        };
        auto next_pos = [](std::int64_t v, std::int64_t step) { return v + step - (v % step); };
        switch (tp.progression) {
            case 0:   // LRCP
                for (int l = 0; l < tp.layers; ++l)
                    for (int r = 0; r <= max_res; ++r)
                        for (int c = 0; c < nc; ++c) {
                            if (r > tcs[std::size_t(c)].coding.levels) continue;
                            const Resolution& rs = tcs[std::size_t(c)].res[std::size_t(r)];
                            for (int k = 0; k < rs.pw * rs.ph; ++k)
                                if (!packet(l, r, c, k)) return;
                        }
                return;
            case 1:   // RLCP
                for (int r = 0; r <= max_res; ++r)
                    for (int l = 0; l < tp.layers; ++l)
                        for (int c = 0; c < nc; ++c) {
                            if (r > tcs[std::size_t(c)].coding.levels) continue;
                            const Resolution& rs = tcs[std::size_t(c)].res[std::size_t(r)];
                            for (int k = 0; k < rs.pw * rs.ph; ++k)
                                if (!packet(l, r, c, k)) return;
                        }
                return;
            case 2:   // RPCL
                for (int r = 0; r <= max_res; ++r)
                    for (std::int64_t y = ty0; y < ty1; y = next_pos(y, steps.second))
                        for (std::int64_t x = tx0; x < tx1; x = next_pos(x, steps.first))
                            for (int c = 0; c < nc; ++c) {
                                int k = precinct_at(c, r, x, y);
                                if (k < 0) continue;
                                for (int l = 0; l < tp.layers; ++l)
                                    if (!packet(l, r, c, k)) return;
                            }
                return;
            case 3:   // PCRL
                for (std::int64_t y = ty0; y < ty1; y = next_pos(y, steps.second))
                    for (std::int64_t x = tx0; x < tx1; x = next_pos(x, steps.first))
                        for (int c = 0; c < nc; ++c)
                            for (int r = 0; r <= max_res; ++r) {
                                int k = precinct_at(c, r, x, y);
                                if (k < 0) continue;
                                for (int l = 0; l < tp.layers; ++l)
                                    if (!packet(l, r, c, k)) return;
                            }
                return;
            default:  // CPRL
                for (int c = 0; c < nc; ++c)
                    for (std::int64_t y = ty0; y < ty1; y = next_pos(y, steps.second))
                        for (std::int64_t x = tx0; x < tx1; x = next_pos(x, steps.first))
                            for (int r = 0; r <= max_res; ++r) {
                                int k = precinct_at(c, r, x, y);
                                if (k < 0) continue;
                                for (int l = 0; l < tp.layers; ++l)
                                    if (!packet(l, r, c, k)) return;
                            }
                return;
        }
    }

    // False at the end of the data (truncated files keep what was read).
    bool read_packet(const u8* data, std::size_t size, std::size_t& at, const TileParams& tp, TileComp&, Resolution& rs, int k, int layer) {
        if (at >= size) return false;
        if (tp.sop && size - at >= 6 && data[at] == 0xff && data[at + 1] == 0x91) at += 6;
        BitReader br(data + at, size - at);
        struct Pending {
            CodeBlock* cb;
            std::size_t len;
        };
        std::vector<Pending> pending;
        try {
            if (br.bit()) {
                for (Band& band : rs.bands) {
                    PrecinctBand& pb = band.precincts[std::size_t(k)];
                    for (std::size_t b = 0; b < pb.blocks.size(); ++b) {
                        CodeBlock& cb = pb.blocks[b];
                        bool included;
                        if (!cb.included) {
                            included = pb.inclusion.decode(br, int(b), layer + 1);
                        } else {
                            included = br.bit() != 0;
                        }
                        if (!included) continue;
                        if (!cb.included) {
                            cb.included = true;
                            int z = 1;
                            while (!pb.zero_bits.decode(br, int(b), z)) {
                                if (++z > 64) throw Bad("zero bit planes");
                            }
                            cb.zero_bitplanes = z - 1;
                        }
                        // number of coding passes (B.10.6)
                        int passes;
                        if (!br.bit()) passes = 1;
                        else if (!br.bit()) passes = 2;
                        else {
                            unsigned v = br.bits(2);
                            if (v != 3) passes = 3 + int(v);
                            else {
                                v = br.bits(5);
                                if (v != 31) passes = 6 + int(v);
                                else passes = 37 + int(br.bits(7));
                            }
                        }
                        while (br.bit()) {
                            if (++cb.lblock > 32) throw Bad("Lblock");
                        }
                        int lg = 0;
                        while ((1 << (lg + 1)) <= passes) ++lg;
                        const int nbits = cb.lblock + lg;
                        if (nbits > 32) throw Bad("segment length");
                        std::size_t len = br.bits(nbits);
                        cb.passes += passes;
                        pending.push_back({&cb, len});
                    }
                }
            }
            at += br.finish();
        } catch (const Bad&) {
            at = size;
            return false;
        }
        if (tp.eph && size - at >= 2 && data[at] == 0xff && data[at + 1] == 0x92) at += 2;
        for (const Pending& p : pending) {
            std::size_t n = std::min(p.len, size - at);
            if (n) p.cb->data.push_back({data + at, n});
            at += n;
            if (n < p.len) return false;
        }
        return true;
    }

    // --- tier 1 and reconstruction -----------------------------------------------------------------------------

    void decode_block(const CodeBlock& cb, Band& band, const TileComp& tc, const CompSiz& cs, int bw, std::vector<u8>& scratch) {
        if (!cb.included || cb.passes == 0 || cb.data.empty()) return;
        const int w = cb.x1 - cb.x0, h = cb.y1 - cb.y0;
        if (w <= 0 || h <= 0) return;
        const u8* d;
        std::size_t n = 0;
        if (cb.data.size() == 1) {
            d = cb.data[0].p;
            n = cb.data[0].n;
        } else {
            for (const Span& s : cb.data) n += s.n;
            scratch.resize(n);
            std::size_t at = 0;
            for (const Span& s : cb.data) {
                std::memcpy(scratch.data() + at, s.p, s.n);
                at += s.n;
            }
            d = scratch.data();
        }
        MQ mq(d, n);
        const u8* labels = band.kind == 3 ? kLabelHH : band.kind == 1 ? kLabelHL : kLabelLLLH;
        BlockDecoder bd(w, h, labels, cb.zero_bitplanes, mq);
        const bool seg = tc.coding.style & 0x20, reset = tc.coding.style & 0x02;
        int type = 2;
        for (int p = 0; p < cb.passes; ++p) {
            if (type == 0) bd.significance();
            else if (type == 1) bd.refinement();
            else bd.cleanup(seg);
            if (reset) bd.reset();
            type = (type + 1) % 3;
        }
        const int gain = band.kind == 0 ? 0 : band.kind == 3 ? 2 : 1;
        const int mb = tc.quant.guard + band.epsilon - 1;
        const bool rev = tc.coding.reversible;
        const float delta = rev ? 1.0f : std::ldexp(1.0f + float(band.mu) / 2048.0f, cs.prec + gain - band.epsilon);
        const float half = rev ? 0.0f : 0.5f;
        for (int y = 0; y < h; ++y) {
            float* out = &band.coeffs[std::size_t(cb.y0 - band.y0 + y) * std::size_t(bw) + std::size_t(cb.x0 - band.x0)];
            for (int x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(w) + std::size_t(x);
                unsigned m = bd.mag(i);
                if (!m) continue;
                float v = (float(m) + half) * delta;
                const int nb = bd.bits(i);
                if (mb > nb) v = std::ldexp(v, mb - nb);
                out[x] = bd.negative(i) ? -v : v;
            }
        }
    }

    void reconstruct(TileComp& tc, const CompSiz& cs) {
        const int NL = tc.coding.levels;
        const int keep = std::max(0, NL - discard_);   // highest resolution index decoded
        std::vector<u8> scratch;
        for (int r = 0; r <= keep && r <= NL; ++r) {
            Resolution& rs = tc.res[std::size_t(r)];
            for (Band& band : rs.bands) {
                const int bw = band.x1 - band.x0, bh = band.y1 - band.y0;
                if (bw <= 0 || bh <= 0) continue;
                band.coeffs.assign(std::size_t(bw) * std::size_t(bh), 0.0f);
                for (const PrecinctBand& pb : band.precincts)
                    for (const CodeBlock& cb : pb.blocks) decode_block(cb, band, tc, cs, bw, scratch);
            }
        }
        // inverse DWT up to the kept resolution
        Resolution& r0 = tc.res[0];
        std::vector<float> ll = std::move(r0.bands[0].coeffs);
        int w = r0.x1 - r0.x0, h = r0.y1 - r0.y0;
        std::vector<float> line, cur;
        for (int r = 1; r <= keep && r <= NL; ++r) {
            Resolution& rs = tc.res[std::size_t(r)];
            const int rw = rs.x1 - rs.x0, rh = rs.y1 - rs.y0;
            if (rw <= 0 || rh <= 0) {
                ll.clear();
                w = rw, h = rh;
                continue;
            }
            cur.assign(std::size_t(rw) * std::size_t(rh), 0.0f);
            // interleave: LL even/even, HL odd/even, LH even/odd, HH odd/odd of the global coordinates
            const Resolution& prev = tc.res[std::size_t(r - 1)];
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int gx = 2 * (prev.x0 + x) - rs.x0, gy = 2 * (prev.y0 + y) - rs.y0;
                    if (gx >= 0 && gx < rw && gy >= 0 && gy < rh)
                        cur[std::size_t(gy) * std::size_t(rw) + std::size_t(gx)] = ll[std::size_t(y) * std::size_t(w) + std::size_t(x)];
                }
            for (Band& band : rs.bands) {
                const int bw = band.x1 - band.x0, bh = band.y1 - band.y0;
                if (bw <= 0 || bh <= 0) continue;
                const int xob = band.kind & 1, yob = band.kind >> 1;
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        const int gx = 2 * (band.x0 + x) + xob - rs.x0, gy = 2 * (band.y0 + y) + yob - rs.y0;
                        if (gx >= 0 && gx < rw && gy >= 0 && gy < rh)
                            cur[std::size_t(gy) * std::size_t(rw) + std::size_t(gx)] = band.coeffs[std::size_t(y) * std::size_t(bw) + std::size_t(x)];
                    }
                std::vector<float>().swap(band.coeffs);
            }
            auto filter = tc.coding.reversible ? synth_53 : synth_97;
            // horizontal, then vertical (F.3.2: HOR_SR first)
            for (int y = 0; y < rh; ++y) filter(&cur[std::size_t(y) * std::size_t(rw)], rs.x0, rs.x1);
            line.resize(std::size_t(rh));
            for (int x = 0; x < rw; ++x) {
                for (int y = 0; y < rh; ++y) line[std::size_t(y)] = cur[std::size_t(y) * std::size_t(rw) + std::size_t(x)];
                filter(line.data(), rs.y0, rs.y1);
                for (int y = 0; y < rh; ++y) cur[std::size_t(y) * std::size_t(rw) + std::size_t(x)] = line[std::size_t(y)];
            }
            ll.swap(cur);
            w = rw, h = rh;
        }
        const Resolution& top = tc.res[std::size_t(std::min(keep, NL))];
        tc.samples = std::move(ll);
        tc.sw = w, tc.sh = h;
        tc.sx0 = top.x0, tc.sy0 = top.y0;
        if (NL < discard_) {
            // cannot happen: choose_discard caps by the smallest level count
            throw Bad("levels");
        }
        // free the structure of this component
        std::vector<Resolution>().swap(tc.res);
    }

    const u8* d_;
    std::size_t n_;
    int max_side_;
    std::uint32_t xsiz_ = 0, ysiz_ = 0, xosiz_ = 0, yosiz_ = 0, xtsiz_ = 0, ytsiz_ = 0, xtosiz_ = 0, ytosiz_ = 0;
    int tiles_x_ = 0, tiles_y_ = 0, num_tiles_ = 0;
    std::vector<CompSiz> comps_;
    TileParams main_;
    std::vector<TileParams> tile_params_;
    std::vector<std::vector<Span>> tile_parts_;
    std::vector<std::vector<u8>> joined_store_;
    std::size_t block_count_ = 0, precinct_count_ = 0;
    int discard_ = 0, out_w_ = 0, out_h_ = 0;
    std::vector<std::vector<float>> planes_;
};

// --- JP2 container ---------------------------------------------------------------------------------------------------

struct Palette {
    int entries = 0, columns = 0;
    std::vector<int> depth;           // per column
    std::vector<std::uint32_t> values;   // entries * columns
};

struct ChannelMap {
    int comp = 0, type = 0, column = 0;
};

struct Boxes {
    const u8* cs = nullptr;
    std::size_t cs_n = 0;
    int enum_cs = 0;
    Palette palette;
    std::vector<ChannelMap> cmap;
    int alpha_channel = -1;
};

void scan_boxes(const u8* d, std::size_t n, Boxes& out, int depth = 0) {
    std::size_t at = 0;
    while (at + 8 <= n) {
        std::uint64_t len = (std::uint64_t(d[at]) << 24) | (std::uint64_t(d[at + 1]) << 16) | (std::uint64_t(d[at + 2]) << 8) | d[at + 3];
        const u8* t = d + at + 4;
        std::size_t head = 8;
        if (len == 1) {
            if (at + 16 > n) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | d[at + 8 + std::size_t(i)];
            head = 16;
        } else if (len == 0) {
            len = n - at;
        }
        if (len < head || len > n - at) return;
        const u8* body = d + at + head;
        const std::size_t size = std::size_t(len) - head;
        if (!std::memcmp(t, "jp2c", 4)) {
            if (!out.cs) {
                out.cs = body;
                out.cs_n = size;
            }
        } else if (!std::memcmp(t, "jp2h", 4) && depth == 0) {
            scan_boxes(body, size, out, depth + 1);
        } else if (!std::memcmp(t, "colr", 4)) {
            if (size >= 7 && body[0] == 1 && !out.enum_cs) out.enum_cs = int((std::uint32_t(body[3]) << 24) | (std::uint32_t(body[4]) << 16) | (std::uint32_t(body[5]) << 8) | body[6]);
        } else if (!std::memcmp(t, "pclr", 4) && size >= 3) {
            Palette p;
            p.entries = (int(body[0]) << 8) | body[1];
            p.columns = body[2];
            std::size_t pos = 3;
            if (size < pos + std::size_t(p.columns)) return;
            for (int c = 0; c < p.columns; ++c) p.depth.push_back(int(body[pos++] & 0x7f) + 1);
            p.values.resize(std::size_t(p.entries) * std::size_t(p.columns));
            for (int e = 0; e < p.entries; ++e)
                for (int c = 0; c < p.columns; ++c) {
                    int bytes = (p.depth[std::size_t(c)] + 7) / 8;
                    std::uint32_t v = 0;
                    for (int k = 0; k < bytes; ++k) {
                        if (pos >= size) return;
                        v = (v << 8) | body[pos++];
                    }
                    p.values[std::size_t(e) * std::size_t(p.columns) + std::size_t(c)] = v;
                }
            out.palette = std::move(p);
        } else if (!std::memcmp(t, "cmap", 4)) {
            for (std::size_t pos = 0; pos + 4 <= size; pos += 4)
                out.cmap.push_back({(int(body[pos]) << 8) | body[pos + 1], body[pos + 2], body[pos + 3]});
        } else if (!std::memcmp(t, "cdef", 4) && size >= 2) {
            int count = (int(body[0]) << 8) | body[1];
            for (int i = 0; i < count && std::size_t(2 + 6 * (i + 1)) <= size; ++i) {
                const u8* e = body + 2 + 6 * i;
                int cn = (int(e[0]) << 8) | e[1], typ = (int(e[2]) << 8) | e[3];
                if (typ == 1 || typ == 2) out.alpha_channel = cn;
            }
        }
        at += std::size_t(len);
    }
}

}  // namespace

bool decode_reduced(const std::uint8_t* data, std::size_t size, int max_side, Image& out, std::string* why) {
    try {
        Boxes boxes;
        if (size >= 2 && data[0] == 0xff && data[1] == 0x4f) {
            boxes.cs = data;
            boxes.cs_n = size;
        } else {
            scan_boxes(data, size, boxes);
        }
        if (!boxes.cs) throw Bad("no codestream");
        Decoder dec(boxes.cs, boxes.cs_n, max_side);
        std::vector<std::vector<float>> planes;
        std::vector<CompSiz> comps;
        int w = 0, h = 0;
        dec.run(planes, comps, w, h);
        if (planes.empty() || w <= 0 || h <= 0) throw Bad("no tile data");
        // level shift, clamp, 8 bits; palette expansion
        auto sample8 = [&](std::size_t c, std::size_t i) -> unsigned {
            const CompSiz& cs = comps[c];
            double v = planes[c][i];
            if (!cs.sgn) v += double(std::int64_t(1) << (cs.prec - 1));
            const double maxv = double((std::uint64_t(1) << cs.prec) - 1);
            v = std::min(maxv, std::max(0.0, std::floor(v + 0.5)));
            return unsigned(std::lround(v * 255.0 / maxv));
        };
        auto sample_raw = [&](std::size_t c, std::size_t i) -> std::int64_t {
            const CompSiz& cs = comps[c];
            double v = planes[c][i];
            if (!cs.sgn) v += double(std::int64_t(1) << (cs.prec - 1));
            const double maxv = double((std::uint64_t(1) << cs.prec) - 1);
            return std::int64_t(std::min(maxv, std::max(0.0, std::floor(v + 0.5))));
        };
        const std::size_t pixels = std::size_t(w) * std::size_t(h);
        const bool palette = boxes.palette.entries > 0 && !boxes.cmap.empty();
        const int channels = palette ? int(boxes.cmap.size()) : int(comps.size());
        if (channels < 1 || channels > 64) throw Bad("channels");
        out.width = w;
        out.height = h;
        out.components = channels;
        out.enum_cs = boxes.enum_cs;
        out.alpha_channel = boxes.alpha_channel < channels ? boxes.alpha_channel : -1;
        out.samples.resize(pixels * std::size_t(channels));
        for (int ch = 0; ch < channels; ++ch) {
            if (palette) {
                const ChannelMap& m = boxes.cmap[std::size_t(ch)];
                if (m.comp < 0 || m.comp >= int(comps.size())) throw Bad("cmap");
                if (m.type == 1) {
                    if (m.column >= boxes.palette.columns) throw Bad("cmap column");
                    const int depth = boxes.palette.depth[std::size_t(m.column)];
                    const double maxv = double((std::uint64_t(1) << depth) - 1);
                    for (std::size_t i = 0; i < pixels; ++i) {
                        const std::int64_t idx = std::min<std::int64_t>(sample_raw(std::size_t(m.comp), i), boxes.palette.entries - 1);
                        const std::uint32_t v = boxes.palette.values[std::size_t(idx) * std::size_t(boxes.palette.columns) + std::size_t(m.column)];
                        out.samples[i * std::size_t(channels) + std::size_t(ch)] = u8(std::lround(v * 255.0 / maxv));
                    }
                } else {
                    for (std::size_t i = 0; i < pixels; ++i)
                        out.samples[i * std::size_t(channels) + std::size_t(ch)] = u8(sample8(std::size_t(m.comp), i));
                }
            } else {
                for (std::size_t i = 0; i < pixels; ++i) out.samples[i * std::size_t(channels) + std::size_t(ch)] = u8(sample8(std::size_t(ch), i));
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return false;
    }
}

}  // namespace eqx::jpx
