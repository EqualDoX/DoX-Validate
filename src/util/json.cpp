#include "util/json.h"

#include <cmath>
#include <cstdio>

namespace eqx {

void JsonWriter::newline() {
    if (!pretty_) return;
    out_.push_back('\n');
    out_.append(first_.size(), ' ');
}

void JsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_.push_back(',');
        first_.back() = false;
        newline();
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    bool empty = first_.back();
    first_.pop_back();
    if (!empty) newline();
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    bool empty = first_.back();
    first_.pop_back();
    if (!empty) newline();
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
    before_value();
    escape_into(out_, k);
    out_.push_back(':');
    if (pretty_) out_.push_back(' ');
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view s) {
    before_value();
    escape_into(out_, s);
    return *this;
}

JsonWriter& JsonWriter::value(bool b) {
    before_value();
    out_.append(b ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t n) {
    before_value();
    out_.append(std::to_string(n));
    return *this;
}

JsonWriter& JsonWriter::value(double d) {
    before_value();
    if (!std::isfinite(d)) {
        out_.append("null");
        return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    out_.append(buf);
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_.append("null");
    return *this;
}

JsonWriter& JsonWriter::raw(std::string_view json) {
    before_value();
    out_.append(json);
    return *this;
}

void JsonWriter::escape_into(std::string& out, std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                case '\b': out.append("\\b"); break;
                case '\f': out.append("\\f"); break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        out.append("\\u00");
                        out.push_back(hex[c >> 4]);
                        out.push_back(hex[c & 15]);
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
            ++i;
            continue;
        }
        // Validate one UTF-8 sequence.
        int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; ++k)
            ok = (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
        unsigned cp = 0;
        if (ok) {
            cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            ok = (len == 2 && cp >= 0x80) || (len == 3 && cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) ||
                 (len == 4 && cp >= 0x10000 && cp <= 0x10FFFF);
        }
        if (ok && ((cp >= 0x80 && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029)) {
            // C1 controls and the Unicode line/paragraph separators are escaped: valid JSON either way, but
            // line-based readers (Python's str.splitlines, JavaScript) would split a JSON-Lines record there.
            out.append("\\u");
            for (int shift = 12; shift >= 0; shift -= 4) out.push_back(hex[(cp >> shift) & 15]);
            i += len;
        } else if (ok) {
            out.append(s.substr(i, len));
            i += len;
        } else {
            out.append("\xEF\xBF\xBD");
            ++i;
        }
    }
    out.push_back('"');
}

}  // namespace eqx
