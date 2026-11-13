// Unit tests of the structure module helpers (src/rules/structure/*).
#include "core/document.h"
#include "core/structure.h"
#include "rules/structure/se_common.h"
#include "rules/structure/structure_rules.h"
#include "unit.h"

using namespace eqx;

EQX_TEST(structure_heading_nesting) {
    // first heading compared with level 0; every jump of more than one level fails, going back up never does
    auto f = structure::heading_nesting_failures({2, 4, 3});
    EXPECT_EQ(f.size(), std::size_t(2));
    EXPECT_EQ(f[0], std::size_t(0));
    EXPECT_EQ(f[1], std::size_t(1));
    auto g = structure::heading_nesting_failures({1, 2, 2, 3, 1, 3});
    EXPECT_EQ(g.size(), std::size_t(1));
    EXPECT_EQ(g[0], std::size_t(5));
    EXPECT(structure::heading_nesting_failures({}).empty());
    EXPECT(structure::heading_nesting_failures({1, 2, 3, 4, 5, 6, 1}).empty());
}

EQX_TEST(structure_duplicate_ids) {
    auto d = structure::duplicate_ids({std::string("n1"), std::nullopt, std::string("n1"), std::string(""),
                                       std::string(""), std::string("n2")});
    EXPECT(!d[0]);
    EXPECT(!d[1]);
    EXPECT(d[2]);
    EXPECT(!d[3]);
    EXPECT(d[4]);   // empty IDs are IDs too (Set<String>.add)
    EXPECT(!d[5]);
}

EQX_TEST(structure_fenote_refs) {
    std::set<ObjRef> referrers{{10, 0}, {11, 0}};
    std::set<ObjRef> own{{11, 0}, {12, 0}};
    auto o = structure::orphan_refs(referrers, own);
    auto g = structure::ghost_refs(referrers, own);
    EXPECT_EQ(o.size(), std::size_t(1));
    EXPECT(o.count(ObjRef{10, 0}));
    EXPECT_EQ(g.size(), std::size_t(1));
    EXPECT(g.count(ObjRef{12, 0}));
    EXPECT(structure::orphan_refs({}, own).empty());
}

EQX_TEST(structure_hn_pattern) {
    EXPECT(se::is_hn("H1"));
    EXPECT(se::is_hn("H10"));
    EXPECT(!se::is_hn("H"));
    EXPECT(!se::is_hn("H0"));
    EXPECT(!se::is_hn("H1a"));
    EXPECT(!se::is_hn("h1"));
}

EQX_TEST(structure_fixture_s02_tables_and_attributes) {
    EQX_REQUIRE_FIXTURES();
    OpenStatus status;
    std::string err;
    auto doc = Document::open(eqx_test::fixture("U1-S02-semantics.pdf"), &status, &err);
    EXPECT(doc != nullptr);
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    se::Attributes attrs(st);
    // table 26 0: two rows; the second TH (29 0) has RowSpan 2 and Scope /Column
    int t = st.find(ObjRef{26, 0});
    EXPECT(t >= 0);
    if (t < 0) return;
    bool ok = true;
    auto rows = se::significance_children(st, st.elem(t).dict, Flavor::Pdf17, &ok);
    EXPECT(ok);
    EXPECT_EQ(rows.size(), std::size_t(2));
    EXPECT_EQ(rows[0].type, std::string("TR"));
    int th = st.find(ObjRef{29, 0});
    EXPECT(th >= 0);
    if (th < 0) return;
    EXPECT_EQ(attrs.integer(st.elem(th).dict, "RowSpan", "Table", 1), std::int64_t(2));
    EXPECT_EQ(attrs.integer(st.elem(th).dict, "ColSpan", "Table", 1), std::int64_t(1));
    EXPECT(attrs.name(st.elem(th).dict, "Scope", "Table", std::nullopt, false) == std::optional<std::string>("Column"));
    // wrong owner: no value
    EXPECT(!attrs.name(st.elem(th).dict, "Scope", "List", std::nullopt, false));
    // Note IDs: 17 0 without, 19 0 / 21 0 with "n1"
    EXPECT(!se::string_key(st.elem(st.find(ObjRef{17, 0})).dict, "ID"));
    EXPECT(se::string_key(st.elem(st.find(ObjRef{21, 0})).dict, "ID") == std::optional<std::string>("n1"));
}
