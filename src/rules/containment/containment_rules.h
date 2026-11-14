// The generated containment rule table as a constant expression (internal to the containment module).
//
// containment_engine.cpp and containment_module.cpp read it in constant evaluation only: the compiled rules and the
// rule-index mapping are computed by the compiler, so the table itself (1667 rules, about 5000 pointers) is not part
// of the validator binary. containment_table.cpp holds the one copy that generated_rules() hands out; it is linked
// only into programs that call generated_rules() or find_generated() (the unit tests).
#pragma once

#include <cstddef>

#include "rules/containment/containment.h"

namespace eqx::containment {

constexpr GenRule kGenRules[] = {
#include "rules/containment/containment_table.inc"
};
constexpr std::size_t kGenRuleCount = sizeof(kGenRules) / sizeof(kGenRules[0]);

}  // namespace eqx::containment
