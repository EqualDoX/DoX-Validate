// Unit tests of the table regularity and header association check (src/rules/tables/table_grid.*).
#include "rules/tables/table_grid.h"
#include "unit.h"

using eqx::tables::CellRef;
using eqx::tables::CellSpec;
using eqx::tables::check_table;
using eqx::tables::TableSpec;

namespace {

CellSpec TH(const char* scope = nullptr, std::int64_t rs = 1, std::int64_t cs = 1, const char* id = nullptr) {
    CellSpec c;
    c.th = true;
    c.rowspan = rs;
    c.colspan = cs;
    if (scope) c.scope = scope;
    if (id) c.id = id;
    return c;
}

CellSpec TD(std::int64_t rs = 1, std::int64_t cs = 1, std::vector<std::string> headers = {}) {
    CellSpec c;
    c.rowspan = rs;
    c.colspan = cs;
    c.headers = std::move(headers);
    return c;
}

TableSpec table(std::vector<std::vector<CellSpec>> rows, bool pdf20 = false) {
    TableSpec t;
    t.rows = std::move(rows);
    t.pdf20 = pdf20;
    return t;
}

bool has(const std::vector<CellRef>& v, int r, int c) {
    for (const auto& x : v)
        if (x.row == r && x.cell == c) return true;
    return false;
}

}  // namespace

EQX_TEST(tables_regular_with_scoped_headers) {
    auto r = check_table(table({{TH("Column"), TH("Column")}, {TD(), TD()}}));
    EXPECT(!r.aborted && r.regular);
    EXPECT(!r.row_with_wrong_column_span && !r.column_with_wrong_row_span && !r.wrong_column_span);
    EXPECT(r.intersecting.empty());
    EXPECT(!r.unconnected_td);
    EXPECT_EQ(r.num_rows, 2);
    EXPECT_EQ(r.num_columns, 2);
}

EQX_TEST(tables_intersection_marks_both_cells) {
    // U1-S02 table 1: TH RowSpan 2 in column 2, TD ColSpan 2 in row 2
    auto r = check_table(table({{TH("Column"), TH("Column", 2)}, {TD(1, 2)}}));
    EXPECT(!r.regular);
    EXPECT_EQ(r.intersecting.size(), std::size_t(2));
    EXPECT(has(r.intersecting, 1, 0));
    EXPECT(has(r.intersecting, 0, 1));
    EXPECT(!r.row_with_wrong_column_span && !r.column_with_wrong_row_span);
}

EQX_TEST(tables_row_span_past_end) {
    auto r = check_table(table({{TH("Column"), TH("Column")}, {TD(), TD(3)}}));
    EXPECT(r.column_with_wrong_row_span.has_value());
    EXPECT_EQ(*r.column_with_wrong_row_span, 1);
    EXPECT(!r.row_with_wrong_column_span);
}

EQX_TEST(tables_short_and_long_rows) {
    auto s = check_table(table({{TH("Column"), TH("Column"), TH("Column")}, {TD(), TD()}}));
    EXPECT(s.row_with_wrong_column_span && *s.row_with_wrong_column_span == 1);
    EXPECT(s.wrong_column_span && *s.wrong_column_span == 2);   // 7.2-43
    auto l = check_table(table({{TH("Column"), TH("Column")}, {TD(1, 3), TD()}}));
    EXPECT(l.row_with_wrong_column_span && *l.row_with_wrong_column_span == 1);
    EXPECT(!l.wrong_column_span);                                  // 7.2-42
}

EQX_TEST(tables_empty_row_has_wrong_span_zero) {
    auto r = check_table(table({{TD(), TD()}, {}}));
    EXPECT(r.row_with_wrong_column_span && *r.row_with_wrong_column_span == 1);
    EXPECT(r.wrong_column_span && *r.wrong_column_span == 0);
}

EQX_TEST(tables_header_association_ua1_and_ua2) {
    // U1-S02 table 5: TH without Scope at (0,1): no TD at (1,0) association in PDF/UA-1
    auto t = table({{TD(), TH()}, {TD(), TD()}});
    auto r = check_table(t);
    EXPECT(r.unconnected_td && r.unconnected_td->row == 1 && r.unconnected_td->cell == 0);
    EXPECT(r.unknown_headers.empty());
    // PDF/UA-2: same table, the TH gets the default scope Column; (1,0) still has no header above or left
    t.pdf20 = true;
    auto r2 = check_table(t);
    EXPECT(r2.unconnected_td && r2.unconnected_td->row == 1 && r2.unconnected_td->cell == 0);
    // PDF/UA-2 default scopes make a plain header row sufficient; PDF/UA-1 does not
    auto ua1 = check_table(table({{TH(), TH()}, {TD(), TD()}}));
    EXPECT(ua1.unconnected_td && ua1.unconnected_td->row == 1 && ua1.unconnected_td->cell == 0);
    auto ua2 = check_table(table({{TH(), TH()}, {TD(), TD()}}, true));
    EXPECT(!ua2.unconnected_td);
}

EQX_TEST(tables_unknown_headers) {
    auto r = check_table(table({{TD(), TH()}, {TD(1, 1, {"no-such-id"}), TD()}}));
    EXPECT(r.unconnected_td && r.unconnected_td->row == 1 && r.unconnected_td->cell == 0);
    EXPECT_EQ(r.unknown_headers, std::string("no-such-id"));
    // known IDs connect the cell
    auto ok = check_table(table({{TD(), TH(nullptr, 1, 1, "h1")}, {TD(1, 1, {"h1"}), TD(1, 1, {"h1"})}}));
    EXPECT(!ok.unconnected_td);
    // an empty unknown header joins to "" (7.5-1, not 7.5-2)
    auto e = check_table(table({{TD(), TH()}, {TD(1, 1, {""}), TD()}}));
    EXPECT(e.unconnected_td && e.unknown_headers.empty());
}

EQX_TEST(tables_scan_stops_after_first_header_run) {
    // single column: TH Column, TD, TH (no scope), TD -> the last TD's upward scan ends at the TD in row 1
    auto r = check_table(table({{TH("Column")}, {TD()}, {TH()}, {TD()}}));
    EXPECT(r.unconnected_td && r.unconnected_td->row == 3);
    // the TH run continues upward to a scoped TH: connected
    auto ok = check_table(table({{TH("Column")}, {TH()}, {TD()}, {TD()}}));
    EXPECT(!ok.unconnected_td);
}

EQX_TEST(tables_no_th_means_no_header_check) {
    auto r = check_table(table({{TD(), TD()}, {TD(), TD()}}));
    EXPECT(r.regular && !r.unconnected_td);
}

EQX_TEST(tables_row_groups_pdf20) {
    TableSpec t = table({{TD(2)}, {TD()}}, true);
    t.group_indexes = {0, 1, 1, 2};   // THead [TR], TBody [TR]
    auto r = check_table(t);
    EXPECT(r.column_with_wrong_row_span && *r.column_with_wrong_row_span == 0);
}

EQX_TEST(tables_malformed_spans) {
    // negative column span followed by another cell: index -1 (Java ArrayIndexOutOfBoundsException)
    auto a = check_table(table({{TD(1, 3)}, {TD(1, -1), TD(), TD()}}));
    EXPECT(a.aborted);
    // zero row span in the first row: the second row is outside the grid
    auto z = check_table(table({{TD(0)}, {TD()}}));
    EXPECT(z.aborted);
    // huge spans: no allocation beyond the limit
    auto h = check_table(table({{TD(1000000000, 1000)}}));
    EXPECT(h.too_large && !h.aborted);
    // int overflow like Java: RowSpan 2^32 + 1 sums to 1 row, then the span exceeds it
    auto w = check_table(table({{TD((std::int64_t(1) << 32) + 1)}}));
    EXPECT(!w.aborted && !w.too_large);
    EXPECT(w.column_with_wrong_row_span && *w.column_with_wrong_row_span == 0);
    // negative dimension
    auto n = check_table(table({{TD(-1)}}));
    EXPECT(n.aborted);
    // zero column span only: zero columns, regular
    auto c = check_table(table({{TD(1, 0)}}));
    EXPECT(!c.aborted && c.regular);
}

EQX_TEST(tables_row_span_wrapping_back_terminates) {
    // RowSpan 2^32 - 1 on the first cell of the second row wraps the row index back to the first row; the row
    // count would cycle between the two rows forever. The table is undecidable instead.
    auto r = check_table(table({{TD()}, {TD(4294967295LL)}}));
    EXPECT(r.aborted);
    EXPECT(r.abort_reason.find("earlier row") != std::string::npos);
    // a wrapped span that lands on a row not visited yet and still ends: counted as before
    auto ok = check_table(table({{TD()}, {TD(4)}, {TD()}, {TD()}, {TD()}, {TD()}}));
    EXPECT(!ok.aborted);
}

EQX_TEST(tables_span_near_int64_max_stays_inside_grid) {
    // RowSpan 2^63 - 1 in row 1: row + span wraps negative and passes the "span past the last row" test; the cell
    // must not be written outside the grid (undecidable instead).
    const std::int64_t big = 9223372036854775807LL;
    auto r = check_table(table({{TD(3), TD()}, {TD(big)}, {TH(), TH()}, {TH(nullptr, 4), TD(1, 4)}}, true));
    EXPECT(r.aborted);
    auto c = check_table(table({{TD(), TD()}, {TD(), TD(1, big)}}));
    EXPECT(c.aborted);
}

// veraPDF 1.30.4 (GFSETable.hasHeaders, PDFUA_1 flavour): in a table with an unscoped TH, a TD is connected only
// through its Headers; the scoped column header above it no longer counts. The positional search stays for PDF 2.0.
// Shape of veraPDF's corpus file 7.5-t01-fail-c: a corner TD, column headers with Scope, row headers without.
EQX_TEST(tables_ua1_headers_only_through_headers_attribute) {
    auto rows = std::vector<std::vector<CellSpec>>{{TD(), TH("Column"), TH("Column")}, {TH(), TD(), TD()}, {TH(), TD(), TD()}};
    TableSpec positional = table(rows);
    auto old = check_table(positional);   // veraPDF 1.30.2 behaviour (default): the column headers connect every TD
    EXPECT(!old.unconnected_td);
    TableSpec ua1 = table(rows);
    ua1.position_headers = false;
    auto now = check_table(ua1);
    EXPECT(now.unconnected_td && now.unconnected_td->row == 1 && now.unconnected_td->cell == 1);
    // with Headers naming TH IDs the TDs stay connected
    auto ided = std::vector<std::vector<CellSpec>>{{TD(), TH("Column", 1, 1, "c1"), TH("Column", 1, 1, "c2")},
                                                   {TH(), TD(1, 1, {"c1"}), TD(1, 1, {"c2"})}};
    TableSpec ua1_ids = table(ided);
    ua1_ids.position_headers = false;
    EXPECT(!check_table(ua1_ids).unconnected_td);
    // PDF/UA-2 keeps the positional search
    TableSpec ua2 = table(rows, true);
    EXPECT(!check_table(ua2).unconnected_td);
}
