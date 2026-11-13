#include "rules/structure/structure_rules.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "rules/module.h"
#include "rules/structure/se_common.h"

namespace eqx::structure {

// ------------------------------------------------------------------------------------------- pure helpers

std::vector<std::size_t> heading_nesting_failures(const std::vector<int>& levels) {
    std::vector<std::size_t> out;
    long long last = 0;   // StaticContainers.lastHeadingNestingLevel starts at 0
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] > last + 1) out.push_back(i);
        last = levels[i];
    }
    return out;
}

std::set<ObjRef> orphan_refs(const std::set<ObjRef>& referrers, const std::set<ObjRef>& own_refs) {
    std::set<ObjRef> out;
    for (const ObjRef& r : referrers)
        if (!own_refs.count(r)) out.insert(r);
    return out;
}

std::set<ObjRef> ghost_refs(const std::set<ObjRef>& referrers, const std::set<ObjRef>& own_refs) {
    std::set<ObjRef> out;
    for (const ObjRef& r : own_refs)
        if (!referrers.count(r)) out.insert(r);
    return out;
}

std::vector<bool> duplicate_ids(const std::vector<std::optional<std::string>>& ids) {
    std::vector<bool> out(ids.size(), false);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (ids[i]) out[i] = !seen.insert(*ids[i]).second;
    return out;
}

// ------------------------------------------------------------------------------------------- rules

namespace {

struct Rules {
    RuleIndex parent17 = R("EQX-UA1-7.1-12");
    RuleIndex parent20 = R("EQX-UA2-8.2.1-2");
    RuleIndex unmapped17 = R("EQX-UA1-7.1-5");
    RuleIndex circular17 = R("EQX-UA1-7.1-6");
    RuleIndex remapped17 = R("EQX-UA1-7.1-7");
    RuleIndex unmapped20 = R("EQX-UA2-8.2.4-1");
    RuleIndex circular20 = R("EQX-UA2-8.2.4-2");
    RuleIndex same_ns20 = R("EQX-UA2-8.2.4-3");
    RuleIndex remapped20 = R("EQX-UA2-8.2.4-4");
    RuleIndex lang_actual = R("EQX-UA1-7.2-21");
    RuleIndex lang_alt = R("EQX-UA1-7.2-22");
    RuleIndex lang_e = R("EQX-UA1-7.2-23");
    RuleIndex figure17 = R("EQX-UA1-7.3-1");
    RuleIndex formula17 = R("EQX-UA1-7.7-1");
    RuleIndex figure20 = R("EQX-UA2-8.2.5.28.2-1");
    RuleIndex nesting = R("EQX-UA1-7.4.2-1");
    RuleIndex h_with_hn = R("EQX-UA1-7.4.4-2");
    RuleIndex hn_with_h = R("EQX-UA1-7.4.4-3");
    RuleIndex h20 = R("EQX-UA2-8.2.5.12-1");
    RuleIndex note_id = R("EQX-UA1-7.9-1");
    RuleIndex note_dup = R("EQX-UA1-7.9-2");
    RuleIndex note20 = R("EQX-UA2-8.2.5.14-1");
    RuleIndex fenote_orphan = R("EQX-UA2-8.2.5.14-2");
    RuleIndex fenote_ghost = R("EQX-UA2-8.2.5.14-3");
    RuleIndex fenote_type = R("EQX-UA2-8.2.5.14-4");
    RuleIndex root_ns = R("EQX-UA2-8.2.5.2-2");
    RuleIndex list_numbering = R("EQX-UA2-8.2.5.25-1");
    RuleIndex math_parent = R("EQX-UA2-8.2.5.29-1");
    RuleIndex toci_ref = R("EQX-UA2-8.2.5.8-1");
};

const Rules& rules() {
    static const Rules r;
    return r;
}

std::string s_detail(const StructElem& e) {
    if (e.S) return "/S /" + *e.S;
    return "/S missing or not a name";
}

// GFPDStructElem.getremappedStandardType in the PDF 1.7 flavour.
std::optional<std::string> remapped17(const StructTree& st, const StructElem& e) {
    if (e.S && StructTree::is_standard17(*e.S)) {
        // hasStandardType(): the own type is standard; it is remapped when its standard type differs
        if (!e.std17 || *e.std17 != *e.S) return *e.S;
        return std::nullopt;
    }
    if (e.std17) {
        // standard type of the standard type (getStructureTypeStandardStructureType of the result)
        auto again = st.standard_type17(*e.std17);
        if (!again || *again != *e.std17) return *e.std17;
    }
    return std::nullopt;
}

// GFPDStructElem.getremappedStandardType in the ISO 32000-2 flavour, for SENonStandard objects (standard type
// null): only the hasStandardType() branch can apply.
std::optional<std::string> remapped20_nonstandard(const StructElem& e) {
    if (e.S && StructTree::is_standard20(e.type)) {
        if (!e.std20 || e.std20->type != e.type.type || e.std20->ns_uri() != e.type.ns_uri()) return *e.S;
    }
    return std::nullopt;
}

// GFSEMathMLStructElem.gethasParentFormulaOrMathML
bool has_parent_formula_or_math(const StructTree& st, const Obj& d) {
    std::unordered_set<const void*> seen;
    Obj parent = cos::get(d, "P");
    while (parent.isDictionary()) {
        if (auto t = st.stype_of(parent)) {
            if (auto std = st.default_type20(*t)) {
                if (std->type == "Formula" || std->ns_uri() == std::optional<std::string>(kNsMathML)) return true;
            }
        }
        if (parent.isIndirect() && !seen.insert(se::identity(parent)).second) break;
        parent = cos::get(parent, "P");
    }
    return false;
}

// GFSETOCI.containsRef: /Ref on the element or on any structure descendant (raw /K walk). veraPDF recurses without
// a guard; a cycle without any /Ref sets *cycle.
bool contains_ref(const Obj& d, bool* cycle) {
    struct Level {
        const void* id;
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    auto has_ref = [](const Obj& o) { return cos::has(o, "Ref") || (o.isDictionary() && const_cast<Obj&>(o).hasKey("/Ref")); };
    if (has_ref(d)) return true;
    std::unordered_set<const void*> path{se::identity(d)};
    std::vector<Level> stack;
    stack.push_back({se::identity(d), se::struct_children(d), 0});
    std::size_t visits = 0;
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            path.erase(top.id);
            stack.pop_back();
            continue;
        }
        Obj child = top.kids[top.next++];
        if (has_ref(child)) return true;
        const void* id = se::identity(child);
        if (!path.insert(id).second || ++visits > 20'000'000) {
            *cycle = true;
            continue;
        }
        stack.push_back({id, se::struct_children(child), 0});
    }
    return false;
}

std::set<ObjRef> ref_keys(const Obj& d) {
    std::set<ObjRef> out;
    Obj ref = cos::get(d, "Ref");
    if (!ref.isArray()) return out;
    for (const Obj& item : cos::array_items(ref)) {
        ObjRef r = ref_of(item);
        if (r.valid()) out.insert(r);
    }
    return out;
}

std::string join_refs(const std::set<ObjRef>& s) {
    std::string out;
    for (const ObjRef& r : s) {
        if (!out.empty()) out += ",";
        out += r.str();
    }
    return out;
}

// GFSENote.hasDuplicateNoteID depends on the order in which veraPDF constructs the SENote objects: a parent's
// children are constructed when the parent is processed (getChildren, cached), and while processing it the
// PDF/UA-1 rule 7.4.4-1 (kidsStandardTypes) also constructs the children of its pass-through descendants; a Table's
// checkTable constructs the children of its row groups and rows. Returns, per Note dictionary, the duplicate flag
// of its first construction (the validated object).
std::unordered_map<const void*, bool> note_duplicates_in_construction_order(const StructTree& st,
                                                                          const std::vector<int>& order) {
    const auto& elems = st.elems();
    std::unordered_map<const void*, bool> first;
    std::unordered_set<std::string> ids;
    std::vector<char> done(elems.size(), 0);

    auto emit = [&](const StructKid& kid) {
        std::string t = kid.elem >= 0 ? elems[kid.elem].standard(Flavor::Pdf17)
                                      : se::standard_type(st, kid.dict, Flavor::Pdf17);
        if (t != "Note") return;
        auto id = se::string_key(kid.dict, "ID");
        bool dup = id ? !ids.insert(*id).second : false;
        first.try_emplace(se::identity(kid.dict), dup);
    };
    auto construct = [&](int i) {
        if (done[i]) return;
        done[i] = 1;
        for (const StructKid& kid : elems[i].kids)
            if (kid.kind == StructKid::Kind::Elem) emit(kid);
    };
    // getChildrenStandardTypes recursion (pass-through children), guarded against cycles; explicit stacks, as these
    // chains have no depth limit
    std::vector<char> on_path(elems.size(), 0);
    auto flatten = [&](int start) {
        struct Level {
            int elem;
            std::size_t next;
        };
        std::vector<Level> stack;
        construct(start);
        if (on_path[start]) return;
        on_path[start] = 1;
        stack.push_back({start, 0});
        while (!stack.empty()) {
            Level& top = stack.back();
            const auto& kids = elems[top.elem].struct_kids;
            if (top.next >= kids.size()) {
                on_path[top.elem] = 0;
                stack.pop_back();
                continue;
            }
            int k = kids[top.next++];
            if (!se::is_pass_through(elems[k].standard(Flavor::Pdf17))) continue;
            construct(k);
            if (on_path[k]) continue;
            on_path[k] = 1;
            stack.push_back({k, 0});
        }
    };
    // structural-significance children
    auto ssc = [&](int start, std::vector<int>& out) {
        struct Level {
            int elem;
            std::size_t next;
        };
        std::vector<Level> stack;
        if (on_path[start]) return;
        on_path[start] = 1;
        stack.push_back({start, 0});
        while (!stack.empty()) {
            Level& top = stack.back();
            const auto& kids = elems[top.elem].struct_kids;
            if (top.next >= kids.size()) {
                on_path[top.elem] = 0;
                stack.pop_back();
                continue;
            }
            int k = kids[top.next++];
            if (!se::is_pass_through(elems[k].standard(Flavor::Pdf17))) {
                out.push_back(k);
                continue;
            }
            if (on_path[k]) continue;
            on_path[k] = 1;
            stack.push_back({k, 0});
        }
    };

    for (const StructKid& kid : st.root_kids())
        if (kid.kind == StructKid::Kind::Elem) emit(kid);
    for (int i : order) {
        flatten(i);
        if (elems[i].standard(Flavor::Pdf17) != "Table") continue;
        std::vector<int> top, rows;
        ssc(i, top);
        for (int c : top) {
            std::string t = elems[c].standard(Flavor::Pdf17);
            if (t == "TR") {
                rows.push_back(c);
            } else if (t == "THead" || t == "TBody" || t == "TFoot") {
                flatten(c);
                std::vector<int> g;
                ssc(c, g);
                for (int r : g)
                    if (elems[r].standard(Flavor::Pdf17) == "TR") rows.push_back(r);
            }
        }
        for (int r : rows) flatten(r);
    }
    return first;
}

}  // namespace

void run_structure_rules(ValidationContext& ctx) {
    const Rules& R = rules();
    StructTree& st = ctx.st;
    if (!st.present()) return;   // without a StructTreeRoot veraPDF creates no structure objects
    const auto& elems = st.elems();
    const bool ref_mode = ctx.reference_dedup;
    const std::vector<int> order = se::element_order(st, ref_mode);
    se::Attributes attrs(st);
    const bool catalog_lang = cos::get(ctx.doc.catalog(), "Lang").isString();

    // FENote references (GFPDStructElem.fillStructElemRefs, PDF/UA-2 flavours): every element with an object key
    // registers the keys named by its /Ref array.
    std::unordered_map<std::uint64_t, std::set<ObjRef>> referrers;
    for (const StructElem& e : elems) {
        if (!e.ref.valid()) continue;
        for (const ObjRef& r : ref_keys(e.dict)) referrers[r.key()].insert(e.ref);
    }

    std::vector<int> hn_levels;
    std::vector<int> hn_elems;
    std::vector<int> h_elems17;
    bool list_cycle = false, toci_cycle = false;

    for (int idx : order) {
        const StructElem& e = elems[idx];
        const Obj& d = e.dict;
        const ObjRef ref = e.ref;

        // 7.1-12 / 8.2.1-2 + PAC StructureElementHasParentKey: no /P entry. veraPDF's containsParent is always true
        // (COSDictionary.getKey never returns null), so this is the normative requirement as PAC checks it.
        if (cos::get(d, "P").isNull()) {
            ctx.report(R.parent17, -1, ref, s_detail(e) + ": no /P entry");
            ctx.report(R.parent20, -1, ref, s_detail(e) + ": no /P entry");
        }

        // ---------------------------------------------------------------- PDF/UA-1 (PDF 1.7 semantics)
        const std::string s17 = e.standard(Flavor::Pdf17);
        const std::optional<bool> circ17 = e.S ? std::optional<bool>(st.circular17(*e.S)) : std::nullopt;
        const auto remap17 = remapped17(st, e);
        if (s17.empty()) {
            // SENonStandard.isNotMappedToStandardType
            if (!remap17 && circ17 != std::optional<bool>(true))
                ctx.report(R.unmapped17, -1, ref, s_detail(e) + " is not mapped to a standard type");
        }
        if (circ17 == std::optional<bool>(true))
            ctx.report(R.circular17, -1, ref, s_detail(e) + ": circular role mapping");
        if (remap17) ctx.report(R.remapped17, -1, ref, "standard type /" + *remap17 + " is remapped");

        if (!catalog_lang) {
            auto at = se::string_or_name_key(d, "ActualText");
            auto alt = se::string_key(d, "Alt");
            auto exp = se::string_key(d, "E");
            if ((at || alt || exp) && !cos::get(d, "Lang").isString() && !st.parent_lang(d).has_value()) {
                if (at) ctx.report(R.lang_actual, -1, ref, s_detail(e) + ": ActualText without a language");
                if (alt) ctx.report(R.lang_alt, -1, ref, s_detail(e) + ": Alt without a language");
                if (exp) ctx.report(R.lang_e, -1, ref, s_detail(e) + ": E without a language");
            }
        }

        if (s17 == "Figure" || s17 == "Formula") {
            auto alt = se::string_key(d, "Alt");
            bool ok = (alt && !alt->empty()) || se::string_or_name_key(d, "ActualText").has_value();
            if (!ok)
                ctx.report(s17 == "Figure" ? R.figure17 : R.formula17, -1, ref,
                           s17 + " without Alt or ActualText");
        }
        if (se::is_hn(s17)) {
            hn_levels.push_back(std::stoi(s17.substr(1)));
            hn_elems.push_back(e.index);
        } else if (s17 == "H") {
            h_elems17.push_back(e.index);
        }
        if (s17 == "Note") {
            auto id = se::string_key(d, "ID");
            if (!id || id->empty()) ctx.report(R.note_id, -1, ref, "Note without ID");
        }

        // ---------------------------------------------------------------- PDF/UA-2, WTPDF (ISO 32000-2 semantics)
        const std::string s20 = e.standard(Flavor::Pdf20);
        const std::optional<bool> circ20 =
            e.S ? std::optional<bool>(st.circular17(*e.S) || st.circular20(e.type)) : std::nullopt;
        if (s20.empty()) {
            auto remap20 = remapped20_nonstandard(e);
            if (!remap20 && circ20 != std::optional<bool>(true))
                ctx.report(R.unmapped20, -1, ref, s_detail(e) + " is not mapped to a standard type");
            if (remap20) ctx.report(R.remapped20, -1, ref, "standard type /" + *remap20 + " is remapped");
        }
        if (circ20 == std::optional<bool>(true))
            ctx.report(R.circular20, -1, ref, s_detail(e) + ": circular role mapping");
        if (e.S) {
            if (auto same = st.role_map_to_same_ns_tag(e.type))
                ctx.report(R.same_ns20, -1, ref, s_detail(e) + " role mapped within namespace " + *same);
        }
        if (e.is_math(Flavor::Pdf20)) {
            if (!has_parent_formula_or_math(st, d))
                ctx.report(R.math_parent, -1, ref, s_detail(e) + ": MathML element outside Formula");
        } else if (s20 == "H") {
            ctx.report(R.h20, -1, ref, "H structure element");
        } else if (s20 == "Note") {
            ctx.report(R.note20, -1, ref, "Note structure element");
        } else if (s20 == "Figure") {
            if (!se::string_key(d, "Alt") && !se::string_or_name_key(d, "ActualText"))
                ctx.report(R.figure20, -1, ref, "Figure without Alt or ActualText");
        } else if (s20 == "FENote") {
            const std::set<ObjRef> own = ref_keys(d);
            if (ref.valid()) {
                auto it = referrers.find(ref.key());
                if (it != referrers.end()) {
                    auto orphans = orphan_refs(it->second, own);
                    if (!orphans.empty())
                        ctx.report(R.fenote_orphan, -1, ref, "referring elements not in /Ref: " + join_refs(orphans));
                }
            }
            static const std::set<ObjRef> none;
            auto it = ref.valid() ? referrers.find(ref.key()) : referrers.end();
            auto ghosts = ghost_refs(it != referrers.end() ? it->second : none, own);
            if (!ghosts.empty())
                ctx.report(R.fenote_ghost, -1, ref, "/Ref elements not referring back: " + join_refs(ghosts));
            auto type = attrs.name(d, "NoteType", "FENote", std::string("None"), false);
            if (type != "Footnote" && type != "Endnote" && type != "None")
                ctx.report(R.fenote_type, -1, ref, "NoteType /" + type.value_or(""));
        } else if (s20 == "TOCI") {
            bool cycle = false;
            if (!contains_ref(d, &cycle)) {
                if (cycle) toci_cycle = true;
                else ctx.report(R.toci_ref, -1, ref, "TOCI without Ref (also on its descendants)");
            }
        } else if (s20 == "L") {
            bool ok = true, labels = false;
            for (const auto& child : se::significance_children(st, d, Flavor::Pdf20, &ok)) {
                if (child.type != "LI") continue;
                auto kids = se::children_standard_types(st, child.dict, Flavor::Pdf20, &ok);
                for (const auto& k : kids)
                    if (k == "Lbl") labels = true;
                if (labels) break;
            }
            if (!ok) {
                list_cycle = true;
            } else if (labels) {
                bool cycle = false;
                auto numbering = attrs.name(d, "ListNumbering", "List", std::string("None"), true, &cycle);
                if (cycle) list_cycle = true;
                else if (numbering == "None")
                    ctx.report(R.list_numbering, -1, ref, "L with Lbl but ListNumbering None");
            }
        }
    }

    // 7.4.2-1: GFSEHn.hasCorrectNestingLevel in validation order
    for (std::size_t i : heading_nesting_failures(hn_levels)) {
        const StructElem& e = elems[hn_elems[i]];
        ctx.report(R.nesting, -1, e.ref, "H" + std::to_string(hn_levels[i]) + " skips a heading level");
    }
    // 7.4.4-2 / 7.4.4-3: deferred rules over the variables usesH / usesHn
    if (!hn_elems.empty())
        for (int i : h_elems17) ctx.report(R.h_with_hn, -1, elems[i].ref, "H used together with Hn");
    if (!h_elems17.empty())
        for (int i : hn_elems) ctx.report(R.hn_with_h, -1, elems[i].ref, "Hn used together with H");

    // 7.9-2: duplicate Note IDs. Default: every Note whose ID an earlier Note (document order) has. Reference
    // mode: veraPDF's construction order (GFSENote constructor), united with that document-order condition (PAC
    // NoteTagHasUniqueID) only when PAC applies.
    {
        std::unordered_map<const void*, bool> dup;
        if (ref_mode) dup = note_duplicates_in_construction_order(st, order);
        const bool doc_order = !ref_mode || ctx.pac_applicable;
        std::unordered_set<std::string> seen;
        for (int idx : order) {
            const StructElem& e = elems[idx];
            if (e.standard(Flavor::Pdf17) != "Note") continue;
            auto id = se::string_key(e.dict, "ID");
            bool order_dup = id && !seen.insert(*id).second && doc_order;
            auto it = dup.find(se::identity(e.dict));
            bool vera_dup = it != dup.end() && it->second;
            if (order_dup || vera_dup) ctx.report(R.note_dup, -1, e.ref, "Note ID \"" + id.value_or("") + "\" is not unique");
        }
    }

    // 8.2.5.2-2: PDStructTreeRoot kidsStandardTypes / firstChildStandardTypeNamespaceURL (ISO 32000-2)
    {
        bool ok = true;
        auto kids = se::children_standard_types(st, st.root(), Flavor::Pdf20, &ok);
        std::string joined;
        for (const auto& k : kids) {
            if (k.empty()) continue;
            if (!joined.empty()) joined += "&";
            joined += k;
        }
        if (!ok) {
            ctx.mark_incomplete(R.root_ns, "pass-through cycle below the StructTreeRoot (veraPDF: stack overflow)");
        } else if (joined == "Document") {
            auto first = se::significance_children(st, st.root(), Flavor::Pdf20, &ok);
            std::optional<std::string> url;
            if (!first.empty()) {
                if (auto t = st.stype_of(first.front().dict))
                    if (auto std = st.default_type20(*t)) url = std->ns_uri();
            }
            if (url != std::optional<std::string>(kNsPdf2))
                ctx.report(R.root_ns, -1, st.root_ref(),
                           "Document root element not in the PDF 2.0 namespace (" + url.value_or("null") + ")");
        }
    }

    if (list_cycle)
        ctx.mark_incomplete(R.list_numbering, "structure cycle while resolving lists (veraPDF: stack overflow)");
    if (toci_cycle) ctx.mark_incomplete(R.toci_ref, "structure cycle below a TOCI (veraPDF: stack overflow)");
}

}  // namespace eqx::structure
