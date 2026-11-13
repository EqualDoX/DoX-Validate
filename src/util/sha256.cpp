#include "util/sha256.h"

#include <qpdf/QPDFCryptoImpl.hh>
#include <qpdf/QPDFCryptoProvider.hh>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace eqx {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    std::size_t blen = 0;
    std::uint64_t total = 0;

    void block(const unsigned char* p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const unsigned char* p, std::size_t n) {
        total += n;
        while (n > 0) {
            std::size_t take = std::min<std::size_t>(64 - blen, n);
            std::memcpy(buf + blen, p, take);
            blen += take;
            p += take;
            n -= take;
            if (blen == 64) {
                block(buf);
                blen = 0;
            }
        }
    }
    std::string hex() {
        std::uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (blen != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (std::uint32_t v : h)
            for (int s = 28; s >= 0; s -= 4) out.push_back(hx[(v >> s) & 15]);
        return out;
    }
};

}  // namespace

std::string sha256_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::vector<unsigned char> buf(1 << 20);
    std::size_t n;
    // qpdf's crypto provider (gnutls / OpenSSL / native) is several times faster than the portable code.
    try {
        auto impl = QPDFCryptoProvider::getImpl();
        impl->SHA2_init(256);
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) impl->SHA2_update(buf.data(), n);
        std::fclose(f);
        impl->SHA2_finalize();
        std::string raw = impl->SHA2_digest();
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (unsigned char c : raw) {
            out.push_back(hx[c >> 4]);
            out.push_back(hx[c & 15]);
        }
        return out;
    } catch (std::exception&) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) return {};
    }
    Sha256 s;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) s.update(buf.data(), n);
    std::fclose(f);
    return s.hex();
}

}  // namespace eqx
