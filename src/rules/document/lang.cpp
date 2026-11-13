#include "rules/document/lang.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace eqx::langreg {
extern const char* const kFileDate;
extern const char* const kLanguages[];
extern const std::size_t kLanguagesCount;
extern const char* const kGrandfathered[];
extern const std::size_t kGrandfatheredCount;
}  // namespace eqx::langreg

namespace eqx::doclang {

namespace {

bool ascii_alpha(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool ascii_digit(char32_t c) { return c >= '0' && c <= '9'; }
bool ascii_alnum(char32_t c) { return ascii_alpha(c) || ascii_digit(c); }

bool in_sorted(const char* const* arr, std::size_t n, const std::string& key) {
    auto it = std::lower_bound(arr, arr + n, key.c_str(),
                               [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    return it != arr + n && key == *it;
}

bool all_of(const std::string& s, bool (*pred)(char32_t)) {
    for (char c : s)
        if (!pred(static_cast<unsigned char>(c))) return false;
    return true;
}

bool is_alpha_n(const std::string& s, std::size_t lo, std::size_t hi) {
    return s.size() >= lo && s.size() <= hi && all_of(s, ascii_alpha);
}
bool is_alnum_n(const std::string& s, std::size_t lo, std::size_t hi) {
    return s.size() >= lo && s.size() <= hi && all_of(s, ascii_alnum);
}

// RFC 5646 section 2.1 "langtag" production (well-formedness only), on lower-cased subtags.
bool well_formed_langtag(const std::vector<std::string>& t) {
    std::size_t i = 0, n = t.size();
    // language = 2*3ALPHA ["-" extlang] / 4ALPHA / 5*8ALPHA
    if (i >= n || !is_alpha_n(t[i], 2, 8)) return false;
    bool short_lang = t[i].size() <= 3;
    ++i;
    if (short_lang) {  // extlang = 3ALPHA *2("-" 3ALPHA)
        int ext = 0;
        while (i < n && ext < 3 && is_alpha_n(t[i], 3, 3)) {
            ++i;
            ++ext;
        }
    }
    if (i < n && is_alpha_n(t[i], 4, 4)) ++i;                                          // script
    if (i < n && (is_alpha_n(t[i], 2, 2) || (t[i].size() == 3 && all_of(t[i], ascii_digit)))) ++i;  // region
    while (i < n) {  // variant = 5*8alphanum / (DIGIT 3alphanum)
        const std::string& s = t[i];
        bool v = is_alnum_n(s, 5, 8) || (s.size() == 4 && ascii_digit(static_cast<unsigned char>(s[0])) &&
                                          all_of(s, ascii_alnum));
        if (!v) break;
        ++i;
    }
    while (i < n && t[i].size() == 1 && t[i] != "x") {  // extension = singleton 1*("-" (2*8alphanum))
        if (!all_of(t[i], ascii_alnum)) return false;
        ++i;
        int k = 0;
        while (i < n && is_alnum_n(t[i], 2, 8)) {
            ++i;
            ++k;
        }
        if (k == 0) return false;
    }
    if (i < n && t[i] == "x") {  // privateuse = "x" 1*("-" (1*8alphanum))
        ++i;
        if (i >= n) return false;
        while (i < n && is_alnum_n(t[i], 1, 8)) ++i;
    }
    return i == n;
}

}  // namespace

bool verapdf_lang_ok(std::u32string_view v) {
    // ^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$ ; the first subtag letters only, later ones letters/digits.
    std::size_t i = 0, n = v.size();
    std::size_t len = 0;
    while (i < n && ascii_alpha(v[i])) {
        ++i;
        ++len;
    }
    if (len < 1 || len > 8) return false;
    while (i < n) {
        if (v[i] != U'-') return false;
        ++i;
        len = 0;
        while (i < n && ascii_alnum(v[i])) {
            ++i;
            ++len;
        }
        if (len < 1 || len > 8) return false;
    }
    return true;
}

bool is_registered_language(std::string_view subtag) {
    std::string k(subtag);
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return in_sorted(langreg::kLanguages, langreg::kLanguagesCount, k);
}

const char* registry_file_date() { return langreg::kFileDate; }

PacLang pac_check(const std::optional<std::u32string>& value) {
    // PAC 26.1 validates only the document language (catalog /Lang): its passed/failed count of
    // PAC's ValidLanguage count is 1 on every development fixture, whatever structure Langs exist.
    if (!value || value->empty()) return PacLang::Missing;
    std::string s;
    for (char32_t c : *value) {
        if (c > 0x7F) return PacLang::Syntax;
        s.push_back(static_cast<char>(std::tolower(static_cast<int>(c))));
    }
    if (in_sorted(langreg::kGrandfathered, langreg::kGrandfatheredCount, s))
        return s.rfind("i-", 0) == 0 ? PacLang::IanaRegistered : PacLang::Valid;
    std::vector<std::string> sub;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = s.find('-', start);
        sub.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    for (auto& t : sub)
        if (!is_alnum_n(t, 1, 8)) return PacLang::Syntax;   // '_', blanks, empty subtags, > 8 characters
    if (sub[0] == "x") return sub.size() >= 2 ? PacLang::PrivateUse : PacLang::Syntax;
    // "i-" tags other than the registered grandfathered ones are not well-formed BCP 47; PAC's condition is
    // about the "i-" prefix, so they are classed with the registrations (warning) rather than as errors.
    if (sub[0] == "i") return sub.size() >= 2 ? PacLang::IanaRegistered : PacLang::Syntax;
    if (!well_formed_langtag(sub)) return PacLang::Syntax;
    // Unknown primary language subtags are reported as a syntax error by PAC 26.1 (probes 'zz', 'zzz', 'qaa',
    // 'abcde'); the separate "primary subtag is unknown" condition was never observed.
    if (!is_registered_language(sub[0])) return PacLang::Syntax;
    return PacLang::Valid;
}

}  // namespace eqx::doclang
