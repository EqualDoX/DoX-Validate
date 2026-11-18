#include "rules/pac_structure/pac_model.h"

#include <array>
#include <initializer_list>

namespace eqx::pac {

namespace {

bool in(std::string_view t, std::initializer_list<std::string_view> set) {
    for (auto s : set)
        if (s == t) return true;
    return false;
}

}  // namespace

bool is_pdf17_type(std::string_view t) {
    return in(t, {"Document", "Part", "Art", "Sect", "Div", "BlockQuote", "Caption", "TOC", "TOCI", "Index",
                  "NonStruct", "Private", "H", "H1", "H2", "H3", "H4", "H5", "H6", "P", "L", "LI", "Lbl", "LBody",
                  "Table", "TR", "TH", "TD", "THead", "TBody", "TFoot", "Span", "Quote", "Note", "Reference",
                  "BibEntry", "Code", "Link", "Annot", "Ruby", "RB", "RT", "RP", "Warichu", "WT", "WP", "Figure",
                  "Formula", "Form"});
}

bool is_hn(std::string_view t) { return t.size() == 2 && t[0] == 'H' && t[1] >= '1' && t[1] <= '6'; }
bool is_heading(std::string_view t) { return t == "H" || is_hn(t); }
int heading_level(std::string_view t) { return is_hn(t) ? t[1] - '0' : t == "H" ? 0 : -1; }

// ISO 32000-1 Table 338 inline-level elements plus Form (PAC treats Form as inline in every placement;
// verified: Form directly in Sect warns in U1-A01 / U1-D04 / U1-S01, Form > Form passes in U1-S01).
bool is_inline(std::string_view t) {
    return in(t, {"Span", "Quote", "Note", "Reference", "BibEntry", "Code", "Link", "Annot", "Ruby", "Warichu", "Form"});
}

// Grouping elements whose kids PAC expects to be block-level. Caption and BlockQuote are grouping for PAC
// (variants of 2026-10-24: Span / Link / inline Formula directly in them warn).
bool is_grouping(std::string_view t) { return in(t, {"Document", "Part", "Art", "Sect", "Div", "Caption", "BlockQuote"}); }

bool is_paragraph_like(std::string_view t) { return t == "P" || t == "Span" || is_heading(t); }

bool is_transparent(std::string_view t) { return t == "NonStruct" || t == "Private"; }

namespace {

// Block-level element parents that hold content (ISO 32000-1 14.8.4.3: paragraph-like and list elements): a
// grouping element directly inside one of them warns. Not TD / TH (table elements; PAC spot check: two Sect
// (role-mapped Textbox) directly in a TD of a real-world sample are not reported) and not TOCI / Caption /
// BlockQuote (grouping elements in 14.8.4.2; an earlier PAC model, 84 of 84 documents, warns only for
// paragraph-like parents). No fixture exercises this condition. Not LBody either (PAC read-out of
// a real-world sample: 20 Sect (role-mapped InlineShape) directly in
// LBody, all 22 Sect passed); Lbl stays (literal reading, no PAC evidence).
bool is_blse(std::string_view t) {
    return t == "P" || is_heading(t) || t == "Lbl";
}

// Block-level content that must not stand directly inside a paragraph-like parent (P > P, P > Table, H1 > P,
// Span > Sect, ... warn: variants of 2026-10-24 and U1-S01's 31 Span > X placements).
bool is_block_level(std::string_view t) {
    return (is_grouping(t) && t != "Document") || t == "P" || is_heading(t) || in(t, {"L", "Table", "TOC", "Index"});
}

// Grouping context of an inline element / illustration (the StructTreeRoot included).
bool is_grouping_context(std::string_view t) { return t == kRoot || is_grouping(t); }

}  // namespace

// Parents with a fixed content model: any other kid is "Invalid use" (fail).
//   TR      probe p22 (28 types each directly in a TR beside a TD: every type failed, U1-S04)
//   L       probe p23 (28 types directly in an L); U1-S01 L > Note / Reference / Figure / Formula / P failed.
//           An L directly in an L passes (Phase 3 PAC probes: L(L(LI, LI)), L(LI, L(LI), LI) and L(LI, L with
//           its own text, which fails MarkedContentIsInLegalPosition only); real-world: 3 nested L of
//           a sample document, 92 L passed)
//   Table, THead, TBody, TFoot, TOC: U1-S01 P directly in each failed (7 of the 8 P placements; P in LI passed)
//   Ruby, Warichu: ISO 32000-1 Tables 339/340 (no PAC evidence; literal reading)
bool is_strict_container(std::string_view t) {
    return in(t, {"TR", "L", "Table", "THead", "TBody", "TFoot", "TOC", "Ruby", "Warichu"});
}

bool strict_allows(std::string_view parent, std::string_view kid) {
    if (parent == "TR") return kid == "TH" || kid == "TD";
    if (parent == "L") return kid == "LI" || kid == "Caption" || kid == "L";
    if (parent == "Table") return in(kid, {"TR", "THead", "TBody", "TFoot", "Caption"});
    if (parent == "THead" || parent == "TBody" || parent == "TFoot") return kid == "TR";
    if (parent == "TOC") return kid == "TOCI" || kid == "TOC" || kid == "Caption";
    if (parent == "Ruby") return in(kid, {"RB", "RT", "RP"});
    if (parent == "Warichu") return kid == "WT" || kid == "WP";
    return true;
}

// Types that may only stand in specific parents (U1-S01: misplaced TOCI, LI, LBody, TR, TH, TD, THead, TBody,
// TFoot, RB, RT, RP, WT, WP fail; U1-S03: each of them as a root element fails instead of the root warning).
bool has_fixed_parent(std::string_view t) {
    return in(t, {"TOCI", "LI", "LBody", "TR", "TH", "TD", "THead", "TBody", "TFoot", "RB", "RT", "RP", "WT", "WP"});
}

bool fixed_parent_allows(std::string_view t, std::string_view parent) {
    if (t == "TOCI") return parent == "TOC";
    if (t == "LI") return parent == "L";
    if (t == "LBody") return parent == "LI";
    if (t == "TR") return in(parent, {"Table", "THead", "TBody", "TFoot"});
    if (t == "TH" || t == "TD") return parent == "TR";
    if (t == "THead" || t == "TBody" || t == "TFoot") return parent == "Table";
    if (t == "RB" || t == "RT" || t == "RP") return parent == "Ruby";
    if (t == "WT" || t == "WP") return parent == "Warichu";
    return true;
}

Outcome classify(const Context& c) {
    const std::string_view t = c.type;
    if (t.empty() || t == "NonStruct") return {};   // PAC 26.1 has no NonStruct check
    // Root elements (kids of the StructTreeRoot). U1-S03: every type except Document and Private warns
    // "used as root element"; the types with a fixed parent fail instead.
    if (c.parent == kRoot) {
        if (t == "Document" || t == "Private") return {};
        if (has_fixed_parent(t)) return {Cond::Invalid, "fixed parent (root element)"};
        return {Cond::AsRoot, "root element"};
    }
    // A Document below the root warns (variants 2026-10-24; U1-S01 Sect > Document).
    if (t == "Document") return {Cond::DocumentNotRoot, "nested Document"};
    // --- fail conditions ------------------------------------------------------------------------------------
    // A Private in a parent with a fixed content model fails itself (no such parent allows it); what stands directly
    // in the Private is its private content and is not failed a second time against that parent (PAC read-out of
    // TR(TD, TD, Private(P)): PrivateTag fails once, PTag passes). NonStruct has no check of its own and is looked
    // through as before.
    const std::string_view container = c.parent == "Private" ? c.parent : c.eff_parent;
    if (is_strict_container(container) && !strict_allows(container, t))
        return {Cond::Invalid, "not allowed in this parent"};
    if (has_fixed_parent(t) && !fixed_parent_allows(t, c.eff_parent)) return {Cond::Invalid, "wrong parent"};
    // A TR in a THead/TBody/TFoot that itself is not in a Table fails (U1-S01: the TRs of Sect > THead / TBody /
    // TFoot and Span > THead / TBody / TFoot fail, 9 of 24 TRs); a THead/TBody/TFoot directly under the
    // StructTreeRoot keeps its TR valid (U1-S03: 4 of 5 TRs pass).
    if (t == "TR" && (c.eff_parent == "THead" || c.eff_parent == "TBody" || c.eff_parent == "TFoot") &&
        c.eff_grandparent != "Table" && c.eff_grandparent != kRoot)
        return {Cond::Invalid, "row group outside a Table"};
    // Rows beside an element their Table or row group does not allow fail too, the parent's own kids only (probes tb_*:
    // Table(TR, Span, TR) and TBody(TR, Span, TR) 3 failures, Table(TBody(TR, TR), Span) and Table(TR(TH, Span, TD), TR) 1;
    // a real-world sample: 14 TR beside one Span).
    if (t == "TR" && c.bad_kids > 0) return {Cond::Invalid, "row beside an element its parent does not allow"};
    if (t == "Caption") {
        // U1-S01: a Caption in the middle of a Table, both Captions of a Table with two, a Caption after the
        // first kid of an L or a TOC fail (veraPDF ua1 7.2-16 / 7.2-39 / 7.2-40 / 7.2-28 semantics).
        if (c.eff_parent == "Table" && (c.captions > 1 || (c.index != 0 && c.index != c.count - 1)))
            return {Cond::Invalid, "Caption position in Table"};
        if ((c.eff_parent == "L" || c.eff_parent == "TOC") && c.index != 0)
            return {Cond::Invalid, "Caption not the first kid"};
    }
    // U1-S01 Table(THead, THead, TBody) / Table(TBody, TFoot, TFoot): 5 of 6 THeads and 5 of 6 TFoots fail;
    // read as "every THead (TFoot) of a Table with more than one" (veraPDF ua1 7.2-11 / 7.2-12 semantics).
    if (t == "THead" && c.eff_parent == "Table" && c.theads > 1) return {Cond::Invalid, "more than one THead"};
    if (t == "TFoot" && c.eff_parent == "Table" && c.tfoots > 1) return {Cond::Invalid, "more than one TFoot"};
    // Order and mix of a Table's kids, ISO 32000-1 Table 337 (detail read-outs d_tb_tbody_before_thead: THead fails, TBody warns; d_tb_tfoot_before_tbody: both fail;
    // d_tb_tr_and_tbody_mixed: TR and TBody fail).
    if (c.eff_parent == "Table") {
        if ((t == "TR" && c.table_has_group) || ((t == "THead" || t == "TBody" || t == "TFoot") && c.table_has_row))
            return {Cond::Invalid, "rows beside row groups in a Table"};
        if (t == "THead" && !c.group_first) return {Cond::Invalid, "THead is not the first row group"};
        if (t == "TFoot" && !c.group_last) return {Cond::Invalid, "TFoot is not the last row group"};
        if (t == "TBody" && c.tfoots_before > 0) return {Cond::Invalid, "TBody after TFoot"};
    }
    // CheckWarnTBodyTagInvalidSyntax: a TBody of a Table without THead. Evidence: PAC read-outs of the real-world
    // spot check (one sample: one Table(TBody) -> warned 1; another: two Table(TBody) ->
    // warned 2; four other files without such tables -> none) and U1-S01 (TBodies 23 / 210 / 128 misplaced ->
    // the 3 failures; Table(TBody, TFoot, TFoot) -> the warning; Table(THead, THead, TBody) -> the pass).
    // Only the first TBody of the Table, without a THead before it (tb_two_tbody: Table(TBody, TBody) warns once; d_tb_tbody_before_thead).
    if (t == "TBody" && c.eff_parent == "Table" && (c.theads_before < 0 ? c.theads == 0 : c.theads_before == 0 && c.tbodies_before == 0))
        return {Cond::TBodyWarn, "TBody in a Table without THead"};
    // --- warning conditions (InappropriateUse) ----------------------------------------------------
    // An inline-level element with Placement /Block counts as a block (a real-world sample, Link in Part;
    // probes pl_*: Link, Code, Form, ... with /Block pass, /Before, /End and no Placement warn).
    if (is_inline(t) && is_grouping_context(c.eff_parent) && c.placement != "Block")
        return {Cond::Inappropriate, "inline element in grouping element"};
    // NonStruct is looked through here, Private is not (gn_h3_nonstruct_h3 warns, gn_h3_private_h3 passes; a real-world sample).
    const std::string_view through = c.parent == "Private" ? c.parent : c.eff_parent;
    if (is_grouping(t) && is_blse(through)) return {Cond::Inappropriate, "grouping element in block-level element"};
    if (is_block_level(t) && is_paragraph_like(through)) return {Cond::Inappropriate, "block-level element in paragraph-like element"};
    // Part belongs directly under Document: gn_{Part,Art,Sect,Div}_Part warn, every other pair of these passes (a real-world sample).
    if (t == "Part" && in(through, {"Part", "Art", "Sect", "Div"})) return {Cond::Inappropriate, "Part in a grouping element"};
    // Layout Placement against the context: an illustration in a grouping context needs exactly Block; nothing
    // laid out as a Block may stand in a paragraph-like parent (Before/Start/End pass; TD/TH/LBody/TOCI accept
    // Block).
    if (t == "Figure" || t == "Formula" || is_inline(t)) {
        if ((t == "Figure" || t == "Formula") && is_grouping_context(c.eff_parent)) {
            if (!c.placement || *c.placement != "Block") return {Cond::Inappropriate, "illustration without Placement Block in grouping element"};
        } else if ((is_paragraph_like(c.eff_parent) || c.eff_parent == "Link") && c.placement && *c.placement == "Block") {  // gl_link_figure_*
            return {Cond::Inappropriate, "Placement Block in paragraph-like element"};
        }
    }
    return {};
}

ContentPos own_content(std::string_view t) {
    // U1-S04: own marked content of Div / Sect / Part / Art warns (4), of L / Table / TOC fails (3); Ruby,
    // Warichu, Index, BlockQuote, Caption and Private with own content pass. U1-C01: Document warns.
    // TR / THead / TBody / TFoot hold only rows / cells (literal ISO 32000-1 reading, no PAC evidence).
    if (in(t, {"Document", "Part", "Art", "Sect", "Div"})) return ContentPos::Warn;
    if (in(t, {"L", "Table", "TOC", "TR", "THead", "TBody", "TFoot"})) return ContentPos::Fail;
    return ContentPos::Ok;
}

std::string rule_id(std::string_view type, Cond cond) {
    std::string t(type);
    switch (cond) {
        case Cond::Invalid: return "EQX-PAC-Fail" + t + "TagInvalidSyntax";
        case Cond::Inappropriate: return "EQX-PAC-Warn" + t + "InappropriateUse";
        case Cond::AsRoot: return "EQX-PAC-Warn" + t + "TagAsRoot";
        case Cond::DocumentNotRoot: return "EQX-PAC-FailDocumentTagIsRoot";
        case Cond::TBodyWarn: return "EQX-PAC-WarnTBodyTagInvalidSyntax";
        case Cond::None: break;
    }
    return {};
}

}  // namespace eqx::pac
