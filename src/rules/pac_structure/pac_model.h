// PAC 26.1 "Structure tree" content model (PDF/UA tab, PDF 1.7 structure types), as pure functions over
// structure types. PAC is closed source; the model reproduces its observable behaviour:
//
//  * PAC variants verified on 2026-10-24 against stored PAC results (84/84 audit files, 168/169 results),
//    and
//  * the per-type passed/warned/failed counts of PAC read-outs on synthetic development fixtures and probes.
//
// Every structure element of a checked type gets at most one outcome per check (PAC counts each element once:
// passed + warned + failed = number of elements of the type in every readout). Fail conditions win over
// warnings. Types are PAC types (pac_type(): a standard /S is taken as is, a non-standard /S through the
// RoleMap), also in PDF 2.0 files (PAC applies PDF/UA-1 semantics everywhere).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eqx::pac {

// Pseudo type of the StructTreeRoot as a parent.
inline constexpr std::string_view kRoot = "\x01StructTreeRoot";

enum class Cond : std::uint8_t {
    None,             // passes
    Invalid,          // TagInvalidSyntax: parent/child rules broken (fail)
    Inappropriate,    // InappropriateUse: questionable context (warning)
    AsRoot,           // TagAsRoot: element at the top of the tree (warning)
    DocumentNotRoot,  // DocumentTagIsRoot: a Document element below the top level (warning)
    TBodyWarn,        // CheckWarnTBodyTagInvalidSyntax (warning)
};

// Where an element stands. Parent types are PAC types; kRoot for the StructTreeRoot; "" for a parent whose
// type is not a standard type (unmapped non-standard type).
struct Context {
    std::string_view type;              // the element's PAC type
    std::string_view parent;            // direct parent
    std::string_view eff_parent;        // nearest ancestor that is not NonStruct / Private
    std::string_view eff_grandparent;   // eff_parent's own effective parent (TR inside THead/TBody/TFoot)
    std::optional<std::string_view> placement;   // Layout /Placement of the element (nullopt: none)
    // Position among the element kids of eff_parent (flattened through NonStruct / Private); only filled
    // when eff_parent is Table, L or TOC.
    int index = 0;
    int count = 1;
    int captions = 0;   // Caption kids of eff_parent
    int theads = 0;     // THead kids of eff_parent
    int tfoots = 0;     // TFoot kids of eff_parent
    int bad_kids = 0;   // kids eff_parent does not allow (Table, THead, TBody, TFoot)
    // Order of the row groups in a Table (kids of eff_parent, Captions not counted)
    bool table_has_row = false, table_has_group = false;   // TR kids; THead, TBody, TFoot kids
    bool group_first = true, group_last = true;            // this kid is the first, the last that is no Caption
    int theads_before = -1, tbodies_before = 0, tfoots_before = 0;   // theads_before < 0: position not filled, theads decides
};

struct Outcome {
    Cond cond = Cond::None;
    const char* reason = "";   // short rule name for the finding detail
};

// The structure-tree outcome of one element (Invalid / Inappropriate / AsRoot / DocumentNotRoot / TBodyWarn).
Outcome classify(const Context& c);

// MarkedContentIsInLegalPosition: outcome for marked content (MCID / MCR kids) directly in an element of `type`.
enum class ContentPos : std::uint8_t { Ok, Warn, Fail };
ContentPos own_content(std::string_view type);

// Type sets of the model (exposed for tests and the module).
bool is_pdf17_type(std::string_view t);          // PDF 1.7 standard structure type (ISO 32000-1 14.8.4)
bool is_hn(std::string_view t);                  // H1..H6
bool is_heading(std::string_view t);             // H or H1..H6
int heading_level(std::string_view t);           // 1..6 for Hn, 0 for H, -1 otherwise
bool is_inline(std::string_view t);              // inline-level elements PAC checks against grouping parents
bool is_grouping(std::string_view t);            // grouping elements (PAC: Caption and BlockQuote included)
bool is_paragraph_like(std::string_view t);      // P, H, Hn, Span: inline content only
bool is_transparent(std::string_view t);         // NonStruct, Private: looked through as parents
bool is_strict_container(std::string_view t);    // parents with a fixed content model (TR, L, Table, ...)
bool strict_allows(std::string_view parent, std::string_view kid);
bool has_fixed_parent(std::string_view t);       // TR, TH, TD, THead, TBody, TFoot, LI, LBody, TOCI, RB, ...
bool fixed_parent_allows(std::string_view t, std::string_view parent);

// Canonical rule id of an outcome for a type ("" when the type has no such condition).
std::string rule_id(std::string_view type, Cond cond);

}  // namespace eqx::pac
