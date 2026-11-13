#include "rules/tables/table_grid.h"

#include <unordered_set>

namespace eqx::tables {

namespace {

// 32-bit two's-complement value of a 64-bit integer.
std::int32_t wrap32(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

// 64-bit sum that wraps instead of overflowing.
std::int64_t add64(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

constexpr const char* kOutOfGrid = "cell index out of bounds (ArrayIndexOutOfBoundsException)";

enum class RowWalk { Counted, NegativeIndex, Cycle };

// Step 1, rows: the first cell of each non-empty row adds its row span; a span n > 1 covers the next n-1 rows.
// The walk is undecidable when the row index wraps to a negative value, or when a wrapped span sends it back to a
// row it has passed (it would revisit that row forever: the walk is deterministic, so a walk that ends visits each
// row at most once and takes at most one step per row).
RowWalk row_count(const TableSpec& spec, std::int32_t& count) {
    const std::int64_t n = static_cast<std::int64_t>(spec.rows.size());
    std::int32_t sum = 0;
    std::int32_t row = 0;
    for (std::int64_t steps = 0; row < n; ++steps) {
        if (row < 0) return RowWalk::NegativeIndex;
        if (steps >= n) return RowWalk::Cycle;
        const std::vector<CellSpec>& cells = spec.rows[static_cast<std::size_t>(row)];
        std::int32_t last = row;   // last row covered by this one
        if (!cells.empty()) {
            const std::int64_t span = cells.front().rowspan;
            sum = wrap32(add64(sum, span));
            if (span > 1) last = wrap32(add64(row, span - 1));
        }
        row = wrap32(std::int64_t(last) + 1);
    }
    count = sum;
    return RowWalk::Counted;
}

// Step 1, columns: the column spans of the first row.
std::int32_t column_count(const TableSpec& spec) {
    std::int32_t sum = 0;
    if (!spec.rows.empty())
        for (const CellSpec& c : spec.rows.front()) sum = wrap32(add64(sum, c.colspan));
    return sum;
}

// Cells numbered in reading order of the rows, with the slot each one starts at.
struct CellIndex {
    std::vector<CellRef> ref;
    std::vector<const CellSpec*> spec;
    std::vector<std::int32_t> top, left;    // anchor slot (-1 until placed)
    std::vector<char> fills;                // occupies at least one slot

    explicit CellIndex(const TableSpec& t) {
        for (std::size_t r = 0; r < t.rows.size(); ++r)
            for (std::size_t k = 0; k < t.rows[r].size(); ++k) {
                ref.push_back(CellRef{static_cast<int>(r), static_cast<int>(k)});
                spec.push_back(&t.rows[r][k]);
            }
        top.assign(ref.size(), -1);
        left.assign(ref.size(), -1);
        fills.assign(ref.size(), 0);
    }
};

// Occupancy of the rows x cols slots: the number of the cell that holds a slot, or -1.
class Grid {
public:
    Grid(std::int32_t rows, std::int32_t cols)
        : rows_(rows), cols_(cols), slot_(static_cast<std::size_t>(std::int64_t(rows) * cols), -1) {}
    std::int32_t rows() const { return rows_; }
    std::int32_t cols() const { return cols_; }
    std::int32_t& at(std::int64_t r, std::int64_t c) { return slot_[static_cast<std::size_t>(r * cols_ + c)]; }
    std::int32_t at(std::int64_t r, std::int64_t c) const { return slot_[static_cast<std::size_t>(r * cols_ + c)]; }

private:
    std::int32_t rows_, cols_;
    std::vector<std::int32_t> slot_;
};

enum class Outcome { Done, Irregular, OutOfGrid };

// Step 2: places every cell of every row; stops at the first irregularity.
Outcome place_cells(const TableSpec& t, CellIndex& cells, Grid& grid, TableResult& res) {
    const std::int32_t nrows = grid.rows(), ncols = grid.cols();
    std::size_t id = 0;
    for (std::int32_t r = 0; r < static_cast<std::int64_t>(t.rows.size()); ++r) {
        const std::vector<CellSpec>& row = t.rows[static_cast<std::size_t>(r)];
        std::int32_t col = 0;
        for (std::size_t k = 0; k < row.size(); ++k, ++id) {
            const std::int64_t cspan = row[k].colspan, rspan = row[k].rowspan;
            for (; col < ncols; ++col) {   // first free slot at or after `col`
                if (r >= nrows || col < 0) return Outcome::OutOfGrid;
                if (grid.at(r, col) < 0) break;
            }
            if (add64(col, cspan) > ncols) {
                res.row_with_wrong_column_span = r;
                return Outcome::Irregular;
            }
            const std::int64_t below = add64(r, rspan);   // first row after the cell
            bool crosses_group = below > nrows;
            if (!crosses_group && t.pdf20)
                for (int boundary : t.group_indexes)
                    if (r < boundary && below > boundary) {
                        crosses_group = true;
                        break;
                    }
            if (crosses_group) {
                res.column_with_wrong_row_span = col;
                return Outcome::Irregular;
            }
            cells.top[id] = r;
            cells.left[id] = col;
            if (rspan > 0 && cspan > 0) {
                if (col < 0) return Outcome::OutOfGrid;
                cells.fills[id] = 1;
                // The span checks above compare wrapped 64-bit sums, so a span near 2^63 passes them; each slot is
                // therefore checked against the grid before it is taken (outside: undecidable, as an index error).
                for (std::int64_t dr = 0; dr < rspan; ++dr)
                    for (std::int64_t dc = 0; dc < cspan; ++dc) {
                        if (r + dr >= nrows || col + dc >= ncols) return Outcome::OutOfGrid;
                        std::int32_t& slot = grid.at(r + dr, col + dc);
                        if (slot >= 0) {
                            res.intersecting = {cells.ref[id], cells.ref[static_cast<std::size_t>(slot)]};
                            return Outcome::Irregular;
                        }
                        slot = static_cast<std::int32_t>(id);
                    }
            }
            col = wrap32(add64(col, cspan));
        }
        if (row.empty() && ncols > 0) {
            res.row_with_wrong_column_span = r;
            res.wrong_column_span = 0;
            return Outcome::Irregular;
        }
    }
    return Outcome::Done;
}

// Step 3: the first row with free slots.
bool rows_complete(const Grid& grid, TableResult& res) {
    for (std::int32_t r = 0; r < grid.rows(); ++r) {
        std::int64_t used = 0;
        for (std::int32_t c = 0; c < grid.cols(); ++c) used += grid.at(r, c) >= 0;
        if (used != grid.cols()) {
            res.row_with_wrong_column_span = r;
            res.wrong_column_span = used;
            return false;
        }
    }
    return true;
}

// Step 4.
class HeaderSearch {
public:
    HeaderSearch(const TableSpec& t, const CellIndex& cells, const Grid& grid) : t_(t), cells_(cells), grid_(grid) {}

    void run(TableResult& res) {
        bool unscoped_th = false;
        for (std::size_t i = 0; i < cells_.spec.size(); ++i) {
            const CellSpec& c = *cells_.spec[i];
            if (!c.th || !cells_.fills[i]) continue;
            if (c.id && !c.id->empty()) th_ids_.insert(*c.id);
            if (!c.scope) unscoped_th = true;
        }
        if (!unscoped_th) return;

        const std::int32_t nrows = grid_.rows(), ncols = grid_.cols();
        const std::vector<bool> row_header = row_header_left_of_slots();
        // Per column: whether the nearest run of TH cells above the current row holds a column header, and
        // whether the slot just above is a TH (the run is still open).
        std::vector<char> run_has_header(static_cast<std::size_t>(ncols), 0), open_run(static_cast<std::size_t>(ncols), 0);
        for (std::int32_t r = 0; r < nrows; ++r) {
            for (std::int32_t c = 0; c < ncols; ++c) {
                const std::size_t id = static_cast<std::size_t>(grid_.at(r, c));
                const CellSpec& cell = *cells_.spec[id];
                if (cell.th || cells_.top[id] != r || cells_.left[id] != c || (r == 0 && c == 0)) continue;
                if (headers_known(cell)) continue;
                bool found = false;
                const std::int64_t col_end = std::int64_t(c) + wrap32(cell.colspan);
                for (std::int64_t cc = c; cc < col_end && !found; ++cc) found = run_has_header[static_cast<std::size_t>(cc)];
                const std::int64_t row_end = std::int64_t(r) + wrap32(cell.rowspan);
                for (std::int64_t rr = r; rr < row_end && !found; ++rr)
                    found = row_header[static_cast<std::size_t>(rr * ncols + c)];
                if (found) continue;
                res.unconnected_td = cells_.ref[id];
                res.unknown_headers = unknown_headers(cell);
                return;
            }
            for (std::int32_t c = 0; c < ncols; ++c) {
                const std::size_t k = static_cast<std::size_t>(c);
                if (is_th(r, c)) {
                    run_has_header[k] = (open_run[k] && run_has_header[k]) || header_for(r, c, "Column");
                    open_run[k] = 1;
                } else {
                    open_run[k] = 0;
                }
            }
        }
    }

private:
    bool is_th(std::int32_t r, std::int32_t c) const { return cells_.spec[static_cast<std::size_t>(grid_.at(r, c))]->th; }

    // Whether the TH at a slot heads in `direction` ("Column" or "Row").
    bool header_for(std::int32_t r, std::int32_t c, const char* direction) const {
        const CellSpec& cell = *cells_.spec[static_cast<std::size_t>(grid_.at(r, c))];
        if (!cell.th) return false;
        std::string scope;
        if (cell.scope) scope = *cell.scope;
        else if (!t_.pdf20) return false;
        else scope = (r == 0) == (c == 0) ? "Both" : r == 0 ? "Column" : "Row";
        return scope == "Both" || scope == direction;
    }

    // For every slot: whether the nearest run of TH cells to its left (in its row) holds a row header.
    std::vector<bool> row_header_left_of_slots() const {
        const std::int32_t nrows = grid_.rows(), ncols = grid_.cols();
        std::vector<bool> out(static_cast<std::size_t>(std::int64_t(nrows) * ncols), false);
        for (std::int32_t r = 0; r < nrows; ++r) {
            bool has = false, open = false;
            for (std::int32_t c = 0; c < ncols; ++c) {
                out[static_cast<std::size_t>(std::int64_t(r) * ncols + c)] = has;
                if (is_th(r, c)) {
                    has = (open && has) || header_for(r, c, "Row");
                    open = true;
                } else {
                    open = false;
                }
            }
        }
        return out;
    }

    bool headers_known(const CellSpec& td) const {
        if (td.headers.empty()) return false;
        for (const std::string& h : td.headers)
            if (!th_ids_.count(h)) return false;
        return true;
    }

    std::string unknown_headers(const CellSpec& td) const {
        std::string out;
        bool listed = false;   // entries may be empty strings, so the separator follows the count, not `out`
        for (const std::string& h : td.headers) {
            if (th_ids_.count(h)) continue;
            if (listed) out += ',';
            out += h;
            listed = true;
        }
        return out;
    }

    const TableSpec& t_;
    const CellIndex& cells_;
    const Grid& grid_;
    std::unordered_set<std::string> th_ids_;
};

}  // namespace

TableResult check_table(const TableSpec& spec, std::int64_t max_cells) {
    TableResult res;
    auto undecidable = [&res](const char* why) {
        res.aborted = true;
        res.abort_reason = why;
        return res;
    };

    std::int32_t nrows = 0;
    switch (row_count(spec, nrows)) {
        case RowWalk::NegativeIndex: return undecidable("row index out of bounds in getNumberOfRows (int overflow)");
        case RowWalk::Cycle:
            return undecidable("row spans lead back to an earlier row (int overflow; no row count; veraPDF does not finish)");
        case RowWalk::Counted: break;
    }
    res.num_rows = nrows;
    if (nrows == 0) return res;
    const std::int32_t ncols = column_count(spec);
    res.num_columns = ncols;
    if (nrows < 0 || ncols < 0) return undecidable("negative table dimension (NegativeArraySizeException)");
    if (std::int64_t(nrows) * ncols > max_cells) {
        res.too_large = true;
        return res;
    }

    CellIndex cells(spec);
    Grid grid(nrows, ncols);
    switch (place_cells(spec, cells, grid, res)) {
        case Outcome::OutOfGrid: return undecidable(kOutOfGrid);
        case Outcome::Irregular: res.regular = false; return res;
        case Outcome::Done: break;
    }
    if (!rows_complete(grid, res)) {
        res.regular = false;
        return res;
    }
    HeaderSearch(spec, cells, grid).run(res);
    return res;
}

}  // namespace eqx::tables
