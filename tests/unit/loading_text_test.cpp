// PDF text string decoding and UTF-8 output against their original implementation (kept here as the reference).
#include <random>
#include <string>

#include "unit.h"
#include "util/text.h"

using namespace eqx;

namespace {

char32_t ref_pdfdoc(unsigned char c) {
    static const char32_t hi[] = {
        0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212, 0x2030, 0x201E,
        0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017D,
        0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0x0000, 0x20AC};
    static const char32_t ctl[] = {0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};
    if (c >= 0x18 && c <= 0x1F) return ctl[c - 0x18];
    if (c == 0x7F) return 0;
    if (c >= 0x80 && c <= 0xA0) return hi[c - 0x80];
    if (c == 0xAD) return 0;
    return c;
}

std::u32string ref_pdf_text_to_u32(std::string_view raw) {
    std::u32string out;
    auto n = raw.size();
    auto b = [&](std::size_t i) { return static_cast<unsigned char>(raw[i]); };
    if (n >= 2 && b(0) == 0xFE && b(1) == 0xFF) {
        for (std::size_t i = 2; i + 1 < n; i += 2) {
            char32_t u = char32_t(b(i)) << 8 | b(i + 1);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                char32_t lo = char32_t(b(i + 2)) << 8 | b(i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    out.push_back(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            out.push_back(u);
        }
        return out;
    }
    if (n >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF) return utf8_to_u32(raw.substr(3));
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char32_t c = ref_pdfdoc(b(i));
        out.push_back(c == 0 && b(i) != 0 ? 0xFFFD : c);
    }
    return out;
}

std::string ref_to_utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

}  // namespace

EQX_TEST(pdf_text_decoding_matches_reference) {
    // every byte on its own and inside a string, in PDFDocEncoding
    for (int b = 0; b < 256; ++b) {
        const std::string one(1, static_cast<char>(b));
        EXPECT(pdf_text_to_u32(one) == ref_pdf_text_to_u32(one));
        const std::string in = "a" + one + "z";
        EXPECT(pdf_text_to_u32(in) == ref_pdf_text_to_u32(in));
        EXPECT_EQ(pdf_text_to_utf8(in), ref_to_utf8(ref_pdf_text_to_u32(in)));
    }
    std::mt19937 rng(7);
    for (int round = 0; round < 3000; ++round) {
        std::string s(rng() % 40, '\0');
        for (char& c : s) c = static_cast<char>(rng());
        switch (rng() % 4) {
            case 0: s = "\xFE\xFF" + s; break;       // UTF-16BE, also with lone and paired surrogates
            case 1: s = "\xEF\xBB\xBF" + s; break;   // UTF-8, also malformed
            case 2: s = "\xFF\xFE" + s; break;       // not a byte order mark for veraPDF
            default: break;
        }
        if (round % 5 == 0 && s.size() > 6) {        // a surrogate pair
            s[2] = '\xD8', s[3] = '\x3D', s[4] = '\xDE', s[5] = '\x00';
        }
        const std::u32string want = ref_pdf_text_to_u32(s);
        EXPECT(pdf_text_to_u32(s) == want);
        EXPECT_EQ(pdf_text_to_utf8(s), ref_to_utf8(want));
        EXPECT_EQ(to_utf8(want), ref_to_utf8(want));
    }
    const std::u32string edge = {0, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF, 0x110000, 0xD800, 0xFFFFFFFF};
    EXPECT_EQ(to_utf8(edge), ref_to_utf8(edge));
    EXPECT(pdf_text_to_u32("").empty());
}
