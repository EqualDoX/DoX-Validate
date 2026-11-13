// Embedded canonical rule catalogue (generated from catalog/rules.json at build time).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

// Convenience: rule index by id, resolved once (use in function-local statics).
inline RuleIndex R(std::string_view id) { return Catalog::get().require(id); }

}  // namespace eqx
