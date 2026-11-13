// dox-validate (EqualDoX PDF Validator): standalone local PDF accessibility validator (PDF/UA-1, PDF/UA-2, WTPDF, WCAG 2.x).
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_RunLength.hh>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/dump.h"
#include "engine/text_model.h"
#include "engine/validate.h"
#include "report/output.h"
#include "rules/module.h"

using namespace eqx;

namespace {

void usage(std::FILE* f) {
    std::fprintf(f,
                 "dox-validate - EqualDoX PDF Validator: automated PDF accessibility checks\n"
                 "               (PDF/UA-1, PDF/UA-2, WTPDF 1.0, WCAG 2.x; https://equaldox.de)\n"
                 "\n"
                 "usage: dox-validate [options] FILE.pdf...\n"
                 "  --format FORMAT     text (default) or json (eqx-validator-output/1, docs/JSON_OUTPUT.md);\n"
                 "                      several files: one JSON object per line (JSON Lines)\n"
                 "  --json, --text      short for --format json / --format text\n"
                 "  --pretty            indented JSON (single file)\n"
                 "  -v, --verbose       text report: every target, every stored location\n"
                 "  --target LIST       what decides the result and the exit status (comma-separated):\n"
                 "                        auto  the PDF/UA part the document declares (pdfuaid:part; PDF/UA-1 when it\n"
                 "                              declares none) and wcag-2.1-AA, the level EN 301 549 references (default)\n"
                 "                        pdfua-1, pdfua-2, wtpdf-1.0-accessibility, wtpdf-1.0-reuse,\n"
                 "                        wcag-{2.0,2.1,2.2}-{A,AA,AAA}  these targets\n"
                 "                        any   every error-severity finding, whatever target it concerns (default\n"
                 "                              with --rules)\n"
                 "                      Every target is always evaluated and reported; --target changes no finding.\n"
                 "  --jobs N            files validated in parallel (default: CPU count)\n"
                 "  --max-locations N   locations stored per rule (default 1000; occurrences are always counted)\n"
                 "  --max-memory-mb N   address space per file in MiB, 0 = no limit (default 4096); what does not fit is\n"
                 "                      reported as not processed or incomplete, never as a pass\n"
                 "  --reference-dedup   validate a reused content stream only at its first use, as veraPDF does\n"
                 "                      (default: every painting is checked; used for differential comparisons)\n"
                 "  --rules PREFIX      evaluate only the rules whose id starts with PREFIX (e.g. EQX-WCAG-) and run only the\n"
                 "                      modules that own them; every other rule stays unevaluated, never passed\n"
                 "  --max-pages N       a file with more than N pages is not processed (\"too many pages\")\n"
                 "  --list-rules        print the rule catalogue (id, severity, module, title) and exit\n"
                 "  --self-check        verify that every catalogue rule has exactly one evaluating module\n"
                 "  --text-model FILE   eqx-text-model/1 JSON (elements, text runs) for semantic checks and exit\n"
                 "  --dump FILE         debug view of the parsed model (structure tree, streams, frames, items)\n"
                 "  --version           print the version\n"
                 "  -h, --help          print this help\n"
                 "\n"
                 "Exit status (the highest over all files):\n"
                 "  0  every file processed; no --target target fails and each was fully evaluated (warnings never\n"
                 "     fail; manual evaluation is still required: this is not a conformance claim)\n"
                 "  1  a --target target fails (machine-checkable violations); with --target any: an error-severity\n"
                 "     finding\n"
                 "  2  a file could not be processed, or a --target target (any: a required rule) could not be fully\n"
                 "     evaluated\n"
                 "  3  usage error\n"
                 "  4  the report could not be written\n"
                 "Reports go to stdout; technical diagnostics go to stderr.\n");
}

// Under its former name eqx-validate (the name existing EqualDoX tooling runs), the defaults are those of that tool:
// --target any, and --version prints "eqx-validator VERSION".
bool invoked_as_legacy_name(const char* argv0) {
    std::string_view a = argv0 ? argv0 : "";
    std::size_t slash = a.find_last_of('/');
    if (slash != std::string_view::npos) a.remove_prefix(slash + 1);
    return a == "eqx-validate";
}

// --target LIST -> basis; false on an unknown name or a mix of any/auto with target names.
bool parse_targets(const std::string& list, ResultBasis& basis) {
    std::vector<int> ts;
    bool any = false, autom = false;
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(',', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? list.size() + 1 : end + 1;
        if (item.empty()) return false;
        if (item == "any") any = true;
        else if (item == "auto") autom = true;
        else {
            int found = -1;
            for (int t = 0; t < T_COUNT; ++t)
                if (item == target_name(t)) found = t;
            if (found < 0) return false;
            if (std::find(ts.begin(), ts.end(), found) == ts.end()) ts.push_back(found);
        }
    }
    if (int(any) + int(autom) + int(!ts.empty()) != 1) return false;
    basis.mode = any ? ResultBasis::Mode::Any : autom ? ResultBasis::Mode::Auto : ResultBasis::Mode::Targets;
    basis.targets = std::move(ts);
    return true;
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;   // the sanitizers' shadow memory does not fit an address-space limit
#else
constexpr bool kSanitized = false;
#endif

// Every validation runs on a stack of this size, whatever RLIMIT_STACK says (8 MiB, 16 MiB, unlimited = 2 MiB threads):
// the recursion limits of the interpreter and the structure walkers (ARCHITECTURE.md section 3) are sized for it.
constexpr std::size_t kStackBytes = std::size_t(256) << 20;
// Decoded size of one stream: a filter bomb ends as an undecodable stream (rules incomplete), it cannot fill the memory.
constexpr unsigned long long kMaxDecodedStreamBytes = 1ull << 30;

struct Thread {
    pthread_t id{};
    std::function<void()> fn;
};

void* thread_entry(void* p) {
    static_cast<Thread*>(p)->fn();
    return nullptr;
}

// Starts fn on a big stack. False: no thread could be created.
bool start_thread(Thread& t) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, kStackBytes);
    bool ok = pthread_create(&t.id, &attr, thread_entry, &t) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}

void limit_memory(unsigned long mb, unsigned threads) {
    if (mb == 0 || kSanitized) return;
    rlim_t bytes = static_cast<rlim_t>(mb) * threads * (1ull << 20) + (static_cast<rlim_t>(threads) + 1) * kStackBytes;
    rlimit cur{};
    if (getrlimit(RLIMIT_AS, &cur) != 0) return;
    if (cur.rlim_cur != RLIM_INFINITY && cur.rlim_cur <= bytes) return;   // never raises what the caller set
    rlimit lim{bytes, cur.rlim_max};
    setrlimit(RLIMIT_AS, &lim);
}

int self_check() {
    const Catalog& c = Catalog::get();
    std::map<std::string, std::size_t> per_module;
    std::size_t unowned = 0;
    for (RuleIndex i = 0; i < c.size(); ++i) {
        const std::string& o = owner_of(i);
        if (o.empty()) {
            ++unowned;
            std::printf("unowned: %s (family %s)\n", c.rule(i).id, c.rule(i).family);
        } else {
            per_module[o]++;
        }
    }
    auto mods = create_modules();
    std::map<std::string, bool> present;
    for (auto& m : mods) present[m->name()] = true;
    for (const auto& name : module_names())
        if (!present[name]) std::printf("module without implementation: %s\n", name.c_str());
    for (const auto& [m, n] : per_module) std::printf("%-14s %5zu rules\n", m.c_str(), n);
    std::printf("total %zu rules, %zu unowned\n", c.size(), unowned);
    return unowned == 0 ? 0 : 1;
}

void list_rules() {
    const Catalog& c = Catalog::get();
    for (RuleIndex i = 0; i < c.size(); ++i) {
        const RuleInfo& r = c.rule(i);
        std::printf("%s\t%s\t%s\t%s\n", r.id, r.severity == 0 ? "error" : "warning", owner_of(i).c_str(), r.title);
    }
}

}  // namespace

static int real_main(int argc, char** argv) {
    Pl_Flate::memory_limit(kMaxDecodedStreamBytes);
    Pl_RunLength::setMemoryLimit(kMaxDecodedStreamBytes);
    bool json = false, pretty = false, verbose = false;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    unsigned long max_memory_mb = 4096;
    Options opt;
    std::vector<std::string> files;
    const bool legacy_name = invoked_as_legacy_name(argc > 0 ? argv[0] : nullptr);
    std::optional<ResultBasis> basis_option;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dox-validate: %s needs a value\n", what);
                std::exit(3);
            }
            return argv[++i];
        };
        if (a.rfind("--format=", 0) == 0 || a == "--format") {
            std::string v = a == "--format" ? std::string(need("--format")) : a.substr(9);
            if (v == "json") json = true;
            else if (v == "text") json = false;
            else {
                std::fprintf(stderr, "dox-validate: --format must be text or json, not '%s'\n", v.c_str());
                return 3;
            }
        }
        else if (a == "--json") json = true;
        else if (a == "--pretty") pretty = true;
        else if (a == "--text") json = false;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--jobs") jobs = static_cast<unsigned>(std::max(1, std::atoi(need("--jobs"))));
        else if (a == "--max-locations") opt.max_locations = static_cast<std::size_t>(std::max(1, std::atoi(need("--max-locations"))));
        else if (a == "--max-memory-mb") max_memory_mb = std::strtoul(need("--max-memory-mb"), nullptr, 10);
        else if (a == "--reference-dedup") opt.reference_dedup = true;
        else if (a == "--rules") {
            opt.rule_prefix = need("--rules");
            const std::vector<bool> sel = rule_selection(opt.rule_prefix);
            if (opt.rule_prefix.empty() || std::find(sel.begin(), sel.end(), true) == sel.end()) {
                std::fprintf(stderr, "dox-validate: --rules %s selects no rule of the catalogue\n", opt.rule_prefix.c_str());
                return 3;
            }
        }
        else if (a == "--max-pages") opt.max_pages = std::max(1, std::atoi(need("--max-pages")));
        else if (a == "--list-rules") { list_rules(); return 0; }
        else if (a == "--dump") {
            limit_memory(max_memory_mb, 1);
            std::string s = dump_model(need("--dump"));
            std::fwrite(s.data(), 1, s.size(), stdout);
            return 0;
        }
        else if (a == "--text-model") {
            limit_memory(max_memory_mb, 1);
            bool ok = false;
            std::string s = text_model_json(need("--text-model"), &ok);
            bool written = std::fwrite(s.data(), 1, s.size(), stdout) == s.size() && std::fflush(stdout) == 0;
            if (!written) return 4;
            return ok ? 0 : 2;
        }
        else if (a == "--self-check") return self_check();
        else if (a == "--target" || a.rfind("--target=", 0) == 0) {
            std::string v = a == "--target" ? std::string(need("--target")) : a.substr(9);
            ResultBasis b;
            if (!parse_targets(v, b)) {
                std::fprintf(stderr, "dox-validate: --target %s: expected auto, any, or target names such as pdfua-1,wcag-2.1-AA\n",
                             v.c_str());
                return 3;
            }
            basis_option = b;
        }
        else if (a == "--version") {
            std::printf("%s %s\n", legacy_name ? "eqx-validator" : kValidatorName, kValidatorVersion);
            return 0;
        }
        else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a.size() > 1 && a[0] == '-' && a != "-") {
            std::fprintf(stderr, "dox-validate: unknown option %s\n", a.c_str());
            usage(stderr);
            return 3;
        } else files.push_back(a);
    }
    if (files.empty()) {
        usage(stderr);
        return 3;
    }
    // Default basis: auto; any under the legacy name and with --rules (a selection leaves every target incomplete).
    ResultBasis basis;
    if (basis_option) basis = *basis_option;
    else if (legacy_name || !opt.rule_prefix.empty()) basis.mode = ResultBasis::Mode::Any;
    std::vector<std::unique_ptr<Result>> results(files.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        for (std::size_t i; (i = next++) < files.size();)
            results[i] = std::make_unique<Result>(validate_file(files[i], opt));
    };
    unsigned n = std::min<unsigned>(jobs, static_cast<unsigned>(files.size()));
    limit_memory(max_memory_mb, std::max(1u, n));
    if (n <= 1) {
        worker();
    } else {
        std::vector<Thread> pool(n);
        std::size_t started = 0;
        for (Thread& t : pool) {
            t.fn = worker;
            if (start_thread(t)) ++started;
        }
        if (started == 0) worker();   // this thread already has the big stack
        for (std::size_t k = 0; k < n; ++k)
            if (pool[k].id != pthread_t{}) pthread_join(pool[k].id, nullptr);
    }
    int status = 0;
    bool written = true;
    for (const auto& r : results) {
        if (json) {
            std::string s = render_json(*r, pretty && files.size() == 1, basis);
            written = std::fwrite(s.data(), 1, s.size(), stdout) == s.size() && written;
            written = std::fputc('\n', stdout) != EOF && written;
        } else {
            std::string s = render_text(*r, verbose, basis);
            written = std::fwrite(s.data(), 1, s.size(), stdout) == s.size() && written;
        }
        const Summary sum = summarize(*r, basis);
        if (!r->processed)
            std::fprintf(stderr, "dox-validate: %s: not processed: %s\n", r->path.c_str(), r->error.c_str());
        else if (sum.required_rules_incomplete)
            std::fprintf(stderr, "dox-validate: %s: %zu required rule(s) could not be evaluated (see rules_incomplete)\n",
                         r->path.c_str(), sum.required_rules_incomplete);
        status = std::max(status, sum.exit_code);
    }
    if (std::fflush(stdout) != 0 || !written) {
        std::fprintf(stderr, "dox-validate: the report could not be written\n");
        return 4;
    }
    return status;
}

// Runs fn on a big stack without starting a thread: with a second thread libstdc++ makes every shared_ptr count atomic and glibc
// locks malloc, which costs 4 % on a tagged 479-page document (qpdf objects are shared_ptrs). False: no stack could be mapped.
bool run_on_big_stack(std::function<void()>& fn) {
    static std::function<void()>* target;
    static ucontext_t main_ctx, big_ctx;
    constexpr std::size_t kGuard = 1 << 16;
    void* stack = mmap(nullptr, kStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED || mprotect(stack, kGuard, PROT_NONE) != 0 || getcontext(&big_ctx) != 0) return false;
    target = &fn;
    big_ctx.uc_stack.ss_sp = stack;
    big_ctx.uc_stack.ss_size = kStackBytes;
    big_ctx.uc_link = &main_ctx;
    makecontext(&big_ctx, +[] { (*target)(); }, 0);
    return swapcontext(&main_ctx, &big_ctx) == 0;
}

int main(int argc, char** argv) {
    int status = 0;
    std::function<void()> body = [&] { status = real_main(argc, argv); };
    if (kSanitized) {   // ASan does not follow a hand-made stack switch
        Thread t;
        t.fn = body;
        if (!start_thread(t)) return 4;
        pthread_join(t.id, nullptr);
        return status;
    }
    if (!run_on_big_stack(body)) {
        std::fprintf(stderr, "dox-validate: cannot map the validation stack\n");
        return 4;
    }
    return status;
}
