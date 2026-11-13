// Findings collector: one record per canonical rule, all locations attached, severity from the catalogue
// (the maximum severity of the equivalent reference findings, never lowered).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/cos.h"
#include "rules/catalog.h"

namespace eqx {

struct Location {
    int page = 0;          // 1-based page number, 0 = not on a page
    ObjRef obj;            // innermost PDF object (invalid = none)
    std::string detail;    // human-readable specifics (element, code, MCID, ...)
};

struct FindingRecord {
    RuleIndex rule = kNoRule;
    std::vector<Location> locations;      // distinct (page, obj, detail), first `max_locations`
    std::uint64_t occurrences = 0;        // every add() call
    std::uint64_t non_pac_occurrences = 0;  // those not through PAC's definition (detail without the "PAC:" prefix)
    std::unordered_set<std::string> keys; // dedup keys of the stored locations
};

class Findings {
public:
    explicit Findings(std::size_t max_locations = 1000) : max_locations_(max_locations) {}

    // Records one violation of `rule` at `loc`. Page is 1-based (use page_number(index)).
    void add(RuleIndex rule, Location loc);
    void add(RuleIndex rule, int page1, ObjRef obj, std::string detail = {}) {
        add(rule, Location{page1, obj, std::move(detail)});
    }
    bool has(RuleIndex rule) const { return by_rule_.count(rule) > 0; }
    void erase(RuleIndex rule) { by_rule_.erase(rule); }

    // Records sorted by catalogue order (rule index).
    std::vector<const FindingRecord*> sorted() const;
    std::size_t size() const { return by_rule_.size(); }

private:
    std::size_t max_locations_;
    std::unordered_map<RuleIndex, FindingRecord> by_rule_;
};

// 0-based page index -> report page number (0 for "no page").
inline int page_number(int index) { return index >= 0 ? index + 1 : 0; }

}  // namespace eqx
