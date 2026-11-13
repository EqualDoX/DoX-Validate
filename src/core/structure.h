// Logical structure model (built once per document), with the standard types of both flavours (role mapping:
// core/role_map.h):
//   Flavor::Pdf17  PDF/UA-1 rules: /RoleMap chains resolved against the PDF 1.7 standard types
//   Flavor::Pdf20  PDF/UA-2 and WTPDF rules: namespaces (/NS, /RoleMapNS) plus /RoleMap
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cos.h"

namespace eqx {

class Document;

// Number tree /Nums arrays indexed on first use: per array, (key, position of the key's last occurrence) sorted by
// key, so a lookup is a binary search instead of a scan of the whole array (StructTree::parent_tree_entry).
// Arrays are identified by their own object number, or by the indirect node that holds a direct array.
struct NumsIndex {
    using Entries = std::vector<std::pair<std::int64_t, int>>;
    std::unordered_map<std::uint64_t, Entries> by_array, by_node;
};

enum class Flavor : std::uint8_t { Pdf17 = 0, Pdf20 = 1 };

inline constexpr const char* kNsPdf = "http://iso.org/pdf/ssn";
inline constexpr const char* kNsPdf2 = "http://iso.org/pdf2/ssn";
inline constexpr const char* kNsMathML = "http://www.w3.org/1998/Math/MathML";

// Structure type with its namespace (veraPDF StructureType). ns null = no namespace given.
struct SType {
    std::string type;
    Obj ns;                                // namespace dictionary or null handle
    // veraPDF getNameSpaceURI(): kNsPdf without namespace dict, else the dict's /NS string (nullopt if absent)
    std::optional<std::string> ns_uri() const;
    ObjRef ns_ref() const { return ref_of(ns); }
    bool same(const SType& o) const;       // same type and same namespace URI
};

struct StructKid {
    enum class Kind : std::uint8_t { Elem, MCID, MCR, OBJR };
    Kind kind = Kind::Elem;
    int elem = -1;            // Elem: index into StructTree::elems (also for an element visited before)
    bool revisit = false;     // Elem: the element was already reached through another kid (shared / cyclic)
    std::int64_t mcid = -1;   // MCID / MCR (-1 = MCR without integer /MCID)
    Obj dict;                 // Elem / MCR / OBJR dictionary
    ObjRef page;              // MCID/MCR/OBJR: /Pg of the MCR/OBJR, else the element's (inherited) /Pg
    ObjRef stm;               // MCR /Stm
    Obj obj;                  // OBJR /Obj
};

struct StructElem {
    int index = -1;
    int parent = -1;          // traversal parent element (-1 = kid of the StructTreeRoot)
    int depth = 0;
    Obj dict;
    ObjRef ref;
    std::optional<std::string> S;   // /S when it is a name
    SType type;                     // veraPDF getStructureType() (S + NS); type empty when S is not a name
    std::optional<std::string> std17;   // standard type, PDF/UA-1 semantics
    std::optional<SType> std20;         // standard type, ISO 32000-2 semantics
    std::vector<StructKid> kids;        // every child in /K order (elements, MCIDs, MCRs, OBJRs)
    std::vector<int> elems;             // tree children: structure elements first reached through this element
    std::vector<int> struct_kids;       // every structure-element child in /K order, also shared/revisited ones
                                        // (veraPDF getStructChildren: kidsStandardTypes, hasContentItems)
    // veraPDF validates each model object once (BaseValidator idSet). A direct structure element has the id
    // "0 0 obj <S>", so of several direct elements with the same /S only the one pushed first is validated,
    // together with its subtree. False for elements veraPDF never validates (simulated push order).
    bool verapdf_checked = true;
    ObjRef page;                        // own /Pg
    ObjRef inherited_page;              // own /Pg, else nearest ancestor's /Pg

    // Standard type in a flavour ("" when none). For Pdf20 this is the type name only.
    std::string standard(Flavor f) const;
    bool is_math(Flavor f) const;       // Pdf20 and standard namespace is MathML
};

class StructTree {
public:
    void build(Document& doc);

    bool present() const { return present_; }
    bool truncated() const { return truncated_; }  // a query stopped at the nesting limit: elements were not seen
    Obj root() const { return root_; }
    ObjRef root_ref() const { return root_ref_; }
    const std::vector<StructElem>& elems() const { return elems_; }
    const StructElem& elem(int i) const { return elems_[i]; }
    const std::vector<int>& root_elems() const { return root_elems_; }
    // Element indices are in pre-order (first kid first, each element at its first occurrence).
    // veraPDF's validation order (simulated BaseValidator traversal): only elements it validates.
    const std::vector<int>& verapdf_order() const { return verapdf_order_; }
    const std::vector<StructKid>& root_kids() const { return root_kids_; }
    // Element index of an indirect structure element dictionary (-1 when not part of the traversal).
    int find(const ObjRef& r) const;

    // /RoleMap entries whose value is a name (veraPDF Map<ASAtom, ASAtom>).
    const std::unordered_map<std::string, std::string>& role_map() const { return role_map_; }

    bool has_parent_tree() const { return parent_tree_.isDictionary(); }
    Obj parent_tree() const { return parent_tree_; }
    // ParentTree entry for a key with veraPDF's PDNumberTreeNode.getObject semantics: /Limits respected on
    // every node, a node with /Nums ignores its /Kids (last duplicate key wins), kids searched in order,
    // first hit wins; null handle when absent.
    Obj parent_tree_entry(std::int64_t key) const;
    // Every leaf entry of the ParentTree (flattened, first occurrence of a key), for enumeration.
    const std::unordered_map<std::int64_t, Obj>& parent_tree_entries() const { return parent_tree_entries_; }
    // veraPDF StructureElementAccessObject.getStructureElement: StructParent wins; else StructParents + MCID.
    Obj struct_elem_for_mcid(const std::optional<std::int64_t>& struct_parent,
                             const std::optional<std::int64_t>& struct_parents, std::optional<std::int64_t> mcid) const;

    // --- veraPDF type semantics -----------------------------------------------------------------
    static bool is_standard17(const std::string& type);
    static bool is_standard20(const SType& t);
    static bool is_pass_through(const std::string& standard_type);   // NonStruct, Div, Part
    std::optional<std::string> standard_type17(const std::string& type) const;
    bool circular17(const std::string& type) const;
    std::optional<SType> default_type20(const SType& t) const;        // rolemap::standard_type20
    bool circular20(const SType& t) const;
    std::optional<std::string> role_map_to_same_ns_tag(const SType& t) const;   // rolemap::remap_within_namespace
    // Standard type of an arbitrary structure element dictionary (also one outside the traversal).
    std::optional<std::string> standard_of(const Obj& elem_dict, Flavor f) const;
    std::optional<SType> stype_of(const Obj& elem_dict) const;

    // veraPDF GFPDStructElem.getparentStandardType: walks /P skipping pass-through parents. Returns
    // "StructTreeRoot", "MathML" (Pdf20, MathML namespace) or the standard type; nullopt when none.
    std::optional<std::string> parent_standard_type(const Obj& elem_dict, Flavor f) const;
    // veraPDF GFPDStructElem.getparentLang: first /Lang on the /P chain (excluding the element itself).
    std::optional<std::string> parent_lang(const Obj& elem_dict) const;
    // True when following /P from this dictionary reaches a dictionary with /Type /StructTreeRoot.
    bool reaches_root(const Obj& elem_dict) const;
    // Any element on the /P chain (including this one) whose standard type is `standard_type`.
    bool has_ancestor_standard(const Obj& elem_dict, const std::string& standard_type, Flavor f) const;

    // Kid standard types joined with '&' with pass-through children flattened (veraPDF kidsStandardTypes).
    std::vector<std::string> kids_standard_types(int elem, Flavor f) const;
    std::vector<std::string> root_kids_standard_types(Flavor f) const;
    // veraPDF gethasContentItems: content item kids, also through pass-through children.
    bool has_content_items(int elem, Flavor f) const;

private:
    mutable bool truncated_ = false;
    // Records one /K item of a node (elem_index -1 = StructTreeRoot); returns the index of a newly created
    // element (to be expanded next), else -1.
    int collect_kid(const Obj& item, int elem_index, ObjRef inherited_page, int depth);
    void simulate_verapdf_visits();

    bool present_ = false;
    Obj root_;
    ObjRef root_ref_;
    std::vector<StructElem> elems_;
    std::vector<int> root_elems_;
    std::vector<int> verapdf_order_;
    std::vector<StructKid> root_kids_;
    std::unordered_map<std::uint64_t, int> by_ref_;
    std::unordered_map<std::string, std::string> role_map_;
    Obj parent_tree_;
    std::unordered_map<std::int64_t, Obj> parent_tree_entries_;
    mutable std::unordered_map<std::int64_t, Obj> parent_tree_cache_;
    mutable NumsIndex parent_tree_nums_;
};

}  // namespace eqx
