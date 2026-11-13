// Rule modules: each owns a fixed set of canonical rules (rules/ownership.cpp) and evaluates all of them
// on every processed document. A module that cannot evaluate a rule on a document reports it as
// incomplete with a reason; an exception in a module marks all of its rules incomplete. Nothing is ever
// silently counted as passed.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/content.h"
#include "core/document.h"
#include "core/structure.h"
#include "fonts/font.h"
#include "rules/catalog.h"
#include "rules/findings.h"

namespace eqx {

struct Incomplete {
    RuleIndex rule;
    std::string reason;
};

struct ValidationContext {
    Document& doc;
    StructTree& st;
    FontRegistry& fonts;
    ContentModel& content;
    Findings& findings;
    std::vector<Incomplete> incomplete;
    // Reproduce veraPDF's once-per-object validation of content streams (skip StreamInstance::repeat).
    // Off by default: eqx evaluates every painting of every content stream.
    bool reference_dedup = false;
    // PAC 26.1 defines its checks for PDF up to 1.7 only ("PAC currently fully supports documents up to and
    // including PDF 1.7"); it applies PDF/UA-1 / PDF 1.7 semantics to PDF 2.0 files. False for PDF 2.0
    // documents: the PAC-only rules (EQX-PAC-*, EQX-PACQ-*) are then not applicable (the engine removes
    // their findings and lists them), and merged rules must apply only their veraPDF definition.
    bool pac_applicable = true;
    // The rules this validation evaluates (Options::rule_prefix), per rule index; empty = every rule. A module
    // may skip work that serves only unselected rules; the engine drops their findings either way.
    std::vector<bool> selected;
    bool selects(RuleIndex rule) const { return selected.empty() || selected[rule]; }
    // pdfuaid:part of the catalog's XMP packet (set by the document module; empty when not declared or not read).
    std::optional<int> declared_pdfua_part;

    void report(RuleIndex rule, int page_index, ObjRef obj, std::string detail = {}) {
        findings.add(rule, page_number(page_index), obj, std::move(detail));
    }
    void mark_incomplete(RuleIndex rule, std::string reason) { incomplete.push_back({rule, std::move(reason)}); }
};

class RuleModule {
public:
    virtual ~RuleModule() = default;
    virtual const char* name() const = 0;
    // Called before the content streams are interpreted; register observers for glyph/operator events.
    virtual void prepare(ValidationContext&, std::vector<ContentObserver*>&) {}
    // Called once the content model is complete. Must evaluate every rule of owned_rules(name()).
    virtual void run(ValidationContext& ctx) = 0;
};

// Canonical rules owned by a module (single source: rules/ownership.cpp).
const std::vector<RuleIndex>& owned_rules(std::string_view module);
// Module owning a rule ("" when none).
const std::string& owner_of(RuleIndex rule);
std::vector<std::string> module_names();

// Every module, in execution order (rules/registry.cpp).
std::vector<std::unique_ptr<RuleModule>> create_modules();

}  // namespace eqx
