#include "core/structure.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <span>
#include <unordered_set>

#include "core/document.h"
#include "core/role_map.h"

namespace eqx {

namespace {

const std::set<std::string>& std17_set() {
    static const std::set<std::string> s = {
        "Document", "Part", "Div", "Caption", "Sect", "NonStruct", "H", "P", "L", "LI", "Lbl", "LBody", "Table",
        "TR", "TH", "TD", "Span", "Link", "Figure", "Formula", "Form", "Art", "BlockQuote", "TOC", "TOCI",
        "Index", "Private", "Quote", "Note", "Reference", "BibEntry", "Code", "H1", "H2", "H3", "H4", "H5", "H6",
        "THead", "TBody", "TFoot", "Annot", "Ruby", "Warichu", "RB", "RT", "RP", "WT", "WP"};
    return s;
}

const std::set<std::string>& std20_set() {
    static const std::set<std::string> s = {
        "Document", "Part", "Div", "Caption", "Sect", "NonStruct", "H", "P", "L", "LI", "Lbl", "LBody", "Table",
        "TR", "TH", "TD", "Span", "Link", "Figure", "Formula", "Form", "THead", "TBody", "TFoot", "Annot", "Ruby",
        "Warichu", "RB", "RT", "RP", "WT", "WP", "DocumentFragment", "Aside", "Title", "FENote", "Sub", "Em",
        "Strong", "Artifact"};
    return s;
}

// Membership in one of the two sets above without a tree search over strings: the names of a set grouped by length,
// compared as bytes. The same answer as set.count(type) > 0.
class NameSet {
public:
    explicit NameSet(const std::set<std::string>& names) {
        for (const std::string& n : names) {
            if (n.size() >= by_length_.size()) by_length_.resize(n.size() + 1);
            by_length_[n.size()].push_back(n);
        }
    }
    bool contains(std::string_view name) const {
        if (name.size() >= by_length_.size()) return false;
        for (const std::string& n : by_length_[name.size()])
            if (name == n) return true;
        return false;
    }

private:
    std::vector<std::vector<std::string>> by_length_;
};

bool in_std17(std::string_view type) {
    static const NameSet names(std17_set());
    return names.contains(type);
}

bool in_std20(std::string_view type) {
    static const NameSet names(std20_set());
    return names.contains(type);
}

bool is_hn(const std::string& t) {
    // ^H[1-9][0-9]*$
    if (t.size() < 2 || t[0] != 'H' || t[1] < '1' || t[1] > '9') return false;
    for (std::size_t i = 2; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

std::optional<std::string> ns_string(const Obj& ns) {
    if (!ns.isDictionary()) return std::nullopt;
    return cos::text(cos::get(ns, "NS"));
}

// The namespace of a structure type that has none: one null object for all of them (Obj::newNull() allocates an
// object per call, which is kept for every structure element).
const Obj& no_namespace() {
    static const Obj null = Obj::newNull();
    return null;
}

std::optional<SType> make_stype(const Obj& type, const Obj& ns) {
    if (!type.isName()) return std::nullopt;
    SType t;
    t.type = *cos::name(type);
    t.ns = ns.isDictionary() ? ns : no_namespace();
    return t;
}

}  // namespace

std::optional<std::string> SType::ns_uri() const {
    if (!ns.isDictionary()) return std::string(kNsPdf);
    return ns_string(ns);
}

bool SType::same(const SType& o) const { return type == o.type && ns_uri() == o.ns_uri(); }

std::string StructTree::standard_rec(const Rec& r, Flavor f) const {
    const TypeFacts& t = types_[r.type];
    if (f == Flavor::Pdf17) return t.std17 ? *t.std17 : std::string();
    if (!t.std20) return std::string();
    if (t.std20->ns_uri() == std::optional<std::string>(kNsMathML)) return "MathML";
    return t.std20->type;
}

std::string StructElem::standard(Flavor f) const {
    if (f == Flavor::Pdf17) return std17 ? *std17 : std::string();
    if (!std20) return std::string();
    if (std20->ns_uri() == std::optional<std::string>(kNsMathML)) return "MathML";
    return std20->type;
}

bool StructElem::is_math(Flavor f) const {
    return f == Flavor::Pdf20 && std20 && std20->ns_uri() == std::optional<std::string>(kNsMathML);
}

bool StructTree::is_standard17(const std::string& type) { return in_std17(type); }

bool StructTree::is_standard20(const SType& t) {
    auto uri = t.ns.isDictionary() ? ns_string(t.ns) : std::nullopt;
    if (t.ns.isDictionary() && uri) {
        if (*uri == kNsPdf) return in_std17(t.type);
        if (*uri == kNsPdf2) return in_std20(t.type) || is_hn(t.type);
        if (*uri == kNsMathML) return true;
        return false;
    }
    return in_std17(t.type);
}

bool StructTree::is_pass_through(const std::string& t) { return t == "NonStruct" || t == "Div" || t == "Part"; }

// The role-mapping answers depend on the type alone: on its name and the /RoleMap (PDF 1.7), on its name, its namespace
// dictionary and the /RoleMap (PDF 2.0). A document has few types and many elements, so each answer is computed once per
// type: by name, and for PDF 2.0 per namespace object (types in a direct namespace dictionary are computed every time).

std::optional<std::string> StructTree::standard_type17(const std::string& type) const {
    auto known = standard17_.find(type);
    if (known == standard17_.end()) known = standard17_.emplace(type, rolemap::standard_type17(role_map_, type)).first;
    return known->second;
}

bool StructTree::circular17(const std::string& type) const {
    auto known = circular17_.find(type);
    if (known == circular17_.end()) known = circular17_.emplace(type, rolemap::circular17(role_map_, type)).first;
    return known->second;
}

StructTree::TypeFacts20* StructTree::facts20(const SType& t) const {
    std::uint64_t ns = 0;   // no namespace dictionary
    if (t.ns.isDictionary()) {
        if (!t.ns.isIndirect()) return nullptr;
        ns = t.ns_ref().key();
    }
    return &facts20_[ns][t.type];
}

std::optional<SType> StructTree::default_type20(const SType& t) const {
    TypeFacts20* f = facts20(t);
    if (!f) return rolemap::standard_type20(role_map_, t);
    if (!f->standard_known) {
        f->standard = rolemap::standard_type20(role_map_, t);
        f->standard_known = true;
    }
    return f->standard;
}

bool StructTree::circular20(const SType& t) const {
    TypeFacts20* f = facts20(t);
    if (!f) return rolemap::circular20(t);
    if (!f->circular_known) {
        f->circular = rolemap::circular20(t);
        f->circular_known = true;
    }
    return f->circular;
}

std::optional<std::string> StructTree::role_map_to_same_ns_tag(const SType& t) const {
    TypeFacts20* f = facts20(t);
    if (!f) return rolemap::remap_within_namespace(t);
    if (!f->same_ns_known) {
        f->same_ns = rolemap::remap_within_namespace(t);
        f->same_ns_known = true;
    }
    return f->same_ns;
}

std::optional<SType> StructTree::stype_of(const Obj& d) const {
    if (!d.isDictionary()) return std::nullopt;
    return make_stype(cos::get(d, "S"), cos::get(d, "NS"));
}

std::optional<std::string> StructTree::standard_of(const Obj& d, Flavor f) const {
    if (!d.isDictionary()) return std::nullopt;
    if (f == Flavor::Pdf17) {
        auto s = cos::name(d, "S");
        if (!s) return std::nullopt;
        return standard_type17(*s);
    }
    auto t = stype_of(d);
    if (!t) return std::nullopt;
    auto st = default_type20(*t);
    if (!st) return std::nullopt;
    if (st->ns_uri() == std::optional<std::string>(kNsMathML)) return std::string("MathML");
    return st->type;
}

std::optional<std::string> StructTree::parent_standard_type(const Obj& d, Flavor f) const {
    // GFPDStructElem.getparentStandardType: the pass-through test uses the standard type name, the MathML
    // namespace is checked only for the final parent.
    Obj parent = cos::get(d, "P");
    if (parent.isNull()) return std::nullopt;
    std::unordered_set<std::uint64_t> seen;
    bool math = false;
    auto std_of = [&](const Obj& p) -> std::optional<std::string> {
        math = false;
        if (f == Flavor::Pdf20) {
            auto t = stype_of(p);
            if (!t) return std::nullopt;
            auto st = default_type20(*t);
            if (!st) return std::nullopt;
            math = st->ns_uri() == std::optional<std::string>(kNsMathML);
            return st->type;
        }
        return standard_of(p, f);
    };
    auto pst = std_of(parent);
    while (pst && is_pass_through(*pst)) {
        if (parent.isIndirect() && !seen.insert(ref_of(parent).key()).second) return std::nullopt;
        parent = cos::get(parent, "P");
        if (parent.isNull()) return std::nullopt;
        pst = std_of(parent);
    }
    if (cos::name_is(parent, "Type", "StructTreeRoot")) return std::string("StructTreeRoot");
    if (math) return std::string("MathML");
    return pst;
}

std::optional<std::string> StructTree::parent_lang(const Obj& d) const {
    std::unordered_set<std::uint64_t> seen;
    if (d.isIndirect()) seen.insert(ref_of(d).key());
    Obj parent = cos::get(d, "P");
    while (parent.isDictionary()) {
        if (parent.isIndirect() && !seen.insert(ref_of(parent).key()).second) break;
        Obj lang = cos::get(parent, "Lang");
        if (lang.isString()) return cos::text(lang);
        parent = cos::get(parent, "P");
    }
    return std::nullopt;
}

bool StructTree::reaches_root(const Obj& d) const {
    // The answer belongs to the dictionary alone: its /P chain is fixed, and every dictionary on the chain has the
    // answer of the walk that passes it (the root reached: all of them reach it; a non-dictionary or a dictionary
    // met again: none does). It is kept per indirect dictionary, so a walk ends at the first one already answered and
    // a tree is walked once in all, not once per element. Elements of the tree keep it by index, other dictionaries
    // (outside the traversal) by object.
    enum : std::uint8_t { kUnknown = 0, kNo = 1, kYes = 2, kWalking = 3 };
    if (reaches_root_elem_.size() != elems_.size()) reaches_root_elem_.assign(elems_.size(), kUnknown);
    auto slot = [&](std::uint64_t key) -> std::uint8_t& {
        const int i = by_ref_.find(key);
        return i >= 0 ? reaches_root_elem_[static_cast<std::size_t>(i)] : reaches_root_other_[key];
    };
    std::vector<std::uint64_t> path;   // the indirect dictionaries of this walk, marked kWalking meanwhile
    bool result = false;
    try {
        Obj e = d;
        while (e.isDictionary()) {
            std::uint64_t key = 0;
            std::uint8_t* state = nullptr;
            bool element = false;   // an element of the traversal: its /Type is absent or /StructElem (collect_kid)
            if (e.isIndirect()) {
                key = ref_of(e).key();
                element = by_ref_.find(key) >= 0;
                state = &slot(key);
                if (*state == kNo || *state == kYes) {
                    result = *state == kYes;
                    break;
                }
            }
            if (!element && cos::name_is(e, "Type", "StructTreeRoot")) {
                result = true;
                break;
            }
            if (state) {
                if (*state == kWalking) break;   // met again in this walk: a loop
                *state = kWalking;
                path.push_back(key);
            }
            e = cos::get(e, "P");
        }
    } catch (...) {
        for (std::uint64_t k : path) slot(k) = kUnknown;   // nothing learned
        throw;
    }
    for (std::uint64_t k : path) slot(k) = result ? kYes : kNo;
    return result;
}

bool StructTree::has_ancestor_standard(const Obj& d, const std::string& standard_type, Flavor f) const {
    std::unordered_set<std::uint64_t> seen;
    Obj e = d;
    while (e.isDictionary()) {
        auto st = standard_of(e, f);
        if (st && *st == standard_type) return true;
        if (e.isIndirect() && !seen.insert(ref_of(e).key()).second) break;
        e = cos::get(e, "P");
    }
    return false;
}

std::vector<std::string> StructTree::kids_standard_types(int i, Flavor f) const {
    std::vector<std::string> out;
    std::function<void(int, int)> rec = [&](int e, int depth) {
        if (depth > 512) {
            truncated_ = true;
            return;
        }
        for (int k : struct_kids_of(elems_[static_cast<std::size_t>(e)])) {
            std::string st = standard_rec(elems_[static_cast<std::size_t>(k)], f);
            if (is_pass_through(st)) rec(k, depth + 1);
            else if (!st.empty()) out.push_back(st);
        }
    };
    rec(i, 0);
    return out;
}

std::vector<std::string> StructTree::root_kids_standard_types(Flavor f) const {
    std::vector<std::string> out;
    std::function<void(int, int)> rec = [&](int e, int depth) {
        if (depth > 512) {
            truncated_ = true;
            return;
        }
        for (int k : struct_kids_of(elems_[static_cast<std::size_t>(e)])) {
            std::string st = standard_rec(elems_[static_cast<std::size_t>(k)], f);
            if (is_pass_through(st)) rec(k, depth + 1);
            else if (!st.empty()) out.push_back(st);
        }
    };
    for (const StructKid& rk : root_kids()) {
        if (rk.kind != StructKid::Kind::Elem || rk.elem < 0) continue;
        int k = rk.elem;
        std::string st = standard_rec(elems_[static_cast<std::size_t>(k)], f);
        if (is_pass_through(st)) rec(k, 1);
        else if (!st.empty()) out.push_back(st);
    }
    return out;
}

bool StructTree::has_content_items(int i, Flavor f) const {
    std::function<bool(int, int)> rec = [&](int e, int depth) -> bool {
        if (depth > 512) {
            truncated_ = true;
            return false;
        }
        const Rec& r = elems_[static_cast<std::size_t>(e)];
        if (r.n_kids != r.n_struct) return true;   // a kid that is no element
        for (int k : struct_kids_of(r))
            if (is_pass_through(standard_rec(elems_[static_cast<std::size_t>(k)], f)) && rec(k, depth + 1)) return true;
        return false;
    };
    return rec(i, 0);
}

int StructTree::find(const ObjRef& r) const {
    return by_ref_.find(r.key());
}

namespace {

// The entries of one /Nums array: integer keys of complete pairs with the position of their last occurrence, sorted.
NumsIndex::Entries index_nums(Obj& nums) {
    NumsIndex::Entries e;
    const int n = nums.getArrayNItems();
    e.reserve(static_cast<std::size_t>(n / 2));
    for (int i = 0; i + 1 < n; i += 2) {
        Obj k = nums.getArrayItem(i);
        if (k.isInteger()) e.emplace_back(k.getIntValue(), i);
    }
    std::stable_sort(e.begin(), e.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::size_t w = 0;   // keep the last of each run of equal keys: stable, so that is the last occurrence
    for (std::size_t j = 0; j < e.size(); ++j)
        if (j + 1 == e.size() || e[j + 1].first != e[j].first) e[w++] = e[j];
    e.resize(w);
    e.shrink_to_fit();
    return e;
}

// The index of `nums` (held by `node`), built on first use; nullptr when neither is an indirect object.
const NumsIndex::Entries* nums_entries(NumsIndex* cache, const Obj& node, Obj& nums) {
    if (!cache) return nullptr;
    std::unordered_map<std::uint64_t, NumsIndex::Entries>* map = nullptr;
    std::uint64_t id = 0;
    if (nums.isIndirect()) {
        map = &cache->by_array;
        id = ref_of(nums).key();
    } else if (node.isIndirect()) {
        map = &cache->by_node;
        id = ref_of(node).key();
    } else {
        return nullptr;
    }
    auto it = map->find(id);
    if (it == map->end()) it = map->emplace(id, index_nums(nums)).first;
    return &it->second;
}

// The value of `key` in the /Nums of a node that has /Nums: the last of duplicate keys; a null handle when the key is
// not there (or /Nums is no array). With `cache`, the array is looked up through its index (the same result as the scan).
Obj nums_value(const Obj& node, std::int64_t key, NumsIndex* cache) {
    Obj found = Obj::newNull();
    Obj nums = const_cast<Obj&>(node).getKey("/Nums");
    if (nums.isArray()) {
        if (const NumsIndex::Entries* e = nums_entries(cache, node, nums)) {
            auto it = std::lower_bound(e->begin(), e->end(), key,
                                       [](const auto& entry, std::int64_t k) { return entry.first < k; });
            if (it != e->end() && it->first == key) found = nums.getArrayItem(it->second + 1);
        } else {
            int n = nums.getArrayNItems();
            for (int i = 0; i + 1 < n; i += 2) {
                Obj k = nums.getArrayItem(i);
                if (k.isInteger() && k.getIntValue() == key) found = nums.getArrayItem(i + 1);   // last wins
            }
        }
    }
    return found;
}

bool integer_limit(const Obj& item, std::int64_t& out) {
    if (!item.isInteger()) return false;
    out = item.getIntValue();
    return true;
}

// PDNumberTreeNode.getObject: Limits, /Nums before /Kids, the last duplicate wins, the first kid that has the key
// decides, a node met again ends its list of kids (LoopedException). On an explicit stack: nesting has no limit.
// With `cache`, a /Nums array is looked up through its index (the same result as the scan).
// (StructTree::parent_tree_entry searches a plain tree through its TreeShape and comes here for any other.)
Obj number_tree_lookup(const Obj& root, std::int64_t key, std::unordered_set<std::uint64_t>& visited,
                       NumsIndex* cache = nullptr) {
    struct Level {
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    std::vector<Level> stack;
    // Opens a node: true with `found` set when its /Nums decide, false when it has kids (pushed) or nothing.
    auto open = [&](const Obj& node, Obj& found) {
        if (!node.isDictionary()) return false;
        Obj limits = cos::get(node, "Limits");
        if (limits.isArray() && limits.getArrayNItems() >= 2 && limits.getArrayItem(0).isInteger() &&
            limits.getArrayItem(1).isInteger()) {
            if (key < limits.getArrayItem(0).getIntValue() || key > limits.getArrayItem(1).getIntValue()) return false;
        }
        if (const_cast<Obj&>(node).hasKey("/Nums")) {
            found = nums_value(node, key, cache);
            return !found.isNull();
        }
        Obj kids = cos::get(node, "Kids");
        if (kids.isArray()) stack.push_back({cos::array_items(kids), 0});
        return false;
    };
    Obj found = Obj::newNull();
    if (open(root, found)) return found;
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            stack.pop_back();
            continue;
        }
        Obj kid = top.kids[top.next++];
        if (kid.isIndirect() && !visited.insert(ref_of(kid).key()).second) {
            stack.pop_back();   // veraPDF: LoopedException leaves this list of kids
            continue;
        }
        if (open(kid, found)) return found;
    }
    return Obj::newNull();
}

}  // namespace

Obj StructTree::parent_tree_entry(std::int64_t key) const {
    if (!parent_tree_.isDictionary()) return Obj::newNull();
    auto it = parent_tree_cache_.find(key);
    if (it != parent_tree_cache_.end()) return it->second;
    // A plain tree through its decoded shape (streams are no nodes here), any other by the walk over the objects.
    if (!parent_tree_shape_) parent_tree_shape_ = std::make_unique<TreeShape<std::int64_t>>(parent_tree_, "Nums", &integer_limit, false);
    Obj r = Obj::newNull();
    Obj found = Obj::newNull();
    const auto leaf = [&](const Obj& node, Obj& value) {
        value = nums_value(node, key, &parent_tree_nums_);
        return !value.isNull();
    };
    switch (parent_tree_shape_->find(key, leaf, found)) {
        case TreeShape<std::int64_t>::Result::Found:
            r = found;
            break;
        case TreeShape<std::int64_t>::Result::NotFound:
            break;
        case TreeShape<std::int64_t>::Result::NotPlain: {
            std::unordered_set<std::uint64_t> visited;
            if (parent_tree_.isIndirect()) visited.insert(ref_of(parent_tree_).key());
            r = number_tree_lookup(parent_tree_, key, visited, &parent_tree_nums_);
            break;
        }
    }
    parent_tree_cache_.emplace(key, r);
    return r;
}

Obj StructTree::struct_elem_for_mcid(const std::optional<std::int64_t>& struct_parent,
                                     const std::optional<std::int64_t>& struct_parents,
                                     std::optional<std::int64_t> mcid) const {
    if (!has_parent_tree()) return Obj::newNull();
    Obj r;
    if (struct_parent) {
        r = parent_tree_entry(*struct_parent);
    } else if (mcid && *mcid > -1 && struct_parents) {
        Obj arr = parent_tree_entry(*struct_parents);
        if (arr.isArray() && arr.getArrayNItems() > *mcid) r = arr.getArrayItem(static_cast<int>(*mcid));
    }
    if (!r || r.isNull()) return Obj::newNull();
    return r;
}

namespace {

// StructElem::keys of a dictionary: which of the keys with a bit it holds (keys as qpdf stores them, with the slash).
std::uint16_t key_bits(const std::map<std::string, Obj>& entries) {
    std::uint16_t bits = 0;
    for (const auto& item : entries) {
        const std::string& k = item.first;
        if (k.empty() || k[0] != '/') continue;
        switch (k.size()) {
            case 2:
                switch (k[1]) {
                    case 'P': bits |= StructElem::kP; break;
                    case 'E': bits |= StructElem::kE; break;
                    case 'A': bits |= StructElem::kA; break;
                    case 'C': bits |= StructElem::kC; break;
                    case 'T': bits |= StructElem::kT; break;
                    default: break;
                }
                break;
            case 3:
                if (k == "/ID") bits |= StructElem::kID;
                break;
            case 4:
                if (k == "/Alt") bits |= StructElem::kAlt;
                else if (k == "/Ref") bits |= StructElem::kRef;
                break;
            case 5:
                if (k == "/Lang") bits |= StructElem::kLang;
                break;
            case 11:
                if (k == "/ActualText") bits |= StructElem::kActualText;
                break;
            default:
                break;
        }
    }
    return bits;
}

}  // namespace

const Obj StructTree::kNoObj;

static_assert(sizeof(ObjRef) == 8, "ObjRef is two ints");

// What only the traversal needs.
struct StructTree::Build {
    // The kid words of the open nodes. A node takes one slot per item of its /K when it is opened (an item is at most
    // one kid) and gives them back when it is closed; nodes are closed in the reverse order of opening.
    std::vector<std::uint64_t> scratch;
    // Type name -> index into types_: without a namespace dictionary, and per indirect namespace dictionary (by
    // object number and generation). A type in a direct namespace dictionary is not looked up: one record each.
    std::unordered_map<std::string, std::uint32_t> plain_types;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint32_t>> ns_types;
    // Page reference (number and generation) -> index into pages_; the last one asked for first.
    std::unordered_map<std::uint64_t, std::uint32_t> page_ids;
    ObjRef last_page;
    std::uint32_t last_page_id = 0;

    static std::uint64_t full_key(const ObjRef& r) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.num)) << 32) | static_cast<std::uint32_t>(r.gen);
    }
    std::uint32_t page_id(const ObjRef& r, std::vector<ObjRef>& pages) {
        if (r == last_page) return last_page_id;
        auto [it, added] = page_ids.try_emplace(full_key(r), static_cast<std::uint32_t>(pages.size()));
        if (added) pages.push_back(r);
        last_page = r;
        last_page_id = it->second;
        return last_page_id;
    }
};

int StructTree::collect_kid(const Obj& item, int elem_index, std::uint32_t inherited_page, int depth, Build& b,
                            std::uint64_t* out) {
    // TaggedPDFHelper.getChildren: a dictionary whose /Type is absent or /StructElem is a structure
    // element; a dictionary or stream with /Type /MCR or /OBJR is a content item; an integer is an MCID.
    if (item.isDictionary()) {
        // The entries of the dictionary, taken once (one copy of its map instead of two map searches per key read
        // below). entry(key) is cos::get(item, key): the value, a null handle for an absent key and for a null value.
        const std::map<std::string, Obj> entries = const_cast<Obj&>(item).getDictAsMap();
        const auto entry = [&](const char* slash_key) -> Obj {
            auto found = entries.find(slash_key);
            if (found == entries.end() || found->second.isNull()) return Obj::newNull();
            return found->second;
        };
        auto type = cos::name(entry("/Type"));
        if (!type || *type == "StructElem") {
            ObjRef r = ref_of(item);
            const int known = r.valid() ? by_ref_.find(r.key()) : -1;
            if (known >= 0) {
                *out = kid(StructKid::Kind::Elem, true, known);   // shared or cyclic: one element, listed in every parent
                return -1;
            }
            // The record is complete before it joins the tree: what is read below can throw (a damaged object), and
            // the tree a failed traversal leaves has every element it reached, whole.
            Rec e;
            e.parent = elem_index;
            e.depth = depth;
            e.dict = item;
            e.ref = r;
            // The facts of the type, stype_of(item) with its two standard types: once per type name and namespace.
            const Obj s = entry("/S");
            if (std::optional<std::string> name = cos::name(s)) {
                const Obj ns = entry("/NS");
                std::uint32_t* id = nullptr;
                if (!ns.isDictionary()) id = &b.plain_types.try_emplace(*name, 0).first->second;
                else if (ns.isIndirect()) id = &b.ns_types[Build::full_key(ref_of(ns))].try_emplace(*name, 0).first->second;
                if (id && *id != 0) {
                    e.type = *id;
                } else {
                    TypeFacts t;
                    t.type.type = *name;
                    t.type.ns = ns.isDictionary() ? ns : no_namespace();
                    t.std20 = default_type20(t.type);
                    t.std17 = standard_type17(*name);
                    t.S = std::move(name);
                    e.type = static_cast<std::uint32_t>(types_.size());
                    if (id) *id = e.type;
                    types_.push_back(std::move(t));
                }
            }
            const std::uint32_t pg = b.page_id(ref_of(entry("/Pg")), pages_);
            e.page = pg;
            e.inherited_page = pages_[pg].valid() ? pg : inherited_page;
            e.keys = key_bits(entries);
            const int idx = static_cast<int>(elems_.size());
            elems_.push_back(std::move(e));
            if (r.valid()) by_ref_.insert(r.key(), idx);
            *out = kid(StructKid::Kind::Elem, false, idx);
            if (elem_index < 0) root_elems_.push_back(idx);
            return idx;
        }
        // a content item (as below for a stream, from the entries at hand)
        if (*type == "MCR") {
            McrRec m;
            m.dict = item;
            if (auto id = cos::integer(entry("/MCID"))) m.mcid = *id;
            ObjRef pg = ref_of(entry("/Pg"));
            m.page = pg.valid() ? pg : pages_[inherited_page];
            m.stm = ref_of(entry("/Stm"));
            *out = kid(StructKid::Kind::MCR, false, static_cast<std::int64_t>(mcrs_.size()));
            mcrs_.push_back(std::move(m));
            return -1;
        }
        if (*type == "OBJR") {
            ObjrRec o;
            o.dict = item;
            o.obj = entry("/Obj");
            ObjRef pg = ref_of(entry("/Pg"));
            o.page = pg.valid() ? pg : pages_[inherited_page];
            *out = kid(StructKid::Kind::OBJR, false, static_cast<std::int64_t>(objrs_.size()));
            objrs_.push_back(std::move(o));
            return -1;
        }
        return -2;   // dictionary of another type: ignored by veraPDF
    }
    if (item.isStream()) {
        auto type = cos::name(item, "Type");
        if (type && *type == "MCR") {
            McrRec m;
            m.dict = item;
            if (auto id = cos::integer(item, "MCID")) m.mcid = *id;
            ObjRef pg = ref_of(cos::get(item, "Pg"));
            m.page = pg.valid() ? pg : pages_[inherited_page];
            m.stm = ref_of(cos::get(item, "Stm"));
            *out = kid(StructKid::Kind::MCR, false, static_cast<std::int64_t>(mcrs_.size()));
            mcrs_.push_back(std::move(m));
            return -1;
        }
        if (type && *type == "OBJR") {
            ObjrRec o;
            o.dict = item;
            o.obj = cos::get(item, "Obj");
            ObjRef pg = ref_of(cos::get(item, "Pg"));
            o.page = pg.valid() ? pg : pages_[inherited_page];
            *out = kid(StructKid::Kind::OBJR, false, static_cast<std::int64_t>(objrs_.size()));
            objrs_.push_back(std::move(o));
            return -1;
        }
        return -2;   // stream of another type: ignored by veraPDF
    }
    if (item.isInteger()) {
        // The page of an MCID kid is the node's inherited /Pg: not stored (StructKids).
        const std::int64_t mcid = item.getIntValue();
        constexpr std::int64_t limit = std::int64_t(1) << 60;
        if (mcid >= -limit && mcid < limit) {
            *out = kid(StructKid::Kind::MCID, false, mcid);
        } else {
            *out = kid(StructKid::Kind::MCID, true, static_cast<std::int64_t>(wide_mcids_.size()));
            wide_mcids_.push_back(mcid);
        }
        return -1;
    }
    return -2;
}

void StructTree::seal_kids(int elem_index, const std::uint64_t* words, std::size_t n) {
    if (elem_index < 0) {
        auto* block = static_cast<std::uint64_t*>(arena_.allocate(n * sizeof(std::uint64_t)));
        std::copy(words, words + n, block);
        root_kids_ = block;
        root_n_kids_ = static_cast<std::uint32_t>(n);
        return;
    }
    std::size_t n_struct = 0, n_shared = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<StructKid::Kind>(words[i] & 3) != StructKid::Kind::Elem) continue;
        ++n_struct;
        if (words[i] & 4) ++n_shared;
    }
    const std::size_t n_ints = n_struct + (n_shared ? 1 + (n_struct - n_shared) : 0);
    auto* block = static_cast<std::uint64_t*>(arena_.allocate(n * sizeof(std::uint64_t) + n_ints * sizeof(int)));
    std::copy(words, words + n, block);
    int* struct_kids = reinterpret_cast<int*>(block + n);
    int* elems = struct_kids + n_struct;
    if (n_shared) *elems++ = static_cast<int>(n_struct - n_shared);
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<StructKid::Kind>(words[i] & 3) != StructKid::Kind::Elem) continue;
        const int k = static_cast<int>(static_cast<std::int64_t>(words[i]) >> 3);
        *struct_kids++ = k;
        if (n_shared && !(words[i] & 4)) *elems++ = k;
    }
    Rec& e = elems_[static_cast<std::size_t>(elem_index)];
    e.block = block;
    e.n_kids = static_cast<std::uint32_t>(n);
    e.n_struct = static_cast<std::uint32_t>(n_struct);
    if (n_shared) e.flags |= kShared;
}

void StructTree::simulate_verapdf_visits() {
    // BaseValidator: depth-first, children pushed last-to-first, an id is registered when pushed and an
    // object whose id is already registered is not pushed. Element id: "N G obj <type> <S>" for indirect
    // elements (unique), "0 0 obj <S>" for direct elements with a name /S, none otherwise.
    // (Every element starts without kChecked.)
    std::unordered_set<std::string> ids;
    std::vector<int> stack;
    auto push_kids = [&](std::span<const int> kids) {
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            const Rec& k = elems_[static_cast<std::size_t>(*it)];
            if (k.ref.valid()) {
                if (!ids.insert("R" + std::to_string(k.ref.key())).second) continue;
            } else if (const auto& s = types_[k.type].S) {
                if (!ids.insert("D" + *s).second) continue;
            }
            stack.push_back(*it);
        }
    };
    std::vector<int> root_struct;
    for (const StructKid& k : root_kids())
        if (k.kind == StructKid::Kind::Elem && k.elem >= 0) root_struct.push_back(k.elem);
    push_kids(root_struct);
    verapdf_order_.reserve(elems_.size());   // each element at most once
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        elems_[static_cast<std::size_t>(i)].flags |= kChecked;
        verapdf_order_.push_back(i);
        push_kids(struct_kids_of(elems_[static_cast<std::size_t>(i)]));
    }
}

void StructTree::build(Document& doc) {
    root_ = cos::get(doc.catalog(), "StructTreeRoot");
    present_ = root_.isDictionary();
    if (!present_) return;
    root_ref_ = ref_of(root_);
    Obj rm = cos::get(root_, "RoleMap");
    if (rm.isDictionary()) {
        for (auto& kv : rm.getDictAsMap()) {
            if (kv.second.isName()) {
                std::string k = kv.first.size() && kv.first[0] == '/' ? kv.first.substr(1) : kv.first;
                role_map_.emplace(k, *cos::name(kv.second));
            }
        }
    }
    parent_tree_ = cos::get(root_, "ParentTree");
    if (parent_tree_.isDictionary()) {
        for (auto& [k, v] : cos::number_tree(parent_tree_)) parent_tree_entries_.emplace(k, v);
    } else {
        parent_tree_ = Obj::newNull();
    }
    // Iterative pre-order walk (first kid first, each new element expanded before its next sibling), so
    // arbitrarily deep trees are safe and element indices follow the logical order.
    struct Frame {
        int elem;                // -1 = StructTreeRoot
        std::vector<Obj> items;
        std::size_t next = 0;
        std::size_t first_kid = 0;   // where the node's kid words start in Build::scratch
        std::size_t n_kids = 0;
    };
    auto items_of = [](const Obj& node) {
        Obj k = cos::get(node, "K");
        std::vector<Obj> items;
        if (k.isArray()) items = cos::array_items(k);
        else if (!k.isNull()) items.push_back(k);
        return items;
    };
    static_assert(sizeof(Rec) == 64, "an element record is 64 bytes");
    Build b;
    types_.emplace_back();   // [0]: /S is no name: no type name, no namespace, no standard type
    types_[0].type.ns = no_namespace();
    pages_.push_back(ObjRef{});
    b.page_ids.emplace(Build::full_key(ObjRef{}), 0);
    std::vector<Frame> stack;
    auto open = [&](int elem, const Obj& node) {
        Frame f{elem, items_of(node), 0, b.scratch.size(), 0};
        b.scratch.resize(f.first_kid + f.items.size());
        stack.push_back(std::move(f));
    };
    try {
        open(-1, root_);
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.next >= f.items.size()) {
                seal_kids(f.elem, b.scratch.data() + f.first_kid, f.n_kids);
                b.scratch.resize(f.first_kid);
                stack.pop_back();
                continue;
            }
            Obj item = f.items[f.next++];
            int parent = f.elem;
            const std::uint32_t ip = parent >= 0 ? elems_[static_cast<std::size_t>(parent)].inherited_page : 0;
            int depth = parent >= 0 ? elems_[static_cast<std::size_t>(parent)].depth + 1 : 0;
            int idx = collect_kid(item, parent, ip, depth, b, &b.scratch[f.first_kid + f.n_kids]);
            if (idx >= -1) ++f.n_kids;
            if (idx >= 0) open(idx, elems_[static_cast<std::size_t>(idx)].dict);   // invalidates f
        }
    } catch (...) {
        // The traversal failed (a damaged object). The caller notes it and goes on with the tree as far as it got:
        // every open node keeps the kids collected so far, and no element is marked as skipped by veraPDF.
        for (const Frame& f : stack) seal_kids(f.elem, b.scratch.data() + f.first_kid, f.n_kids);
        for (std::size_t i = 0; i < elems_.size(); ++i) elems_[i].flags |= kChecked;
        throw;
    }
    simulate_verapdf_visits();
}

}  // namespace eqx
