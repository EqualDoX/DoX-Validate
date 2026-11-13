#include "rules/pac_quality/pac_quality_text.h"

#include <array>

#include "util/text.h"

namespace eqx::pacq {

namespace {

bool ascii_alnum(char32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool ascii_alpha(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char32_t ascii_lower(char32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Host / domain label character: ASCII letters and digits, '-' and any non-ASCII letter-like code point
// (internationalised domain names). White space, punctuation and symbols below U+0080 end a label.
bool label_char(char32_t c) { return ascii_alnum(c) || c == '-' || (c >= 0xC0 && !is_whitespace(c)); }

bool starts_with_ci(std::u32string_view s, std::size_t at, std::string_view lit) {
    if (at + lit.size() > s.size()) return false;
    for (std::size_t i = 0; i < lit.size(); ++i)
        if (ascii_lower(s[at + i]) != char32_t(lit[i])) return false;
    return true;
}

// Length of a domain "label(.label)*.tld" starting at `at` (tld: two or more letters); 0 when none.
std::size_t domain_len(std::u32string_view s, std::size_t at) {
    std::size_t i = at, last_dot = 0, end = 0;
    int labels = 0;
    while (true) {
        std::size_t b = i;
        while (i < s.size() && label_char(s[i])) ++i;
        if (i == b) break;
        ++labels;
        // a label must be followed by '.' and another label to continue
        bool tld = true;
        for (std::size_t k = b; k < i; ++k)
            if (!ascii_alpha(s[k]) && s[k] < 0x80) tld = false;
        if (labels >= 2 && tld && i - b >= 2) end = i;
        if (i + 1 < s.size() && s[i] == '.' && label_char(s[i + 1])) {
            last_dot = i;
            ++i;
            continue;
        }
        break;
    }
    (void)last_dot;
    return end > at ? end - at : 0;
}

std::string utf8(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

bool local_part_char(char32_t c) {
    return ascii_alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

}  // namespace

bool is_whitespace(char32_t c) { return is_unicode_space(c); }

bool whitespace_only(std::u32string_view s) {
    for (char32_t c : s)
        if (!is_whitespace(c)) return false;
    return true;
}

std::optional<std::string> find_linkable(std::u32string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        bool boundary = i == 0 || !(ascii_alnum(s[i - 1]) || s[i - 1] == '.' || s[i - 1] == '/' || s[i - 1] == '@');
        // http:// and https:// URLs (scheme preceded by a word boundary)
        if (boundary && (s[i] == 'h' || s[i] == 'H')) {
            std::size_t len = 0;
            if (starts_with_ci(s, i, "https://")) len = 8;
            else if (starts_with_ci(s, i, "http://")) len = 7;
            if (len && i + len < s.size() && label_char(s[i + len]) && s[i + len] != '-') {
                std::size_t e = i + len;
                while (e < s.size() && !is_whitespace(s[e])) ++e;
                return utf8(s.substr(i, e - i));
            }
        }
        // www.<label>.<tld>
        if (boundary && (s[i] == 'w' || s[i] == 'W') && starts_with_ci(s, i, "www.")) {
            std::size_t d = domain_len(s, i + 4);
            if (d) {
                std::size_t e = i + 4 + d;
                while (e < s.size() && !is_whitespace(s[e])) ++e;
                return utf8(s.substr(i, e - i));
            }
        }
        // e-mail: local@domain.tld
        if (s[i] == '@' && i > 0) {
            std::size_t b = i;
            while (b > 0 && local_part_char(s[b - 1])) --b;
            while (b < i && (s[b] == '.' || s[b] == '-')) ++b;   // no leading dot
            if (b == i || s[i - 1] == '.') continue;
            std::size_t d = domain_len(s, i + 1);
            // PAC's expression takes no hyphen in the domain of an address (probe lm_hy_dom and four real-world samples:
            // not flagged), in the local part it does (lm_hy_local), and in the host of a URL (lm_http_hy).
            if (d && s.substr(i + 1, d).find(U'-') != std::u32string_view::npos) continue;
            if (d) return utf8(s.substr(b, i + 1 + d - b));
        }
    }
    return std::nullopt;
}

// PAC: "This check matches alternative text against a regular expression to determine if it is generated from a local path
// or filename". Reconstructed from single-condition PAC runs (probes alt_*, alt2_*):
//   flagged  image1.png, IMAGE.PNG, a.bar, a.svg, Bericht.pdf, example.org, images/photo.jpg, https://example.org/a.png,
//            file:///a/b.png, C:\Users\docs, C:\My Docs\a.png, G:\Folien\Logo_neu.jpg
//            123.456, a.mp3, a.html, foto.Jpeg, file:///tmp/a, \\\\server\\share\\a.png
//   passed   \\\\server\\share\\a, a.jpeg2, G:/a/b, logo neu.jpg, Mein Logo.jpg, report v2.docx, Das Bild logo.jpg, Beschreibung: G:\Folien\logo.jpg, a.ai, v1.2, Abb.1,
//            /home/a/b, images/photo, http://x.org/a
// So: a drive path with a backslash (spaces allowed), a file: URL, or a text without white space that ends in a dot and 3 or
// 4 letters or digits.
bool looks_like_path_or_file_name(std::string_view raw) {
    std::u32string s32 = utf8_to_u32(raw);
    std::size_t b = 0, e = s32.size();
    while (b < e && is_whitespace(s32[b])) ++b;
    while (e > b && is_whitespace(s32[e - 1])) --e;
    std::u32string_view s(s32.data() + b, e - b);
    if (s.empty()) return false;
    if (s.size() >= 3 && ascii_alpha(s[0]) && s[1] == ':' && s[2] == '\\') return true;
    if (starts_with_ci(s, 0, "file:/")) return true;
    for (char32_t c : s)
        if (is_whitespace(c)) return false;
    std::size_t dot = s.rfind(U'.');
    if (dot == std::u32string_view::npos || dot == 0) return false;
    const std::size_t ext = s.size() - dot - 1;
    if (ext < 3 || ext > 4) return false;
    for (char32_t c : s.substr(dot + 1))
        if (!ascii_alnum(c)) return false;
    return true;
}

std::u32string normalize_toc_text(std::u32string_view s) {
    std::u32string out;
    bool space = false;
    for (char32_t c : s) {
        if (is_whitespace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(U' ');
        space = false;
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
        out.push_back(c);
    }
    // strip trailing page numbers, leader dots, tabs and punctuation ("Introduction ........ 3")
    auto trailing = [](char32_t c) {
        return (c >= '0' && c <= '9') || c == '.' || c == ' ' || c == 0x2026 || c == 0x00B7 || c == '_' || c == '-' ||
               c == ':' || c == ',' || c == ';';
    };
    while (!out.empty() && trailing(out.back())) out.pop_back();
    std::size_t lead = 0;
    while (lead < out.size() && (out[lead] == ' ' || out[lead] == '.' || out[lead] == 0x2026)) ++lead;
    return out.substr(lead);
}

bool toc_text_matches(std::u32string_view toc_entry, std::u32string_view heading) {
    std::u32string a = normalize_toc_text(toc_entry), b = normalize_toc_text(heading);
    if (a.empty() || b.empty()) return false;
    if (a.find(b) != std::u32string::npos || b.find(a) != std::u32string::npos) return true;
    // A heading hyphenated over its lines matches the unhyphenated entry (a real-world sample: "preg-nancy", "coun-tries").
    auto unhyphenated = [](const std::u32string& t) {
        std::u32string o;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == U'-' || t[i] == 0x00AD) {
                if (i + 1 < t.size() && t[i + 1] == U' ') ++i;
                continue;
            }
            o.push_back(t[i]);
        }
        return o;
    };
    const std::u32string a2 = unhyphenated(a), b2 = unhyphenated(b);
    return a2.find(b2) != std::u32string::npos || b2.find(a2) != std::u32string::npos;
}

}  // namespace eqx::pacq
