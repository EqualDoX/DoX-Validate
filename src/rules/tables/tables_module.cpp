// Tables module: table regularity and header association (tables/table_grid.h; PDF/UA-1 7.2-15, 7.2-41/42/43,
// 7.5-1/2; PDF/UA-2 and WTPDF 8.2.5.26-1..6) with results compatible with veraPDF 1.30.2 (7.5-1: 1.30.4), and the PAC 26.1 table
// checks TablesAreRegular, THHasAssociatedCells and Quality CompleteTables.
//
// veraPDF evaluates checkTable once per SETable object (the first time a table property is read, i.e. when the
// Table is processed, before its cells); the cells it marks are the same model objects that are validated later,
// so hasIntersection / hasConnectedHeader are properties of the table run. The table is built from the Table's
// structural-significance children (Part/Div/NonStruct flattened): TR children and the TRs of THead/TBody/TFoot,
// each row holding its TH/TD structural-significance children; all types are standard types in the profile's
// flavour (PDF 1.7 role mapping for PDF/UA-1, ISO 32000-2 namespaces for PDF/UA-2 and WTPDF).
//
// PAC semantics (no source; inferred from the inventory and PAC read-outs of synthetic development fixtures): one TablesAreRegular warning per Table whose grid is irregular (counts 4 of 6 in
// U1-S02: the intersecting, row-span-past-end, short-row and long-row tables); one THHasAssociatedCells
// failure per TH in a table row without a Scope (Row/Column/Both) and not named by any /Headers of
// the table (U1-S02: the two scope-less TH fail, the nine TH with Scope /Column pass, also in irregular tables);
// one CompleteTables warning per Table without a TH or without a TD cell (U1-S01/S03/S04/Q01 tables without TH warn,
// the six U1-S02 tables with TH and TD pass). PAC is a PDF/UA-1 checker: PDF 1.7 standard types.
#include <unordered_set>

#include "rules/module.h"
#include "rules/structure/se_common.h"
#include "rules/tables/table_grid.h"

namespace eqx {

namespace {

struct FlavourRules {
    RuleIndex intersection, wrong_row_span, wrong_column_span_long, wrong_column_span_short, no_header, unknown_id;
};

struct BuiltTable {
    tables::TableSpec spec;
    std::vector<std::vector<Obj>> cells;   // dictionaries, parallel to spec.rows
    bool ok = true;                        // false: pass-through cycle (veraPDF: stack overflow)
};

BuiltTable build_table(const StructTree& st, const se::Attributes& attrs, const Obj& table, Flavor f) {
    BuiltTable b;
    b.spec.pdf20 = f == Flavor::Pdf20;
    b.spec.position_headers = f == Flavor::Pdf20;   // veraPDF 1.30.4: no positional header search for PDF/UA-1
    std::vector<Obj> trs;
    // GFSETable.getTR
    for (const auto& child : se::significance_children(st, table, f, &b.ok)) {
        if (child.type == "TR") {
            trs.push_back(child.dict);
        } else if (child.type == "THead" || child.type == "TBody" || child.type == "TFoot") {
            b.spec.group_indexes.push_back(static_cast<int>(trs.size()));
            for (const auto& g : se::significance_children(st, child.dict, f, &b.ok))
                if (g.type == "TR") trs.push_back(g.dict);
            b.spec.group_indexes.push_back(static_cast<int>(trs.size()));
        }
    }
    for (const Obj& tr : trs) {
        std::vector<tables::CellSpec> row;
        std::vector<Obj> dicts;
        for (const auto& c : se::significance_children(st, tr, f, &b.ok)) {
            if (c.type != "TD" && c.type != "TH") continue;
            tables::CellSpec cell;
            cell.th = c.type == "TH";
            cell.colspan = attrs.integer(c.dict, "ColSpan", "Table", 1);
            cell.rowspan = attrs.integer(c.dict, "RowSpan", "Table", 1);
            if (cell.th) {
                cell.scope = attrs.name(c.dict, "Scope", "Table", std::nullopt, false);
                cell.id = se::string_key(c.dict, "ID");
            } else {
                // GFSETD.getHeaders: the string entries of the Headers array
                Obj h = attrs.array(c.dict, "Headers", "Table");
                for (const Obj& item : cos::array_items(h))
                    if (item.isString()) cell.headers.push_back(*cos::text(item));
            }
            row.push_back(std::move(cell));
            dicts.push_back(c.dict);
        }
        b.spec.rows.push_back(std::move(row));
        b.cells.push_back(std::move(dicts));
    }
    return b;
}

// The /Headers entries of every cell of the table (TH and TD), for the PAC TH association check.
std::unordered_set<std::string> referenced_ids(const se::Attributes& attrs, const BuiltTable& b) {
    std::unordered_set<std::string> out;
    for (const auto& row : b.cells)
        for (const Obj& d : row)
            for (const Obj& item : cos::array_items(attrs.array(d, "Headers", "Table")))
                if (item.isString()) out.insert(*cos::text(item));
    return out;
}

class Module final : public RuleModule {
public:
    const char* name() const override { return "tables"; }

    void run(ValidationContext& ctx) override {
        static const FlavourRules ua1{R("EQX-UA1-7.2-15"), R("EQX-UA1-7.2-41"), R("EQX-UA1-7.2-42"),
                                      R("EQX-UA1-7.2-43"), R("EQX-UA1-7.5-1"),  R("EQX-UA1-7.5-2")};
        static const FlavourRules ua2{R("EQX-UA2-8.2.5.26-1"), R("EQX-UA2-8.2.5.26-2"), R("EQX-UA2-8.2.5.26-3"),
                                      R("EQX-UA2-8.2.5.26-4"), R("EQX-UA2-8.2.5.26-5"), R("EQX-UA2-8.2.5.26-6")};
        static const RuleIndex pac_regular = R("EQX-PAC-WarnTablesAreRegular");
        static const RuleIndex pac_th = R("EQX-PAC-THHasAssociatedCells_CheckTHs_TableHeaderCellHasNoAssociatedSubcells");
        static const RuleIndex pac_complete = R("EQX-PACQ-WarningCompleteTables");

        StructTree& st = ctx.st;
        if (!st.present()) return;
        se::Attributes attrs(st);
        bool incomplete17 = false, incomplete20 = false, incomplete_pac = false;
        std::string why17, why20;

        // default: every element in document order; --reference-dedup: only the model objects veraPDF validates
        // (direct elements sharing an /S are validated once), in its order
        const bool ref_mode = ctx.reference_dedup;
        // (the map is read in reference mode only)
        const auto by_identity = ref_mode ? se::elem_index_map(st) : std::unordered_map<const void*, int>();
        auto checked = [&](const Obj& d) {
            if (!ref_mode) return true;
            auto it = by_identity.find(se::identity(d));
            return it != by_identity.end() && st.elem(it->second).verapdf_checked;
        };
        for (int idx : se::element_order(st, ref_mode)) {
            const StructElem& e = st.elem(idx);
            for (Flavor f : {Flavor::Pdf17, Flavor::Pdf20}) {
                if (e.standard(f) != "Table") continue;
                const FlavourRules& fr = f == Flavor::Pdf17 ? ua1 : ua2;
                BuiltTable b = build_table(st, attrs, e.dict, f);
                tables::TableResult res;
                if (b.ok) res = tables::check_table(b.spec);
                const bool undecidable = !b.ok || res.aborted || res.too_large;
                if (undecidable) {
                    std::string why = !b.ok ? "pass-through structure cycle in table " + e.ref.str() +
                                                  " (veraPDF: stack overflow, validation aborted)"
                                      : res.too_large ? "table " + e.ref.str() + ": grid of " +
                                                            std::to_string(res.num_rows) + " x " +
                                                            std::to_string(res.num_columns) + " cells too large"
                                                      : "table " + e.ref.str() + ": " + res.abort_reason +
                                                            (res.abort_reason.find("veraPDF") == std::string::npos
                                                                 ? " (veraPDF aborts the validation)"
                                                                 : "");
                    (f == Flavor::Pdf17 ? incomplete17 : incomplete20) = true;
                    (f == Flavor::Pdf17 ? why17 : why20) = why;
                } else {
                    if (res.column_with_wrong_row_span)
                        ctx.report(fr.wrong_row_span, -1, e.ref,
                                   "row span past the table end in column " +
                                       std::to_string(*res.column_with_wrong_row_span));
                    if (res.row_with_wrong_column_span) {
                        std::string row = std::to_string(*res.row_with_wrong_column_span);
                        if (res.wrong_column_span)
                            ctx.report(fr.wrong_column_span_short, -1, e.ref,
                                       "row " + row + " spans " + std::to_string(*res.wrong_column_span) + " of " +
                                           std::to_string(res.num_columns) + " columns");
                        else
                            ctx.report(fr.wrong_column_span_long, -1, e.ref,
                                       "row " + row + " exceeds " + std::to_string(res.num_columns) + " columns");
                    }
                    for (const auto& c : res.intersecting)
                        if (checked(b.cells[c.row][c.cell]))
                            ctx.report(fr.intersection, -1, ref_of(b.cells[c.row][c.cell]), "cell overlaps another cell");
                    if (res.unconnected_td) {
                        const Obj& td = b.cells[res.unconnected_td->row][res.unconnected_td->cell];
                        if (!checked(td)) {
                        } else if (res.unknown_headers.empty())
                            ctx.report(fr.no_header, -1, ref_of(td), "TD without determinable header cells");
                        else
                            ctx.report(fr.unknown_id, -1, ref_of(td), "Headers name unknown IDs: " + res.unknown_headers);
                    }
                }

                if (f != Flavor::Pdf17) continue;
                // ---- PAC (PDF/UA-1)
                if (!b.ok || res.too_large) {
                    incomplete_pac = true;
                    continue;
                }
                if (!res.regular || res.aborted) ctx.report(pac_regular, -1, e.ref, "irregular table");
                bool any_th = false, any_td = false;
                for (const auto& row : b.spec.rows)
                    for (const auto& c : row) (c.th ? any_th : any_td) = true;
                if (!any_th || !any_td)
                    ctx.report(pac_complete, -1, e.ref, !any_th ? "table without TH" : "table without TD");
                if (any_th) {
                    auto refs = referenced_ids(attrs, b);
                    for (std::size_t r = 0; r < b.spec.rows.size(); ++r) {
                        for (std::size_t k = 0; k < b.spec.rows[r].size(); ++k) {
                            const auto& c = b.spec.rows[r][k];
                            if (!c.th) continue;
                            if (c.scope && (*c.scope == "Row" || *c.scope == "Column" || *c.scope == "Both")) continue;
                            if (c.id && !c.id->empty() && refs.count(*c.id)) continue;
                            ctx.report(pac_th, -1, ref_of(b.cells[r][k]), "TH without Scope and not named by Headers");
                        }
                    }
                }
            }
        }
        if (incomplete17)
            for (RuleIndex r : {ua1.intersection, ua1.wrong_row_span, ua1.wrong_column_span_long,
                                ua1.wrong_column_span_short, ua1.no_header, ua1.unknown_id})
                ctx.mark_incomplete(r, why17);
        if (incomplete20)
            for (RuleIndex r : {ua2.intersection, ua2.wrong_row_span, ua2.wrong_column_span_long,
                                ua2.wrong_column_span_short, ua2.no_header, ua2.unknown_id})
                ctx.mark_incomplete(r, why20);
        if (incomplete_pac)
            for (RuleIndex r : {pac_regular, pac_th, pac_complete})
                ctx.mark_incomplete(r, "table structure cycle or table grid too large");
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_tables_module() { return std::make_unique<Module>(); }

}  // namespace eqx
