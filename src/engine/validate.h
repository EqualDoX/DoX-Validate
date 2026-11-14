// Validation of one file: parse once, interpret content once, run every rule module, assess.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "report/assess.h"
#include "rules/findings.h"

namespace eqx {

#ifndef DOX_VALIDATE_VERSION
#define DOX_VALIDATE_VERSION "0.0.0-dev"
#endif

inline constexpr const char* kValidatorName = "dox-validate";
inline constexpr const char* kValidatorVersion = DOX_VALIDATE_VERSION;

struct Options {
    std::size_t max_locations = 1000;
    bool timings = true;
    bool reference_dedup = false;   // see ValidationContext::reference_dedup
    // Evaluate only the rules whose id starts with this prefix and run only the modules that own one of them
    // (empty = every rule). The other rules are left unevaluated, never passed.
    std::string rule_prefix;
    // More pages than this: the file is not processed ("too many pages"); 0 = no limit.
    int max_pages = 0;
    // The process exits as soon as this file is reported (a one-file run): the document, its models and qpdf's
    // objects are then left to the operating system instead of being destroyed one by one, which takes about a
    // tenth of the run on a large document. Never set when another validation follows, or in builds that must see
    // everything destroyed (sanitizers, -DDOX_FULL_TEARDOWN).
    bool process_exits_after = false;
};

// The rules a validation evaluates: every rule (empty) or those whose id starts with `prefix`.
std::vector<bool> rule_selection(const std::string& prefix);

struct ModuleTiming {
    std::string module;
    double ms = 0;
};

struct Result {
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    bool processed = false;
    std::string error;              // why the file could not be processed
    std::string pdf_version;
    int pages = 0;
    bool tagged = false;
    bool file_repaired = false;     // qpdf reconstructed or replaced something: the rules saw a repair, not the file
    std::optional<int> declared_pdfua_part;   // pdfuaid:part of the catalog's XMP metadata, when declared
    Findings findings;
    std::vector<bool> evaluated;    // per rule index
    std::vector<bool> incomplete;   // per rule index
    std::vector<std::pair<RuleIndex, std::string>> incomplete_reasons;
    std::vector<RuleIndex> not_applicable;   // evaluated, outside the rule's defined scope (PAC rules on PDF 2.0)
    std::string not_applicable_reason;
    Assessments assessments;
    std::vector<std::string> diagnostics;   // internal problems (module exceptions, parse warnings)
    std::string rule_prefix;                // Options::rule_prefix of this validation
    std::vector<bool> selected;             // per rule index: in the selection (empty = every rule)
    bool in_selection(RuleIndex r) const { return selected.empty() || selected[r]; }
    double hash_ms = 0, parse_ms = 0, structure_ms = 0, content_ms = 0, assess_ms = 0, total_ms = 0;
    std::vector<ModuleTiming> module_ms;
    explicit Result(std::size_t max_locations) : findings(max_locations) {}
};

Result validate_file(const std::string& path, const Options& opt);

}  // namespace eqx
