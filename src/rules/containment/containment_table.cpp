// The generated rule table for callers that read it at run time (unit tests): see containment_rules.h.
#include <string_view>
#include <unordered_map>

#include "rules/containment/containment.h"
#include "rules/containment/containment_rules.h"

namespace eqx::containment {

const GenRule* generated_rules(std::size_t* n) {
    *n = kGenRuleCount;
    return kGenRules;
}

int find_generated(std::string_view id) {
    static const std::unordered_map<std::string_view, int> index = [] {
        std::unordered_map<std::string_view, int> m;
        for (std::size_t i = 0; i < kGenRuleCount; ++i) m.emplace(kGenRules[i].id, static_cast<int>(i));
        return m;
    }();
    auto it = index.find(id);
    return it == index.end() ? -1 : it->second;
}

}  // namespace eqx::containment
