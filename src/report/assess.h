// Conformance assessments (specification: docs/STANDARDS.md). PDF/UA, WTPDF and the nine WCAG targets are assessed independently; a combined
// statement is derived from one PDF/UA part and one WCAG target. Automated checks never yield "pass".
#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "rules/catalog.h"
#include "rules/findings.h"

namespace eqx {

inline constexpr const char* kStatusFail = "fail";
inline constexpr const char* kStatusIncomplete = "incomplete";
inline constexpr const char* kStatusAutomatedPass = "automated_pass_manual_required";

struct TargetAssessment {
    std::string status;
    std::vector<RuleIndex> failed_rules;
    std::vector<RuleIndex> warning_rules;
    std::vector<RuleIndex> unevaluated_rules;
    std::span<const char* const> manual_required;   // the catalogue's list for the target (generated strings)
};

struct Assessments {
    TargetAssessment single[T_COUNT];
    // combined[u][w]: u = 0 pdfua-1, 1 pdfua-2; w = WCAG target index 0..8 (T_WCAG20_A + w)
    std::string combined[2][9];
};

// evaluated[i]: rule i ran on this document; incomplete[i]: rule i could not be evaluated.
Assessments assess(const Findings& findings, const std::vector<bool>& evaluated, const std::vector<bool>& incomplete,
                   bool processed);

std::string combine(const std::string& pdfua, const std::string& wcag);

}  // namespace eqx
