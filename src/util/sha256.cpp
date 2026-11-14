#include "util/sha256.h"

#include <cstdio>
#include <cstring>
#include <memory>

#if defined(__x86_64__) && defined(__GNUC__)
#include <cpuid.h>
#include <immintrin.h>
#define EQX_SHA_NI 1
#endif

namespace eqx {

namespace {

alignas(16) constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// Portable block function. The eight working variables rotate through the argument list instead of being moved.
void blocks_portable(std::uint32_t h[8], const unsigned char* p, std::size_t blocks) {
    for (; blocks > 0; --blocks, p += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            std::uint32_t v;
            std::memcpy(&v, p + 4 * i, 4);
            w[i] = __builtin_bswap32(v);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
#define EQX_SHA_ROUND(a, b, c, d, e, f, g, hh, i)                                                                \
    do {                                                                                                          \
        const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + (g ^ (e & (f ^ g))) + K[i] + w[i]; \
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) | (c & (a | b)));              \
        d += t1;                                                                                                  \
        hh = t1 + t2;                                                                                             \
    } while (0)
        for (int i = 0; i < 64; i += 8) {
            EQX_SHA_ROUND(a, b, c, d, e, f, g, hh, i);
            EQX_SHA_ROUND(hh, a, b, c, d, e, f, g, i + 1);
            EQX_SHA_ROUND(g, hh, a, b, c, d, e, f, i + 2);
            EQX_SHA_ROUND(f, g, hh, a, b, c, d, e, i + 3);
            EQX_SHA_ROUND(e, f, g, hh, a, b, c, d, i + 4);
            EQX_SHA_ROUND(d, e, f, g, hh, a, b, c, i + 5);
            EQX_SHA_ROUND(c, d, e, f, g, hh, a, b, i + 6);
            EQX_SHA_ROUND(b, c, d, e, f, g, hh, a, i + 7);
        }
#undef EQX_SHA_ROUND
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
}

#ifdef EQX_SHA_NI
// SHA extensions (Intel SHA-NI; also AMD since Zen): two rounds per sha256rnds2, the message schedule through
// sha256msg1/sha256msg2. About 30 times fewer instructions per byte than the portable code.
__attribute__((target("sha,sse4.1,ssse3")))
void blocks_sha_ni(std::uint32_t h[8], const unsigned char* p, std::size_t blocks) {
    const __m128i kBigEndian = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[0]));      // DCBA
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[4]));   // HGFE
    tmp = _mm_shuffle_epi32(tmp, 0xB1);                                         // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);                                   // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);                           // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);                                // CDGH
    for (; blocks > 0; --blocks, p += 64) {
        const __m128i save0 = state0, save1 = state1;
        __m128i m[4];
        for (int i = 0; i < 16; ++i) {   // four rounds each
            __m128i w;
            if (i < 4) {
                w = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), kBigEndian);
            } else {
                // W[t..t+3] from W[t-16..t-1]: sigma0 terms (msg1), W[t-7..t-4], sigma1 terms (msg2)
                const __m128i x0 = m[i & 3], x1 = m[(i + 1) & 3], x2 = m[(i + 2) & 3], x3 = m[(i + 3) & 3];
                w = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(x0, x1), _mm_alignr_epi8(x3, x2, 4)), x3);
            }
            m[i & 3] = w;
            __m128i wk = _mm_add_epi32(w, _mm_load_si128(reinterpret_cast<const __m128i*>(&K[4 * i])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, wk);
            wk = _mm_shuffle_epi32(wk, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, wk);
        }
        state0 = _mm_add_epi32(state0, save0);
        state1 = _mm_add_epi32(state1, save1);
    }
    tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[4]), state1);
}

bool cpu_has_sha_ni() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    const bool ssse3 = (c >> 9) & 1, sse41 = (c >> 19) & 1;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    const bool sha = (b >> 29) & 1;
    return ssse3 && sse41 && sha;
}
#endif

}  // namespace

Sha256::Sha256()
    : blocks_(blocks_portable),
      h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {
#ifdef EQX_SHA_NI
    static const bool sha_ni = cpu_has_sha_ni();
    if (sha_ni) blocks_ = blocks_sha_ni;
#endif
}

Sha256 Sha256::portable() {
    Sha256 s;
    s.blocks_ = blocks_portable;
    return s;
}

void Sha256::update(const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    total_ += n;
    if (blen_ > 0) {
        const std::size_t take = n < 64 - blen_ ? n : 64 - blen_;
        std::memcpy(buf_ + blen_, p, take);
        blen_ += take;
        p += take;
        n -= take;
        if (blen_ < 64) return;
        blocks_(h_, buf_, 1);
        blen_ = 0;
    }
    if (n >= 64) {
        blocks_(h_, p, n / 64);
        p += n - n % 64;
        n %= 64;
    }
    if (n > 0) {
        std::memcpy(buf_, p, n);
        blen_ = n;
    }
}

std::string Sha256::hex() {
    const std::uint64_t bits = total_ * 8;
    unsigned char pad[72] = {0x80};
    const std::size_t pad_len = (blen_ < 56 ? 56 : 120) - blen_;
    for (int i = 0; i < 8; ++i) pad[pad_len + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(pad, pad_len + 8);
    static const char hx[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 8; ++k) out[8 * i + k] = hx[(h_[i] >> (28 - 4 * k)) & 15];
    return out;
}

std::string sha256_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    constexpr std::size_t kChunk = std::size_t(1) << 16;
    const std::unique_ptr<unsigned char[]> buf(new unsigned char[kChunk]);
    Sha256 s;
    std::size_t n;
    while ((n = std::fread(buf.get(), 1, kChunk, f)) > 0) s.update(buf.get(), n);
    std::fclose(f);
    return s.hex();
}

}  // namespace eqx
