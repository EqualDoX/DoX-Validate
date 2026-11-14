#include "report/output.h"

#include <cstdio>
#include <cstring>
#include <map>

#include "rules/module.h"
#include "util/json.h"

namespace eqx {

namespace {

const char* severity_name(std::uint8_t s) { return s == 0 ? "error" : "warning"; }
const char* kind_name(std::uint8_t k) { return k == 0 ? "violation" : k == 1 ? "warning" : "recommendation"; }

// Plain-language reading of a combined status (none of them is a conformance claim).
const char* combined_meaning(const std::string& s) {
    if (s == "both")
        return "no automated failure for either target; manual evaluation is still required for both; not a "
               "conformance claim";
    if (s == "pdfua_only") return "the WCAG target fails; the PDF/UA target has no automated failure; manual evaluation required";
    if (s == "wcag_only") return "the PDF/UA target fails; the WCAG target has no automated failure; manual evaluation required";
    if (s == "neither") return "both targets fail";
    if (s == "pdfua_fail_wcag_incomplete") return "the PDF/UA target fails; the WCAG target could not be fully evaluated";
    if (s == "wcag_fail_pdfua_incomplete") return "the WCAG target fails; the PDF/UA target could not be fully evaluated";
    return "at least one target could not be fully evaluated";
}

// The ids of the rules [first, last) as array elements. The catalogue has them rendered ("ID","ID",...): compact
// output copies the run in one piece, indented output writes one element per line.
void rule_run(JsonWriter& w, RuleIndex first, RuleIndex last) {
    if (!w.pretty()) {
        w.raw(catalog_ids_json(first, last));
        return;
    }
    for (RuleIndex r = first; r < last; ++r) w.raw(catalog_ids_json(r, r + 1));
}

void rule_list(JsonWriter& w, const std::vector<RuleIndex>& rules) {
    w.begin_array();
    for (std::size_t i = 0; i < rules.size();) {
        std::size_t j = i + 1;
        while (j < rules.size() && rules[j] == rules[j - 1] + 1) ++j;   // consecutive rules: one run
        rule_run(w, rules[i], rules[j - 1] + 1);
        i = j;
    }
    w.end_array();
}

}  // namespace

std::vector<int> basis_targets(const ResultBasis& basis, const Result& r) {
    switch (basis.mode) {
        case ResultBasis::Mode::Any: return {};
        case ResultBasis::Mode::Targets: return basis.targets;
        case ResultBasis::Mode::Auto: break;
    }
    return {r.declared_pdfua_part == 2 ? T_PDFUA2 : T_PDFUA1, T_WCAG21_AA};
}

Summary summarize(const Result& r, const ResultBasis& basis) {
    const Catalog& cat = Catalog::get();
    Summary s;
    s.targets = basis_targets(basis, r);
    if (!r.processed) {
        s.incomplete_targets = s.targets;
        return s;
    }
    for (const FindingRecord* f : r.findings.sorted()) {
        if (cat.rule(f->rule).severity == 0) {
            ++s.error_rules;
            s.error_occurrences += f->occurrences;
        } else {
            ++s.warning_rules;
            s.warning_occurrences += f->occurrences;
        }
    }
    s.rules_incomplete = r.incomplete_reasons.size();
    for (RuleIndex i = 0; i < cat.size(); ++i) {
        const bool evaluated = r.evaluated[i];
        if (evaluated && !r.incomplete[i]) {   // the usual case: nothing else to count for this rule
            ++s.rules_evaluated;
            continue;
        }
        if (!cat.rule(i).required || !r.in_selection(i)) continue;
        if (!evaluated) ++s.rules_incomplete;   // "not implemented"
        ++s.required_rules_incomplete;          // not evaluated, or incomplete
    }
    s.rules_not_applicable = r.not_applicable.size();
    bool incomplete = false, failed = false;
    if (basis.mode == ResultBasis::Mode::Any) {
        incomplete = s.required_rules_incomplete > 0;
        failed = s.error_rules > 0;
    } else {
        for (int t : s.targets) {
            const TargetAssessment& ta = r.assessments.single[t];
            if (!ta.unevaluated_rules.empty()) s.incomplete_targets.push_back(t);
            if (ta.status == "fail") s.failed_targets.push_back(t);
        }
        incomplete = !s.incomplete_targets.empty();
        failed = !s.failed_targets.empty();
    }
    if (incomplete) {
        s.result = "incomplete";
        s.exit_code = 2;
    } else if (failed) {
        s.result = "errors_found";
        s.exit_code = 1;
    } else {
        s.result = "no_errors_found";
        s.exit_code = 0;
    }
    return s;
}

std::string render_json(const Result& r, bool pretty, const ResultBasis& basis) {
    const Catalog& cat = Catalog::get();
    const Summary sum = summarize(r, basis);
    JsonWriter w(pretty);
    // The list of evaluated rules is most of a typical report (one id per catalogue rule).
    w.reserve(catalog_ids_json(0, static_cast<RuleIndex>(cat.size())).size() + 32768);
    w.begin_object();
    w.key("schema").value("eqx-validator-output/1");
    w.key("validator").begin_object();
    w.key("name").value(kValidatorName);
    w.key("version").value(kValidatorVersion);
    w.key("catalog_sha256").value(catalog_sha256());
    w.key("rule_semantics").value("veraPDF 1.30.2 (ua1, ua2, wt1a, wt1r) and PAC 26.1.0.0 (PDF/UA, WCAG, Quality); "
                                  "reference tools are not run");
    w.end_object();
    w.key("profiles").begin_array();
    for (int t = 0; t < T_COUNT; ++t) w.value(target_name(t));
    w.end_array();
    w.key("input").begin_object();
    w.key("path").value(r.path);
    w.key("sha256").value(r.sha256);
    w.key("bytes").value(static_cast<std::int64_t>(r.bytes));
    if (r.processed) {
        w.key("pdf_version").value(r.pdf_version);
        w.key("pages").value(r.pages);
        w.key("tagged").value(r.tagged);
        w.key("pdfua_part");
        if (r.declared_pdfua_part) w.value(*r.declared_pdfua_part);
        else w.null();
    }
    w.end_object();
    if (!r.rule_prefix.empty()) w.key("rule_selection").value(r.rule_prefix);   // --rules: only these rules were evaluated
    w.key("processed").value(r.processed);
    if (r.processed) w.key("file_repaired").value(r.file_repaired);
    if (!r.processed) w.key("error").value(r.error);
    w.key("summary").begin_object();
    w.key("result").value(sum.result);
    w.key("exit_code").value(sum.exit_code);
    w.key("basis").value(basis.mode == ResultBasis::Mode::Any ? "any" : basis.mode == ResultBasis::Mode::Auto ? "auto" : "targets");
    auto targets = [&](const char* key, const std::vector<int>& ts) {
        w.key(key).begin_array();
        for (int t : ts) w.value(target_name(t));
        w.end_array();
    };
    targets("targets", sum.targets);
    targets("failed_targets", sum.failed_targets);
    targets("incomplete_targets", sum.incomplete_targets);
    w.key("error_rules").value(static_cast<std::int64_t>(sum.error_rules));
    w.key("warning_rules").value(static_cast<std::int64_t>(sum.warning_rules));
    w.key("error_occurrences").value(static_cast<std::int64_t>(sum.error_occurrences));
    w.key("warning_occurrences").value(static_cast<std::int64_t>(sum.warning_occurrences));
    w.key("rules_evaluated").value(static_cast<std::int64_t>(sum.rules_evaluated));
    w.key("rules_incomplete").value(static_cast<std::int64_t>(sum.rules_incomplete));
    w.key("rules_not_applicable").value(static_cast<std::int64_t>(sum.rules_not_applicable));
    w.end_object();

    w.key("findings").begin_array();
    for (const FindingRecord* f : r.findings.sorted()) {
        const RuleInfo& ri = cat.rule(f->rule);
        w.begin_object();
        w.key("rule").value(ri.id);
        w.key("severity").value(severity_name(ri.severity));
        w.key("kind").value(kind_name(ri.kind));
        w.key("title").value(ri.title);
        w.key("explanation").value(ri.detection);
        w.key("module").value(owner_of(f->rule));
        w.key("occurrences").value(static_cast<std::int64_t>(f->occurrences));
        w.key("non_pac_occurrences").value(static_cast<std::int64_t>(f->non_pac_occurrences));
        w.key("locations").begin_array();
        for (const Location& l : f->locations) {
            w.begin_object();
            w.key("page");
            if (l.page > 0) w.value(l.page);
            else w.null();
            w.key("obj");
            if (l.obj.valid()) w.value(l.obj.str());
            else w.null();
            if (!l.detail.empty()) w.key("detail").value(l.detail);
            w.end_object();
        }
        w.end_array();
        w.key("references").raw(ri.references_json);
        w.end_object();
    }
    w.end_array();

    w.key("rules_evaluated").begin_array();
    if (r.processed) {
        const RuleIndex n = static_cast<RuleIndex>(cat.size());
        for (RuleIndex i = 0; i < n;) {
            if (!(r.evaluated[i] && !r.incomplete[i])) {
                ++i;
                continue;
            }
            RuleIndex j = i + 1;
            while (j < n && r.evaluated[j] && !r.incomplete[j]) ++j;
            rule_run(w, i, j);
            i = j;
        }
    }
    w.end_array();
    w.key("rules_incomplete").begin_array();
    for (const auto& [rule, reason] : r.incomplete_reasons) {
        w.begin_object();
        w.key("rule").value(cat.rule(rule).id);
        w.key("reason").value(reason);
        w.end_object();
    }
    if (r.processed) {
        for (RuleIndex i = 0; i < cat.size(); ++i)
            if (!r.evaluated[i] && cat.rule(i).required && r.in_selection(i)) {
                w.begin_object();
                w.key("rule").value(cat.rule(i).id);
                w.key("reason").value("not implemented");
                w.end_object();
            }
    }
    w.end_array();

    w.key("rules_not_applicable").begin_array();
    for (RuleIndex rule : r.not_applicable) {
        w.begin_object();
        w.key("rule").value(cat.rule(rule).id);
        w.key("reason").value(r.not_applicable_reason);
        w.end_object();
    }
    w.end_array();

    w.key("assessments").begin_object();
    for (int t = 0; t < T_COUNT; ++t) {
        const TargetAssessment& ta = r.assessments.single[t];
        w.key(target_name(t)).begin_object();
        w.key("status").value(ta.status);
        w.key("failed_rules");
        rule_list(w, ta.failed_rules);
        w.key("warning_rules");
        rule_list(w, ta.warning_rules);
        w.key("unevaluated_rules");
        rule_list(w, ta.unevaluated_rules);
        w.key("manual_required").begin_array();
        for (const auto& m : ta.manual_required) w.value(m);
        w.end_array();
        w.end_object();
    }
    for (int u = 0; u < 2; ++u)
        for (int wi = 0; wi < 9; ++wi) {
            std::string key = std::string(target_name(u)) + "+" + target_name(T_WCAG20_A + wi);
            w.key(key).begin_object();
            w.key("status").value(r.assessments.combined[u][wi]);
            w.key("meaning").value(combined_meaning(r.assessments.combined[u][wi]));
            w.end_object();
        }
    w.end_object();

    w.key("diagnostics").begin_array();
    for (const auto& d : r.diagnostics) w.value(d);
    w.end_array();
    w.key("timing_ms").begin_object();
    w.key("hash").value(r.hash_ms);
    w.key("parse").value(r.parse_ms);
    w.key("structure").value(r.structure_ms);
    w.key("interpret").value(r.content_ms);
    for (const auto& m : r.module_ms) w.key(m.module).value(m.ms);
    w.key("assess").value(r.assess_ms);
    w.key("total").value(r.total_ms);
    w.end_object();
    w.end_object();
    return w.take();
}

std::string render_text(const Result& r, bool verbose, const ResultBasis& basis) {
    const Catalog& cat = Catalog::get();
    const Summary sum = summarize(r, basis);
    std::string out;
    char buf[512];
    out += r.path + "\n";
    if (!r.processed) {
        out += "  Result: NOT PROCESSED - " + r.error + "\n";
        out += "  No target could be assessed (every target: incomplete).\n";
        for (const auto& d : r.diagnostics) out += "  diagnostic: " + d + "\n";
        return out;
    }
    std::snprintf(buf, sizeof buf, "  PDF %s, %d page(s), %s, %.1f ms%s\n", r.pdf_version.c_str(), r.pages,
                  r.tagged ? "tagged" : "untagged", r.total_ms, r.file_repaired ? ", structure repaired on reading" : "");
    out += buf;
    auto names = [](const std::vector<int>& ts) {
        std::string s;
        for (int t : ts) s += (s.empty() ? "" : ", ") + std::string(target_name(t));
        return s;
    };
    if (basis.mode == ResultBasis::Mode::Any) {
        if (sum.exit_code == 2)
            std::snprintf(buf, sizeof buf,
                          "  Result: INCOMPLETE - %zu required rule(s) could not be evaluated; %zu rule(s) with errors, "
                          "%zu with warnings\n",
                          sum.required_rules_incomplete, sum.error_rules, sum.warning_rules);
        else if (sum.exit_code == 1)
            std::snprintf(buf, sizeof buf, "  Result: ERRORS FOUND - %zu rule(s) with errors (%llu occurrence(s)), %zu with warnings\n",
                          sum.error_rules, static_cast<unsigned long long>(sum.error_occurrences), sum.warning_rules);
        else
            std::snprintf(buf, sizeof buf,
                          "  Result: NO ERRORS FOUND - %zu rule(s) with warnings; manual evaluation is still required\n",
                          sum.warning_rules);
        out += buf;
    } else {
        std::string line = "  Result (" + names(sum.targets) + "): ";
        if (sum.exit_code == 2) {
            line += "INCOMPLETE - not fully evaluated: " + names(sum.incomplete_targets);
            if (!sum.failed_targets.empty()) line += "; failing: " + names(sum.failed_targets);
        } else if (sum.exit_code == 1) {
            line += "ERRORS FOUND - failing: " + names(sum.failed_targets);
        } else {
            line += "NO ERRORS FOUND - automated checks only; manual evaluation is still required";
        }
        std::snprintf(buf, sizeof buf, " (%zu rule(s) with errors, %zu with warnings in all targets)\n", sum.error_rules,
                      sum.warning_rules);
        out += line + buf;
    }
    out += "  Assessments (automated checks only; manual evaluation is always required):\n";
    bool shown_by_default[T_COUNT] = {true, true, true, false, false, false, false,
                                      false, true, false, false, true, false};
    for (int t : sum.targets) shown_by_default[t] = true;
    std::size_t hidden = 0;
    for (int t = 0; t < T_COUNT; ++t) {
        if (!verbose && !shown_by_default[t]) {
            ++hidden;
            continue;
        }
        const TargetAssessment& ta = r.assessments.single[t];
        std::snprintf(buf, sizeof buf, "    %-26s %-32s failed %zu, warnings %zu, unevaluated %zu, manual %zu\n",
                      target_name(t), ta.status.c_str(), ta.failed_rules.size(), ta.warning_rules.size(),
                      ta.unevaluated_rules.size(), ta.manual_required.size());
        out += buf;
    }
    for (auto [label, s] : {std::pair<const char*, const std::string*>{"pdfua-1+wcag-2.1-AA", &r.assessments.combined[0][4]},
                            {"pdfua-2+wcag-2.2-AA", &r.assessments.combined[1][7]}}) {
        out += "    " + std::string(label) + std::string(label[0] ? 26 - std::strlen(label) : 0, ' ') + " " + *s +
               " (" + combined_meaning(*s) + ")\n";
    }
    auto recs = r.findings.sorted();
    std::snprintf(buf, sizeof buf, "  Findings: %zu rule(s) failed (%zu error, %zu warning)\n", recs.size(), sum.error_rules,
                  sum.warning_rules);
    out += buf;
    for (int severity = 0; severity < 2; ++severity) {
        for (const FindingRecord* f : recs) {
            const RuleInfo& ri = cat.rule(f->rule);
            if ((ri.severity == 0) != (severity == 0)) continue;
            out += std::string("    [") + severity_name(ri.severity) + "] " + ri.id + "  " + ri.title + "\n";
            std::size_t shown = 0;
            for (const Location& l : f->locations) {
                if (!verbose && shown >= 3) {
                    std::snprintf(buf, sizeof buf, "        ... %zu more location(s), %llu occurrence(s)\n",
                                  f->locations.size() - shown, static_cast<unsigned long long>(f->occurrences));
                    out += buf;
                    break;
                }
                std::string loc = "        at";
                if (l.page > 0) loc += " page " + std::to_string(l.page);
                if (l.obj.valid()) loc += " obj " + l.obj.str();
                if (!l.detail.empty()) loc += ": " + l.detail;
                out += loc + "\n";
                ++shown;
            }
        }
    }
    std::size_t inc = r.incomplete_reasons.size();
    std::size_t notimpl = 0;
    for (RuleIndex i = 0; i < cat.size(); ++i)
        if (!r.evaluated[i] && cat.rule(i).required && r.in_selection(i)) ++notimpl;
    if (!r.not_applicable.empty()) {
        std::snprintf(buf, sizeof buf, "  Not applicable: %zu PAC-derived rule(s) (%s)\n", r.not_applicable.size(),
                      r.not_applicable_reason.c_str());
        out += buf;
    }
    if (inc || notimpl) {
        std::snprintf(buf, sizeof buf, "  Not evaluated: %zu incomplete, %zu not implemented\n", inc, notimpl);
        out += buf;
        std::size_t listed = 0;
        for (const auto& [rule, reason] : r.incomplete_reasons) {
            if (!verbose && listed >= 5) {
                std::snprintf(buf, sizeof buf, "        ... %zu more (-v lists all)\n", inc - listed);
                out += buf;
                break;
            }
            out += std::string("        ") + cat.rule(rule).id + ": " + reason + "\n";
            ++listed;
        }
    }
    for (const auto& d : r.diagnostics) out += "  diagnostic: " + d + "\n";
    if (!verbose && hidden) {
        std::snprintf(buf, sizeof buf, "  (%zu more target(s) and all locations: -v; full report: --format json)\n", hidden);
        out += buf;
    }
    return out;
}

}  // namespace eqx
