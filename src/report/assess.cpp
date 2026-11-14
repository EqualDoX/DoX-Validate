#include "report/assess.h"

#include <algorithm>
#include <bit>

namespace eqx {

std::string combine(const std::string& u, const std::string& w) {
    if (u == kStatusAutomatedPass && w == kStatusAutomatedPass) return "both";
    if (u == kStatusFail && w == kStatusFail) return "neither";
    if (u == kStatusAutomatedPass && w == kStatusFail) return "pdfua_only";
    if (u == kStatusFail && w == kStatusAutomatedPass) return "wcag_only";
    if (u == kStatusFail && w == kStatusIncomplete) return "pdfua_fail_wcag_incomplete";
    if (w == kStatusFail && u == kStatusIncomplete) return "wcag_fail_pdfua_incomplete";
    return "undetermined";
}

Assessments assess(const Findings& findings, const std::vector<bool>& evaluated, const std::vector<bool>& incomplete,
                   bool processed) {
    const Catalog& cat = Catalog::get();
    Assessments a;
    auto recs = findings.sorted();
    for (int t = 0; t < T_COUNT; ++t) {
        const std::uint32_t bit = 1u << t;
        TargetAssessment& ta = a.single[t];
        for (const FindingRecord* f : recs) {
            const RuleInfo& r = cat.rule(f->rule);
            if (r.severity == static_cast<std::uint8_t>(Severity::Error) && (r.targets & bit)) ta.failed_rules.push_back(f->rule);
        }
        for (const FindingRecord* f : recs) {
            const RuleInfo& r = cat.rule(f->rule);
            bool failed = std::find(ta.failed_rules.begin(), ta.failed_rules.end(), f->rule) != ta.failed_rules.end();
            if (!failed && (r.concerns & bit)) ta.warning_rules.push_back(f->rule);
        }
    }
    // Required rules that were not evaluated, per target they count for: one pass over the catalogue, so each
    // target's list is in catalogue order.
    for (RuleIndex i = 0; i < cat.size(); ++i) {
        const RuleInfo& r = cat.rule(i);
        if (!r.required) continue;
        if (!(!processed || !evaluated[i] || incomplete[i])) continue;
        for (std::uint32_t m = r.targets & ((1u << T_COUNT) - 1); m != 0; m &= m - 1)
            a.single[std::countr_zero(m)].unevaluated_rules.push_back(i);
    }
    for (int t = 0; t < T_COUNT; ++t) {
        TargetAssessment& ta = a.single[t];
        if (!ta.failed_rules.empty()) ta.status = kStatusFail;
        else if (!ta.unevaluated_rules.empty() || !processed) ta.status = kStatusIncomplete;
        else ta.status = kStatusAutomatedPass;
        ta.manual_required = cat.manual_required_list(t);
    }
    for (int u = 0; u < 2; ++u)
        for (int w = 0; w < 9; ++w) a.combined[u][w] = combine(a.single[u].status, a.single[T_WCAG20_A + w].status);
    return a;
}

}  // namespace eqx
