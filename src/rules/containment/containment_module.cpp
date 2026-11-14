// Module `containment`: every canonical rule of the containment.* families (ISO/TS 32005 Table 5 rules of
// veraPDF's ua2/wt1a/wt1r profiles, PDF/UA-1 7.2-x and 7.4.4-1, PDF/UA-2 8.2.5.x). The rule table is
// generated from the veraPDF profile tests; containment_engine.cpp evaluates it.
// Locations: the structure element whose property is tested (the StructTreeRoot for root rules), no page.
#include <array>

#include "rules/containment/containment.h"
#include "rules/containment/containment_rules.h"
#include "rules/module.h"

namespace eqx {

namespace {

// Catalogue index of every generated rule, resolved by the compiler (a generated rule that the catalogue does not
// know fails the build).
constexpr std::array<RuleIndex, containment::kGenRuleCount> kGenToRule = []() consteval {
    std::array<RuleIndex, containment::kGenRuleCount> v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = catalog_index::index_of(containment::kGenRules[i].id);
    return v;
}();

class Module final : public RuleModule {
public:
    const char* name() const override { return "containment"; }

    void run(ValidationContext& ctx) override {
        const auto& gen_to_rule = kGenToRule;
        // A rule owned by this module but absent from the generated table is never silently passed.
        static const std::vector<RuleIndex> missing = [this] {
            std::vector<bool> generated(Catalog::get().size(), false);
            for (RuleIndex r : kGenToRule) generated[r] = true;
            std::vector<RuleIndex> v;
            for (RuleIndex r : owned_rules(name()))
                if (!generated[r]) v.push_back(r);
            return v;
        }();
        for (RuleIndex r : missing) ctx.mark_incomplete(r, "rule missing from the generated containment table");

        containment::Sink sink;
        sink.violation = [&](std::size_t gen, ObjRef obj, std::string detail) {
            ctx.report(gen_to_rule[gen], -1, obj, std::move(detail));
        };
        sink.incomplete = [&](std::size_t gen, std::string reason) {
            ctx.mark_incomplete(gen_to_rule[gen], std::move(reason));
        };
        containment::evaluate(ctx.st, sink, ctx.reference_dedup);
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_containment_module() { return std::make_unique<Module>(); }

}  // namespace eqx
