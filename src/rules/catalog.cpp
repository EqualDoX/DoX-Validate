#include "rules/catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eqx {

const char* target_name(int target) {
    static const char* const names[T_COUNT] = {
        "pdfua-1", "pdfua-2", "wtpdf-1.0-accessibility", "wtpdf-1.0-reuse",
        "wcag-2.0-A", "wcag-2.0-AA", "wcag-2.0-AAA",
        "wcag-2.1-A", "wcag-2.1-AA", "wcag-2.1-AAA",
        "wcag-2.2-A", "wcag-2.2-AA", "wcag-2.2-AAA"};
    return (target >= 0 && target < T_COUNT) ? names[target] : "?";
}

Catalog::Catalog() { rules_ = catalog_rules(&n_); }

const Catalog& Catalog::get() {
    static const Catalog instance;
    return instance;
}

RuleIndex Catalog::find(std::string_view id) const {
    // The generated table is sorted by id (byte order): binary search.
    const RuleInfo* b = rules_;
    const RuleInfo* e = rules_ + n_;
    const RuleInfo* it = std::lower_bound(b, e, id, [](const RuleInfo& r, std::string_view v) {
        return std::string_view(r.id) < v;
    });
    if (it != e && std::string_view(it->id) == id) return static_cast<RuleIndex>(it - b);
    return kNoRule;
}

RuleIndex Catalog::require(std::string_view id) const {
    RuleIndex i = find(id);
    if (i == kNoRule) {
        std::fprintf(stderr, "eqx: internal error: unknown canonical rule id %.*s\n", int(id.size()), id.data());
        std::abort();
    }
    return i;
}

std::vector<std::string> Catalog::manual_required(int target) const {
    std::vector<std::string> out;
    const char* const* p = catalog_manual(target);
    for (; p && *p; ++p) out.emplace_back(*p);
    return out;
}

}  // namespace eqx
