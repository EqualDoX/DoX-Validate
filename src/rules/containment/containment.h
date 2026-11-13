// Containment rules: the parent/child tests of veraPDF 1.30.2 over its structure-tree properties
// (ISO/TS 32005 Table 5 of the ua2/wt1a/wt1r profiles, PDF/UA-1 7.2-x / 7.4.4-1, PDF/UA-2 8.2.5.x).
//
// The rule table (containment_table.inc) is generated from veraPDF 1.30.2's profile tests:
// every test is a disjunction of atoms over the properties of one veraPDF model object
//   kidsStandardTypes   GFPDStructTreeNode.getkidsStandardTypes: '&'-joined standard types of the struct
//                       children, Part/Div/NonStruct children replaced by their own children (recursively),
//                       null standard types dropped
//   parentStandardType  GFPDStructElem.getparentStandardType: /P chain, Part/Div/NonStruct skipped,
//                       "StructTreeRoot" / "MathML" (PDF/UA-2 flavours) / standard type / null
//   hasContentItems     GFPDStructTreeNode.gethasContentItems: MCID/MCR/OBJR in /K, also through
//                       pass-through children
//   isGrouping          GFPDStructElem.getisGrouping
// evaluated with JavaScript semantics (''.split('&') is [''], null != 'X', /re/.test(null) is false).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/cos.h"
#include "core/structure.h"

namespace eqx::containment {

enum class AK : std::uint8_t {
    GROUPING_IS,           // isGrouping == (n != 0)
    NO_CONTENT_ITEMS,      // hasContentItems == false
    PARENT_EQ,             // parentStandardType == arg
    PARENT_NE,             // parentStandardType != arg
    PARENT_NOT_HN,         // /^H[1-9][0-9]*$/.test(parentStandardType) == false
    PARENT_IN,             // /^(a|b|...)$/.test(parentStandardType)
    KID_COUNT,             // kidsStandardTypes.split('&').filter(pred).length <cmp> n
    KIDS_EQ,               // kidsStandardTypes == arg
    KIDS_NOT_CONTAINS,     // kidsStandardTypes.indexOf(arg) < 0   (arg = '&X&')
    KIDS_NOT_AFTER_FIRST,  // kidsStandardTypes.split('&').slice(1).findIndex(elem => elem == arg) < 0
    KIDS_ALL_IN,           // /^(a|b)(&(a|b))*$/.test(kidsStandardTypes)
};
enum : std::uint8_t { PRED_NONE, PRED_EQ, PRED_HN, PRED_NOT_IN };
enum : std::uint8_t { CMP_EQ, CMP_LE, CMP_LT, CMP_GT };

struct GenAtom {
    AK kind;
    std::uint8_t pred;
    std::uint8_t cmp;
    std::int32_t n;
    const char* arg;   // name, '&'-joined sequence or '|'-joined set
};

struct GenRule {
    const char* id;         // canonical rule id
    Flavor flavour;         // Pdf17 for ua1 rules, Pdf20 for ua2/wt1a/wt1r rules
    const char* object;     // standard type of the SE object, "Hn", "*" (every PDStructElem), "#root"
    std::uint8_t natoms;
    GenAtom atoms[3];       // disjunction: the test passes when one atom is true
};

// The generated table.
const GenRule* generated_rules(std::size_t* n);

// Abstract object for unit tests: the veraPDF properties of one tested object.
struct ObjectView {
    std::optional<std::string> standard_type;          // own standard type (isGrouping)
    std::optional<std::string> parent_standard_type;   // parentStandardType (nullopt = null)
    std::vector<std::string> kids;                     // kidsStandardTypes tokens (no nulls)
    bool has_content_items = false;
};
// True when the generated rule's test passes on the object (same evaluator as the module).
bool test_passes(const GenRule& rule, const ObjectView& view);
// Index of a rule in the generated table; -1 when unknown.
int find_generated(std::string_view id);

// Evaluation of every generated rule on a document's structure tree.
struct Sink {
    // Rule `gen` (index into generated_rules) failed on `obj` (structure element or StructTreeRoot).
    std::function<void(std::size_t gen, ObjRef obj, std::string detail)> violation;
    // Rule `gen` cannot be decided on this document (reference semantics do not terminate).
    std::function<void(std::size_t gen, std::string reason)> incomplete;
};
// reference_dedup = false (default): every element of the tree is evaluated (each indirect element once,
// every direct element). true: exactly the elements veraPDF validates (StructTree::verapdf_order).
void evaluate(const StructTree& st, const Sink& sink, bool reference_dedup = false);

}  // namespace eqx::containment
