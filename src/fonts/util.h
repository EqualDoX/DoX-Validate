// Java numeric / string semantics the font parsers reproduce (veraPDF is Java), plus small helpers.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eqx {

// Long.valueOf(s, radix): digits only (no sign handled here: the PostScript lexer handles signs).
std::optional<std::int64_t> java_parse_long(std::string_view s, int radix);
// Double.parseDouble on a string of digits and '.' (as produced by BaseParser.readNumber).
std::optional<double> java_parse_double_digits(std::string_view s);
// Float.parseFloat on CFF real nibble strings ("-1.5E-3").
std::optional<float> java_parse_float(std::string_view s);
std::int64_t java_round(double v);          // Math.round(double)
std::int64_t java_d2l(double v);            // (long) d
std::int32_t java_d2i(double v);            // (int) d
std::int32_t java_f2i(float v);             // (int) f
bool java_double_equals(double a, double b);   // Double.equals

std::string ascii85_decode(std::string_view in);

// Java String (UTF-16) helpers.
std::u16string utf16be_decode(std::string_view bytes);   // new String(bytes, UTF_16BE)
std::u16string latin1_to_u16(std::string_view bytes);    // new String(bytes, ISO_8859_1)
std::u32string u16_to_u32(const std::u16string& s);      // lone surrogates -> U+FFFD
std::string u16_to_utf8(const std::u16string& s);

// zlib inflate (qpdf Pl_Flate); nullopt on failure.
std::optional<std::string> inflate(std::string_view data, std::size_t size_hint = 0);
// One zlib inflate call over possibly incomplete input (java.util.zip.Inflater.inflate: Z_PARTIAL_FLUSH): every
// byte decodable from `data`, at most `limit` bytes.
std::string inflate_prefix(std::string_view data, std::size_t limit);

}  // namespace eqx
