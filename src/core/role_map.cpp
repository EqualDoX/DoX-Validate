#include "core/role_map.h"

#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

namespace eqx::rolemap {

namespace {

constexpr std::size_t kMaxSteps = 9999;   // longest PDF 2.0 chain followed

// ------------------------------------------------------------------------------------------------ PDF 1.7

// The names a type maps to through the /RoleMap, in order; `cycle`: the last one was met before.
struct NameChain {
    std::vector<std::string> next;
    bool cycle = false;
};

NameChain follow(const RoleMap& role_map, const std::string& start) {
    NameChain chain;
    std::unordered_set<std::string> seen{start};
    std::string at = start;
    for (auto hit = role_map.find(at); hit != role_map.end(); hit = role_map.find(at)) {
        at = hit->second;
        chain.next.push_back(at);
        if (!seen.insert(at).second) {
            chain.cycle = true;
            break;
        }
    }
    return chain;
}

// ------------------------------------------------------------------------------------------------ PDF 2.0

// A type on a chain: its name and, when it has a namespace dictionary, that dictionary's object number.
struct Identity {
    std::string type;
    bool in_namespace = false;
    std::uint64_t namespace_object = 0;
    bool operator==(const Identity& o) const {
        return type == o.type && in_namespace == o.in_namespace && namespace_object == o.namespace_object;
    }
};

struct IdentityHash {
    std::size_t operator()(const Identity& i) const {
        return std::hash<std::string>()(i.type) ^ (std::hash<std::uint64_t>()(i.namespace_object) * 31u + i.in_namespace);
    }
};

Identity identity_of(const SType& t) {
    if (!t.ns.isDictionary()) return Identity{t.type, false, 0};
    return Identity{t.type, true, t.ns_ref().key()};
}

std::optional<SType> typed(const Obj& name, const Obj& ns) {
    if (!name.isName()) return std::nullopt;
    return SType{*cos::name(name), ns.isDictionary() ? ns : Obj::newNull()};
}

// The type `t` maps to in one step. A namespace with a RoleMapNS dictionary maps through it alone (a name value
// lands in no namespace, an array [type namespace] in that namespace); /RoleMap applies to types without a
// namespace and to types of the PDF 1.7 namespace, and only when `role_map` is given.
std::optional<SType> step(const SType& t, const RoleMap* role_map) {
    if (t.ns.isDictionary()) {
        Obj ns_map = cos::get(t.ns, "RoleMapNS");
        if (ns_map.isDictionary()) {
            const std::string key = "/" + t.type;
            if (!ns_map.hasKey(key)) return std::nullopt;
            Obj target = ns_map.getKey(key);
            if (target.isName()) return typed(target, Obj::newNull());
            if (target.isArray() && target.getArrayNItems() >= 2) return typed(target.getArrayItem(0), target.getArrayItem(1));
            return std::nullopt;
        }
        if (cos::text(cos::get(t.ns, "NS")) != std::optional<std::string>(kNsPdf)) return std::nullopt;
    }
    if (role_map == nullptr) return std::nullopt;
    auto hit = role_map->find(t.type);
    if (hit == role_map->end()) return std::nullopt;
    return SType{hit->second, Obj::newNull()};
}

struct TypeChain {
    std::vector<SType> next;   // the types `start` maps to, in order
    bool cycle = false;        // the last entry was met before on the chain
};

TypeChain follow(const SType& start, const RoleMap* role_map) {
    TypeChain chain;
    std::unordered_set<Identity, IdentityHash> seen{identity_of(start)};
    while (chain.next.size() < kMaxSteps) {
        std::optional<SType> to = step(chain.next.empty() ? start : chain.next.back(), role_map);
        if (!to) break;
        const bool again = !seen.insert(identity_of(*to)).second;
        chain.next.push_back(std::move(*to));
        if (again) {
            chain.cycle = true;
            break;
        }
    }
    return chain;
}

// Steps of the RoleMapNS chain that enter a namespace the chain was in before: `on_revisit(uri, landed_on,
// left_with)` receives the namespace URI, the type stepped to and the type the chain was last at in that
// namespace; it returns true to stop.
template <class F>
void namespace_revisits(const SType& start, F on_revisit) {
    const TypeChain chain = follow(start, nullptr);
    std::map<std::string, std::string> last_type_in;
    const SType* from = &start;
    for (const SType& to : chain.next) {
        if (auto uri = from->ns_uri()) last_type_in[*uri] = from->type;
        if (auto uri = to.ns_uri()) {
            auto seen = last_type_in.find(*uri);
            if (seen != last_type_in.end() && on_revisit(*uri, to, seen->second)) return;
        }
        from = &to;
    }
}

}  // namespace

std::optional<std::string> standard_type17(const RoleMap& role_map, const std::string& type) {
    const NameChain chain = follow(role_map, type);
    const std::size_t fresh = chain.next.size() - (chain.cycle ? 1 : 0);   // entries not met before
    if (fresh == 0) return StructTree::is_standard17(type) ? std::optional<std::string>(type) : std::nullopt;
    for (std::size_t i = 0; i < fresh; ++i)
        if (StructTree::is_standard17(chain.next[i])) return chain.next[i];
    return std::nullopt;
}

bool circular17(const RoleMap& role_map, const std::string& type) { return follow(role_map, type).cycle; }

std::optional<SType> standard_type20(const RoleMap& role_map, const SType& type) {
    const TypeChain chain = follow(type, &role_map);
    const std::size_t fresh = chain.next.size() - (chain.cycle ? 1 : 0);
    if (fresh == 0) return StructTree::is_standard20(type) ? std::optional<SType>(type) : std::nullopt;
    for (std::size_t i = 0; i < fresh; ++i)
        if (StructTree::is_standard20(chain.next[i])) return chain.next[i];
    return std::nullopt;
}

bool circular20(const SType& type) {
    bool circular = false;
    namespace_revisits(type, [&](const std::string&, const SType& to, const std::string& left_with) {
        circular = to.type == left_with;
        return circular;
    });
    return circular;
}

std::optional<std::string> remap_within_namespace(const SType& type) {
    std::optional<std::string> out;
    namespace_revisits(type, [&](const std::string& uri, const SType& to, const std::string& left_with) {
        if (to.type != left_with) out = uri + ":" + left_with;
        return true;
    });
    return out;
}

}  // namespace eqx::rolemap
