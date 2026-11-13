#include "core/structure.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
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

std::optional<SType> make_stype(const Obj& type, const Obj& ns) {
    if (!type.isName()) return std::nullopt;
    SType t;
    t.type = *cos::name(type);
    t.ns = ns.isDictionary() ? ns : Obj::newNull();
    return t;
}

}  // namespace

std::optional<std::string> SType::ns_uri() const {
    if (!ns.isDictionary()) return std::string(kNsPdf);
    return ns_string(ns);
}

bool SType::same(const SType& o) const { return type == o.type && ns_uri() == o.ns_uri(); }

std::string StructElem::standard(Flavor f) const {
    if (f == Flavor::Pdf17) return std17 ? *std17 : std::string();
    if (!std20) return std::string();
    if (std20->ns_uri() == std::optional<std::string>(kNsMathML)) return "MathML";
    return std20->type;
}

bool StructElem::is_math(Flavor f) const {
    return f == Flavor::Pdf20 && std20 && std20->ns_uri() == std::optional<std::string>(kNsMathML);
}

bool StructTree::is_standard17(const std::string& type) { return std17_set().count(type) > 0; }

bool StructTree::is_standard20(const SType& t) {
    auto uri = t.ns.isDictionary() ? ns_string(t.ns) : std::nullopt;
    if (t.ns.isDictionary() && uri) {
        if (*uri == kNsPdf) return std17_set().count(t.type) > 0;
        if (*uri == kNsPdf2) return std20_set().count(t.type) > 0 || is_hn(t.type);
        if (*uri == kNsMathML) return true;
        return false;
    }
    return std17_set().count(t.type) > 0;
}

bool StructTree::is_pass_through(const std::string& t) { return t == "NonStruct" || t == "Div" || t == "Part"; }

std::optional<std::string> StructTree::standard_type17(const std::string& type) const {
    return rolemap::standard_type17(role_map_, type);
}

bool StructTree::circular17(const std::string& type) const { return rolemap::circular17(role_map_, type); }

std::optional<SType> StructTree::default_type20(const SType& t) const { return rolemap::standard_type20(role_map_, t); }

bool StructTree::circular20(const SType& t) const { return rolemap::circular20(t); }

std::optional<std::string> StructTree::role_map_to_same_ns_tag(const SType& t) const {
    return rolemap::remap_within_namespace(t);
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
    std::unordered_set<std::uint64_t> seen;
    Obj e = d;
    while (e.isDictionary()) {
        if (cos::name_is(e, "Type", "StructTreeRoot")) return true;
        if (e.isIndirect() && !seen.insert(ref_of(e).key()).second) break;
        e = cos::get(e, "P");
    }
    return false;
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
        for (int k : elems_[e].struct_kids) {
            std::string st = elems_[k].standard(f);
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
        for (int k : elems_[e].struct_kids) {
            std::string st = elems_[k].standard(f);
            if (is_pass_through(st)) rec(k, depth + 1);
            else if (!st.empty()) out.push_back(st);
        }
    };
    for (const StructKid& rk : root_kids_) {
        if (rk.kind != StructKid::Kind::Elem || rk.elem < 0) continue;
        int k = rk.elem;
        std::string st = elems_[k].standard(f);
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
        for (const auto& k : elems_[e].kids)
            if (k.kind != StructKid::Kind::Elem) return true;
        for (int k : elems_[e].struct_kids)
            if (is_pass_through(elems_[k].standard(f)) && rec(k, depth + 1)) return true;
        return false;
    };
    return rec(i, 0);
}

int StructTree::find(const ObjRef& r) const {
    auto it = by_ref_.find(r.key());
    return it == by_ref_.end() ? -1 : it->second;
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

// PDNumberTreeNode.getObject: Limits, /Nums before /Kids, the last duplicate wins, the first kid that has the key
// decides, a node met again ends its list of kids (LoopedException). On an explicit stack: nesting has no limit.
// With `cache`, a /Nums array is looked up through its index (the same result as the scan below).
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
    std::unordered_set<std::uint64_t> visited;
    if (parent_tree_.isIndirect()) visited.insert(ref_of(parent_tree_).key());
    Obj r = number_tree_lookup(parent_tree_, key, visited, &parent_tree_nums_);
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

int StructTree::collect_kid(const Obj& item, int elem_index, ObjRef inherited_page, int depth) {
    // TaggedPDFHelper.getChildren: a dictionary whose /Type is absent or /StructElem is a structure
    // element; a dictionary or stream with /Type /MCR or /OBJR is a content item; an integer is an MCID.
    std::vector<StructKid>& kids = elem_index < 0 ? root_kids_ : elems_[elem_index].kids;
    StructKid kid;
    if (item.isDictionary()) {
        auto type = cos::name(item, "Type");
        if (!type || *type == "StructElem") {
            kid.kind = StructKid::Kind::Elem;
            kid.dict = item;
            ObjRef r = ref_of(item);
            auto it = r.valid() ? by_ref_.find(r.key()) : by_ref_.end();
            if (it != by_ref_.end()) {
                kid.elem = it->second;   // shared or cyclic: one element, listed in every parent
                kid.revisit = true;
                kids.push_back(kid);
                if (elem_index >= 0) elems_[elem_index].struct_kids.push_back(it->second);
                return -1;
            }
            int idx = static_cast<int>(elems_.size());
            StructElem e;
            e.index = idx;
            e.parent = elem_index;
            e.depth = depth;
            e.dict = item;
            e.ref = r;
            e.S = cos::name(item, "S");
            if (auto st = stype_of(item)) {
                e.type = *st;
                e.std20 = default_type20(*st);
            } else {
                e.type.ns = Obj::newNull();
            }
            if (e.S) e.std17 = standard_type17(*e.S);
            e.page = ref_of(cos::get(item, "Pg"));
            e.inherited_page = e.page.valid() ? e.page : inherited_page;
            elems_.push_back(std::move(e));      // may reallocate: re-take references below
            if (r.valid()) by_ref_.emplace(r.key(), idx);
            kid.elem = idx;
            std::vector<StructKid>& kids2 = elem_index < 0 ? root_kids_ : elems_[elem_index].kids;
            kids2.push_back(kid);
            if (elem_index < 0) root_elems_.push_back(idx);
            else {
                elems_[elem_index].elems.push_back(idx);
                elems_[elem_index].struct_kids.push_back(idx);
            }
            return idx;
        }
    }
    if (item.isDictionary() || item.isStream()) {
        auto type = cos::name(item, "Type");
        if (type && *type == "MCR") {
            kid.kind = StructKid::Kind::MCR;
            kid.dict = item;
            if (auto m = cos::integer(item, "MCID")) kid.mcid = *m;
            ObjRef pg = ref_of(cos::get(item, "Pg"));
            kid.page = pg.valid() ? pg : inherited_page;
            kid.stm = ref_of(cos::get(item, "Stm"));
            kids.push_back(kid);
        } else if (type && *type == "OBJR") {
            kid.kind = StructKid::Kind::OBJR;
            kid.dict = item;
            kid.obj = cos::get(item, "Obj");
            ObjRef pg = ref_of(cos::get(item, "Pg"));
            kid.page = pg.valid() ? pg : inherited_page;
            kids.push_back(kid);
        }
        return -1;   // dictionary of another type: ignored by veraPDF
    }
    if (item.isInteger()) {
        kid.kind = StructKid::Kind::MCID;
        kid.mcid = item.getIntValue();
        kid.page = inherited_page;
        kids.push_back(kid);
    }
    return -1;
}

void StructTree::simulate_verapdf_visits() {
    // BaseValidator: depth-first, children pushed last-to-first, an id is registered when pushed and an
    // object whose id is already registered is not pushed. Element id: "N G obj <type> <S>" for indirect
    // elements (unique), "0 0 obj <S>" for direct elements with a name /S, none otherwise.
    for (StructElem& e : elems_) e.verapdf_checked = false;
    std::unordered_set<std::string> ids;
    std::vector<int> stack;
    auto push_kids = [&](const std::vector<int>& kids) {
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            const StructElem& k = elems_[*it];
            if (k.ref.valid()) {
                if (!ids.insert("R" + std::to_string(k.ref.key())).second) continue;
            } else if (k.S) {
                if (!ids.insert("D" + *k.S).second) continue;
            }
            stack.push_back(*it);
        }
    };
    std::vector<int> root_struct;
    for (const StructKid& k : root_kids_)
        if (k.kind == StructKid::Kind::Elem && k.elem >= 0) root_struct.push_back(k.elem);
    push_kids(root_struct);
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        elems_[i].verapdf_checked = true;
        verapdf_order_.push_back(i);
        push_kids(elems_[i].struct_kids);
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
    };
    auto items_of = [](const Obj& node) {
        Obj k = cos::get(node, "K");
        std::vector<Obj> items;
        if (k.isArray()) items = cos::array_items(k);
        else if (!k.isNull()) items.push_back(k);
        return items;
    };
    std::vector<Frame> stack;
    stack.push_back({-1, items_of(root_), 0});
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next >= f.items.size()) {
            stack.pop_back();
            continue;
        }
        Obj item = f.items[f.next++];
        int parent = f.elem;
        ObjRef ip = parent >= 0 ? elems_[parent].inherited_page : ObjRef{};
        int depth = parent >= 0 ? elems_[parent].depth + 1 : 0;
        int idx = collect_kid(item, parent, ip, depth);
        if (idx >= 0) stack.push_back({idx, items_of(elems_[idx].dict), 0});   // invalidates f
    }
    simulate_verapdf_visits();
}

}  // namespace eqx
