#include "util/text.h"

namespace eqx {

namespace {

// PDFDocEncoding (ISO 32000-2 Annex D, Table D.2): code -> Unicode. 0 marks an undefined code; codes
// 0x00-0x17 and 0x20-0x7E map to themselves except where listed.
char32_t pdfdoc(unsigned char c) {
    static const char32_t hi[] = {
        // 0x80 .. 0xA0
        0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212, 0x2030, 0x201E,
        0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017D,
        0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0x0000, 0x20AC};
    static const char32_t ctl[] = {  // 0x18 .. 0x1F
        0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};
    if (c >= 0x18 && c <= 0x1F) return ctl[c - 0x18];
    if (c == 0x7F) return 0;
    if (c >= 0x80 && c <= 0xA0) return hi[c - 0x80];
    if (c == 0xAD) return 0;
    return c;  // 0x00-0x17, 0x20-0x7E, 0xA1-0xFF (Latin-1)
}

}  // namespace

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        append_utf8(out, 0xFFFD);
    }
}

std::string to_utf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

std::u32string utf8_to_u32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::u32string pdf_text_to_u32(std::string_view raw) {
    std::u32string out;
    auto n = raw.size();
    auto b = [&](std::size_t i) { return static_cast<unsigned char>(raw[i]); };
    // veraPDF COSString.getString: only FE FF selects UTF-16 (big-endian); FF FE is not supported and the
    // bytes are read as PDFDocEncoding.
    if (n >= 2 && b(0) == 0xFE && b(1) == 0xFF) {
        const bool be = true;
        for (std::size_t i = 2; i + 1 < n; i += 2) {
            char32_t u = be ? (char32_t(b(i)) << 8 | b(i + 1)) : (char32_t(b(i + 1)) << 8 | b(i));
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                char32_t lo = be ? (char32_t(b(i + 2)) << 8 | b(i + 3)) : (char32_t(b(i + 3)) << 8 | b(i + 2));
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
        char32_t c = pdfdoc(b(i));
        out.push_back(c == 0 && b(i) != 0 ? 0xFFFD : c);
    }
    return out;
}

std::string pdf_text_to_utf8(std::string_view raw) { return to_utf8(pdf_text_to_u32(raw)); }

bool is_unicode_space(char32_t c) {
    switch (c) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x85: case 0xA0:
        case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

bool is_whitespace_only(std::u32string_view s) {
    for (char32_t c : s)
        if (!is_unicode_space(c)) return false;
    return true;
}

std::string trim_ascii(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r' || s[b] == '\f' || s[b] == '\0')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == '\f' || s[e - 1] == '\0')) --e;
    return std::string(s.substr(b, e - b));
}

}  // namespace eqx
