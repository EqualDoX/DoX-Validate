#include "rules/structure/se_common.h"

#include <unordered_set>

namespace eqx::se {

const void* identity(const Obj& h) {
    Obj copy = h;
    return copy.getObj().get();
}

std::optional<std::string> string_key(const Obj& dict, std::string_view key) {
    return cos::text(cos::get(dict, key));
}

std::optional<std::string> string_or_name_key(const Obj& dict, std::string_view key) {
    Obj v = cos::get(dict, key);
    if (v.isString()) return cos::text(v);
    if (v.isName()) return cos::name(v);
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------- attributes

namespace {

// Visits the attribute objects an /A value or a ClassMap entry holds, in order: the dictionaries of an array (other
// array items, such as revision numbers, are not attribute objects), or the value itself when it is a dictionary.
// Stops at the first object for which `visit` returns true; returns whether that happened.
template <class Visit>
bool visit_attribute_objects(const Obj& holder, Visit&& visit) {
    if (holder.isArray()) {
        for (const Obj& item : cos::array_items(holder))
            if (item.isDictionary() && visit(item)) return true;
        return false;
    }
    return holder.isDictionary() && visit(holder);
}

}  // namespace

Attributes::Attributes(const StructTree& st) {
    class_map_ = st.present() ? cos::get(st.root(), "ClassMap") : Obj::newNull();
    if (!class_map_.isDictionary()) class_map_ = Obj::newNull();
}

Obj Attributes::value(const Obj& elem, std::string_view attr, std::string_view owner, qpdf_object_type_e type) const {
    if (!elem.isDictionary()) return Obj::newNull();
    Obj found = Obj::newNull();
    auto supplies = [&](const Obj& attribute_object) {
        const std::optional<std::string> o = cos::name(attribute_object, "O");
        if (!o || *o != owner) return false;
        Obj entry = cos::get(attribute_object, attr);
        if (entry.isNull() || entry.getTypeCode() != type) return false;
        found = entry;
        return true;
    };
    if (visit_attribute_objects(cos::get(elem, "A"), supplies)) return found;
    if (class_map_.isNull()) return Obj::newNull();
    // Attribute classes: /C is a class name or an array of them (non-names are skipped).
    Obj classes = cos::get(elem, "C");
    std::vector<Obj> names;
    if (classes.isName()) names.push_back(classes);
    else if (classes.isArray())
        for (const Obj& entry : cos::array_items(classes))
            if (entry.isName()) names.push_back(entry);
    for (const Obj& name : names)
        if (visit_attribute_objects(cos::get(class_map_, *cos::name(name)), supplies)) return found;
    return Obj::newNull();
}

std::int64_t Attributes::integer(const Obj& elem, std::string_view attr, std::string_view owner,
                                 std::int64_t dflt) const {
    const Obj v = value(elem, attr, owner, ::ot_integer);
    return v.isInteger() ? v.getIntValue() : dflt;
}

std::optional<std::string> Attributes::name(const Obj& elem, std::string_view attr, std::string_view owner,
                                            std::optional<std::string> dflt, bool inheritable, bool* cycle) const {
    // The element itself, then (inheritable attributes) its /P ancestors, until one supplies a name value.
    std::unordered_set<const void*> on_path;
    for (Obj at = elem; at.isDictionary(); at = cos::get(at, "P")) {
        if (!on_path.insert(identity(at)).second) {
            if (cycle) *cycle = true;
            return dflt;
        }
        const Obj v = value(at, attr, owner, ::ot_name);
        if (v.isName()) return cos::name(v);
        if (!inheritable) return dflt;
    }
    return dflt;
}

Obj Attributes::array(const Obj& elem, std::string_view attr, std::string_view owner) const {
    return value(elem, attr, owner, ::ot_array);
}

// ------------------------------------------------------------------------------------------- tree helpers

namespace {

bool is_struct_elem_dict(const Obj& d) {
    if (!d.isDictionary()) return false;
    auto type = cos::name(d, "Type");
    return !type || *type == "StructElem";
}

}  // namespace

std::vector<Obj> struct_children(const Obj& node) {
    std::vector<Obj> out;
    Obj k = cos::get(node, "K");
    if (k.isNull()) return out;
    if (is_struct_elem_dict(k)) {
        out.push_back(k);
    } else if (k.isArray()) {
        for (const Obj& item : cos::array_items(k))
            if (is_struct_elem_dict(item)) out.push_back(item);
    }
    return out;
}

std::string standard_type(const StructTree& st, const Obj& elem, Flavor f) {
    ObjRef r = ref_of(elem);
    if (r.valid()) {
        int i = st.find(r);
        if (i >= 0) return st.elem(i).standard(f);
    }
    return st.standard_of(elem, f).value_or(std::string());
}

namespace {

// Elements visited by one walk: pass-through elements shared by several parents multiply (veraPDF does not end either).
constexpr std::size_t kMaxVisits = 20'000'000;

// Depth-first over pass-through elements on an explicit stack, in /K order; `leaf` receives every other child. A cycle
// or a runaway expansion clears *ok, as the reference does not terminate there.
template <class Leaf>
void walk_children(const StructTree& st, const Obj& node, Flavor f, bool* ok, Leaf leaf) {
    struct Level {
        const void* id;
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    std::vector<Level> stack;
    std::unordered_set<const void*> path;
    path.insert(identity(node));
    stack.push_back({identity(node), struct_children(node), 0});
    std::size_t visits = 0;
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            path.erase(top.id);
            stack.pop_back();
            continue;
        }
        Obj child = top.kids[top.next++];
        if (++visits > kMaxVisits) {
            if (ok) *ok = false;
            return;
        }
        std::string t = standard_type(st, child, f);
        if (!is_pass_through(t)) {
            leaf(child, std::move(t));
            continue;
        }
        const void* id = identity(child);
        if (!path.insert(id).second) {
            if (ok) *ok = false;
            continue;
        }
        stack.push_back({id, struct_children(child), 0});
    }
}

}  // namespace

std::vector<TypedChild> significance_children(const StructTree& st, const Obj& node, Flavor f, bool* ok) {
    std::vector<TypedChild> out;
    walk_children(st, node, f, ok, [&](const Obj& child, std::string t) { out.push_back({child, std::move(t)}); });
    return out;
}

std::vector<std::string> children_standard_types(const StructTree& st, const Obj& node, Flavor f, bool* ok) {
    std::vector<std::string> out;
    walk_children(st, node, f, ok, [&](const Obj&, std::string t) { out.push_back(std::move(t)); });
    return out;
}

std::vector<int> element_order(const StructTree& st, bool reference_dedup) {
    if (reference_dedup) return st.verapdf_order();
    std::vector<int> order(st.elems().size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    return order;
}

std::unordered_map<const void*, int> elem_index_map(const StructTree& st) {
    std::unordered_map<const void*, int> m;
    for (const StructElem& e : st.elems()) m.emplace(identity(e.dict), e.index);
    return m;
}

bool is_hn(const std::string& t) {
    if (t.size() < 2 || t[0] != 'H' || t[1] < '1' || t[1] > '9') return false;
    for (std::size_t i = 2; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

}  // namespace eqx::se
