// Unit tests of the PAC 26.1 structure model (pure functions over parent/child types).
#include "rules/pac_structure/pac_model.h"
#include "unit.h"

using namespace eqx::pac;

namespace {

Cond cls(std::string_view type, std::string_view parent, std::string_view eff = {}, std::string_view grand = kRoot) {
    Context c;
    c.type = type;
    c.parent = parent;
    c.eff_parent = eff.empty() ? parent : eff;
    c.eff_grandparent = grand;
    return classify(c).cond;
}

Cond cls_pos(std::string_view type, std::string_view parent, int index, int count, int captions = 0, int theads = 0,
             int tfoots = 0) {
    Context c;
    c.type = type;
    c.parent = c.eff_parent = parent;
    c.eff_grandparent = "Sect";
    c.index = index;
    c.count = count;
    c.captions = captions;
    c.theads = theads;
    c.tfoots = tfoots;
    return classify(c).cond;
}

Cond cls_place(std::string_view type, std::string_view parent, std::optional<std::string_view> placement) {
    Context c;
    c.type = type;
    c.parent = c.eff_parent = parent;
    c.eff_grandparent = "Document";
    c.placement = placement;
    return classify(c).cond;
}

}  // namespace

EQX_TEST(pac_structure_root_elements) {
    // U1-S03: Document and Private pass, fixed-parent types fail, everything else warns as a root element.
    EXPECT(cls("Document", kRoot) == Cond::None);
    EXPECT(cls("Private", kRoot) == Cond::None);
    for (auto t : {"Part", "Art", "Sect", "Div", "BlockQuote", "Caption", "TOC", "Index", "H", "H1", "H6", "P", "L",
                   "Lbl", "Table", "Span", "Quote", "Note", "Reference", "BibEntry", "Code", "Link", "Annot", "Ruby",
                   "Warichu", "Figure", "Formula", "Form"})
        EXPECT(cls(t, kRoot) == Cond::AsRoot);
    for (auto t : {"TOCI", "LI", "LBody", "TR", "TH", "TD", "THead", "TBody", "TFoot", "RB", "RT", "RP", "WT", "WP"})
        EXPECT(cls(t, kRoot) == Cond::Invalid);
    EXPECT(cls("Document", "Sect") == Cond::DocumentNotRoot);
    EXPECT(cls("Document", "Document") == Cond::DocumentNotRoot);
}

EQX_TEST(pac_structure_strict_containers) {
    // Probe p22 / U1-S04: every type other than TH / TD directly in a TR fails; probe p23: the same in an L,
    // except an L itself (Phase 3 PAC probes: L directly in L passes as only kid, beside LIs and with own text).
    for (auto t : {"H", "H1", "Div", "Sect", "Part", "Art", "Span", "L", "Lbl", "Link", "Note", "Quote", "Ruby",
                   "Warichu", "TOC", "Table", "Index", "BibEntry", "Code", "Form", "Private", "BlockQuote", "Annot"}) {
        EXPECT(cls(t, "TR") == Cond::Invalid);
        if (std::string_view(t) != "L") EXPECT(cls(t, "L") == Cond::Invalid);
    }
    EXPECT(cls("L", "L") == Cond::None);
    EXPECT(cls("TD", "TR") == Cond::None);
    EXPECT(cls("TH", "TR") == Cond::None);
    EXPECT(cls("LI", "L") == Cond::None);
    // U1-S01: P in Table, TR, L, TOC, THead, TBody, TFoot fails; P in LI and in a nested Document passes.
    for (auto p : {"Table", "TR", "L", "TOC", "THead", "TBody", "TFoot"}) EXPECT(cls("P", p) == Cond::Invalid);
    EXPECT(cls("P", "LI") == Cond::None);
    EXPECT(cls("P", "Document") == Cond::None);
    EXPECT(cls("P", "Span") == Cond::Inappropriate);
    // NonStruct is looked through: TD in TR > NonStruct.
    EXPECT(cls("TD", "NonStruct", "TR") == Cond::None);
    EXPECT(cls("NonStruct", "TR") == Cond::None);
}

EQX_TEST(pac_structure_private_content) {
    // TR(TD, TD, Private(P)): PAC fails the Private once and not the P inside it. The Private is the misplaced
    // element in every parent with a fixed content model; its direct content is not failed against that parent again.
    for (auto parent : {"TR", "L", "Table", "THead", "TBody", "TFoot", "TOC"}) {
        EXPECT(cls("Private", parent) == Cond::Invalid);
        for (auto kid : {"P", "H1", "Span", "Figure", "Sect", "L", "Table"}) EXPECT(cls(kid, "Private", parent) == Cond::None);
    }
    // The same element directly in the parent, or under a NonStruct (which has no check of its own), still fails.
    EXPECT(cls("P", "TR") == Cond::Invalid);
    EXPECT(cls("P", "NonStruct", "TR") == Cond::Invalid);
    EXPECT(cls("Span", "NonStruct", "L") == Cond::Invalid);
    // Types with a fixed parent are judged by where the Private stands, as before.
    EXPECT(cls("TD", "Private", "TR") == Cond::None);
    EXPECT(cls("TD", "Private", "Table") == Cond::Invalid);
    EXPECT(cls("LI", "Private", "L") == Cond::None);
    EXPECT(cls("LI", "Private", "TR") == Cond::Invalid);
    EXPECT(cls("TR", "Private", "Table", "Sect") == Cond::None);
}

EQX_TEST(pac_structure_fixed_parents) {
    EXPECT(cls("TOCI", "Sect") == Cond::Invalid);
    EXPECT(cls("TOCI", "TOC") == Cond::None);
    EXPECT(cls("LI", "Span") == Cond::Invalid);
    EXPECT(cls("LBody", "Sect") == Cond::Invalid);
    EXPECT(cls("LBody", "LI", "LI", "Sect") == Cond::None);   // a misplaced LI keeps its LBody valid
    EXPECT(cls("TD", "Sect") == Cond::Invalid);
    EXPECT(cls("TD", "TR", "TR", "Sect") == Cond::None);      // a misplaced TR keeps its TD valid
    EXPECT(cls("RB", "Span") == Cond::Invalid);
    EXPECT(cls("RB", "Ruby") == Cond::None);
    EXPECT(cls("WP", "Warichu") == Cond::None);
    // TR: in a row group that is not in a Table fails (U1-S01), unless the row group is a root element (U1-S03).
    EXPECT(cls("TR", "Table", "Table", "Sect") == Cond::None);
    EXPECT(cls("TR", "THead", "THead", "Table") == Cond::None);
    EXPECT(cls("TR", "THead", "THead", "Sect") == Cond::Invalid);
    EXPECT(cls("TR", "TBody", "TBody", "Span") == Cond::Invalid);
    EXPECT(cls("TR", "TFoot", "TFoot", kRoot) == Cond::None);
    EXPECT(cls("TR", "Sect") == Cond::Invalid);
}

EQX_TEST(pac_structure_table_sections_and_captions) {
    // U1-S01 Table(THead, THead, TBody): both THeads fail, the TBody passes; Table(THead, TR) passes.
    EXPECT(cls_pos("THead", "Table", 0, 3, 0, 2) == Cond::Invalid);
    EXPECT(cls_pos("THead", "Table", 1, 3, 0, 2) == Cond::Invalid);
    EXPECT(cls_pos("TBody", "Table", 2, 3, 0, 2) == Cond::None);
    EXPECT(cls_pos("THead", "Table", 0, 2, 0, 1) == Cond::None);
    // Table(TBody, TFoot, TFoot): both TFoots fail, the TBody (no THead) warns; Table(TR, TFoot) passes;
    // real-world Table(TBody) warns as well (PAC spot check).
    EXPECT(cls_pos("TFoot", "Table", 1, 3, 0, 0, 2) == Cond::Invalid);
    EXPECT(cls_pos("TBody", "Table", 0, 3, 0, 0, 2) == Cond::TBodyWarn);
    EXPECT(cls_pos("TBody", "Table", 0, 1, 0, 0, 0) == Cond::TBodyWarn);
    EXPECT(cls_pos("TFoot", "Table", 1, 2, 0, 0, 1) == Cond::None);
    // Captions: Table first or last and only one; L and TOC first only.
    EXPECT(cls_pos("Caption", "Table", 0, 3, 1) == Cond::None);
    EXPECT(cls_pos("Caption", "Table", 2, 3, 1) == Cond::None);
    EXPECT(cls_pos("Caption", "Table", 1, 3, 1) == Cond::Invalid);
    EXPECT(cls_pos("Caption", "Table", 0, 3, 2) == Cond::Invalid);
    EXPECT(cls_pos("Caption", "L", 0, 2, 1) == Cond::None);
    EXPECT(cls_pos("Caption", "L", 1, 2, 1) == Cond::Invalid);
    EXPECT(cls_pos("Caption", "TOC", 1, 2, 1) == Cond::Invalid);
    EXPECT(cls("Caption", "Span") == Cond::Inappropriate);
    EXPECT(cls("Caption", "Sect") == Cond::None);
}

EQX_TEST(pac_structure_content_model_warnings) {
    // Inline-level elements directly in grouping elements (NonStruct / Private looked through).
    for (auto g : {"Document", "Part", "Art", "Sect", "Div", "Caption", "BlockQuote"})
        for (auto t : {"Span", "Link", "Note", "Annot", "Form", "Quote", "Code", "Reference", "BibEntry", "Ruby", "Warichu"})
            EXPECT(cls(t, g) == Cond::Inappropriate);
    EXPECT(cls("Link", "NonStruct", "Document") == Cond::Inappropriate);
    for (auto p : {"P", "TOCI", "LBody", "TD", "TH", "Lbl", "Span", "H1"}) EXPECT(cls("Link", p) == Cond::None);
    EXPECT(cls("Form", "Form") == Cond::None);
    EXPECT(cls("Code", "BibEntry") == Cond::None);
    // Grouping elements in block-level elements, block-level elements in paragraph-like ones; a grouping element
    // in a table cell passes (PAC spot check: Sect in TD not reported), in a list body too (Phase 3 PAC read-out:
    // 20 Sect directly in LBody passed).
    EXPECT(cls("Div", "Lbl") == Cond::Inappropriate);
    EXPECT(cls("Sect", "LBody") == Cond::None);
    EXPECT(cls("Div", "LBody") == Cond::None);
    EXPECT(cls("Sect", "TD") == Cond::None);
    EXPECT(cls("Sect", "P") == Cond::Inappropriate);
    for (auto t : {"Part", "Art", "Sect", "Div", "BlockQuote", "Caption", "TOC", "Index", "H", "H3", "P", "L", "Table"})
        EXPECT(cls(t, "Span") == Cond::Inappropriate);
    EXPECT(cls("P", "P") == Cond::Inappropriate);
    EXPECT(cls("P", "H1") == Cond::Inappropriate);
    EXPECT(cls("Table", "Div") == Cond::None);
    EXPECT(cls("L", "Div") == Cond::None);
    EXPECT(cls("Span", "P") == Cond::None);
    EXPECT(cls("Lbl", "Index") == Cond::None);
    EXPECT(cls("Lbl", "Sect") == Cond::None);   // Lbl has no "possibly inappropriate" condition
}

EQX_TEST(pac_structure_placement) {
    EXPECT(cls_place("Figure", "Sect", std::nullopt) == Cond::Inappropriate);
    EXPECT(cls_place("Figure", "Sect", "Block") == Cond::None);
    EXPECT(cls_place("Formula", "Document", "End") == Cond::Inappropriate);
    EXPECT(cls_place("Figure", "P", std::nullopt) == Cond::None);
    EXPECT(cls_place("Figure", "P", "Block") == Cond::Inappropriate);
    EXPECT(cls_place("Note", "P", "Block") == Cond::Inappropriate);
    EXPECT(cls_place("Note", "P", "Inline") == Cond::None);
    EXPECT(cls_place("Note", "P", "Before") == Cond::None);
    EXPECT(cls_place("Link", "TD", "Block") == Cond::None);
    EXPECT(cls_place("Formula", "TD", "Block") == Cond::None);
    EXPECT(cls_place("Link", "Span", "Block") == Cond::Inappropriate);
}

EQX_TEST(pac_structure_marked_content_and_headings) {
    for (auto t : {"Document", "Part", "Art", "Sect", "Div"}) EXPECT(own_content(t) == ContentPos::Warn);
    for (auto t : {"L", "Table", "TOC"}) EXPECT(own_content(t) == ContentPos::Fail);
    for (auto t : {"P", "Ruby", "Warichu", "Index", "BlockQuote", "Caption", "Private", "Span", "TD"})
        EXPECT(own_content(t) == ContentPos::Ok);
    EXPECT_EQ(heading_level("H"), 0);
    EXPECT_EQ(heading_level("H4"), 4);
    EXPECT_EQ(heading_level("H7"), -1);
    EXPECT_EQ(heading_level("P"), -1);
    EXPECT_EQ(rule_id("Span", Cond::Inappropriate), std::string("EQX-PAC-WarnSpanInappropriateUse"));
    EXPECT_EQ(rule_id("TR", Cond::Invalid), std::string("EQX-PAC-FailTRTagInvalidSyntax"));
    EXPECT_EQ(rule_id("Part", Cond::AsRoot), std::string("EQX-PAC-WarnPartTagAsRoot"));
    EXPECT_EQ(rule_id("Document", Cond::DocumentNotRoot), std::string("EQX-PAC-FailDocumentTagIsRoot"));
    EXPECT_EQ(rule_id("TBody", Cond::TBodyWarn), std::string("EQX-PAC-WarnTBodyTagInvalidSyntax"));
}
