// Which module evaluates which canonical rule. Families come from catalog/rules.json
// ("family" field); a pattern ending in '*' matches a family prefix. Explicit ids win over families.
// Every catalogue rule must have exactly one owner (checked by `dox-validate --self-check`).
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rules/module.h"

namespace eqx {

namespace {

// The table below is evaluated by the compiler (kOwnerOfRule): no process matches a family at start-up.
struct Owner {
    std::string_view module;
    std::array<std::string_view, 24> families{};   // unused entries stay empty
    std::array<std::string_view, 8> ids{};
};

constexpr Owner kOwners[] = {
    {"content",
     {"content.*", "lang.page_text", "lang.span_marked_content", "xobject.*", "pua.actualtext", "pua.alt",
      "pac.ContrastOfText", "pac.PDFParsable"},
     {}},
    {"fonts", {"font.*", "pua.glyph_without_alternative"}, {}},
    {"containment", {"containment.*"}, {}},
    {"structure",
     {"heading.*", "rolemap.*", "structure.parent_entry_missing", "structure.root_document_namespace",
      "alt.*", "note.*", "toc.*", "list.*", "math.*", "lang.struct_alternatives"},
     {}},
    {"tables", {"table.*", "pac.TablesAreRegular", "pac.THHasAssociatedCells", "pac.CompleteTables"}, {}},
    {"pac_structure",
     {"pac.*Tag", "pac.MarkedContentIsInLegalPosition", "pac.FirstHeadingIsH1", "pac.NoHeadingsSkipped",
      "pac.HeadingStructureTypesNotMixed", "pac.FigureHasBBox", "pac.StructureElementHasParentKey",
      "pac.StructureHasCorruptElements", "pac.StructuralParentTree"},
     {}},
    {"pac_quality",
     {"pac.ActualTextHasLanguage", "pac.AltTextHasLanguage", "pac.ExpansionTexHasLanguage",
      "pac.AltTextsAreNotGenerated", "pac.ArtifactedBodyParagraphs", "pac.DocumentHasHeadings",
      "pac.LIContainsExactlyOneBody", "pac.MissingLinkTags", "pac.NoteContainsLbl", "pac.NoteHasReferringLinkOnPage",
      "pac.NoteTagInsidePTagCheck", "pac.OutlineIsPresent", "pac.TOCIContainsLink",
      "pac.TOCILinksPointToCorrectDestination", "pac.TaggedContentFoundOutsidePage",
      "pac.TextTagContainsOnlyWhitespace", "pac.TextTagHasAltText", "wcag.*"},
     {}},
    {"document",
     {"identification.*", "metadata.*", "syntax.*", "viewer.*", "encryption.*", "oc.*", "embedded.*", "xfa.*",
      "lang.catalog_missing", "lang.invalid_value", "pac.ValidLanguage", "lang.outline", "lang.xmp_x_default",
      "dest.*", "pua.text_string", "structure.no_structtreeroot", "pac.DocumentTitleIsNotGenerated"},
     {}},
    {"annotations",
     {"annot.*", "form.*", "link.*", "media.*", "page.*", "lang.annot_contents", "lang.field_tu",
      "pac.AnnotationHasAltText"},
     {}},
};
constexpr std::size_t kOwnerCount = sizeof(kOwners) / sizeof(kOwners[0]);
constexpr std::int8_t kNoOwner = -1;

constexpr bool match(std::string_view p, std::string_view family) {
    if (p == "pac.*Tag") {
        // PAC structure-type checks: families "pac.<Type>Tag" (PTag, TOCITag, DocumentTag, ...)
        return family.size() > 7 && family.substr(0, 4) == "pac." && family.substr(family.size() - 3) == "Tag";
    }
    if (!p.empty() && p.back() == '*') return family.substr(0, p.size() - 1) == p.substr(0, p.size() - 1);
    return family == p;
}

// Owner (index into kOwners) of every catalogue rule; kNoOwner when none.
// Precedence: an explicit id (the last owner that lists it), then an exact family (the first owner), then the first
// pattern with '*' in owner order ("pac.*Tag" must not swallow families that end in "Tag" of other owners: explicit
// families first). An exact family that no owner lists cannot match as a pattern either.
consteval std::array<std::int8_t, catalog_index::kRuleCount> owner_of_rules() {
    using namespace catalog_index;
    std::array<std::int8_t, kFamilyCount> of_family{};
    for (std::size_t f = 0; f < kFamilyCount; ++f) {
        const std::string_view fam = kFamilies[f];
        std::int8_t found = kNoOwner;
        for (std::size_t o = 0; o < kOwnerCount && found == kNoOwner; ++o)
            for (std::string_view p : kOwners[o].families)
                if (!p.empty() && p.find('*') == std::string_view::npos && p == fam) {
                    found = static_cast<std::int8_t>(o);
                    break;
                }
        for (std::size_t o = 0; o < kOwnerCount && found == kNoOwner; ++o)
            for (std::string_view p : kOwners[o].families)
                if (!p.empty() && p.find('*') != std::string_view::npos && match(p, fam)) {
                    found = static_cast<std::int8_t>(o);
                    break;
                }
        of_family[f] = found;
    }
    std::array<std::int8_t, kRuleCount> owner{};
    for (std::size_t i = 0; i < kRuleCount; ++i) owner[i] = of_family[kRuleFamily[i]];
    for (std::size_t o = 0; o < kOwnerCount; ++o)
        for (std::string_view id : kOwners[o].ids) {
            if (id.empty()) continue;
            for (std::size_t i = 0; i < kRuleCount; ++i)
                if (kRuleIds[i] == id) owner[i] = static_cast<std::int8_t>(o);
        }
    return owner;
}

constexpr std::array<std::int8_t, catalog_index::kRuleCount> kOwnerOfRule = owner_of_rules();

struct Table {
    std::vector<RuleIndex> by_module[kOwnerCount];
    std::string names[kOwnerCount + 1];   // the last one is "": no owner
};

const Table& table() {
    static const Table t = [] {
        Table t;
        std::size_t count[kOwnerCount] = {};
        for (std::int8_t o : kOwnerOfRule)
            if (o != kNoOwner) ++count[o];
        for (std::size_t o = 0; o < kOwnerCount; ++o) {
            t.names[o] = kOwners[o].module;
            t.by_module[o].reserve(count[o]);
        }
        for (RuleIndex i = 0; i < kOwnerOfRule.size(); ++i)
            if (kOwnerOfRule[i] != kNoOwner) t.by_module[kOwnerOfRule[i]].push_back(i);
        return t;
    }();
    return t;
}

}  // namespace

const std::vector<RuleIndex>& owned_rules(std::string_view module) {
    static const std::vector<RuleIndex> empty;
    for (std::size_t o = 0; o < kOwnerCount; ++o)
        if (kOwners[o].module == module) return table().by_module[o];
    return empty;
}

const std::string& owner_of(RuleIndex rule) {
    const std::int8_t o = kOwnerOfRule[rule];
    return table().names[o == kNoOwner ? kOwnerCount : static_cast<std::size_t>(o)];
}

std::vector<std::string> module_names() {
    std::vector<std::string> out;
    for (const Owner& o : kOwners) out.emplace_back(o.module);
    return out;
}

}  // namespace eqx
