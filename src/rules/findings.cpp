#include "rules/findings.h"

#include <algorithm>

namespace eqx {

void Findings::add(RuleIndex rule, Location loc) {
    if (rule == kNoRule) return;
    FindingRecord& r = by_rule_[rule];
    r.rule = rule;
    ++r.occurrences;
    if (loc.detail.rfind("PAC:", 0) != 0) ++r.non_pac_occurrences;
    if (r.locations.size() >= max_locations_) return;   // saturated: count only, no key
    std::string key = std::to_string(loc.page) + '|' + loc.obj.str() + '|' + loc.detail;
    if (!r.keys.insert(std::move(key)).second) return;
    r.locations.push_back(std::move(loc));
}

std::vector<const FindingRecord*> Findings::sorted() const {
    std::vector<const FindingRecord*> out;
    out.reserve(by_rule_.size());
    for (const auto& kv : by_rule_) out.push_back(&kv.second);
    std::sort(out.begin(), out.end(), [](const FindingRecord* a, const FindingRecord* b) { return a->rule < b->rule; });
    return out;
}

}  // namespace eqx
