// Portions of this file (bytebuffer_encoding, latin1_convert, is_control and fix_controls, marked below) are a
// C++ translation of the Adobe XMP Toolkit for Java as distributed in veraPDF 1.30.2 (verapdf-xmp-core: ByteBuffer,
// Latin1Converter, Utils, FixASCIIControlsReader), Copyright 2006 Adobe Systems Incorporated, BSD 3-Clause License
// (LICENSES/BSD-3-Clause.txt). The rest of the file is EqualDoX's own work.
#include "rules/document/xml_dom.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "util/text.h"

namespace eqx::xml {

namespace {

using u32 = std::u32string;

constexpr std::size_t kMaxDepth = 512;   // deeper nesting is rejected (no real packet comes close)

bool is_xml_char(char32_t c) {
    return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) ||
           (c >= 0x10000 && c <= 0x10FFFF);
}
bool is_space(char32_t c) { return c == 0x20 || c == 0x9 || c == 0xD || c == 0xA; }

bool name_start(char32_t c) {
    return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) ||
           (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
           (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
           (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
           (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}
bool name_char(char32_t c) {
    return name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
           (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

std::string to8(const u32& s, std::size_t b, std::size_t e) {
    std::string o;
    o.reserve(e - b);
    for (std::size_t i = b; i < e; ++i) append_utf8(o, s[i]);
    return o;
}

// ------------------------------------------------------------------------------------------- decoding
enum class Enc { UTF8, UTF16BE, UTF16LE, UTF32BE, UTF32LE, Latin1, ASCII, CP1252, Unsupported };

const char32_t kCp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

unsigned char ub(std::string_view b, std::size_t i) { return static_cast<unsigned char>(b[i]); }

// Strict decoders (Xerces' readers): malformed input rejects the packet.
bool decode_strict(std::string_view b, std::size_t i, Enc enc, u32& out) {
    out.clear();
    const std::size_t n = b.size();
    switch (enc) {
        case Enc::UTF8:
            while (i < n) {
                unsigned c = ub(b, i);
                if (c < 0x80) {
                    out.push_back(c);
                    ++i;
                    continue;
                }
                int extra;
                char32_t cp, min;
                if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
                else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
                else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
                else return false;
                for (int k = 1; k <= extra; ++k) {
                    if (i + k >= n || (ub(b, i + k) & 0xC0) != 0x80) return false;
                    cp = (cp << 6) | (ub(b, i + k) & 0x3F);
                }
                if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
                out.push_back(cp);
                i += extra + 1;
            }
            return true;
        case Enc::UTF16BE:
        case Enc::UTF16LE: {
            if ((n - i) % 2) return false;
            bool be = enc == Enc::UTF16BE;
            while (i + 1 < n) {
                char32_t u = be ? (ub(b, i) << 8 | ub(b, i + 1)) : (ub(b, i + 1) << 8 | ub(b, i));
                i += 2;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    if (i + 1 >= n) return false;
                    char32_t l = be ? (ub(b, i) << 8 | ub(b, i + 1)) : (ub(b, i + 1) << 8 | ub(b, i));
                    if (l < 0xDC00 || l > 0xDFFF) return false;
                    i += 2;
                    u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                } else if (u >= 0xDC00 && u <= 0xDFFF) {
                    return false;
                }
                out.push_back(u);
            }
            return true;
        }
        case Enc::UTF32BE:
        case Enc::UTF32LE: {
            if ((n - i) % 4) return false;
            bool be = enc == Enc::UTF32BE;
            for (; i + 3 < n; i += 4) {
                char32_t u = be ? (char32_t(ub(b, i)) << 24 | ub(b, i + 1) << 16 | ub(b, i + 2) << 8 | ub(b, i + 3))
                                : (char32_t(ub(b, i + 3)) << 24 | ub(b, i + 2) << 16 | ub(b, i + 1) << 8 | ub(b, i));
                if (u > 0x10FFFF) return false;
                out.push_back(u);
            }
            return true;
        }
        case Enc::Latin1:
            for (; i < n; ++i) out.push_back(ub(b, i));
            return true;
        case Enc::ASCII:
            for (; i < n; ++i) {
                if (ub(b, i) >= 0x80) return false;
                out.push_back(ub(b, i));
            }
            return true;
        case Enc::CP1252:
            for (; i < n; ++i) {
                unsigned c = ub(b, i);
                out.push_back(c >= 0x80 && c <= 0x9F ? kCp1252[c - 0x80] : c);
            }
            return true;
        case Enc::Unsupported:
            return false;
    }
    return false;
}

std::string ascii_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

Enc encoding_by_name(const std::string& name) {
    std::string n = ascii_lower(name);
    if (n == "utf-8" || n == "utf8") return Enc::UTF8;
    if (n == "iso-8859-1" || n == "iso8859-1" || n == "iso_8859-1" || n == "latin1" || n == "l1" || n == "cp819" ||
        n == "ibm819" || n == "iso8859_1" || n == "iso-ir-100" || n == "csisolatin1")
        return Enc::Latin1;
    if (n == "us-ascii" || n == "ascii" || n == "iso646-us" || n == "us") return Enc::ASCII;
    if (n == "windows-1252" || n == "cp1252") return Enc::CP1252;
    return Enc::Unsupported;
}

// Value of the encoding pseudo-attribute of an XML declaration at the start of an ASCII-compatible byte stream.
std::optional<std::string> decl_encoding(std::string_view b, std::size_t i) {
    if (b.compare(i, 5, "<?xml") != 0) return std::nullopt;
    std::size_t end = b.find("?>", i);
    if (end == std::string_view::npos) return std::nullopt;
    std::string_view d = b.substr(i + 5, end - i - 5);
    std::size_t k = d.find("encoding");
    if (k == std::string_view::npos) return std::nullopt;
    k += 8;
    while (k < d.size() && is_space(static_cast<unsigned char>(d[k]))) ++k;
    if (k >= d.size() || d[k] != '=') return std::nullopt;
    ++k;
    while (k < d.size() && is_space(static_cast<unsigned char>(d[k]))) ++k;
    if (k >= d.size() || (d[k] != '"' && d[k] != '\'')) return std::nullopt;
    char q = d[k++];
    std::size_t e = d.find(q, k);
    if (e == std::string_view::npos) return std::nullopt;
    return std::string(d.substr(k, e - k));
}

// Xerces XMLEntityManager: encoding from the BOM / first bytes, then the XML declaration.
bool decode_attempt1(std::string_view b, u32& out, std::string& enc_name) {
    std::size_t n = b.size();
    auto at = [&](std::size_t i) -> int { return i < n ? ub(b, i) : -1; };
    if (at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
        enc_name = "UTF-8";
        if (!decode_strict(b, 3, Enc::UTF8, out)) return false;
        // the declaration may still name another ASCII-compatible encoding; Xerces then reports a conflict
        // only for non-UTF-8 names (treated as rejection here)
        if (auto e = decl_encoding(b, 3); e && encoding_by_name(*e) != Enc::UTF8) return false;
        return true;
    }
    if (at(0) == 0x00 && at(1) == 0x00 && at(2) == 0xFE && at(3) == 0xFF) {
        enc_name = "UTF-32BE";
        return decode_strict(b, 4, Enc::UTF32BE, out);
    }
    if (at(0) == 0xFF && at(1) == 0xFE && at(2) == 0x00 && at(3) == 0x00) {
        enc_name = "UTF-32LE";
        return decode_strict(b, 4, Enc::UTF32LE, out);
    }
    if (at(0) == 0xFE && at(1) == 0xFF) {
        enc_name = "UTF-16BE";
        return decode_strict(b, 2, Enc::UTF16BE, out);
    }
    if (at(0) == 0xFF && at(1) == 0xFE) {
        enc_name = "UTF-16LE";
        return decode_strict(b, 2, Enc::UTF16LE, out);
    }
    if (at(0) == 0x00 && at(1) == 0x00 && at(2) == 0x00 && at(3) == 0x3C) {
        enc_name = "UTF-32BE";
        return decode_strict(b, 0, Enc::UTF32BE, out);
    }
    if (at(0) == 0x3C && at(1) == 0x00 && at(2) == 0x00 && at(3) == 0x00) {
        enc_name = "UTF-32LE";
        return decode_strict(b, 0, Enc::UTF32LE, out);
    }
    if (at(0) == 0x00 && at(1) == 0x3C && at(2) == 0x00 && at(3) == 0x3F) {
        enc_name = "UTF-16BE";
        return decode_strict(b, 0, Enc::UTF16BE, out);
    }
    if (at(0) == 0x3C && at(1) == 0x00 && at(2) == 0x3F && at(3) == 0x00) {
        enc_name = "UTF-16LE";
        return decode_strict(b, 0, Enc::UTF16LE, out);
    }
    if (at(0) == 0x4C && at(1) == 0x6F && at(2) == 0xA7 && at(3) == 0x94) return false;   // EBCDIC
    Enc enc = Enc::UTF8;
    enc_name = "UTF-8";
    if (auto e = decl_encoding(b, 0)) {
        enc = encoding_by_name(*e);
        enc_name = *e;
        if (enc == Enc::Unsupported) return false;
    }
    return decode_strict(b, 0, enc, out);
}

// --------------------------------------------------------------- second attempt (XMPMetaParser fallback)
// [BSD-3-Clause portion, Adobe XMP Toolkit] org.verapdf.xmp.impl.ByteBuffer.getEncoding
std::string bytebuffer_encoding(std::string_view b) {
    std::size_t n = b.size();
    if (n < 2) return "UTF-8";
    if (ub(b, 0) == 0) {
        if (n < 4 || ub(b, 1) != 0) return "UTF-16BE";
        if (ub(b, 2) == 0xFE && ub(b, 3) == 0xFF) return "UTF-32BE";
        return "UTF-32";
    }
    if (ub(b, 0) < 0x80) {
        if (ub(b, 1) != 0) return "UTF-8";
        if (n < 4 || ub(b, 2) != 0) return "UTF-16LE";
        return "UTF-32LE";
    }
    if (ub(b, 0) == 0xEF) return "UTF-8";
    if (ub(b, 0) == 0xFE) return "UTF-16";
    if (n < 4 || ub(b, 2) != 0) return "UTF-16";
    return "UTF-32";
}

void append_cp1252_utf8(std::string& out, unsigned char c) {
    // Latin1Converter.convertToUTF8: undefined cp1252 codes become a space
    if (c == 0x81 || c == 0x8D || c == 0x8F || c == 0x90 || c == 0x9D) {
        out.push_back(' ');
        return;
    }
    append_utf8(out, c >= 0x80 && c <= 0x9F ? kCp1252[c - 0x80] : c);
}

// [BSD-3-Clause portion, Adobe XMP Toolkit] org.verapdf.xmp.impl.Latin1Converter.convert (only applied to
// "UTF-8" buffers)
std::string latin1_convert(std::string_view b) {
    std::string out;
    out.reserve(b.size() + b.size() / 3);
    unsigned char ahead[8];
    int read_ahead = 0, expected = 0;
    bool in_char = false;
    for (std::size_t i = 0; i < b.size(); ++i) {
        unsigned char c = ub(b, i);
        if (!in_char) {
            if (c < 0x7F) {
                out.push_back(static_cast<char>(c));
            } else if (c >= 0xC0) {
                expected = -1;
                for (unsigned t = c; expected < 8 && (t & 0x80); t <<= 1) ++expected;
                ahead[read_ahead++] = c;
                in_char = true;
            } else {
                append_cp1252_utf8(out, c);   // 0x7F..0xBF (0x7F itself is passed through as a code)
            }
        } else {
            if (expected > 0 && (c & 0xC0) == 0x80) {
                ahead[read_ahead++] = c;
                if (--expected == 0) {
                    out.append(reinterpret_cast<const char*>(ahead), read_ahead);
                    read_ahead = 0;
                    in_char = false;
                }
            } else {
                append_cp1252_utf8(out, ahead[0]);
                i = i - read_ahead;
                read_ahead = 0;
                in_char = false;
            }
        }
        if (read_ahead >= 8) {   // defensive: never overrun the read-ahead buffer
            append_cp1252_utf8(out, ahead[0]);
            i = i - read_ahead + 1;
            read_ahead = 0;
            in_char = false;
        }
    }
    if (in_char)
        for (int j = 0; j < read_ahead; ++j) append_cp1252_utf8(out, ahead[j]);
    return out;
}

// java.io.InputStreamReader: malformed input becomes U+FFFD.
u32 decode_lenient(std::string_view b, const std::string& enc) {
    u32 out;
    std::size_t n = b.size(), i = 0;
    if (enc == "UTF-8") return utf8_to_u32(b);
    bool utf32 = enc.rfind("UTF-32", 0) == 0;
    bool be = true;
    if (enc == "UTF-16" || enc == "UTF-32") {   // BOM detection, default big-endian, BOM removed
        if (!utf32 && n >= 2 && ub(b, 0) == 0xFF && ub(b, 1) == 0xFE) { be = false; i = 2; }
        else if (!utf32 && n >= 2 && ub(b, 0) == 0xFE && ub(b, 1) == 0xFF) i = 2;
        else if (utf32 && n >= 4 && ub(b, 0) == 0 && ub(b, 1) == 0 && ub(b, 2) == 0xFE && ub(b, 3) == 0xFF) i = 4;
        else if (utf32 && n >= 4 && ub(b, 0) == 0xFF && ub(b, 1) == 0xFE && ub(b, 2) == 0 && ub(b, 3) == 0) {
            be = false;
            i = 4;
        }
    } else {
        be = enc.find("LE") == std::string::npos;
    }
    if (utf32) {
        for (; i + 3 < n; i += 4) {
            char32_t u = be ? (char32_t(ub(b, i)) << 24 | ub(b, i + 1) << 16 | ub(b, i + 2) << 8 | ub(b, i + 3))
                            : (char32_t(ub(b, i + 3)) << 24 | ub(b, i + 2) << 16 | ub(b, i + 1) << 8 | ub(b, i));
            out.push_back(u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFD : u);
        }
        if (i < n) out.push_back(0xFFFD);
        return out;
    }
    while (i + 1 < n) {
        char32_t u = be ? (ub(b, i) << 8 | ub(b, i + 1)) : (ub(b, i + 1) << 8 | ub(b, i));
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            char32_t l = be ? (ub(b, i) << 8 | ub(b, i + 1)) : (ub(b, i + 1) << 8 | ub(b, i));
            if (l >= 0xDC00 && l <= 0xDFFF) {
                i += 2;
                out.push_back(0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00));
                continue;
            }
        }
        out.push_back(u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
    }
    if (i < n) out.push_back(0xFFFD);
    return out;
}

// [BSD-3-Clause portion, Adobe XMP Toolkit] org.verapdf.xmp.impl.Utils.isControlChar (on a Java char)
bool is_control(char32_t c) {
    return (c <= 0x1F || c == 0x7F) && c != 0x09 && c != 0x0A && c != 0x0D;
}

// [BSD-3-Clause portion, Adobe XMP Toolkit] org.verapdf.xmp.impl.FixASCIIControlsReader: control characters and character references to control
// characters (up to 5 decimal / 4 hex digits) become a space.
u32 fix_controls(const u32& in) {
    u32 out;
    out.reserve(in.size());
    std::size_t n = in.size();
    for (std::size_t i = 0; i < n; ++i) {
        char32_t c = in[i];
        if (c == '&' && i + 1 < n && in[i + 1] == '#') {
            std::size_t j = i + 2;
            std::uint32_t value = 0;
            bool ok = false;
            if (j < n && in[j] == 'x') {
                ++j;
                int digits = 0;
                while (j < n && digits <= 4) {
                    char32_t h = in[j];
                    int d = (h >= '0' && h <= '9') ? int(h - '0') : (h >= 'a' && h <= 'f') ? int(h - 'a' + 10)
                            : (h >= 'A' && h <= 'F') ? int(h - 'A' + 10) : -1;
                    if (d < 0) break;
                    value = value * 16 + d;
                    ++digits;
                    ++j;
                }
                ok = digits <= 4 && j < n && in[j] == ';';
            } else if (j < n && in[j] >= '0' && in[j] <= '9') {
                int digits = 0;
                while (j < n && in[j] >= '0' && in[j] <= '9' && digits <= 5) {
                    value = value * 10 + (in[j] - '0');
                    ++digits;
                    ++j;
                }
                ok = digits <= 5 && j < n && in[j] == ';';
            }
            if (ok && is_control(value & 0xFFFF)) {
                out.push_back(' ');
                i = j;
                continue;
            }
        }
        out.push_back(is_control(c) ? U' ' : c);
    }
    return out;
}

// ------------------------------------------------------------------------------------------- parsing
struct Binding {
    std::string prefix, uri;
};

class Parser {
public:
    Parser(const u32& s, Document& doc) : s_(s), doc_(doc) {}

    bool run() {
        if (starts("<?xml") && p_ + 5 < s_.size() && (is_space(s_[p_ + 5]) || s_[p_ + 5] == '?')) {
            if (!xml_decl()) return false;
        }
        bindings_.push_back({"xml", kNsXml});
        bool root_seen = false;
        while (true) {
            skip_ws();
            if (eof()) break;
            if (starts("<?")) {
                Node pi;
                if (!processing_instruction(pi)) return false;
                doc_.children.push_back(std::move(pi));
            } else if (starts("<!--")) {
                if (!comment()) return false;
            } else if (starts("<!DOCTYPE")) {
                return fail("DOCTYPE is disallowed");
            } else if (!root_seen && s_[p_] == '<' && p_ + 1 < s_.size() && name_start(s_[p_ + 1])) {
                Node root;
                if (!element(root)) return false;
                doc_.children.push_back(std::move(root));
                root_seen = true;
            } else {
                return fail(root_seen ? "content after the root element" : "content is not allowed in prolog");
            }
        }
        if (!root_seen) return fail("premature end of file");
        return true;
    }

    const std::string& error() const { return err_; }

private:
    const u32& s_;
    Document& doc_;
    std::size_t p_ = 0;
    std::string err_;
    std::vector<Binding> bindings_;

    bool eof() const { return p_ >= s_.size(); }
    bool starts(const char* lit) const {
        std::size_t i = p_;
        for (; *lit; ++lit, ++i)
            if (i >= s_.size() || s_[i] != static_cast<unsigned char>(*lit)) return false;
        return true;
    }
    bool fail(const std::string& m) {
        if (err_.empty()) err_ = m + " (offset " + std::to_string(p_) + ")";
        return false;
    }
    bool skip_ws() {
        std::size_t b = p_;
        while (!eof() && is_space(s_[p_])) ++p_;
        return p_ > b;
    }
    bool name(std::size_t& b, std::size_t& e) {
        if (eof() || !name_start(s_[p_])) return false;
        b = p_++;
        while (!eof() && name_char(s_[p_])) ++p_;
        e = p_;
        return true;
    }
    // Namespace-aware QName check (Xerces): at most one colon, neither part empty, local part an NCName.
    bool split_qname(std::size_t b, std::size_t e, std::string& prefix, std::string& local) {
        std::size_t colon = e;
        for (std::size_t i = b; i < e; ++i)
            if (s_[i] == ':') {
                if (colon != e) return false;
                colon = i;
            }
        if (colon == e) {
            prefix.clear();
            local = to8(s_, b, e);
            return true;
        }
        if (colon == b || colon + 1 == e || !name_start(s_[colon + 1])) return false;
        prefix = to8(s_, b, colon);
        local = to8(s_, colon + 1, e);
        return true;
    }

    bool xml_decl() {
        p_ += 5;
        bool has_version = false;
        while (true) {
            bool ws = skip_ws();
            if (starts("?>")) {
                p_ += 2;
                break;
            }
            if (!ws) return fail("malformed XML declaration");
            std::size_t b, e;
            if (!name(b, e)) return fail("malformed XML declaration");
            std::string key = to8(s_, b, e);
            skip_ws();
            if (eof() || s_[p_] != '=') return fail("malformed XML declaration");
            ++p_;
            skip_ws();
            if (eof() || (s_[p_] != '"' && s_[p_] != '\'')) return fail("malformed XML declaration");
            char32_t q = s_[p_++];
            std::size_t vb = p_;
            while (!eof() && s_[p_] != q) ++p_;
            if (eof()) return fail("malformed XML declaration");
            std::string v = to8(s_, vb, p_);
            ++p_;
            if (key == "version") {
                if (v.size() < 3 || v.compare(0, 2, "1.") != 0 ||
                    !std::all_of(v.begin() + 2, v.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return fail("invalid XML version");
                has_version = true;
            } else if (key == "encoding") {
                if (!has_version || v.empty()) return fail("malformed XML declaration");
            } else if (key == "standalone") {
                if (!has_version || (v != "yes" && v != "no")) return fail("malformed XML declaration");
            } else {
                return fail("malformed XML declaration");
            }
        }
        if (!has_version) return fail("XML declaration without version");
        return true;
    }

    bool comment() {   // at "<!--"
        p_ += 4;
        while (true) {
            if (eof()) return fail("unterminated comment");
            if (s_[p_] == '-' && p_ + 1 < s_.size() && s_[p_ + 1] == '-') {
                if (p_ + 2 < s_.size() && s_[p_ + 2] == '>') {
                    p_ += 3;
                    return true;
                }
                return fail("'--' in comment");
            }
            ++p_;
        }
    }

    bool processing_instruction(Node& out) {   // at "<?"
        p_ += 2;
        std::size_t b, e;
        if (!name(b, e)) return fail("malformed processing instruction");
        std::string target = to8(s_, b, e);
        if (ascii_lower(target) == "xml") return fail("reserved processing instruction target");
        if (target.find(':') != std::string::npos) return fail("colon in processing instruction target");
        bool ws = skip_ws();
        std::size_t db = p_;
        while (true) {
            if (eof()) return fail("unterminated processing instruction");
            if (starts("?>")) break;
            if (!ws) return fail("malformed processing instruction");
            ++p_;
        }
        out.kind = Node::Kind::PI;
        out.target = std::move(target);
        out.value = to8(s_, db, p_);
        p_ += 2;
        return true;
    }

    // Reference at '&': predefined entity or character reference; appends the replacement text.
    bool reference(u32& out) {
        ++p_;
        if (!eof() && s_[p_] == '#') {
            ++p_;
            bool hex = !eof() && s_[p_] == 'x';
            if (hex) ++p_;
            std::uint64_t v = 0;
            int digits = 0;
            while (!eof() && s_[p_] != ';') {
                char32_t h = s_[p_];
                int d = (h >= '0' && h <= '9') ? int(h - '0')
                        : hex && (h >= 'a' && h <= 'f') ? int(h - 'a' + 10)
                        : hex && (h >= 'A' && h <= 'F') ? int(h - 'A' + 10) : -1;
                if (d < 0) return fail("malformed character reference");
                v = v * (hex ? 16 : 10) + d;
                if (v > 0x10FFFF) v = 0x110000;
                ++digits;
                ++p_;
            }
            if (eof() || digits == 0) return fail("malformed character reference");
            ++p_;
            if (!is_xml_char(static_cast<char32_t>(v))) return fail("character reference to an invalid character");
            out.push_back(static_cast<char32_t>(v));
            return true;
        }
        std::size_t b, e;
        if (!name(b, e) || eof() || s_[p_] != ';') return fail("malformed entity reference");
        ++p_;
        std::string n = to8(s_, b, e);
        if (n == "lt") out.push_back('<');
        else if (n == "gt") out.push_back('>');
        else if (n == "amp") out.push_back('&');
        else if (n == "apos") out.push_back('\'');
        else if (n == "quot") out.push_back('"');
        else return fail("undefined entity '" + n + "'");
        return true;
    }

    bool attr_value(std::string& out) {
        if (eof() || (s_[p_] != '"' && s_[p_] != '\'')) return fail("attribute value expected");
        char32_t q = s_[p_++];
        u32 v;
        while (true) {
            if (eof()) return fail("unterminated attribute value");
            char32_t c = s_[p_];
            if (c == q) {
                ++p_;
                break;
            }
            if (c == '<') return fail("'<' in attribute value");
            if (c == '&') {
                if (!reference(v)) return false;
                continue;
            }
            v.push_back(is_space(c) ? U' ' : c);   // attribute-value normalisation (CDATA type)
            ++p_;
        }
        out = to8(v, 0, v.size());
        return true;
    }

    std::optional<std::string> lookup(const std::string& prefix) const {
        for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
            if (it->prefix == prefix) {
                if (prefix.empty() && it->uri.empty()) return std::nullopt;   // xmlns="" undeclares
                return it->uri;
            }
        return std::nullopt;
    }

    // Start tag at '<' (name start checked by the caller); fills `el` and reports whether it was empty.
    bool start_tag(Node& el, bool& empty, std::size_t& added) {
        ++p_;
        std::size_t b, e;
        if (!name(b, e)) return fail("element name expected");
        el.kind = Node::Kind::Element;
        el.qname = to8(s_, b, e);
        if (!split_qname(b, e, el.prefix, el.local)) return fail("invalid qualified name '" + el.qname + "'");
        while (true) {
            bool ws = skip_ws();
            if (eof()) return fail("unterminated start tag");
            if (starts("/>")) {
                p_ += 2;
                empty = true;
                break;
            }
            if (s_[p_] == '>') {
                ++p_;
                empty = false;
                break;
            }
            if (!ws) return fail("whitespace required between attributes");
            std::size_t ab, ae;
            if (!name(ab, ae)) return fail("attribute name expected");
            Attr a;
            a.qname = to8(s_, ab, ae);
            if (!split_qname(ab, ae, a.prefix, a.local)) return fail("invalid attribute name '" + a.qname + "'");
            skip_ws();
            if (eof() || s_[p_] != '=') return fail("'=' expected");
            ++p_;
            skip_ws();
            if (!attr_value(a.value)) return false;
            for (const Attr& o : el.attrs)
                if (o.qname == a.qname) return fail("duplicate attribute '" + a.qname + "'");
            el.attrs.push_back(std::move(a));
        }
        // namespace declarations of this element
        added = 0;
        for (const Attr& a : el.attrs) {
            if (a.prefix.empty() && a.qname == "xmlns") {
                if (a.value == kNsXml || a.value == kNsXmlns) return fail("illegal default namespace");
                bindings_.push_back({"", a.value});
                ++added;
            } else if (a.prefix == "xmlns") {
                if (a.local == "xmlns") return fail("the xmlns prefix cannot be declared");
                if (a.value.empty()) return fail("empty namespace for prefix '" + a.local + "'");
                if (a.local == "xml" ? a.value != kNsXml : (a.value == kNsXml || a.value == kNsXmlns))
                    return fail("illegal namespace binding of prefix '" + a.local + "'");
                bindings_.push_back({a.local, a.value});
                ++added;
            }
        }
        if (el.prefix == "xmlns") return fail("element prefix 'xmlns'");
        if (!el.prefix.empty()) {
            el.ns = lookup(el.prefix);
            if (!el.ns) return fail("unbound prefix '" + el.prefix + "'");
        } else {
            el.ns = lookup("");
        }
        for (Attr& a : el.attrs) {
            if (a.is_xmlns()) {
                a.ns = kNsXmlns;
            } else if (!a.prefix.empty()) {
                a.ns = lookup(a.prefix);
                if (!a.ns) return fail("unbound prefix '" + a.prefix + "'");
            }
        }
        for (std::size_t i = 0; i < el.attrs.size(); ++i)
            for (std::size_t j = i + 1; j < el.attrs.size(); ++j)
                if (el.attrs[i].ns && el.attrs[j].ns && *el.attrs[i].ns == *el.attrs[j].ns &&
                    el.attrs[i].local == el.attrs[j].local)
                    return fail("duplicate expanded attribute name");
        std::stable_sort(el.attrs.begin(), el.attrs.end(),
                         [](const Attr& x, const Attr& y) { return x.qname < y.qname; });
        return true;
    }

    // Root element and its content; iterative so that deep nesting cannot overflow the C++ stack.
    bool element(Node& root) {
        struct Frame {
            Node* node;
            std::size_t added;
            u32 text;
        };
        std::vector<Frame> stack;
        bool empty = false;
        std::size_t added = 0;
        if (!start_tag(root, empty, added)) return false;
        if (empty) {
            bindings_.resize(bindings_.size() - added);
            return true;
        }
        stack.push_back({&root, added, {}});
        auto flush = [&](Frame& f) {
            if (f.text.empty()) return;
            Node t;
            t.kind = Node::Kind::Text;
            t.value = to8(f.text, 0, f.text.size());
            f.node->children.push_back(std::move(t));
            f.text.clear();
        };
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (eof()) return fail("unterminated element '" + f.node->qname + "'");
            char32_t c = s_[p_];
            if (c == '<') {
                if (starts("</")) {
                    flush(f);
                    p_ += 2;
                    std::size_t b, e;
                    if (!name(b, e) || to8(s_, b, e) != f.node->qname) return fail("mismatched end tag");
                    skip_ws();
                    if (eof() || s_[p_] != '>') return fail("malformed end tag");
                    ++p_;
                    bindings_.resize(bindings_.size() - f.added);
                    stack.pop_back();
                } else if (starts("<!--")) {
                    if (!comment()) return false;   // comments do not split text (Xerces appends)
                } else if (starts("<![CDATA[")) {
                    flush(f);
                    p_ += 9;
                    std::size_t b = p_;
                    while (!eof() && !starts("]]>")) ++p_;
                    if (eof()) return fail("unterminated CDATA section");
                    Node cd;
                    cd.kind = Node::Kind::CData;
                    cd.value = to8(s_, b, p_);
                    p_ += 3;
                    f.node->children.push_back(std::move(cd));
                } else if (starts("<?")) {
                    flush(f);
                    Node pi;
                    if (!processing_instruction(pi)) return false;
                    f.node->children.push_back(std::move(pi));
                } else if (p_ + 1 < s_.size() && name_start(s_[p_ + 1])) {
                    flush(f);
                    if (stack.size() >= kMaxDepth) return fail("nesting too deep");
                    Node child;
                    if (!start_tag(child, empty, added)) return false;
                    f.node->children.push_back(std::move(child));
                    Node* cp = &f.node->children.back();
                    if (empty)
                        bindings_.resize(bindings_.size() - added);
                    else
                        stack.push_back({cp, added, {}});
                } else {
                    return fail("malformed markup");
                }
            } else if (c == '&') {
                if (!reference(f.text)) return false;
            } else {
                if (c == ']' && starts("]]>")) return fail("']]>' in character data");
                f.text.push_back(c);
                ++p_;
            }
        }
        return true;
    }
};

bool parse_text(u32 text, Document& doc, std::string* error) {
    // line-end normalisation (XML 1.0 2.11)
    u32 s;
    s.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c == '\r') {
            s.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            s.push_back(c);
        }
    }
    for (std::size_t i = 0; i < s.size(); ++i)
        if (!is_xml_char(s[i])) {
            if (error) *error = "invalid XML character U+" + std::to_string(static_cast<unsigned long>(s[i]));
            return false;
        }
    doc.children.clear();
    Parser p(s, doc);
    if (!p.run()) {
        if (error) *error = p.error();
        doc.children.clear();
        return false;
    }
    return true;
}

}  // namespace

bool java_whitespace(char32_t c) {
    if ((c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) return true;
    if (c == 0x20 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A && c != 0x2007) || c == 0x205F || c == 0x3000)
        return true;
    return c == 0x2028 || c == 0x2029;
}

bool parse(std::string_view bytes, Mode mode, Document& doc, std::string* error) {
    u32 text;
    std::string enc;
    std::string err1;
    if (decode_attempt1(bytes, text, enc)) {
        doc.encoding = enc;
        if (parse_text(std::move(text), doc, &err1)) return true;
    } else {
        err1 = "cannot decode the packet as " + enc;
    }
    if (mode == Mode::Single) {
        if (error) *error = err1;
        return false;
    }
    // XMPMetaParser.parseXmlFromBytebuffer: Latin1Converter (UTF-8 buffers), FixASCIIControlsReader over an
    // InputStreamReader in the buffer's encoding, parsed as a character stream (no BOM handling).
    std::string enc2 = bytebuffer_encoding(bytes);
    std::string converted;
    std::string_view src = bytes;
    if (enc2 == "UTF-8") {
        converted = latin1_convert(bytes);
        src = converted;
    }
    u32 fixed = fix_controls(decode_lenient(src, enc2));
    doc.encoding = enc2;
    std::string err2;
    if (parse_text(std::move(fixed), doc, &err2)) return true;
    if (error) *error = err1 + "; retry: " + err2;
    return false;
}

}  // namespace eqx::xml
