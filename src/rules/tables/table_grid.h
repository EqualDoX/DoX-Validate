// Table regularity and header association (PDF/UA-1 7.2 / 7.5, PDF/UA-2 8.2.5.26) as a pure function over a
// description of one Table element.
//
// Behaviour (the reference-compatible reading used by the EQX-UA1/UA2 table rules):
//
//  1. Dimensions. The row count adds the RowSpan of the first cell of every non-empty row; a first cell with
//     RowSpan n > 1 makes the next n-1 rows count as covered (they add nothing). The column count adds the ColSpan
//     values of the first row's cells. Both sums and the row index use 32-bit two's-complement arithmetic, so
//     absurd span values wrap; a row index or a dimension that ends up negative makes the table undecidable
//     (`aborted`), as does a wrapped span that sends the row index back to a row already passed (the count would
//     never end) and a cell that has to be placed outside the grid. A row count of 0 ends the check with a
//     regular result. A grid above `max_cells` is not evaluated (`too_large`).
//  2. Placement, row by row and cell by cell over every row (also rows beyond the row count). Each cell takes the
//     first free column of its row at or after the position where the previous cell ended. A cell is irregular
//     when it reaches past the last column (row_with_wrong_column_span = its row) or past the last row
//     (column_with_wrong_row_span = its column); for PDF 2.0 also when its row span crosses the boundary between
//     two row groups (THead/TBody/TFoot). A cell with positive spans occupies its slots; meeting a slot that is
//     already taken makes the table irregular and lists both cells in `intersecting`. A row without cells is
//     irregular when the table has columns (wrong_column_span = 0). The first irregularity ends the check.
//  3. Completeness: the first row (up to the row count) with free slots is irregular, with wrong_column_span =
//     the number of occupied slots of that row.
//  4. Headers, only when some TH cell has no Scope: a TD (other than one anchored at the top-left slot) is
//     connected when every entry of its Headers array names the ID of a TH, or when a header is found for one of
//     its columns above it or for one of its rows to its left. Looking up a column from a slot, cells that are no
//     TH are passed over until the first TH; the uninterrupted run of TH cells from there on decides: it is a
//     header when one of them has Scope Column or Both (PDF 2.0: a TH without Scope counts as Column in the first
//     row, Row in the first column, Both elsewhere and at the top-left slot). The same applies to the left with
//     Row. The first TD in reading order of the grid that stays unconnected is reported, with its Headers entries
//     that name no TH ID (comma-separated).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eqx::tables {

struct CellSpec {
    bool th = false;                       // TH (else TD)
    std::int64_t colspan = 1;              // ColSpan attribute (default 1)
    std::int64_t rowspan = 1;              // RowSpan attribute (default 1)
    std::optional<std::string> scope;      // TH: Scope attribute (name, owner Table); nullopt when absent
    std::optional<std::string> id;         // TH: /ID string
    std::vector<std::string> headers;      // TD: string entries of the Headers attribute array
};

struct TableSpec {
    // The TR rows of the table in order (rows of THead/TBody/TFoot included in place); each row holds only its
    // TH/TD cells, in order.
    std::vector<std::vector<CellSpec>> rows;
    // Row counts at the start and the end of each THead/TBody/TFoot group (boundaries between row groups).
    std::vector<int> group_indexes;
    // PDF 2.0 reading: row-group boundary check and the default scope of a TH by position.
    bool pdf20 = false;
};

struct CellRef {
    int row = 0;    // index into TableSpec::rows
    int cell = 0;   // index into the row
    bool operator==(const CellRef& o) const { return row == o.row && cell == o.cell; }
};

struct TableResult {
    bool aborted = false;        // undecidable table (negative dimension or index, cell outside the grid)
    std::string abort_reason;
    bool too_large = false;      // grid above the cell limit (not evaluated)
    std::int64_t num_rows = 0;
    std::int64_t num_columns = 0;
    bool regular = true;         // false for every irregularity of steps 2 and 3
    std::optional<std::int64_t> row_with_wrong_column_span;
    std::optional<std::int64_t> column_with_wrong_row_span;
    std::optional<std::int64_t> wrong_column_span;            // occupied slots of a short row
    std::vector<CellRef> intersecting;                        // the two overlapping cells
    std::optional<CellRef> unconnected_td;                    // step 4
    std::string unknown_headers;                              // its Headers entries naming no TH ID
};

// Default limit on rows * columns of the grid.
inline constexpr std::int64_t kMaxGridCells = std::int64_t(1) << 24;

TableResult check_table(const TableSpec& spec, std::int64_t max_cells = kMaxGridCells);

}  // namespace eqx::tables
