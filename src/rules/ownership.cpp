// Which module evaluates which canonical rule. Families come from catalog/rules.json
// ("family" field); a pattern ending in '*' matches a family prefix. Explicit ids win over families.
// Every catalogue rule must have exactly one owner (checked by `dox-validate --self-check`).
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <mutex>

#include "rules/module.h"

namespace eqx {

namespace {

struct Owner {
    const char* module;
    std::vector<const char*> families;
    std::vector<const char*> ids;
};

const std::vector<Owner>& owners() {
    static const std::vector<Owner> o = {
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
    return o;
}

bool match(std::string_view p, std::string_view family) {
    if (p == "pac.*Tag") {
        // PAC structure-type checks: families "pac.<Type>Tag" (PTag, TOCITag, DocumentTag, ...)
        return family.size() > 7 && family.substr(0, 4) == "pac." && family.substr(family.size() - 3) == "Tag";
    }
    if (!p.empty() && p.back() == '*') return family.substr(0, p.size() - 1) == p.substr(0, p.size() - 1);
    return family == p;
}

struct Table {
    std::map<std::string, std::vector<RuleIndex>, std::less<>> by_module;
    std::vector<std::string> owner;   // per rule index
};

const Table& table() {
    static const Table t = [] {
        Table t;
        const Catalog& c = Catalog::get();
        t.owner.assign(c.size(), "");
        // Precedence: an explicit id (the last owner that lists it), then an exact family (the first owner), then the
        // first pattern with '*' in owner order ("pac.*Tag" must not swallow families that end in "Tag" of other
        // owners: explicit families first). An exact family that is not in `exact` cannot match as a pattern either.
        std::unordered_map<std::string_view, const char*> by_id, exact;
        std::vector<std::pair<std::string_view, const char*>> patterns;
        for (const Owner& o : owners()) {
            t.by_module[o.module];
            for (const char* id : o.ids) by_id[id] = o.module;
            for (const char* f : o.families) {
                std::string_view fv(f);
                if (fv.find('*') == std::string_view::npos) exact.emplace(fv, o.module);
                else patterns.emplace_back(fv, o.module);
            }
        }
        for (RuleIndex i = 0; i < c.size(); ++i) {
            const RuleInfo& r = c.rule(i);
            const std::string_view fam = r.family;
            const char* found = nullptr;
            if (auto it = by_id.find(r.id); it != by_id.end()) found = it->second;
            else if (auto e = exact.find(fam); e != exact.end()) found = e->second;
            else
                for (const auto& [p, module] : patterns)
                    if (match(p, fam)) {
                        found = module;
                        break;
                    }
            t.owner[i] = found ? found : "";
            if (found) t.by_module[found].push_back(i);
        }
        return t;
    }();
    return t;
}

}  // namespace

const std::vector<RuleIndex>& owned_rules(std::string_view module) {
    static const std::vector<RuleIndex> empty;
    auto it = table().by_module.find(module);
    return it == table().by_module.end() ? empty : it->second;
}

const std::string& owner_of(RuleIndex rule) { return table().owner[rule]; }

std::vector<std::string> module_names() {
    std::vector<std::string> out;
    for (const Owner& o : owners()) out.push_back(o.module);
    return out;
}

}  // namespace eqx
