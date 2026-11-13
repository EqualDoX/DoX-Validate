// Structure module: role mapping, headings, alternatives, notes, TOCI, lists, MathML placement, the /P entry and
// the language of structure-element alternatives, with the semantics of veraPDF 1.30.2 (validation-model
// GFPDStructElem / GFSE* classes, parser PDStructElem / TaggedPDFHelper / TaggedPDFRoleMapHelper /
// AttributeHelper) plus the merged PAC 26.1 conditions (union semantics).
//
// Object model: veraPDF creates one SE object per structure element through GFSEFactory, typed by the
// element's standard type in the profile's flavour (PDF/UA-1: PDF 1.7 role-map resolution = Flavor::Pdf17;
// PDF/UA-2, WTPDF: ISO 32000-2 namespaces = Flavor::Pdf20). An element whose standard type is null is an
// SENonStandard, a MathML-namespace element an SEMathMLStructElem, H1..Hn an SEHn. The elements are visited in
// the validator's depth-first order, which is StructTree's element order.
#include <unordered_map>
#include <unordered_set>

#include "rules/module.h"
#include "rules/structure/se_common.h"
#include "rules/structure/structure_rules.h"

namespace eqx {

namespace {

class Module final : public RuleModule {
public:
    const char* name() const override { return "structure"; }
    void run(ValidationContext& ctx) override { structure::run_structure_rules(ctx); }
};

}  // namespace

std::unique_ptr<RuleModule> make_structure_module() { return std::make_unique<Module>(); }

}  // namespace eqx
