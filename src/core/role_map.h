// Role mapping of structure types: ISO 32000-1 14.8.4 (the /RoleMap of the structure tree root) and ISO 32000-2
// 14.8.6 (namespaces with /RoleMapNS; /RoleMap for types without a namespace and in the PDF 1.7 namespace).
//
// Every query follows the mapping chain of a type: the type it maps to, the type that one maps to, and so on, up
// to a type without a mapping or the first type met before on the chain (a cycle). Types on a PDF 2.0 chain are
// told apart by name and namespace dictionary (direct namespace dictionaries count as one). PDF 2.0 chains are
// followed for at most 9999 steps.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "core/structure.h"

namespace eqx::rolemap {

using RoleMap = std::unordered_map<std::string, std::string>;

// PDF 1.7: the first standard type the chain reaches; a type without a (new) mapping is its own standard type
// when it is standard. nullopt when the chain reaches none.
std::optional<std::string> standard_type17(const RoleMap& role_map, const std::string& type);
// PDF 1.7: the chain ends in a cycle.
bool circular17(const RoleMap& role_map, const std::string& type);

// PDF 2.0: as standard_type17, with namespaces (RoleMapNS first, /RoleMap as described above).
std::optional<SType> standard_type20(const RoleMap& role_map, const SType& type);
// PDF 2.0, following RoleMapNS only: the chain returns to the type most recently seen in some namespace.
bool circular20(const SType& type);
// PDF 2.0, following RoleMapNS only: at the first step into a namespace already on the chain, the type the chain
// left that namespace with ("<namespace URI>:<type>") when the step lands on another type of it; nullopt otherwise.
std::optional<std::string> remap_within_namespace(const SType& type);

}  // namespace eqx::rolemap
