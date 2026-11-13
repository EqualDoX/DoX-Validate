// Lookups over the generated font tables (generated/font_tables.cpp, generated/font_cmaps.cpp).
#include "fonts/data.h"

#include <algorithm>
#include <cstring>

namespace eqx::fontdata {

namespace {

// Bytewise comparison (the generator sorts by the latin-1 bytes).
inline int cmp(const char* a, std::string_view b) {
    std::size_t la = std::strlen(a);
    int c = std::memcmp(a, b.data(), std::min(la, b.size()));
    if (c != 0) return c;
    return la < b.size() ? -1 : la > b.size() ? 1 : 0;
}

template <class T, class Key>
const T* bsearch_by(const T* arr, std::size_t n, std::string_view key, Key k) {
    std::size_t lo = 0, hi = n;
    while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        int c = cmp(k(arr[mid]), key);
        if (c == 0) return &arr[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

bool in_sorted(const char* const* arr, std::size_t n, std::string_view key) {
    return bsearch_by(arr, n, key, [](const char* s) { return s; }) != nullptr;
}

}  // namespace

const AglEntry* agl_find(std::string_view name) {
    return bsearch_by(kAgl, kAglCount, name, [](const AglEntry& e) { return e.name; });
}

bool symbol_set_has(std::string_view name) { return in_sorted(kSymbolSetNames, kSymbolSetCount, name); }
bool zapf_dingbats_has(std::string_view name) { return in_sorted(kZapfDingbatsNames, kZapfDingbatsCount, name); }

const StdMetrics* std_metrics(std::string_view font_name) {
    for (std::size_t i = 0; i < kStdMetricsCount; ++i)
        if (font_name == kStdMetrics[i].font_name) return &kStdMetrics[i];
    return nullptr;
}

int std_width(const StdMetrics& m, std::string_view glyph_name, bool* found) {
    const NameWidth* w = bsearch_by(m.widths, m.width_count, glyph_name, [](const NameWidth& e) { return e.name; });
    if (found) *found = w != nullptr;
    if (w) return w->width;
    // StandardFontMetrics.getWidth: unknown names take the .notdef width when the AFM has one, else 0.
    const NameWidth* nd = bsearch_by(m.widths, m.width_count, ".notdef", [](const NameWidth& e) { return e.name; });
    return nd ? nd->width : 0;
}

const PredefinedCMap* predefined_cmap(std::string_view name) {
    return bsearch_by(kPredefinedCMaps, kPredefinedCMapsCount, name, [](const PredefinedCMap& e) { return e.name; });
}

int mac_os_roman_code(std::string_view glyph_name) {
    const NameCode* e = bsearch_by(kMacOsRoman, kMacOsRomanCount, glyph_name, [](const NameCode& c) { return c.name; });
    return e ? e->code : -1;
}

}  // namespace eqx::fontdata
