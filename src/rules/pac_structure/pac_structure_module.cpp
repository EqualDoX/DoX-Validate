// Module pac_structure: PAC 26.1 PDF/UA-tab structure checks without a veraPDF equivalent.
//
//   Structure tree     per type TagInvalidSyntax / InappropriateUse / TagAsRoot,
//                      DocumentTagIsRoot, MarkedContentIsInLegalPosition (pac_model.h)
//   Structure elements FirstHeadingIsH1, NoHeadingsSkipped, HeadingStructureTypesNotMixed, FigureHasBBox
//   PDF Syntax         StructureElementHasParentKey2, StructureHasCorruptElements (8), StructuralParentTree (7)
//
// Masking: without a StructTreeRoot PAC reports the file as untagged and its structure checks are not applicable
// (U1-D02). A missing or false /MarkInfo /Marked masks nothing: a structure tree is checked all the same (probes mk_none, mk_false;
// a real-world sample).
// PAC gives no locations; findings name the structure element (or page, annotation, ParentTree) flagged.
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rules/module.h"
#include "rules/pac_structure/pac_model.h"
#include "rules/structure/se_common.h"
#include "util/text.h"

namespace eqx {

namespace {

using pac::Cond;

struct Layout {
    bool bbox = false;
    std::optional<std::string> placement;
};

// Layout attributes of /A (dictionary, or array of dictionaries and revision numbers); first value wins.
Layout layout_of(const Obj& elem) {
    Layout l;
    Obj a = cos::get(elem, "A");
    std::vector<Obj> dicts;
    if (a.isDictionary()) dicts.push_back(a);
    else if (a.isArray())
        for (auto& x : cos::array_items(a))
            if (x.isDictionary()) dicts.push_back(x);
    for (auto& d : dicts) {
        if (!cos::name_is(d, "O", "Layout")) continue;
        if (!l.bbox && cos::has(d, "BBox")) l.bbox = true;
        if (!l.placement)
            if (auto p = cos::name(d, "Placement")) l.placement = *p;
    }
    return l;
}

// Up to two distinct page keys (0 = no /Pg), enough to decide "on a single page".
struct PageSet {
    std::uint64_t v[2] = {0, 0};
    int n = 0;
    void add(std::uint64_t k) {
        for (int i = 0; i < n; ++i)
            if (v[i] == k) return;
        if (n < 2) v[n] = k;
        n = std::min(n + 1, 2);
    }
};

struct SibInfo {
    int index = 0, count = 1, captions = 0, theads = 0, tfoots = 0, bad_kids = 0;
    bool has_row = false, has_group = false, group_first = true, group_last = true;
    int theads_before = 0, tbodies_before = 0, tfoots_before = 0;
};

class Module final : public RuleModule {
public:
    const char* name() const override { return "pac_structure"; }
    void run(ValidationContext& ctx) override;

private:
    void structure_tree(ValidationContext& ctx);
    void headings(ValidationContext& ctx);
    void figures(ValidationContext& ctx);
    void parent_keys(ValidationContext& ctx);
    void corrupt_elements(ValidationContext& ctx);
    void parent_tree(ValidationContext& ctx);
    void irregular_rows(ValidationContext& ctx);

    std::vector<std::string> ty_;   // PAC type per element ("" = not a standard type)
    std::vector<int> eff_;          // effective parent per element (-1 = StructTreeRoot)
};

std::string_view type_name(const std::vector<std::string>& ty, int idx) { return idx < 0 ? pac::kRoot : ty[idx]; }

std::string display(const StructTree& st, const std::vector<std::string>& ty, int idx) {
    if (idx < 0) return "StructTreeRoot";
    if (!ty[idx].empty()) return ty[idx];
    const auto& s = st.elem(idx).S;
    return s ? *s : std::string("(no /S)");
}

RuleIndex find_rule(const std::string& id) {
    if (id.empty()) return kNoRule;
    RuleIndex r = Catalog::get().find(id);
    if (r == kNoRule || owner_of(r) != "pac_structure") return kNoRule;
    return r;
}

// PAC judges the /RoleMap itself, whether or not an element uses an entry (ISO 14289-1 7.1, Matterhorn 02-001, 02-003, 02-004 name the mappings, not their use;
// veraPDF looks only at used types). Measured on unused entries, one PAC run each (probes d_ua_rm_unused_*):
//   standard key -> other type   "standard type is remapped" (7.1-7; Word writes /Ruby, /RB, /RT -> /Span: a real-world sample, 3 of 18 entries)
//   key -> value neither standard nor mapped   "neither mapped to a standard type" (7.1-5; /Foo -> /Bar, /Foo -> /Artifact, /P -> /Foo both conditions)
//   key -> itself   "mapped in a circular fashion" (7.1-6; /Foo -> /Foo). A longer cycle of unused entries passes PAC (/Aa -> /Bb, /Bb -> /Aa, used it hangs PAC); it is
//   still a circular mapping by Matterhorn 02-003, veraPDF reports it when used; not reported here for unused entries (open, recorded in pac_rule_evidence.json).
void role_map_entries(ValidationContext& ctx) {
    static const RuleIndex remapped = R("EQX-UA1-7.1-7");
    static const RuleIndex unmapped = R("EQX-UA1-7.1-5");
    static const RuleIndex circular = R("EQX-UA1-7.1-6");
    const auto& rm = ctx.st.role_map();
    std::map<std::string, std::string> sorted(rm.begin(), rm.end());
    // A value that is no name (ISO 32000-1 Table 322): PAC 26.1 aborts the whole check (an internal error, probe ua_rm_unused_value_string), it never passes the file.
    Obj raw = cos::get(ctx.st.root(), "RoleMap");
    if (!raw.isNull() && !raw.isDictionary()) ctx.report(unmapped, -1, ObjRef{}, "PAC: /RoleMap is not a dictionary (PAC aborts on such a file)");
    if (raw.isDictionary())
        for (const auto& key : const_cast<Obj&>(raw).getKeys()) {
            Obj v = raw.getKey(key);
            if (!v.isName() && !v.isNull())
                ctx.report(unmapped, -1, ObjRef{}, "PAC: RoleMap entry " + key + " is not a name (PAC aborts on such a file)");
        }
    for (const auto& [from, to] : sorted) {
        if (StructTree::is_standard17(from)) ctx.report(remapped, -1, ObjRef{}, "PAC: standard type /" + from + " is remapped to /" + to + " in the RoleMap");
        if (from == to) {
            // A standard type mapped to itself counts twice (rn_doc_self; a real-world sample: 9 such entries, 18 failures).
            ctx.report(circular, -1, ObjRef{}, "PAC: structure type /" + from + " is mapped to itself in the RoleMap");
        } else if (!StructTree::is_standard17(to) && rm.find(to) == rm.end()) {
            // Only the entry whose own value is not mapped (rn_chain_unknown: /Foo -> /Bar, /Bar -> /Baz, one failure).
            ctx.report(unmapped, -1, ObjRef{}, "PAC: RoleMap entry /" + from + " -> /" + to + ": /" + to + " is neither standard nor mapped");
        }
    }
}

void Module::run(ValidationContext& ctx) {
    const StructTree& st = ctx.st;
    // PAC 26.1 defines its checks for PDF <= 1.7 only: for PDF 2.0 the engine lists every PAC rule as not
    // applicable and drops their findings, so nothing is evaluated here.
    if (!ctx.pac_applicable) return;
    if (!st.present()) return;
    role_map_entries(ctx);

    // PAC types: a standard /S is used as is (U1-S02: the standard type Quote remapped to Span still counts as
    // a Quote), a non-standard /S through the RoleMap (PDF 1.7 semantics, also for PDF 2.0 files).
    const auto& E = st.elems();
    ty_.assign(E.size(), std::string());
    eff_.assign(E.size(), -1);
    for (std::size_t i = 0; i < E.size(); ++i) {
        const auto& e = E[i];
        if (e.S) {
            if (pac::is_pdf17_type(*e.S)) ty_[i] = *e.S;
            else if (auto s = st.standard_type17(*e.S)) ty_[i] = *s;
        }
        int p = e.parent;   // parents precede their kids in the traversal order
        eff_[i] = (p >= 0 && pac::is_transparent(ty_[p])) ? eff_[p] : p;
    }
    // An element whose type never reaches a standard type although the map holds it (/Foo -> /Foo): PAC also says "neither mapped" for the used type (p_rn_used_self).
    {
        static const RuleIndex unmapped = R("EQX-UA1-7.1-5");
        std::set<std::string> done;
        for (const auto& e : E)
            if (e.S && !StructTree::is_standard17(*e.S) && !st.standard_type17(*e.S) && st.role_map().count(*e.S) && done.insert(*e.S).second)
                ctx.report(unmapped, -1, e.ref, "PAC: /" + *e.S + " used, its RoleMap entry does not reach a standard type");
    }
    structure_tree(ctx);
    headings(ctx);
    figures(ctx);
    parent_keys(ctx);
    corrupt_elements(ctx);
    parent_tree(ctx);
    irregular_rows(ctx);
}

// ------------------------------------------------------------------------------------------ irregular rows
// TablesAreRegular: the tables module decides the rule per table (veraPDF's checkRegular). PAC counts rows: every row whose
// width (column spans plus the row spans reaching into it) differs from the first row's. Measured: PAC's counts on
// four real-world samples (15, 14, 3 and 2 rows) are all reproduced by this rule.
void Module::irregular_rows(ValidationContext& ctx) {
    static const RuleIndex rule = R("EQX-PAC-WarnTablesAreRegular");
    if (!ctx.findings.has(rule)) return;
    const StructTree& st = ctx.st;
    const auto& E = st.elems();
    se::Attributes attrs(st);
    std::vector<ObjRef> rows_found;
    std::vector<int> rows, stack;
    for (std::size_t t = 0; t < E.size(); ++t) {
        if (ty_[t] != "Table") continue;
        rows.clear();
        stack.assign(E[t].elems.rbegin(), E[t].elems.rend());
        while (!stack.empty() && rows.size() < 100000) {
            const int k = stack.back();
            stack.pop_back();
            const std::string& ty = ty_[k];
            if (ty == "TR") {
                rows.push_back(k);
            } else if (ty == "THead" || ty == "TBody" || ty == "TFoot" || ty == "NonStruct" || ty == "Private" || ty == "Part" || ty == "Div") {
                for (auto it = E[k].elems.rbegin(); it != E[k].elems.rend(); ++it) stack.push_back(*it);
            }
        }
        std::unordered_map<std::size_t, std::int64_t> carried;
        std::int64_t first = -1;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::int64_t width = carried[r];
            for (int c : E[rows[r]].elems) {
                if (ty_[c] != "TD" && ty_[c] != "TH") continue;
                const std::int64_t cs = std::max<std::int64_t>(1, attrs.integer(E[c].dict, "ColSpan", "Table", 1));
                const std::int64_t rs = std::clamp<std::int64_t>(attrs.integer(E[c].dict, "RowSpan", "Table", 1), 1, 100000);
                width += cs;
                for (std::size_t z = r + 1; z < r + std::size_t(rs) && z < rows.size(); ++z) carried[z] += cs;
            }
            if (first < 0) first = width;
            else if (width != first) rows_found.push_back(E[rows[r]].ref);
        }
    }
    if (rows_found.empty()) return;   // irregular for another reason (overlapping cells): the table-level findings stay
    ctx.findings.erase(rule);
    for (const ObjRef& r : rows_found) ctx.report(rule, -1, r, "row width differs from the table's first row");
}

// ------------------------------------------------------------------------------------------- structure tree
void Module::structure_tree(ValidationContext& ctx) {
    const StructTree& st = ctx.st;
    const auto& E = st.elems();
    // Sibling positions inside Table / L / TOC (kids flattened through NonStruct / Private).
    std::vector<SibInfo> sib(E.size());
    std::vector<int> flat, stack;
    for (std::size_t p = 0; p < E.size(); ++p) {
        const std::string& pt = ty_[p];
        if (pt != "Table" && pt != "L" && pt != "TOC" && pt != "THead" && pt != "TBody" && pt != "TFoot") continue;
        flat.clear();
        stack.assign(E[p].elems.rbegin(), E[p].elems.rend());
        while (!stack.empty()) {
            int k = stack.back();
            stack.pop_back();
            if (pac::is_transparent(ty_[k]) && flat.size() + stack.size() < 1000000) {
                for (auto it = E[k].elems.rbegin(); it != E[k].elems.rend(); ++it) stack.push_back(*it);
                continue;
            }
            flat.push_back(k);
        }
        SibInfo base;
        base.count = static_cast<int>(flat.size());
        for (int k : flat) {
            base.captions += ty_[k] == "Caption";
            base.theads += ty_[k] == "THead";
            base.tfoots += ty_[k] == "TFoot";
        }
        // Kids the parent does not allow, and a Caption of a Table not at its first or last place.
        for (std::size_t j = 0; j < flat.size(); ++j) {
            const std::string& kt = ty_[flat[j]];
            if (kt.empty()) continue;
            if (!pac::strict_allows(pt, kt) || (pt == "Table" && kt == "Caption" && (base.captions > 1 || (j != 0 && j + 1 != flat.size())))) ++base.bad_kids;
        }
        int first_group = -1, last_group = -1;
        for (std::size_t j = 0; j < flat.size(); ++j) {
            const std::string& kt = ty_[flat[j]];
            base.has_row = base.has_row || kt == "TR";
            base.has_group = base.has_group || kt == "THead" || kt == "TBody" || kt == "TFoot";
            if (kt != "Caption") {
                if (first_group < 0) first_group = static_cast<int>(j);
                last_group = static_cast<int>(j);
            }
        }
        int theads = 0, tbodies = 0, tfoots = 0;
        for (std::size_t j = 0; j < flat.size(); ++j) {
            SibInfo& si = sib[flat[j]];
            si = base;
            si.index = static_cast<int>(j);
            si.group_first = static_cast<int>(j) == first_group;
            si.group_last = static_cast<int>(j) == last_group;
            si.theads_before = theads;
            si.tbodies_before = tbodies;
            si.tfoots_before = tfoots;
            const std::string& kt = ty_[flat[j]];
            theads += kt == "THead";
            tbodies += kt == "TBody";
            tfoots += kt == "TFoot";
        }
    }

    static const RuleIndex warn_mc = R("EQX-PAC-WarnMarkedContentIsInLegalPosition");
    static const RuleIndex fail_mc = R("EQX-PAC-FailMarkedContentIsInLegalPosition");
    for (std::size_t i = 0; i < E.size(); ++i) {
        const auto& e = E[i];
        const std::string& t = ty_[i];
        if (t.empty()) continue;
        pac::Context c;
        c.type = t;
        c.parent = type_name(ty_, e.parent);
        c.eff_parent = type_name(ty_, eff_[i]);
        c.eff_grandparent = eff_[i] >= 0 ? type_name(ty_, eff_[eff_[i]]) : pac::kRoot;
        // PAC takes the parent from /P: without one, or naming the StructTreeRoot, the element counts as a root element wherever /K places it (d_cr_se_no_p, d_cr_se_p_wrong: "P used as root element").
        // A content syntax error masks this warning (U1-X01: same element, Structure tree passes).
        static const RuleIndex parsable = R("EQX-PAC-FailPDFParsable");
        if (!ctx.findings.has(parsable)) {
            Obj up = cos::get(e.dict, "P");
            if (up.isNull() || (up.isDictionary() && st.root_ref().valid() && ref_of(up) == st.root_ref())) {
                c.parent = pac::kRoot;
                c.eff_parent = pac::kRoot;
                c.eff_grandparent = pac::kRoot;
            }
        }
        Layout lay;
        if (t == "Figure" || t == "Formula" || pac::is_inline(t)) {
            lay = layout_of(e.dict);
            if (lay.placement) c.placement = *lay.placement;
        }
        const SibInfo& s = sib[i];
        c.index = s.index;
        c.count = s.count;
        c.captions = s.captions;
        c.theads = s.theads;
        c.tfoots = s.tfoots;
        c.bad_kids = s.bad_kids;
        c.table_has_row = s.has_row;
        c.table_has_group = s.has_group;
        c.group_first = s.group_first;
        c.group_last = s.group_last;
        c.theads_before = s.theads_before;
        c.tbodies_before = s.tbodies_before;
        c.tfoots_before = s.tfoots_before;
        pac::Outcome o = pac::classify(c);
        if (o.cond != Cond::None) {
            RuleIndex r = find_rule(pac::rule_id(t, o.cond));
            if (r != kNoRule) {
                std::string detail = t + " in " + display(st, ty_, e.parent);
                if (eff_[i] != e.parent) detail += " (context " + display(st, ty_, eff_[i]) + ")";
                if (lay.placement) detail += ", Placement " + *lay.placement;
                detail += ": ";
                detail += o.reason;
                ctx.report(r, -1, e.ref, std::move(detail));
            }
        }
        // MarkedContentIsInLegalPosition: one finding per element holding marked content directly (U1-S04:
        // 7 elements with one MCID each, U1-C01: Document; structcheck counted per element).
        pac::ContentPos cp = pac::own_content(t);
        if (cp != pac::ContentPos::Ok) {
            int n = 0;
            for (const auto& k : e.kids)
                if (k.kind == StructKid::Kind::MCID || k.kind == StructKid::Kind::MCR) ++n;
            if (n > 0)
                ctx.report(cp == pac::ContentPos::Fail ? fail_mc : warn_mc, -1, e.ref,
                           t + " holds " + std::to_string(n) + " marked-content sequence(s) directly");
        }
    }
}

// ------------------------------------------------------------------------------------------------- headings
void Module::headings(ValidationContext& ctx) {
    static const RuleIndex first_h1 = R("EQX-PAC-FailFirstHeadingIsH1");
    static const RuleIndex skipped = R("EQX-PAC-FailNoHeadingsSkipped");
    static const RuleIndex mixed = R("EQX-PAC-FailCaptionHeadingStructureTypesNotMixed");
    const auto& E = ctx.st.elems();
    int first_h = -1, first_hn = -1, prev = 0;
    for (std::size_t i = 0; i < E.size(); ++i) {
        int lvl = pac::heading_level(ty_[i]);
        // H7 and deeper count by their name, whatever the RoleMap says (ISO 14289-1 7.4.2 names them; d_hd_h7: H1, then H7 mapped to P fails; d_hx_h7_p_after_h6 passes).
        if (E[i].S && E[i].S->size() >= 2 && (*E[i].S)[0] == 'H' && !pac::is_pdf17_type(*E[i].S)) {
            const std::string& nm = *E[i].S;
            bool digits = true;
            for (std::size_t k = 1; k < nm.size(); ++k) digits = digits && nm[k] >= '0' && nm[k] <= '9';
            if (digits && nm.size() <= 4 && std::stoi(nm.substr(1)) >= 7) lvl = std::stoi(nm.substr(1));
        }
        if (lvl < 0) continue;
        if (lvl == 0) {
            if (first_h < 0) first_h = static_cast<int>(i);
            continue;
        }
        // Numbered headings in logical (depth-first) order: the first must be H1 (U1-H01: H2 first fails), each
        // later one at most one level deeper than the previous one (U1-H01: H2 -> H4 fails; NoHeadingsSkipped
        // counts every Hn, the first one passing).
        if (first_hn < 0) {
            first_hn = static_cast<int>(i);
            if (lvl != 1) ctx.report(first_h1, -1, E[i].ref, "first numbered heading is H" + std::to_string(lvl));
        } else if (lvl > prev + 1) {
            ctx.report(skipped, -1, E[i].ref, "H" + std::to_string(lvl) + " follows H" + std::to_string(prev));
        }
        prev = lvl;
    }
    // One failure per document using both H and Hn (U1-H02 / U1-S01 / U1-S03 / U1-S04: 1 each).
    if (first_h >= 0 && first_hn >= 0)
        ctx.report(mixed, -1, E[first_h].ref, "H used together with " + ty_[first_hn]);
}

// -------------------------------------------------------------------------------------------------- figures
void Module::figures(ValidationContext& ctx) {
    // /Alt of white space only is no alternative text (PAC fails it, al_fig_space_alt, al_fig_nbsp_alt; Matterhorn 13-004). veraPDF asks only for the key.
    {
        static const RuleIndex alt_rule = R("EQX-UA1-7.3-1");
        const auto& F = ctx.st.elems();
        for (std::size_t i = 0; i < F.size(); ++i) {
            if (ty_[i] != "Figure" || cos::get(F[i].dict, "ActualText").isString()) continue;
            Obj alt = cos::get(F[i].dict, "Alt");
            if (!alt.isString()) continue;
            const std::u32string text = pdf_text_to_u32(alt.getStringValue());
            bool blank = !text.empty();
            for (char32_t c : text) blank = blank && (c == 0x20 || c == 0xA0 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x2003 || c == 0x2002 || c == 0x3000);
            if (blank) ctx.report(alt_rule, -1, F[i].ref, "PAC: Figure /Alt holds white space only");
        }
    }
    // FigureHasBBox: a Figure whose content lies on at most one page and that has no Layout /BBox fails
    // (structcheck fig_no_bbox; U1-S01, U1-S02, U1-S03). Pages of MCID, MCR and OBJR kids, also of descendants.
    static const RuleIndex rule = R("EQX-PAC-FailFigureHasBBox");
    const auto& E = ctx.st.elems();
    std::vector<PageSet> pages(E.size());
    for (std::size_t j = E.size(); j-- > 0;) {
        const auto& e = E[j];
        PageSet& ps = pages[j];
        for (const auto& k : e.kids) {
            if (k.kind == StructKid::Kind::Elem) continue;
            ps.add(k.page.valid() ? k.page.key() : 0);
        }
        for (int k : e.elems)
            for (int x = 0; x < pages[k].n; ++x) ps.add(pages[k].v[x]);
        if (ty_[j] == "Figure" && ps.n <= 1 && !layout_of(e.dict).bbox)
            ctx.report(rule, -1, e.ref, "Figure without Layout BBox");
    }
}

// ---------------------------------------------------------------------------------------- parent (/P) keys
void Module::parent_keys(ValidationContext& ctx) {
    // StructureElementHasParentKey2: /P names another object than the element's actual parent. A missing /P
    // is a different condition ("has no parent"), which PAC 26.1 did not report (U1-X01).
    static const RuleIndex rule = R("EQX-PAC-FailStructureElementHasParentKey2");
    const StructTree& st = ctx.st;
    const auto& E = st.elems();
    // Further parents of shared elements (a dictionary referenced from several /K entries).
    std::unordered_map<int, std::vector<int>> extra;
    auto note_shared = [&](const std::vector<StructKid>& kids, int parent) {
        for (const auto& k : kids)
            if (k.kind == StructKid::Kind::Elem && k.revisit && k.elem >= 0) extra[k.elem].push_back(parent);
    };
    note_shared(st.root_kids(), -1);
    for (std::size_t i = 0; i < E.size(); ++i) note_shared(E[i].kids, static_cast<int>(i));
    auto same = [&](const Obj& p, int parent) {
        Obj actual = parent >= 0 ? E[parent].dict : st.root();
        // A direct parent dictionary cannot be referenced by /P: undecidable, never reported.
        if (!actual.isIndirect()) return true;
        return p.isIndirect() && ref_of(p) == ref_of(actual);
    };
    for (std::size_t i = 0; i < E.size(); ++i) {
        Obj p = cos::get(E[i].dict, "P");
        if (p.isNull()) continue;
        // Also when another parent's /K lists the element: /P names only one of them, the other fails (d_ow_elem_two_parents).
        bool ok = same(p, E[i].parent);
        if (auto it = extra.find(static_cast<int>(i)); it != extra.end() && ok)
            for (int q : it->second)
                if (!same(p, q)) ok = false;
        if (ok) continue;
        std::string detail = display(st, ty_, static_cast<int>(i)) + ": /P ";
        detail += p.isIndirect() ? ref_of(p).str() : std::string("(direct object)");
        detail += ", actual parent " + display(st, ty_, E[i].parent);
        ObjRef ar = E[i].parent >= 0 ? E[E[i].parent].ref : st.root_ref();
        if (ar.valid()) detail += " " + ar.str();
        ctx.report(rule, -1, E[i].ref, std::move(detail));
    }
}

// ---------------------------------------------------------------------------------------- corrupt elements
void Module::corrupt_elements(ValidationContext& ctx) {
    // StructureHasCorruptElements: the dictionaries in /K of the StructTreeRoot and of every structure element.
    // PAC 26.1 aborts on every variant (probes p03, p05-p11), so these follow
    // the literal condition texts: a kid dictionary is a structure element (/S, /Type optional = StructElem),
    // a marked-content reference (/MCID, /Type MCR required) or an object reference (/Obj, /Type OBJR required).
    static const RuleIndex bad_elem_type = R("EQX-PAC-FailStructureHasCorruptElementsInvalidStructureElementType");
    static const RuleIndex no_s = R("EQX-PAC-FailStructureHasCorruptElementsStructureTypeMissing");
    static const RuleIndex mcr_no_type = R("EQX-PAC-FailStructureHasCorruptElementsMCRTypeMissing");
    static const RuleIndex bad_mcr_type = R("EQX-PAC-FailStructureHasCorruptElementsInvalidMCRType");
    static const RuleIndex no_mcid = R("EQX-PAC-FailStructureHasCorruptElementsMCIDMissing");
    static const RuleIndex objr_no_type = R("EQX-PAC-FailStructureHasCorruptElementsTypeOfOBJRMissing");
    static const RuleIndex bad_objr_type = R("EQX-PAC-FailStructureHasCorruptElementsInvalidOBJRType");
    static const RuleIndex no_obj = R("EQX-PAC-FailStructureHasCorruptElementsObjMissing");
    static const RuleIndex parsable = R("EQX-PAC-FailPDFParsable");
    const StructTree& st = ctx.st;
    // Kids PAC stops on (reported as an invalid kid, cr_se_k_null, cr_se_k_string, cr_se_k_page; a negative MCID, ow_mcid_negative) and entries of another type (/Alt, /Lang, cr_se_alt_name,
    // cr_se_lang_int). PAC closes such documents; a silent pass would hide them.
    for (const auto& e : st.elems())
        for (const char* key : {"Alt", "ActualText", "E", "Lang"}) {
            Obj v = cos::get(e.dict, key);
            if (!v.isNull() && !v.isString()) ctx.report(parsable, -1, e.ref, std::string("PAC: structure element /") + key + " is not a string (PAC closes the document)");
        }
    auto scan = [&](const Obj& node, ObjRef owner) {
        Obj k = cos::get(node, "K");
        if (k.isNull()) return;
        std::vector<Obj> items;
        if (k.isArray()) items = cos::array_items(k);
        else items.push_back(k);
        for (auto& it : items) {
            if (!it.isDictionary()) {
                if (it.isInteger()) {
                    if (it.getIntValue() < 0) ctx.report(parsable, -1, owner, "PAC: negative MCID in /K (PAC closes the document)");
                } else if (k.isArray()) {
                    ctx.report(parsable, -1, owner, "PAC: a /K entry is neither a dictionary nor an integer (PAC closes the document)");
                }
                continue;
            }
            ObjRef where = it.isIndirect() ? ref_of(it) : owner;
            auto type = cos::name(it, "Type");
            bool has_s = cos::has(it, "S"), has_mcid = cos::has(it, "MCID"), has_obj = cos::has(it, "Obj");
            bool s_ok = cos::name(it, "S").has_value();
            if (!type) {
                if (has_s) {
                    if (!s_ok) ctx.report(no_s, -1, where, "/S is not a name");
                } else if (has_mcid) {
                    ctx.report(mcr_no_type, -1, where, "marked-content reference without /Type");
                    if (!cos::integer(it, "MCID")) ctx.report(no_mcid, -1, where, "/MCID is not an integer");
                } else if (has_obj) {
                    ctx.report(objr_no_type, -1, where, "object reference without /Type");
                } else {
                    ctx.report(no_s, -1, where, "structure element without /S");
                }
            } else if (*type == "StructElem") {
                if (!s_ok) ctx.report(no_s, -1, where, has_s ? "/S is not a name" : "structure element without /S");
            } else if (*type == "MCR") {
                if (!cos::integer(it, "MCID")) ctx.report(no_mcid, -1, where, "MCR without integer /MCID");
            } else if (*type == "OBJR") {
                if (cos::get(it, "Obj").isNull()) ctx.report(no_obj, -1, where, "OBJR without /Obj");
            } else if (has_mcid) {
                ctx.report(bad_mcr_type, -1, where, "marked-content reference with /Type /" + *type);
            } else if (has_obj) {
                ctx.report(bad_objr_type, -1, where, "object reference with /Type /" + *type);
            } else {
                ctx.report(bad_elem_type, -1, where, "structure element with /Type /" + *type);
            }
        }
    };
    scan(st.root(), st.root_ref());
    for (const auto& e : st.elems()) scan(e.dict, e.ref);
}

// ------------------------------------------------------------------------------------------- parent tree
void Module::parent_tree(ValidationContext& ctx) {
    static const RuleIndex missing = R("EQX-PAC-FailStructuralParentTreeMissing");
    static const RuleIndex page_no_sp = R("EQX-PAC-FailStructuralParentTreePageHasNoStructPatents");
    static const RuleIndex annot_no_sp = R("EQX-PAC-FailStructuralParentTreeAnnotationHasNoStructPatent");
    static const RuleIndex entry_missing = R("EQX-PAC-FailStructuralParentTreeEntryMissing");
    static const RuleIndex mcid_missing = R("EQX-PAC-FailStructuralParentTreeEntryMissingMCID");
    static const RuleIndex invalid_entry = R("EQX-PAC-FailStructuralParentTreeInvalidEntry");
    static const RuleIndex inconsistent = R("EQX-PAC-FailStructuralParentTreeInvalidReference");
    const StructTree& st = ctx.st;
    const auto& E = st.elems();
    const ContentModel& cm = ctx.content;
    const auto& pages = ctx.doc.pages();

    bool content_items = false;
    for (const auto& k : st.root_kids())
        if (k.kind != StructKid::Kind::Elem) content_items = true;
    for (const auto& e : E)
        for (const auto& k : e.kids)
            if (k.kind != StructKid::Kind::Elem) content_items = true;
    // Not applicable without content items and without a ParentTree (PAC release notes 24.4.3); a missing
    // ParentTree is the only condition reported then (U1-X03).
    if (!st.has_parent_tree()) {
        if (content_items) ctx.report(missing, -1, st.root_ref(), "StructTreeRoot without /ParentTree");
        return;
    }
    if (cm.budget_exceeded)
        for (RuleIndex r : {page_no_sp, entry_missing, mcid_missing, invalid_entry, inconsistent})
            ctx.mark_incomplete(r, "content interpretation stopped at the operator budget");

    std::set<int> pages_flagged;
    std::set<std::int64_t> keys_flagged;
    std::set<std::pair<std::int64_t, std::int64_t>> seen, reported, mcid_flagged;
    for (const auto& f : cm.frames) {
        if (!f.mcid || *f.mcid < 0 || f.stream < 0) continue;
        const StreamInstance& s = cm.streams[f.stream];
        if (!s.semantic) continue;
        bool page_stream = s.kind == StreamInstance::Kind::Page;
        bool own_form = s.kind == StreamInstance::Kind::Form && s.struct_parents.has_value();
        if (!page_stream && !own_form) continue;   // MCIDs of forms without own /StructParents: not modelled
        int pi = s.page;
        ObjRef owner = page_stream ? (pi >= 0 ? pages[pi].ref : ObjRef{}) : s.ref;
        if (!s.struct_parents) {
            if (pi >= 0 && pages_flagged.insert(pi).second)
                ctx.report(page_no_sp, pi, owner, "page with MCIDs but without /StructParents");
            continue;
        }
        std::int64_t key = *s.struct_parents;
        if (!seen.insert({key, *f.mcid}).second) continue;
        Obj entry = st.parent_tree_entry(key);
        if (entry.isNull()) {
            if (keys_flagged.insert(key).second)
                ctx.report(entry_missing, pi, owner, "no ParentTree entry for StructParents " + std::to_string(key));
            continue;
        }
        if (!entry.isArray()) {
            if (keys_flagged.insert(key).second)
                ctx.report(invalid_entry, pi, owner, "ParentTree entry " + std::to_string(key) + " is not an array");
            continue;
        }
        std::string where = "StructParents " + std::to_string(key) + ", MCID " + std::to_string(*f.mcid);
        // An MCID beyond the array with no element claiming it passes PAC (ct_orphan_mcid, ct_artifact_with_mcid): only the element side counts, below.
        if (*f.mcid >= entry.getArrayNItems()) continue;
        Obj item = entry.getArrayItem(static_cast<int>(*f.mcid));
        // A null entry is not this condition for PAC (probe st_pt_null: only the content is "not tagged"); an array too short is
        // (st_pt_short).
        if (item.isNull()) continue;
        if (!item.isDictionary()) {
            ctx.report(invalid_entry, pi, owner, where + ": entry is not a structure element");
            continue;
        }
        // PAC reads consistency from the element's side only: an entry naming an element that does not list the MCID passes when no element claims it otherwise
        // (d_oe_entry_elem_without_kid, d_oe_entry_other_elem_no_claim; ow_mcr_wrong_pg).
    }

    // An integer MCID needs /Pg on its element or an ancestor: PAC refuses the file as corrupted (ow_int_mcid_no_pg).
    {
        static const RuleIndex parsable = R("EQX-PAC-FailPDFParsable");
        for (std::size_t i = 0; i < E.size(); ++i)
            for (const auto& k : E[i].kids)
                if (k.kind == StructKid::Kind::MCID && !k.page.valid()) {
                    ctx.report(parsable, -1, E[i].ref, "PAC: MCID without a page: no /Pg on the element or its ancestors (PAC refuses the file)");
                    break;
                }
    }
    // The other direction: every MCID kid of an element against the ParentTree entry of its page. PAC: StructuralParentTreeInvalidReference for each kid whose entry names another element (two
    // elements claim one sequence: two real-world samples, 4 and 21 kids, measured exactly; both invisible before,
    // the content side sees a consistent owner).
    for (std::size_t i = 0; i < E.size(); ++i) {
        if (!E[i].ref.valid()) continue;
        for (const auto& k : E[i].kids) {
            if ((k.kind != StructKid::Kind::MCID && k.kind != StructKid::Kind::MCR) || k.mcid < 0 || k.stm.valid() || !k.page.valid()) continue;
            const int pi = ctx.doc.page_index(k.page);
            if (pi < 0) continue;
            auto sp = cos::integer(pages[pi].dict, "StructParents");
            if (!sp) continue;
            Obj entry = st.parent_tree_entry(*sp);
            if (!entry.isArray()) continue;
            if (k.mcid >= entry.getArrayNItems()) {
                // an element names an MCID the page's array does not reach (wq_p_mcr_missing: StructuralParentTreeEntryMissingMCID)
                if (mcid_flagged.insert({*sp, k.mcid}).second)
                    ctx.report(mcid_missing, pi, E[i].ref, "StructParents " + std::to_string(*sp) + ", MCID " + std::to_string(k.mcid) + " named by " + E[i].ref.str() + ": array too short");
                continue;
            }
            Obj item = entry.getArrayItem(static_cast<int>(k.mcid));
            if (!item.isDictionary() || !item.isIndirect() || ref_of(item) == E[i].ref) continue;
            if (!reported.insert({*sp, k.mcid}).second) continue;
            ctx.report(inconsistent, pi, E[i].ref, "MCID " + std::to_string(k.mcid) + " of " + E[i].ref.str() + ": the ParentTree entry names " + ref_of(item).str());
        }
    }

    // Annotations referenced by an OBJR: /StructParent required, its entry must be the referencing element.
    auto check_objr = [&](const StructKid& k, int holder) {
        Obj a = k.obj;
        if (!a.isDictionary() || a.isStream()) return;   // form XObjects are not annotations
        if (!cos::name_is(a, "Type", "Annot") && !cos::name(a, "Subtype")) return;
        int pi = k.page.valid() ? ctx.doc.page_index(k.page) : -1;
        if (pi < 0) pi = ctx.doc.page_index(ref_of(cos::get(a, "P")));
        auto sp = cos::integer(a, "StructParent");
        if (!sp) {
            ctx.report(annot_no_sp, pi, ref_of(a), "annotation in the structure tree without /StructParent");
            return;
        }
        Obj entry = st.parent_tree_entry(*sp);
        if (entry.isNull()) {
            // StructuralParentTreeEntryMissing, for annotations too (an_link_structparent_noentry, detail read-out)
            if (keys_flagged.insert(*sp).second)
                ctx.report(entry_missing, pi, ref_of(a), "no ParentTree entry for StructParent " + std::to_string(*sp));
            return;
        }
        if (!entry.isDictionary()) {
            ctx.report(invalid_entry, pi, ref_of(a), "ParentTree entry " + std::to_string(*sp) + " is not a structure element");
            return;
        }
        if (holder < 0 || !entry.isIndirect() || !E[holder].ref.valid()) return;
        if (ref_of(entry) != E[holder].ref)
            ctx.report(inconsistent, pi, ref_of(a),
                       "StructParent " + std::to_string(*sp) + " names " + ref_of(entry).str() + ", OBJR is in " +
                           E[holder].ref.str());
    };
    for (std::size_t i = 0; i < E.size(); ++i)
        for (const auto& k : E[i].kids)
            if (k.kind == StructKid::Kind::OBJR) check_objr(k, static_cast<int>(i));

    // A Link annotation whose Link element does not lead to the StructTreeRoot through /P is "not nested" for PAC
    // (a real-world sample: P > Link, the P without /P; lk_orphan: the P with /P, only missing from the
    // kids of its parent, passes). veraPDF's structParentStandardType reads only the entry (annotations module).
    static const RuleIndex link_nested = R("EQX-UA1-7.18.5-1");
    for (std::size_t pi = 0; pi < pages.size(); ++pi)
        for (const Obj& a : cos::array_items(cos::get(pages[pi].dict, "Annots"))) {
            if (!cos::name_is(a, "Subtype", "Link")) continue;
            auto sp = cos::integer(a, "StructParent");
            if (!sp) continue;
            Obj entry = st.parent_tree_entry(*sp);
            if (!entry.isDictionary() || !cos::name_is(entry, "S", "Link")) continue;
            Obj e = entry;
            bool rooted = false;
            for (int guard = 0; e.isDictionary() && guard < 4096 && !rooted; ++guard) {
                Obj up = cos::get(e, "P");
                if (up.isDictionary() && ref_of(up) == st.root_ref()) rooted = true;
                e = up;
            }
            if (!rooted) ctx.report(link_nested, int(pi), ref_of(a), "Link annotation: its Link element " + ref_of(entry).str() + " does not lead to the StructTreeRoot");
        }
}

}  // namespace

std::unique_ptr<RuleModule> make_pac_structure_module() { return std::make_unique<Module>(); }

}  // namespace eqx
