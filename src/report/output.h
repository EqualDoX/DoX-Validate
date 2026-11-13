// Report rendering: eqx-validator-output/1 JSON (docs/JSON_OUTPUT.md) and a text summary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "engine/validate.h"

namespace eqx {

// What decides the overall result and the exit status (--target).
//   Auto      the PDF/UA part the document declares (pdfuaid:part 2: pdfua-2, otherwise pdfua-1) and wcag-2.1-AA
//   Targets   the listed targets
//   Any       every error-severity finding of the evaluated rules, whatever target it concerns (the behaviour of
//             eqx-validate, the default under that name and with --rules)
struct ResultBasis {
    enum class Mode { Auto, Targets, Any } mode = Mode::Auto;
    std::vector<int> targets;   // Mode::Targets
};

// The targets that decide for this file (empty for Mode::Any).
std::vector<int> basis_targets(const ResultBasis& basis, const Result& r);

// The overall result of one file, as the exit status reports it.
//   not_processed    the file could not be opened or read (exit 2)
//   incomplete       processed, but a deciding target (Any: a required rule) could not be fully evaluated (exit 2;
//                    findings are still listed)
//   errors_found     a deciding target fails (Any: an error-severity finding) (exit 1)
//   no_errors_found  no deciding target fails and all were fully evaluated (exit 0); warnings do not change it, and
//                    manual evaluation is still required: this is not a conformance claim
struct Summary {
    const char* result = "not_processed";
    int exit_code = 2;
    std::vector<int> targets, failed_targets, incomplete_targets;   // empty for Mode::Any
    std::size_t error_rules = 0, warning_rules = 0;
    std::uint64_t error_occurrences = 0, warning_occurrences = 0;
    std::size_t rules_evaluated = 0, rules_incomplete = 0, required_rules_incomplete = 0, rules_not_applicable = 0;
};

Summary summarize(const Result& r, const ResultBasis& basis);

std::string render_json(const Result& r, bool pretty, const ResultBasis& basis);
std::string render_text(const Result& r, bool verbose, const ResultBasis& basis);

}  // namespace eqx
