// Logical structure model (built once per document), with the standard types of both flavours (role mapping:
// core/role_map.h):
//   Flavor::Pdf17  PDF/UA-1 rules: /RoleMap chains resolved against the PDF 1.7 standard types
//   Flavor::Pdf20  PDF/UA-2 and WTPDF rules: namespaces (/NS, /RoleMapNS) plus /RoleMap
#pragma once

#include <cstdint>
#include <memory>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cos.h"
#include "core/struct_store.h"
#include "core/tree_shape.h"
#include "util/chunked_vector.h"

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

class StructTree;

// The tree keeps its elements and their kids as packed records (StructTree, private part). What a reader gets are the
// views below: values with the members a plain record would have, made when an element or a kid is asked for. They
// refer to the tree, which has to outlive them, and cannot be assigned or default-constructed.

// One child in /K of an element or of the StructTreeRoot.
struct StructKid {
    enum class Kind : std::uint8_t { Elem, MCID, MCR, OBJR };
    Kind kind;
    int elem;                 // Elem: index into StructTree::elems (also for an element visited before), else -1
    bool revisit;             // Elem: the element was already reached through another kid (shared / cyclic)
    std::int64_t mcid;        // MCID / MCR (-1 = MCR without integer /MCID), else -1
    const Obj& dict;          // Elem / MCR / OBJR dictionary (an uninitialised handle for an MCID)
    ObjRef page;              // MCID/MCR/OBJR: /Pg of the MCR/OBJR, else the element's (inherited) /Pg
    ObjRef stm;               // MCR /Stm
    const Obj& obj;           // OBJR /Obj (an uninitialised handle otherwise)
};

// The kids of one node in /K order: size(), [i], range-for (each kid is made as it is reached).
class StructKids {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = StructKid;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = StructKid;

        iterator(const StructKids* kids, std::size_t i) : kids_(kids), i_(i) {}
        StructKid operator*() const { return (*kids_)[i_]; }
        iterator& operator++() {
            ++i_;
            return *this;
        }
        iterator operator++(int) {
            iterator t = *this;
            ++i_;
            return t;
        }
        bool operator==(const iterator& o) const { return i_ == o.i_; }
        bool operator!=(const iterator& o) const { return i_ != o.i_; }

    private:
        const StructKids* kids_;
        std::size_t i_;
    };

    StructKids(const StructTree* tree, const std::uint64_t* words, std::uint32_t n, ObjRef page)
        : tree_(tree), words_(words), n_(n), page_(page) {}
    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    [[gnu::always_inline]] inline StructKid operator[](std::size_t i) const;
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, n_}; }

private:
    const StructTree* tree_;
    const std::uint64_t* words_;   // the packed kids (StructTree::kid)
    std::uint32_t n_;
    ObjRef page_;                  // the page of an MCID kid: the node's inherited /Pg
};

struct StructElem {
    int index;
    int parent;               // traversal parent element (-1 = kid of the StructTreeRoot)
    int depth;
    const Obj& dict;
    ObjRef ref;
    // The four facts of the element's type: one record per type in the tree, shared by its elements.
    const std::optional<std::string>& S;        // /S when it is a name
    const SType& type;                          // veraPDF getStructureType() (S + NS); type empty when S is not a name
    const std::optional<std::string>& std17;    // standard type, PDF/UA-1 semantics
    const std::optional<SType>& std20;          // standard type, ISO 32000-2 semantics
    // The number of that record, below StructTree::type_count(): elements with the same type_id have the same S, type,
    // std17 and std20, so whatever a reader derives from these four alone it can keep per type_id.
    std::uint32_t type_id;
    StructKids kids;                    // every child in /K order (elements, MCIDs, MCRs, OBJRs)
    std::span<const int> elems;         // tree children: structure elements first reached through this element
    std::span<const int> struct_kids;   // every structure-element child in /K order, also shared/revisited ones
                                        // (veraPDF getStructChildren: kidsStandardTypes, hasContentItems)
    // veraPDF validates each model object once (BaseValidator idSet). A direct structure element has the id
    // "0 0 obj <S>", so of several direct elements with the same /S only the one pushed first is validated,
    // together with its subtree. False for elements veraPDF never validates (simulated push order).
    bool verapdf_checked;
    std::uint16_t keys;                 // bits of Key: the keys the dictionary holds
    // /P is an indirect reference to the object of `parent` (to the StructTreeRoot for parent -1): cos::get(dict, "P")
    // is that dictionary, known without reading /P.
    bool p_is_parent;
    // Every item of /K is one of `kids`: none was passed over (a name, a string, a dictionary of another /Type, ...).
    bool k_is_kids;
    ObjRef page;                        // own /Pg
    ObjRef inherited_page;              // own /Pg, else nearest ancestor's /Pg

    // Which of these keys the element's dictionary holds, whatever their values (noted when the element is collected).
    // Most elements have none of /Alt, /ActualText, /E, /Lang, /ID, /Ref, /C, and a rule that reads such a key from
    // every element asks here first: get() is cos::get(dict, name) without the dictionary lookup for an absent key.
    enum Key : std::uint16_t {
        kP = 1 << 0, kAlt = 1 << 1, kActualText = 1 << 2, kE = 1 << 3, kLang = 1 << 4,
        kA = 1 << 5, kC = 1 << 6, kID = 1 << 7, kRef = 1 << 8, kT = 1 << 9,
    };
    bool has_key(Key k) const { return (keys & k) != 0; }
    Obj get(Key k, std::string_view name) const { return has_key(k) ? cos::get(dict, name) : Obj::newNull(); }

    // Standard type in a flavour ("" when none). For Pdf20 this is the type name only.
    std::string standard(Flavor f) const;
    bool is_math(Flavor f) const;       // Pdf20 and standard namespace is MathML
};

// The elements of a tree in index order: size(), [i], range-for.
class StructElems {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = StructElem;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = StructElem;

        iterator(const StructTree* tree, std::size_t i) : tree_(tree), i_(i) {}
        [[gnu::always_inline]] inline StructElem operator*() const;
        iterator& operator++() {
            ++i_;
            return *this;
        }
        iterator operator++(int) {
            iterator t = *this;
            ++i_;
            return t;
        }
        bool operator==(const iterator& o) const { return i_ == o.i_; }
        bool operator!=(const iterator& o) const { return i_ != o.i_; }

    private:
        const StructTree* tree_;
        std::size_t i_;
    };

    explicit StructElems(const StructTree* tree) : tree_(tree) {}
    inline std::size_t size() const;
    bool empty() const { return size() == 0; }
    [[gnu::always_inline]] inline StructElem operator[](std::size_t i) const;
    iterator begin() const { return {tree_, 0}; }
    iterator end() const { return {tree_, size()}; }

private:
    const StructTree* tree_;
};

class StructTree {
public:
    void build(Document& doc);

    bool present() const { return present_; }
    bool truncated() const { return truncated_; }  // a query stopped at the nesting limit: elements were not seen
    Obj root() const { return root_; }
    ObjRef root_ref() const { return root_ref_; }
    StructElems elems() const { return StructElems(this); }
    // (A view is made in place, inlined: only the members that are read get built.)
    [[gnu::always_inline]] inline StructElem elem(int i) const;
    std::size_t type_count() const { return types_.size(); }   // StructElem::type_id is below it
    const std::vector<int>& root_elems() const { return root_elems_; }
    // Element indices are in pre-order (first kid first, each element at its first occurrence).
    // veraPDF's validation order (simulated BaseValidator traversal): only elements it validates.
    const std::vector<int>& verapdf_order() const { return verapdf_order_; }
    StructKids root_kids() const { return StructKids(this, root_kids_, root_n_kids_, ObjRef{}); }
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
    friend class StructKids;
    friend class StructElems;

    // --- storage ---------------------------------------------------------------------------------
    // An element is 64 bytes whatever its type and however many kids it has. What its type says (S, type, std17,
    // std20) is in one TypeFacts per type of the tree, its two pages are numbers in pages_, and its kids are one
    // block of the arena:
    //     n_kids words        the kids in /K order (kid())
    //     n_struct ints       StructElem::struct_kids: the elements among the kids, in the same order
    //     only with kShared:  one int n, then n ints: StructElem::elems (struct_kids without the revisited ones;
    //                         without kShared no kid is revisited and elems is struct_kids)
    // The records are in chunks, so they are never copied and there is no spare capacity beyond the last chunk.
    struct Rec {
        Obj dict;
        ObjRef ref;
        std::int32_t parent = -1;
        std::int32_t depth = 0;
        std::uint32_t type = 0;             // index into types_
        std::uint32_t page = 0;             // index into pages_: own /Pg
        std::uint32_t inherited_page = 0;   // index into pages_
        std::uint32_t n_kids = 0;
        const std::uint64_t* block = nullptr;
        std::uint32_t n_struct = 0;
        std::uint16_t keys = 0;
        std::uint8_t flags = 0;             // kChecked, kShared, kParentByP, kAllKids
    };
    enum : std::uint8_t { kChecked = 1, kShared = 2, kParentByP = 4, kAllKids = 8 };
    struct TypeFacts {
        std::optional<std::string> S;
        SType type;
        std::optional<std::string> std17;
        std::optional<SType> std20;
    };
    // A kid is one word: the kind in bits 0-1, a flag in bit 2, a signed value in the 61 bits above.
    //   Elem  value = element index, flag = revisit
    //   MCID  value = the MCID; with the flag (an MCID outside 61 bits) the index of the MCID in wide_mcids_
    //   MCR   value = index into mcrs_
    //   OBJR  value = index into objrs_
    static std::uint64_t kid(StructKid::Kind kind, bool flag, std::int64_t value) {
        return (static_cast<std::uint64_t>(value) << 3) | (flag ? 4u : 0u) | static_cast<std::uint64_t>(kind);
    }
    struct McrRec {
        Obj dict;
        ObjRef page, stm;
        std::int64_t mcid = -1;
    };
    struct ObjrRec {
        Obj dict, obj;
        ObjRef page;
    };
    [[gnu::always_inline]] inline StructKid kid_view(std::uint64_t word, ObjRef mcid_page) const;
    static const int* ints_of(const Rec& r) { return reinterpret_cast<const int*>(r.block + r.n_kids); }
    static std::span<const int> struct_kids_of(const Rec& r) { return {ints_of(r), r.n_struct}; }
    static std::span<const int> elems_of(const Rec& r) {
        const int* ints = ints_of(r);
        if (!(r.flags & kShared)) return {ints, r.n_struct};
        return {ints + r.n_struct + 1, static_cast<std::size_t>(ints[r.n_struct])};
    }
    std::string standard_rec(const Rec& r, Flavor f) const;   // StructElem::standard

    mutable bool truncated_ = false;
    struct Build;   // what only the traversal needs (structure.cpp)
    // Records one /K item of a node (elem_index -1 = StructTreeRoot) as a kid word in *out. Returns the index of a
    // newly created element (to be expanded next; the /K value its dictionary holds, if any, in *kids_out), -1 for any other kid,
    // -2 when the item is no kid (nothing written).
    int collect_kid(const Obj& item, int elem_index, std::uint32_t inherited_page, int depth, Build& b, std::uint64_t* out,
                    Obj* kids_out);
    // Gives a node (elem_index -1 = StructTreeRoot) its kids: `n` words. `all_items`: every item of its /K is among them.
    void seal_kids(int elem_index, const std::uint64_t* words, std::size_t n, bool all_items);
    void simulate_verapdf_visits();

    static const Obj kNoObj;   // StructKid::dict / obj of a kid that has none: a default-constructed handle

    bool present_ = false;
    Obj root_;
    ObjRef root_ref_;
    ChunkedVector<Rec, 512> elems_;
    std::vector<TypeFacts> types_;        // [0]: an element whose /S is no name
    std::vector<ObjRef> pages_;           // [0]: no page
    BlockArena arena_;                    // the kid blocks
    ChunkedVector<McrRec, 256> mcrs_;
    ChunkedVector<ObjrRec, 256> objrs_;
    std::vector<std::int64_t> wide_mcids_;
    std::vector<int> root_elems_;
    std::vector<int> verapdf_order_;
    const std::uint64_t* root_kids_ = nullptr;   // the kids of the StructTreeRoot, in the arena
    std::uint32_t root_n_kids_ = 0;
    RefIndex by_ref_;
    std::unordered_map<std::string, std::string> role_map_;
    Obj parent_tree_;
    std::unordered_map<std::int64_t, Obj> parent_tree_entries_;
    mutable std::unordered_map<std::int64_t, Obj> parent_tree_cache_;
    mutable NumsIndex parent_tree_nums_;
    mutable std::unique_ptr<TreeShape<std::int64_t>> parent_tree_shape_;   // the /Kids and /Limits, as lookups reach them
    // The role-mapping answers per type, computed at the first question (standard_type17 ... role_map_to_same_ns_tag).
    struct TypeFacts20 {
        bool standard_known = false, circular_known = false, same_ns_known = false;
        std::optional<SType> standard;
        bool circular = false;
        std::optional<std::string> same_ns;
    };
    TypeFacts20* facts20(const SType& t) const;   // nullptr for a type in a direct namespace dictionary (not kept)
    mutable std::unordered_map<std::string, std::optional<std::string>> standard17_;
    mutable std::unordered_map<std::string, bool> circular17_;
    mutable std::unordered_map<std::uint64_t, std::unordered_map<std::string, TypeFacts20>> facts20_;   // by namespace object
    // reaches_root per dictionary, filled as the /P chains are walked: elements of the traversal by index, other
    // indirect dictionaries by object.
    mutable std::vector<std::uint8_t> reaches_root_elem_;
    mutable std::unordered_map<std::uint64_t, std::uint8_t> reaches_root_other_;
};

inline StructKid StructTree::kid_view(std::uint64_t word, ObjRef mcid_page) const {
    const auto kind = static_cast<StructKid::Kind>(word & 3);
    const bool flag = (word & 4) != 0;
    const std::int64_t value = static_cast<std::int64_t>(word) >> 3;
    switch (kind) {
        case StructKid::Kind::Elem:
            return {kind, static_cast<int>(value), flag, -1, elems_[static_cast<std::size_t>(value)].dict, ObjRef{}, ObjRef{}, kNoObj};
        case StructKid::Kind::MCID:
            return {kind, -1, false, flag ? wide_mcids_[static_cast<std::size_t>(value)] : value, kNoObj, mcid_page, ObjRef{}, kNoObj};
        case StructKid::Kind::MCR: {
            const McrRec& m = mcrs_[static_cast<std::size_t>(value)];
            return {kind, -1, false, m.mcid, m.dict, m.page, m.stm, kNoObj};
        }
        case StructKid::Kind::OBJR:
        default: {
            const ObjrRec& o = objrs_[static_cast<std::size_t>(value)];
            return {kind, -1, false, -1, o.dict, o.page, ObjRef{}, o.obj};
        }
    }
}

inline StructElem StructTree::elem(int i) const {
    const Rec& r = elems_[static_cast<std::size_t>(i)];
    const TypeFacts& t = types_[r.type];
    const ObjRef inherited = pages_[r.inherited_page];
    return {i,       r.parent, r.depth, r.dict, r.ref, t.S, t.type, t.std17, t.std20, r.type,
            StructKids(this, r.block, r.n_kids, inherited), elems_of(r), struct_kids_of(r), (r.flags & kChecked) != 0,
            r.keys,  (r.flags & kParentByP) != 0, (r.flags & kAllKids) != 0, pages_[r.page], inherited};
}

inline StructKid StructKids::operator[](std::size_t i) const { return tree_->kid_view(words_[i], page_); }
inline StructElem StructElems::iterator::operator*() const { return tree_->elem(static_cast<int>(i_)); }
inline std::size_t StructElems::size() const { return tree_->elems_.size(); }
inline StructElem StructElems::operator[](std::size_t i) const { return tree_->elem(static_cast<int>(i)); }

}  // namespace eqx
