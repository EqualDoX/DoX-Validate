// Unit tests of the JSON string escaper (output must be valid JSON and one record per line).
#include <string>

#include "unit.h"
#include "util/json.h"

namespace {
std::string esc(std::string_view s) {
    std::string out;
    eqx::JsonWriter::escape_into(out, s);
    return out;
}
}  // namespace

EQX_TEST(util_json_escape_controls_and_separators) {
    EXPECT_EQ(esc("a\nb\"c\\"), std::string("\"a\\nb\\\"c\\\\\""));
    EXPECT_EQ(esc("\x01\x7f"), std::string("\"\\u0001\\u007f\""));
    EXPECT_EQ(esc("x\xC2\x85y"), std::string("\"x\\u0085y\""));          // NEL (C1 control)
    EXPECT_EQ(esc("\xE2\x80\xA8\xE2\x80\xA9"), std::string("\"\\u2028\\u2029\""));
    EXPECT_EQ(esc("\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80"), std::string("\"\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80\""));
}

EQX_TEST(util_json_escape_invalid_utf8) {
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_EQ(esc("\xC0\x80"), "\"" + fffd + fffd + "\"");                  // overlong 2-byte
    EXPECT_EQ(esc("\xED\xA0\x80"), "\"" + fffd + fffd + fffd + "\"");       // surrogate
    EXPECT_EQ(esc("\xF0\x80\x80\x80"), "\"" + fffd + fffd + fffd + fffd + "\"");   // overlong 4-byte
    EXPECT_EQ(esc("\xF5\x80\x80\x80"), "\"" + fffd + fffd + fffd + fffd + "\"");   // above U+10FFFF
    EXPECT_EQ(esc("\xE2\x82"), "\"" + fffd + fffd + "\"");                  // truncated
}
