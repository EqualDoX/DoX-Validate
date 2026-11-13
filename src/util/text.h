// Text helpers: PDF text strings, UTF-8 encoding, code point classification.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eqx {

// Decodes a PDF text string (ISO 32000-2 7.9.2.2) to UTF-8 the way veraPDF's COSString.getString does:
// FE FF -> UTF-16BE, EF BB BF -> UTF-8, otherwise (also FF FE) PDFDocEncoding.
std::string pdf_text_to_utf8(std::string_view raw);

// Same, as code points.
std::u32string pdf_text_to_u32(std::string_view raw);

void append_utf8(std::string& out, char32_t cp);
std::string to_utf8(const std::u32string& s);
std::u32string utf8_to_u32(std::string_view s);

// Private use areas (BMP U+E000-F8FF and planes 15/16), the definition veraPDF uses for PUA checks.
inline bool is_pua(char32_t c) {
    return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
}

// Unicode White_Space property.
bool is_unicode_space(char32_t c);

// True when every code point is white space (an empty string is whitespace-only).
bool is_whitespace_only(std::u32string_view s);

std::string trim_ascii(std::string_view s);

}  // namespace eqx
