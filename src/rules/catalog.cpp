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

Catalog::Catalog() {
    rules_ = catalog_rules(&n_);
    // R("...") and the ownership table are computed from the generated index; both come from one generator run.
    if (n_ != catalog_index::kRuleCount) {
        std::fprintf(stderr, "eqx: internal error: the generated catalogue index does not match the catalogue\n");
        std::abort();
    }
}

const Catalog& Catalog::get() {
    static const Catalog instance;
    return instance;
}

RuleIndex Catalog::find(std::string_view id) const {
    // The generated table is sorted by id (byte order): binary search.
    const RuleInfo* b = rules_;
    const RuleInfo* e = rules_ + n_;
    // One pass over the id per step: no strlen. The ids contain no NUL byte; an id with one is not in the table.
    const auto cmp = [](const char* rule_id, std::string_view v) {   // <0, 0, >0 like rule_id <=> v
        const int c = std::strncmp(rule_id, v.data(), v.size());
        return c != 0 ? c : (rule_id[v.size()] != '\0' ? 1 : 0);
    };
    if (id.find('\0') != std::string_view::npos) return kNoRule;
    const RuleInfo* it = std::lower_bound(b, e, id, [&](const RuleInfo& r, std::string_view v) { return cmp(r.id, v) < 0; });
    if (it != e && cmp(it->id, id) == 0) return static_cast<RuleIndex>(it - b);
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

std::span<const char* const> Catalog::manual_required_list(int target) const {
    const char* const* p = catalog_manual(target);
    std::size_t n = 0;
    while (p && p[n]) ++n;
    return {p, n};
}

std::vector<std::string> Catalog::manual_required(int target) const {
    std::vector<std::string> out;
    const char* const* p = catalog_manual(target);
    for (; p && *p; ++p) out.emplace_back(*p);
    return out;
}

}  // namespace eqx
