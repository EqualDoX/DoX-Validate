#include "fonts/util.h"

#include <qpdf/Pl_Flate.hh>
#include <zlib.h>
#include <qpdf/Pl_String.hh>

#include <cmath>
#include <cstdlib>
#include <limits>

namespace eqx {

std::optional<std::int64_t> java_parse_long(std::string_view s, int radix) {
    if (s.empty() || radix < 2 || radix > 36) return std::nullopt;
    unsigned __int128 v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return std::nullopt;
        if (d >= radix) return std::nullopt;
        v = v * static_cast<unsigned>(radix) + static_cast<unsigned>(d);
        if (v > static_cast<unsigned __int128>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    }
    return static_cast<std::int64_t>(v);
}

std::optional<double> java_parse_double_digits(std::string_view s) {
    int dots = 0, digits = 0;
    for (char c : s) {
        if (c == '.') ++dots;
        else if (c >= '0' && c <= '9') ++digits;
        else return std::nullopt;
    }
    if (dots > 1 || digits == 0) return std::nullopt;
    std::string t(s);
    return std::strtod(t.c_str(), nullptr);
}

std::optional<float> java_parse_float(std::string_view s) {
    std::size_t i = 0, n = s.size();
    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
    std::size_t int_digits = 0, frac_digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++int_digits;
    if (i < n && s[i] == '.') {
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++frac_digits;
    }
    if (int_digits + frac_digits == 0) return std::nullopt;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        std::size_t e = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++e;
        if (e == 0) return std::nullopt;
    }
    if (i != n) return std::nullopt;
    std::string t(s);
    return std::strtof(t.c_str(), nullptr);
}

std::int64_t java_round(double v) {
    if (std::isnan(v)) return 0;
    double f = std::floor(v + 0.5);
    return java_d2l(f);
}

std::int64_t java_d2l(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 9.2233720368547758e18) return std::numeric_limits<std::int64_t>::max();
    if (v <= -9.2233720368547758e18) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(v);
}

std::int32_t java_d2i(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return std::numeric_limits<std::int32_t>::max();
    if (v <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(v);
}

std::int32_t java_f2i(float v) { return java_d2i(static_cast<double>(v)); }

bool java_double_equals(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == 0 && b == 0) return std::signbit(a) == std::signbit(b);
    return a == b;
}

std::string ascii85_decode(std::string_view in) {
    std::string out;
    std::uint32_t tuple = 0;
    int count = 0;
    for (char ch : in) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + (c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((tuple >> (8 * k)) & 0xFF));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
        for (int k = 0; k < count - 1; ++k) out.push_back(static_cast<char>((tuple >> (8 * (3 - k))) & 0xFF));
    }
    return out;
}

std::u16string utf16be_decode(std::string_view b) {
    std::u16string out;
    out.reserve(b.size() / 2 + 1);
    std::size_t n = b.size() / 2;
    auto unit = [&](std::size_t k) -> char16_t {
        return static_cast<char16_t>((static_cast<unsigned char>(b[2 * k]) << 8) | static_cast<unsigned char>(b[2 * k + 1]));
    };
    for (std::size_t k = 0; k < n; ++k) {
        char16_t u = unit(k);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (k + 1 < n) {
                char16_t l = unit(k + 1);
                if (l >= 0xDC00 && l <= 0xDFFF) {
                    out.push_back(u);
                    out.push_back(l);
                    ++k;
                    continue;
                }
            }
            out.push_back(0xFFFD);
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(u);
        }
    }
    if (b.size() % 2) out.push_back(0xFFFD);
    return out;
}

std::u16string latin1_to_u16(std::string_view bytes) {
    std::u16string out;
    out.reserve(bytes.size());
    for (char c : bytes) out.push_back(static_cast<unsigned char>(c));
    return out;
}

std::u32string u16_to_u32(const std::u16string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t k = 0; k < s.size(); ++k) {
        char16_t u = s[k];
        if (u >= 0xD800 && u <= 0xDBFF && k + 1 < s.size() && s[k + 1] >= 0xDC00 && s[k + 1] <= 0xDFFF) {
            out.push_back(0x10000 + ((static_cast<char32_t>(u) - 0xD800) << 10) + (s[k + 1] - 0xDC00));
            ++k;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(u);
        }
    }
    return out;
}

std::string u16_to_utf8(const std::u16string& s) {
    std::string out;
    for (char32_t c : u16_to_u32(s)) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::string inflate_prefix(std::string_view data, std::size_t limit) {
    std::string out(limit, '\0');
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(limit);
    ::inflate(&zs, Z_PARTIAL_FLUSH);
    out.resize(limit - zs.avail_out);
    inflateEnd(&zs);
    return out;
}

std::optional<std::string> inflate(std::string_view data, std::size_t size_hint) {
    std::string out;
    out.reserve(size_hint);
    try {
        Pl_String sink("eqx-font-data", nullptr, out);
        Pl_Flate fl("eqx-font-inflate", &sink, Pl_Flate::a_inflate);
        fl.write(reinterpret_cast<unsigned char const*>(data.data()), data.size());
        fl.finish();
    } catch (std::exception&) {
        return std::nullopt;
    }
    return out;
}

}  // namespace eqx
