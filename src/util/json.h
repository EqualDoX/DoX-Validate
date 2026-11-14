// Minimal streaming JSON writer (RFC 8259). Strings are emitted as valid UTF-8: bytes that are not
// well-formed UTF-8 (raw PDF names, broken strings) are replaced by U+FFFD.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eqx {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view k);
    JsonWriter& value(std::string_view s);
    JsonWriter& value(const char* s) { return value(std::string_view(s)); }
    JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
    JsonWriter& value(bool b);
    JsonWriter& value(std::int64_t n);
    JsonWriter& value(int n) { return value(static_cast<std::int64_t>(n)); }
    JsonWriter& value(std::size_t n) { return value(static_cast<std::int64_t>(n)); }
    JsonWriter& value(double d);
    JsonWriter& null();
    // Inserts an already serialised JSON value verbatim. In compact output it may also be several array elements
    // joined by commas ("a","b","c"): they are written as they are.
    JsonWriter& raw(std::string_view json);
    // Room for a document of about this size (the report writer knows what it is going to write).
    void reserve(std::size_t bytes) { out_.reserve(bytes); }
    bool pretty() const { return pretty_; }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

    static void escape_into(std::string& out, std::string_view s);

private:
    void before_value();
    void newline();
    std::string out_;
    std::vector<bool> first_;   // per open container: no element written yet
    bool after_key_ = false;
    bool pretty_;
};

}  // namespace eqx
