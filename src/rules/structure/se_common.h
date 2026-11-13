// Shared helpers of the structure and tables modules: string entries, attribute lookup, structure children and
// the structurally significant children of an element, with results compatible with the reference validator
// (veraPDF 1.30.2) where the rules compare against it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/cos.h"
#include "core/structure.h"

namespace eqx::se {

// Identity of a PDF object (same underlying qpdf object, direct or indirect).
const void* identity(const Obj& h);

// PDObject.getStringKey: the value of a string entry decoded like COSString.getString (names and every other
// type give nullopt).
std::optional<std::string> string_key(const Obj& dict, std::string_view key);

// COSObject.getString of an entry: string values decoded, names as their name value (veraPDF's
// PDStructElem.getActualText().getString() therefore accepts a name), every other type nullopt.
std::optional<std::string> string_or_name_key(const Obj& dict, std::string_view key);

// Attribute lookup (ISO 32000-1 / ISO 32000-2 14.7.6): the attribute objects of an element are those of its /A entry
// (a dictionary or an array of them) followed by those of each class /C names in the ClassMap; the first attribute
// object with the requested owner (/O) whose entry for the attribute has the requested type supplies the value.
class Attributes {
public:
    explicit Attributes(const StructTree& st);

    // getAttributeValue: the first value of the wanted type (qpdf type code), or a null handle.
    Obj value(const Obj& elem, std::string_view attr, std::string_view owner, qpdf_object_type_e type) const;

    // getIntegerAttributeValue (ColSpan, RowSpan: default 1).
    std::int64_t integer(const Obj& elem, std::string_view attr, std::string_view owner, std::int64_t dflt) const;

    // getNameAttributeValue. `inheritable` follows /P like veraPDF (ListNumbering). A /P cycle (veraPDF:
    // StackOverflowError, validation aborted) sets *cycle when given and returns the default.
    std::optional<std::string> name(const Obj& elem, std::string_view attr, std::string_view owner,
                                    std::optional<std::string> dflt, bool inheritable, bool* cycle = nullptr) const;

    // getArrayAttributeValue (Headers): null handle when absent.
    Obj array(const Obj& elem, std::string_view attr, std::string_view owner) const;

private:
    Obj class_map_;
};

// TaggedPDFHelper.getStructChildren: /K itself when it is a structure element dictionary (no /Type or
// /Type /StructElem), else the structure element dictionaries of a /K array, in order.
std::vector<Obj> struct_children(const Obj& node);

// Standard type of an element dictionary in a flavour, as GFSEFactory assigns it to the model object
// (Pdf20: "MathML" for the MathML namespace); "" when none.
std::string standard_type(const StructTree& st, const Obj& elem, Flavor f);

// GFPDStructTreeNode.getStructuralSignificanceChildren: struct children with Part/Div/NonStruct children replaced
// by their own structural-significance children (recursively, document order). veraPDF recurses without a guard
// (a pass-through cycle is a StackOverflowError); `ok` is set to false in that case.
struct TypedChild {
    Obj dict;
    std::string type;   // standard type in the flavour ("" = none)
};
std::vector<TypedChild> significance_children(const StructTree& st, const Obj& node, Flavor f, bool* ok);

// GFPDStructTreeNode.getChildrenStandardTypes: standard types of the children, pass-through children flattened
// (empty strings = null standard types are kept, like the Java list).
std::vector<std::string> children_standard_types(const StructTree& st, const Obj& node, Flavor f, bool* ok);

inline bool is_pass_through(const std::string& t) { return t == "NonStruct" || t == "Div" || t == "Part"; }

// The elements evaluated, in evaluation order. Default: every element in document order (pre-order). With
// reference_dedup (CLI --reference-dedup): veraPDF's validated set and order (StructTree::verapdf_order; a direct
// element shares the id "0 0 obj <S>" with every other direct element of that /S and is validated once).
std::vector<int> element_order(const StructTree& st, bool reference_dedup);

// Map from object identity to element index (direct and indirect elements).
std::unordered_map<const void*, int> elem_index_map(const StructTree& st);

// ^H[1-9][0-9]*$
bool is_hn(const std::string& t);

}  // namespace eqx::se
