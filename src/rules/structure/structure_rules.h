// Rules of the structure module and their pure helpers (unit-tested in tests/unit/structure_*.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/cos.h"

namespace eqx {
struct ValidationContext;
}

namespace eqx::structure {

void run_structure_rules(ValidationContext& ctx);

// GFSEHn.gethasCorrectNestingLevel over the SEHn objects in validation order: a heading fails when its level is
// more than one deeper than the previous heading's level (the first heading is compared with level 0). Returns
// the indices (into `levels`) of the failing headings.
std::vector<std::size_t> heading_nesting_failures(const std::vector<int>& levels);

// GFSEFENote.getorphanRefs / getghostRefs. `referrers` = keys of the elements whose /Ref array names the FENote
// (StaticContainers.getStructElementsRefs()[key]), `own_refs` = keys named by the FENote's own /Ref array.
// Each result is empty when veraPDF's property is null.
std::set<ObjRef> orphan_refs(const std::set<ObjRef>& referrers, const std::set<ObjRef>& own_refs);
std::set<ObjRef> ghost_refs(const std::set<ObjRef>& referrers, const std::set<ObjRef>& own_refs);

// GFSENote noteID bookkeeping: IDs in construction order; returns true for every entry whose ID was seen before
// (hasDuplicateNoteID). nullopt entries (no ID) never count.
std::vector<bool> duplicate_ids(const std::vector<std::optional<std::string>>& ids);

}  // namespace eqx::structure
