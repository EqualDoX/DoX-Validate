#include "engine/validate.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFCryptoProvider.hh>

#if __has_include(<qpdf/global.hh>)
#include <qpdf/global.hh>   // qpdf >= 12.3
#define EQX_QPDF_LIMIT_ERRORS 1
#endif

#if __has_include(<sys/single_threaded.h>)
#include <sys/single_threaded.h>   // glibc >= 2.32
#define EQX_SINGLE_THREADED __libc_single_threaded
#endif

#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <set>

#include "core/content.h"
#include "core/document.h"
#include "core/source_file.h"
#include "core/structure.h"
#include "fonts/font.h"
#include "rules/module.h"
#include "util/sha256.h"
#include "util/unread.h"

namespace eqx {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// qpdf initialises process-wide state lazily and without synchronisation (its crypto provider, used for hashing
// and decryption, and objects created by the first QPDF instance). With --jobs several threads hit that first use
// at once; ThreadSanitizer reported the races and they could corrupt the heap. The first use happens here, once,
// before any validation of this process proceeds. A process that never had a second thread has no such race and
// skips it (the dummy document costs more than 1 % of a small document's run).
void init_qpdf_once() {
#ifdef EQX_SINGLE_THREADED
    if (EQX_SINGLE_THREADED) return;
#endif
    static std::once_flag once;
    std::call_once(once, [] {
        auto impl = QPDFCryptoProvider::getImpl();
        impl->SHA2_init(256);
        impl->SHA2_update(reinterpret_cast<unsigned char const*>("eqx"), 3);
        impl->SHA2_finalize();
        (void)impl->SHA2_digest();
        static const char kPdf[] =
            "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[]/Count 0>>endobj\n"
            "trailer<</Root 1 0 R>>\n%%EOF\n";
        try {
            QPDF q;
            q.setSuppressWarnings(true);
            q.processMemoryFile("eqx-init", kPdf, std::strlen(kPdf));
            (void)q.getRoot();
        } catch (std::exception&) {
        }
    });
}

// Messages about a file qpdf read as written (both qpdf 12.2 and 12.4 spellings). Everything else means that what the
// rules saw may differ from what the file says: a reconstructed table, a replaced object, a damaged stream.
bool harmless_qpdf_warning(const std::string& w) {
    return w.find("a common error handled correctly") != std::string::npos ||
           w.find("is not one plus the highest object number") != std::string::npos ||
           w.find("): object has offset 0") != std::string::npos ||
           // a deflate stream without its end marker: found in 2 of 400 real converted documents, the data decodes; veraPDF
           // reads such a stream as well (a stream that really breaks off in the data gets "error decoding stream data")
           w.find("input stream is complete but output may still be valid") != std::string::npos;
}

}  // namespace

namespace {

// Everything one validation builds. Members are destroyed in reverse order: modules, content, fonts, structure,
// document.
struct Models {
    std::unique_ptr<Document> doc;
    StructTree st;
    std::optional<FontRegistry> fonts;
    ContentModel content;
    std::vector<std::unique_ptr<RuleModule>> modules;
};

// Ends a validation: destroys the models, unless the process exits right after it (Options::process_exits_after).
struct ModelsEnd {
    bool abandon = false;
    void operator()(Models* m) const {
        if (!abandon) delete m;
    }
};

Result validate_once(const std::string& path, const Options& opt, bool recover, bool* again);

}  // namespace

std::vector<bool> rule_selection(const std::string& prefix) {
    if (prefix.empty()) return {};
    const Catalog& cat = Catalog::get();
    std::vector<bool> sel(cat.size(), false);
    for (RuleIndex r = 0; r < cat.size(); ++r) sel[r] = std::string_view(cat.rule(r).id).rfind(prefix, 0) == 0;
    return sel;
}

Result validate_file(const std::string& path, const Options& opt) {
    init_qpdf_once();
    bool again = false;
    Result r = validate_once(path, opt, false, &again);
    // qpdf met the damage only after the rules had started reading: read again through the recovered view.
    if (again) return validate_once(path, opt, true, &again);
    return r;
}

namespace {

Result validate_once(const std::string& path, const Options& opt, bool recover, bool* again) {
    take_unread();
    auto t0 = std::chrono::steady_clock::now();
    const Catalog& cat = Catalog::get();
    Result res(opt.max_locations);
    res.path = path;
    res.evaluated.assign(cat.size(), false);
    res.incomplete.assign(cat.size(), false);
    res.rule_prefix = opt.rule_prefix;
    res.selected = rule_selection(opt.rule_prefix);
    // One open file serves the hash and the parser (core/source_file.h); what is no readable regular file goes
    // through stdio as before.
    std::shared_ptr<SourceFile> source = SourceFile::open(path);
    res.sha256 = source ? source->sha256_hex() : sha256_file(path);
    res.hash_ms = ms_since(t0);

    OpenStatus status;
    std::string error;
    std::unique_ptr<Models, ModelsEnd> models(new Models());
    std::unique_ptr<Document>& doc = models->doc;
    doc = Document::open(path, &status, &error, recover, std::move(source));
    res.parse_ms = ms_since(t0);
    if (!doc) {
        res.processed = false;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        if (!size_error) res.bytes = static_cast<std::uint64_t>(size);
        switch (status) {
            case OpenStatus::NotFound: res.error = "file not found or not readable"; break;
            case OpenStatus::PasswordRequired:
                res.error = "encrypted with a user password: the document cannot be opened (" + error + ")";
                break;
            default: res.error = "the file cannot be parsed as PDF (" + error + ")"; break;
        }
        res.assessments = assess(res.findings, res.evaluated, res.incomplete, false);
        res.total_ms = ms_since(t0);
        return res;
    }
    res.bytes = doc->file_size();
    res.pdf_version = doc->header_version();
    res.pages = static_cast<int>(doc->pages().size());
    if (opt.max_pages > 0 && res.pages > opt.max_pages) {
        res.processed = false;
        res.error = "too many pages: " + std::to_string(res.pages) + ", the limit is " + std::to_string(opt.max_pages);
        res.assessments = assess(res.findings, res.evaluated, res.incomplete, false);
        res.total_ms = ms_since(t0);
        return res;
    }

    StructTree& st = models->st;
    FontRegistry& fonts = models->fonts.emplace(*doc);
    ContentModel& content = models->content;
    const std::string version = doc->effective_version();
    ValidationContext ctx{*doc, st, fonts, content, res.findings, {}, opt.reference_dedup,
                          version.empty() || version[0] < '2', res.selected, {}};
    auto tcm = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<RuleModule>>& modules = models->modules;
    modules = create_modules();
    if (!res.selected.empty()) {   // only the modules that own a selected rule
        std::erase_if(modules, [&](const std::unique_ptr<RuleModule>& m) {
            for (RuleIndex r : owned_rules(m->name()))
                if (res.selected[r]) return false;
            return true;
        });
    }
    if (double cm = ms_since(tcm); cm > 1.0) res.module_ms.push_back({"create_modules", cm});
    std::vector<bool> module_failed(modules.size(), false);
    auto ts = std::chrono::steady_clock::now();
    bool structure_failed = false;
    try {
        st.build(*doc);
        res.tagged = st.present();
    } catch (std::exception& e) {
        res.diagnostics.push_back(std::string("structure tree: ") + e.what());
        structure_failed = true;
    }
    res.structure_ms = ms_since(ts);
    std::vector<ContentObserver*> observers;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        auto tp = std::chrono::steady_clock::now();
        try {
            modules[i]->prepare(ctx, observers);
        } catch (std::exception& e) {
            module_failed[i] = true;
            res.diagnostics.push_back(std::string(modules[i]->name()) + ": prepare failed: " + e.what());
        }
        double pm = ms_since(tp);
        if (pm > 1.0) res.module_ms.push_back({std::string(modules[i]->name()) + ".prepare", pm});
    }
    auto tc = std::chrono::steady_clock::now();
    bool content_failed = false;
    try {
        interpret_document(*doc, st, fonts, content, observers);
    } catch (std::exception& e) {
        content_failed = true;
        res.diagnostics.push_back(std::string("content interpretation failed: ") + e.what());
    }
    res.content_ms = ms_since(tc);
    std::vector<std::string> module_names;
    for (const auto& m : modules) module_names.emplace_back(m->name());
    for (std::size_t i = 0; i < modules.size(); ++i) {
        if (!module_failed[i]) {
            auto tm = std::chrono::steady_clock::now();
            try {
                modules[i]->run(ctx);
            } catch (std::exception& e) {
                module_failed[i] = true;
                res.diagnostics.push_back(module_names[i] + ": " + e.what());
            }
            res.module_ms.push_back({module_names[i], ms_since(tm)});
        }
        // A module's tables (and its observers' state) are not read after its run: released here, they do not add to
        // what the modules still to run build up.
        modules[i].reset();
    }
    // qpdf's repairs (damaged cross-reference table, broken objects or streams) are reported, so a result based
    // on a reconstructed file is recognisable as such (veraPDF refuses some of these files).
    {
        std::vector<std::string> warns = doc->warnings();
        std::set<std::string> seen;
        std::size_t shown = 0;
        for (std::string w : warns) {
            if (w.rfind(path, 0) == 0) w.erase(0, path.size());
            while (!w.empty() && (w[0] == ':' || w[0] == ' ')) w.erase(0, 1);
            if (!seen.insert(w).second) continue;
            if (!harmless_qpdf_warning(w)) res.file_repaired = true;
            if (++shown <= 10) res.diagnostics.push_back("file structure repaired by qpdf: " + w);
        }
        if (shown > 10)
            res.diagnostics.push_back("file structure: " + std::to_string(shown - 10) + " further qpdf warning(s)");
    }
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const std::string& name = module_names[i];
        for (RuleIndex r : owned_rules(name)) {
            res.evaluated[r] = true;
            if (module_failed[i]) {
                res.incomplete[r] = true;
                res.incomplete_reasons.emplace_back(r, "module " + name + " failed on this document");
            }
        }
    }
    if (content_failed || content.budget_exceeded) {
        for (const char* m : {"content", "fonts", "pac_quality"})
            for (RuleIndex r : owned_rules(m))
                if (!res.incomplete[r]) {
                    res.incomplete[r] = true;
                    res.incomplete_reasons.emplace_back(r, content.budget_exceeded
                                                               ? "content interpretation stopped at the operator budget"
                                                               : "content streams could not be interpreted");
                }
    }
    // What was not read cannot pass: an undecodable content stream or a form nested beyond the limit leaves content
    // unseen, a structure tree that could not be built or was cut off leaves elements unseen.
    bool undecodable = false;
    for (const StreamInstance& s : content.streams) undecodable = undecodable || !s.decode_error.empty();
    if (undecodable || content.form_depth_exceeded) {
        const char* why = undecodable ? "a content stream could not be decoded" : "a form is nested beyond the interpreter's limit";
        for (const char* m : {"content", "fonts", "pac_quality"})
            for (RuleIndex r : owned_rules(m))
                if (!res.incomplete[r]) {
                    res.incomplete[r] = true;
                    res.incomplete_reasons.emplace_back(r, why);
                }
    }
    if (structure_failed || st.truncated()) {
        for (const char* m : {"structure", "tables", "containment", "pac_structure"})
            for (RuleIndex r : owned_rules(m))
                if (!res.incomplete[r]) {
                    res.incomplete[r] = true;
                    res.incomplete_reasons.emplace_back(r, "the structure tree could not be read completely");
                }
    }
    // An internal limit stopped reading (page tree, name/number tree, content lexer) or qpdf replaced part of the file:
    // nothing can pass on this document. Findings stay; a target with a failed rule still fails.
    if (res.file_repaired) note_unread("qpdf repaired the file structure, the rules saw a reconstruction");
#ifdef EQX_QPDF_LIMIT_ERRORS
    // A qpdf limit (container size, nesting, errors per object) cut something off. The count is per process: with --jobs a
    // neighbour's limit error also opens this file's verdict, which errs on the safe side.
    if (qpdf::global::limit_errors() != 0) note_unread("a limit of the PDF parser (qpdf) cut part of the file off");
#endif
    for (const std::string& why : take_unread()) {
        res.diagnostics.push_back("not read completely: " + why);
        for (RuleIndex r = 0; r < cat.size(); ++r)
            if (!res.incomplete[r]) {
                res.incomplete[r] = true;
                res.incomplete_reasons.emplace_back(r, why);
            }
    }
    res.declared_pdfua_part = ctx.declared_pdfua_part;
    for (const Incomplete& inc : ctx.incomplete) {
        if (inc.rule == kNoRule) {
            res.diagnostics.push_back("incomplete without a rule: " + inc.reason);
            continue;
        }
        if (!res.incomplete[inc.rule]) {
            res.incomplete[inc.rule] = true;
            res.incomplete_reasons.emplace_back(inc.rule, inc.reason);
        }
    }
    if (!ctx.pac_applicable) {
        res.not_applicable_reason = "PAC 26.1 checks are defined for PDF up to 1.7; this is a PDF " + version + " document";
        for (RuleIndex r = 0; r < cat.size(); ++r) {
            std::string_view id = cat.rule(r).id;
            if ((id.rfind("EQX-PAC-", 0) == 0 || id.rfind("EQX-PACQ-", 0) == 0) && res.in_selection(r)) {
                res.findings.erase(r);
                res.not_applicable.push_back(r);
            }
        }
    }
    // A rule selection: what the modules reported for other rules is dropped, those rules stay unevaluated.
    if (!res.selected.empty()) {
        for (RuleIndex r = 0; r < cat.size(); ++r)
            if (!res.selected[r]) {
                res.findings.erase(r);
                res.evaluated[r] = false;
                res.incomplete[r] = false;
            }
        std::erase_if(res.incomplete_reasons, [&](const auto& ir) { return !res.selected[ir.first]; });
    }
    // Rules no module owns are never evaluated (they stay unevaluated, never passed).
    res.processed = true;
    auto ta = std::chrono::steady_clock::now();
    res.assessments = assess(res.findings, res.evaluated, res.incomplete, true);
    res.assess_ms = ms_since(ta);
    res.total_ms = ms_since(t0);
    *again = !doc->recovered() && doc->table_rebuilt();
    models.get_deleter().abandon = opt.process_exits_after && !*again;
    return res;
}

}  // namespace

}  // namespace eqx
