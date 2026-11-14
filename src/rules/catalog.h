// Embedded canonical rule catalogue (generated from catalog/rules.json at build time).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "catalog_index.h"   // generated: rule ids and families as constant expressions

namespace eqx {

enum class Severity : std::uint8_t { Error = 0, Warning = 1 };
enum class RuleKind : std::uint8_t { Violation = 0, Warning = 1, Recommendation = 2 };

// The 13 single conformance targets, in the bit order of RuleInfo::targets / ::concerns.
enum Target : int {
    T_PDFUA1 = 0, T_PDFUA2, T_WTPDF_A, T_WTPDF_R,
    T_WCAG20_A, T_WCAG20_AA, T_WCAG20_AAA,
    T_WCAG21_A, T_WCAG21_AA, T_WCAG21_AAA,
    T_WCAG22_A, T_WCAG22_AA, T_WCAG22_AAA,
    T_COUNT
};
const char* target_name(int target);

struct RuleInfo {
    const char* id;
    const char* title;
    std::uint8_t severity;   // Severity
    std::uint8_t kind;       // RuleKind
    std::uint32_t targets;   // targets a finding of this rule fails (normative mappings, derive.py semantics)
    std::uint32_t concerns;  // targets the rule concerns at all (warning_rules)
    std::uint8_t required;   // automated rule a target needs before it may be an automated pass
    const char* family;
    const char* detection;
    const char* references_json;  // {"standards":[...],"sources":[...]} pre-rendered compact JSON
};

using RuleIndex = std::uint32_t;
constexpr RuleIndex kNoRule = 0xffffffffu;

// Generated functions (catalog_data.cpp).
const RuleInfo* catalog_rules(std::size_t* n);
const char* const* catalog_manual(int target);
const char* catalog_sha256();
// The ids of the rules [first, last) as the JSON report writes them: "ID","ID",...,"ID" (no escaping needed, which
// the generator checks). Empty when first >= last.
std::string_view catalog_ids_json(RuleIndex first, RuleIndex last);

class Catalog {
public:
    static const Catalog& get();

    std::size_t size() const { return n_; }
    const RuleInfo& rule(RuleIndex i) const { return rules_[i]; }
    // Index of a canonical id; kNoRule when unknown.
    RuleIndex find(std::string_view id) const;
    // Index of a canonical id; aborts (programming error) when unknown.
    RuleIndex require(std::string_view id) const;
    std::vector<std::string> manual_required(int target) const;

private:
    Catalog();
    const RuleInfo* rules_ = nullptr;
    std::size_t n_ = 0;
};

namespace catalog_index {
// Index of a canonical id in the generated id table. An unknown id is not a constant expression: it fails the build.
consteval RuleIndex index_of(std::string_view id) {
    std::size_t lo = 0, hi = kRuleCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kRuleIds[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo == kRuleCount || kRuleIds[lo] != id) throw "unknown canonical rule id";
    return static_cast<RuleIndex>(lo);
}
}  // namespace catalog_index

// Rule index by id. A string literal is resolved by the compiler (an unknown id does not compile); any other
// string is looked up at run time and aborts when unknown.
template <std::size_t N>
consteval RuleIndex R(const char (&id)[N]) { return catalog_index::index_of(std::string_view(id, N - 1)); }
inline RuleIndex R(std::string_view id) { return Catalog::get().require(id); }

}  // namespace eqx
