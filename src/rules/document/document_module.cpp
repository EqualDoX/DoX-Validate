// Document-level rules: identification, metadata, catalog settings, encryption, optional content, embedded
// files, XFA, document/outline/XMP language, Lang syntax of every CosLang veraPDF visits, destinations and
// PUA in human-readable text strings.
//
// veraPDF 1.30.2 semantics are cited per rule (GF* model classes). veraPDF's model walk, which decides which
// objects a rule sees, is: CosDocument -> EmbeddedFiles, xref, trailer (the whole COS graph, depth first),
// indirectObjects, then the PD layer (PDDocument -> pages/annotations, metadata, AcroForm, actions, outlines,
// OC properties, structure tree, Lang). Objects with an ID (indirect PD objects, XMP properties) are validated
// once; the first visit wins.
//
// PAC 26.1 conditions merged into a canonical UA-1 rule (union semantics) are evaluated only when
// ctx.pac_applicable (PAC defines its checks for PDF <= 1.7 only); their findings carry "PAC:" in the detail.
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rules/document/lang.h"
#include "rules/document/xml_dom.h"
#include "rules/document/xmp.h"
#include "rules/module.h"
#include "rules/pac_quality/pac_quality_text.h"
#include "util/text.h"

namespace eqx {

namespace {

constexpr const char* kWtpdfReuse = "http://pdfa.org/declarations/wtpdf#reuse1.0";
constexpr const char* kWtpdfAccessibility = "http://pdfa.org/declarations/wtpdf#accessibility1.0";

bool is_dict(const Obj& o) { return o.isDictionary(); }                       // COS_DICT (not a stream)
bool is_dict_based(const Obj& o) { return o.isDictionary() || o.isStream(); }  // COSObjType.isDictionaryBased

// COSDictionary.knownKey: the key is present with a non-null value
bool known(const Obj& h, const char* key) { return cos::has(h, key); }

// COSDictionary.getStringKey: decoded text of a string value; null for names and other types
std::optional<std::u32string> string_key(const Obj& h, const char* key) {
    Obj v = cos::get(h, key);
    if (!v.isString()) return std::nullopt;
    return pdf_text_to_u32(v.getStringValue());
}

ObjRef ref_or(const Obj& o, ObjRef fallback) {
    ObjRef r = ref_of(o);
    return r.valid() ? r : fallback;
}

std::string u8(const std::u32string& s) { return to_utf8(s); }

std::string clip(std::string s) {
    if (s.size() > 80) s = s.substr(0, 77) + "...";
    return s;
}

bool has_pua(const std::u32string& s) {
    for (char32_t c : s)
        if (is_pua(c)) return true;
    return false;
}

// veraPDF PDFParser.parseHeader: the first line containing "%PDF-" (or "PDF-"), the whole line as read.
std::string verapdf_header(const std::string& head) {
    std::size_t pos = 0;
    auto next_line = [&](bool& eof) {
        if (pos >= head.size()) {
            eof = true;
            return std::string();
        }
        std::size_t e = head.find_first_of("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? head.size() : e + 1;
        return line;
    };
    bool eof = false;
    std::string line = next_line(eof);
    if (line.find("%PDF-") != std::string::npos) return line;
    while (!eof) {
        line = next_line(eof);
        if (line.find("%PDF-") != std::string::npos || line.find("PDF-") != std::string::npos) return line;
        if (!line.empty() && line[0] >= '0' && line[0] <= '9') return line;
    }
    return line;
}

bool header_ok(const std::string& h) {   // /^%PDF-1\.[0-7]$/
    return h.size() == 8 && h.compare(0, 7, "%PDF-1.") == 0 && h[7] >= '0' && h[7] <= '7';
}

// PDMetadata.isMetadataObject
bool is_metadata_object(const Obj& o) {
    return o.isStream() && cos::name_is(o, "Type", "Metadata") && cos::name_is(o, "Subtype", "XML");
}

// /Names arrays of a name tree indexed on first use: per array, (hash of the key as text, position of the key) sorted,
// so a lookup converts and compares only the keys with the lookup key's hash instead of every key of the array.
// Arrays are identified by their own object number, or by the indirect node that holds a direct array.
struct NamesIndex {
    using Entries = std::vector<std::pair<std::uint64_t, int>>;
    std::unordered_map<std::uint64_t, Entries> by_array, by_node;
};

std::uint64_t text_hash(const std::u32string& s) {   // FNV-1a over the code points
    std::uint64_t h = 1469598103934665603ull;
    for (char32_t c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

NamesIndex::Entries index_names(Obj& names) {
    NamesIndex::Entries e;
    const int n = names.getArrayNItems();
    e.reserve(static_cast<std::size_t>(n / 2 + 1));
    for (int i = 0; i < n; i += 2) {
        Obj k = names.getArrayItem(i);
        if (k.isString()) e.emplace_back(text_hash(pdf_text_to_u32(k.getStringValue())), i);
    }
    std::sort(e.begin(), e.end());   // by hash, then by position
    e.shrink_to_fit();
    return e;
}

// The index of `names` (held by `node`), built on first use; nullptr when neither is an indirect object.
const NamesIndex::Entries* names_entries(NamesIndex* cache, const Obj& node, Obj& names) {
    if (!cache) return nullptr;
    std::unordered_map<std::uint64_t, NamesIndex::Entries>* map = nullptr;
    std::uint64_t id = 0;
    if (names.isIndirect()) {
        map = &cache->by_array;
        id = ref_of(names).key();
    } else if (node.isIndirect()) {
        map = &cache->by_node;
        id = ref_of(node).key();
    } else {
        return nullptr;
    }
    auto it = map->find(id);
    if (it == map->end()) it = map->emplace(id, index_names(names)).first;
    return &it->second;
}

// The value of `key` in the /Names of a node that has /Names: true with `found` set to the value at the last position
// whose key equals `key` (a null object when that key has no value), false when the key is not there or /Names is no
// array. With `cache`, the array is searched through its index (the same result as the scan).
bool names_value(const Obj& node, const std::u32string& key, NamesIndex* cache, Obj& found) {
    bool hit = false;
    Obj names = cos::get(node, "Names");
    if (names.isArray()) {
        int n = names.getArrayNItems();
        if (const NamesIndex::Entries* e = names_entries(cache, node, names)) {
            const std::uint64_t h = text_hash(key);
            auto lo = std::lower_bound(e->begin(), e->end(), std::make_pair(h, std::numeric_limits<int>::min()));
            auto hi = std::upper_bound(e->begin(), e->end(), std::make_pair(h, std::numeric_limits<int>::max()));
            for (auto it = hi; it != lo;) {   // the last position first
                --it;
                if (pdf_text_to_u32(names.getArrayItem(it->second).getStringValue()) != key) continue;
                found = it->second + 1 < n ? names.getArrayItem(it->second + 1) : Obj::newNull();
                hit = true;
                break;
            }
        } else {
            for (int i = 0; i < n; i += 2) {
                Obj k = names.getArrayItem(i);
                if (!k.isString() || pdf_text_to_u32(k.getStringValue()) != key) continue;
                found = i + 1 < n ? names.getArrayItem(i + 1) : Obj::newNull();
                hit = true;
            }
        }
    }
    return hit;
}

bool text_limit(const Obj& item, std::u32string& out) {
    if (!item.isString()) return false;
    out = pdf_text_to_u32(item.getStringValue());
    return true;
}

// PDNameTreeNode.getObject(key): Limits, Names (first match per LinkedHashMap: the last value of a key), Kids. On an
// explicit stack: nesting has no limit; a node met again ends its branch. With `cache`, a /Names array is searched
// through its index (the same result as the scan: the last position whose key equals `key`).
// (A plain tree is searched through its TreeShape, see is_struct_destination; this walk serves any other.)
Obj name_tree_lookup(const Obj& root, const std::u32string& key, std::unordered_set<std::uint64_t>& seen,
                     NamesIndex* cache = nullptr) {
    std::vector<std::pair<std::vector<Obj>, std::size_t>> stack;
    Obj found;
    auto open = [&](const Obj& node) {
        if (!is_dict_based(node)) return false;
        if (node.isIndirect() && !seen.insert(ref_of(node).key()).second) return false;
        Obj limits = cos::get(node, "Limits");
        if (limits.isArray() && limits.getArrayNItems() >= 2 && limits.getArrayItem(0).isString() &&
            limits.getArrayItem(1).isString()) {
            auto lo = pdf_text_to_u32(limits.getArrayItem(0).getStringValue());
            auto hi = pdf_text_to_u32(limits.getArrayItem(1).getStringValue());
            if (key < lo || key > hi) return false;
        }
        if (known(node, "Names")) return names_value(node, key, cache, found);
        if (known(node, "Kids")) stack.push_back({cos::array_items(cos::get(node, "Kids")), 0});
        return false;
    };
    if (open(root)) return found;
    while (!stack.empty()) {
        auto& top = stack.back();
        if (top.second >= top.first.size()) {
            stack.pop_back();
            continue;
        }
        Obj kid = top.first[top.second++];
        if (open(kid)) return found;
    }
    return Obj();
}

// Values of a name tree the way GFCosDocument.getNamesEmbeddedFiles reads them (Names of each node, then Kids).
void name_tree_values(const Obj& root, std::vector<Obj>& out, std::unordered_set<std::uint64_t>& seen) {
    std::vector<std::pair<std::vector<Obj>, std::size_t>> stack;
    auto open = [&](const Obj& node) {
        if (!is_dict_based(node)) return;
        if (node.isIndirect() && !seen.insert(ref_of(node).key()).second) return;
        Obj names = cos::get(node, "Names");
        if (names.isArray()) {
            std::vector<std::pair<std::u32string, Obj>> map;   // LinkedHashMap: first position, last value
            int n = names.getArrayNItems();
            for (int i = 0; i < n; i += 2) {
                Obj k = names.getArrayItem(i);
                if (!k.isString()) continue;
                auto key = pdf_text_to_u32(k.getStringValue());
                Obj v = i + 1 < n ? names.getArrayItem(i + 1) : Obj::newNull();
                auto it = std::find_if(map.begin(), map.end(), [&](auto& e) { return e.first == key; });
                if (it != map.end()) it->second = v;
                else map.emplace_back(key, v);
            }
            for (auto& e : map) out.push_back(e.second);
        }
        Obj kids = cos::get(node, "Kids");
        if (kids.isArray()) stack.push_back({cos::array_items(kids), 0});
    };
    open(root);
    while (!stack.empty()) {
        auto& top = stack.back();
        if (top.second >= top.first.size()) {
            stack.pop_back();
            continue;
        }
        Obj kid = top.first[top.second++];
        open(kid);
    }
}

// "Generated-looking" document title (PAC Quality DocumentTitleIsNotGenerated, condition
// WarningDocumentTitleIsFilenameOrPath): a path or a bare file name with a document/image extension.
bool title_is_filename_or_path(const std::string& raw) {
    std::string t = trim_ascii(raw);
    if (t.empty()) return false;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string l = lower(t);
    bool drive = t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) && t[1] == ':' &&
                 (t[2] == '\\' || t[2] == '/');
    if (drive || t.rfind("\\\\", 0) == 0 || l.rfind("file:", 0) == 0 || t.find('\\') != std::string::npos)
        return true;
    if (t[0] == '/' && t.find('/', 1) != std::string::npos && t.find(' ') == std::string::npos) return true;
    static const char* exts[] = {"pdf", "doc", "docx", "docm", "dot", "dotx", "odt", "ott", "rtf", "txt", "xls",
                                 "xlsx", "xlsm", "ods", "ppt", "pptx", "pptm", "odp", "indd", "indt", "idml",
                                 "qxp", "qxd", "pub", "pages", "key", "numbers", "htm", "html", "xml", "tex",
                                 "ps", "eps", "ai", "psd", "jpg", "jpeg", "png", "gif", "tif", "tiff", "bmp",
                                 "svg", "fm", "wpd", "wps", "xps", "oxps", "vsd", "vsdx", "mpp", "msg", "eml"};
    std::size_t dot = l.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= l.size()) return false;
    std::string ext = l.substr(dot + 1);
    if (std::find_if(std::begin(exts), std::end(exts), [&](const char* e) { return ext == e; }) == std::end(exts))
        return false;
    for (char c : t.substr(0, dot))
        if (c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

class Module final : public RuleModule {
public:
    const char* name() const override { return "document"; }

    void run(ValidationContext& ctx) override {
        ctx_ = &ctx;
        cat_ = ctx.doc.catalog();
        cat_ref_ = ref_of(cat_);
        guarded({"EQX-UA1-6.1-1"}, [&] { header(); });
        guarded({"EQX-UA1-6.1-1", "EQX-UA2-6.2-1"}, [&] { file_structure(); });
        guarded({"EQX-UA1-6.2-1", "EQX-UA2-6.2-1", "EQX-UA1-7.1-4", "EQX-UA1-7.1-10", "EQX-UA2-8.11.2-1",
                 "EQX-UA1-7.1-11", "EQX-UA2-8.2.1-1"},
                [&] { catalog_settings(); });
        guarded({"EQX-UA1-7.11-1", "EQX-UA2-8.14.1-1"}, [&] { cos_walk(); embedded_files(); });
        guarded({"EQX-UA1-7.1-8", "EQX-UA2-8.11.1-2", "EQX-UA1-5-1", "EQX-UA1-5-2", "EQX-UA1-5-3", "EQX-UA1-5-4",
                 "EQX-UA1-5-5", "EQX-UA2-5-1", "EQX-UA2-5-2", "EQX-UA2-5-3", "EQX-UA2-5-4", "EQX-UA2-5-5",
                 "EQX-UA1-7.1-9", "EQX-UA2-8.11.1-1", "EQX-WTPDF-6.1.2-1", "EQX-WTPDF-6.1.3-1", "EQX-UA1-7.2-33",
                 "EQX-PACQ-WarningDocumentTitleIsFilenameOrPath"},
                [&] { metadata(); });
        guarded({"EQX-UA2-8.4.4-1", "EQX-UA1-7.2-29", "EQX-UA2-8.4.4-2", "EQX-UA2-8.6-1",
                 "EQX-PAC-FailValidLanguagePrimaryTagX", "EQX-PAC-WarnValidLanguagePrimaryTagI",
                 "EQX-PAC-FailValidLanguagePrimaryTagOther", "EQX-UA1-7.2-2"},
                [&] { languages(); });
        guarded({"EQX-UA1-7.10-1", "EQX-UA1-7.10-2", "EQX-UA2-8.7-1", "EQX-UA2-8.7-2"},
                [&] { optional_content(); });
        guarded({"EQX-UA1-7.15-1", "EQX-UA2-8.10.1-3"}, [&] { acroform(); });
        guarded({"EQX-UA1-7.16-1"}, [&] { encryption(); });
        guarded({"EQX-UA2-8.8-1", "EQX-UA2-8.8-2"}, [&] { destinations(); });
    }

private:
    ValidationContext* ctx_ = nullptr;
    Obj cat_;
    ObjRef cat_ref_;
    std::vector<Obj> cos_filespecs_;          // dictionaries with /Type /Filespec anywhere in the COS graph
    std::vector<Obj> other_metadata_;         // GFCosDict.getMetadata: /Metadata of non-catalog dictionaries
    bool main_metadata_shadowed_ = false;     // catalog XMP stream reached first through another dictionary
    std::vector<std::pair<Obj, ObjRef>> outlines_;   // veraPDF OutlinesHelper items (object, report ref)

    void report(const char* rule, int page, ObjRef obj, std::string detail = {}) {
        ctx_->report(R(rule), page, obj, std::move(detail));
    }

    void guarded(std::initializer_list<const char*> rules, const std::function<void()>& fn) {
        try {
            fn();
        } catch (std::exception& e) {
            for (const char* r : rules) ctx_->mark_incomplete(R(r), std::string("damaged object: ") + e.what());
        }
    }

    // ------------------------------------------------------------------------------- syntax / settings
    void header() {
        // GFCosDocument.getheader (UA-1 6.1-1): /^%PDF-1\.[0-7]$/.test(header) on the whole header line
        std::string h = verapdf_header(ctx_->doc.file_head());
        if (!header_ok(h)) report("EQX-UA1-6.1-1", -1, {}, "header '" + clip(h) + "'");
    }

    // What this validator reads here is qpdf's recovered view of a damaged file: ISO 32000-1 7.5.2 (the header is the
    // first line), 7.5.5 (the last line holds %%EOF, startxref gives the cross-reference section). veraPDF refuses most
    // such files and PAC reports them corrupted, so neither ever passes one; no rule of either says so. Rules found on
    // the recovered view stay reported, and the verdict stays open instead of passing.
    void file_structure() {
        std::vector<std::string> why;
        const std::string& head = ctx_->doc.file_head();
        const std::size_t at = head.find("%PDF-");
        if (at != 0) why.push_back("the header is not at the start of the file");
        // PDFParser.findLastXRef: the keyword must lie within the last 1024 bytes
        // (the document kept these bytes when it was opened; the file is not read again)
        if (ctx_->doc.file_tail().find("startxref") == std::string::npos) why.push_back("no startxref in the last 1024 bytes");
        for (const std::string& w : ctx_->doc.warnings())
            if (w.find("reconstruct cross-reference table") != std::string::npos) {
                why.push_back("the cross-reference table had to be rebuilt");
                break;
            }
        if (why.empty()) return;
        std::string reason = "damaged file, validated on a recovered view (ISO 32000-1 7.5):";
        for (std::size_t i = 0; i < why.size(); ++i) reason += (i ? "; " : " ") + why[i];
        ctx_->mark_incomplete(R("EQX-UA1-6.1-1"), reason);
        ctx_->mark_incomplete(R("EQX-UA2-6.2-1"), reason);
    }

    void catalog_settings() {
        // GFCosDocument.getMarked / getSuspects / getDisplayDocTitle: the entry of a /MarkInfo resp.
        // /ViewerPreferences dictionary (a stream is not a dictionary); null when absent or not boolean.
        Obj mi = cos::get(cat_, "MarkInfo");
        std::optional<bool> marked, suspects;
        if (is_dict(mi)) {
            marked = cos::boolean(mi, "Marked");
            suspects = cos::boolean(mi, "Suspects");
        }
        if (!(marked && *marked)) {   // Marked == true; PAC MarkedIsTrue (same condition)
            std::string d = is_dict(mi) ? "MarkInfo /Marked is not true" : "no MarkInfo dictionary";
            report("EQX-UA1-6.2-1", -1, cat_ref_, d);
            report("EQX-UA2-6.2-1", -1, cat_ref_, d);
        }
        if (suspects && *suspects) report("EQX-UA1-7.1-4", -1, cat_ref_, "MarkInfo /Suspects true");
        Obj vp = cos::get(cat_, "ViewerPreferences");
        std::optional<bool> ddt;
        if (is_dict(vp)) ddt = cos::boolean(vp, "DisplayDocTitle");
        if (!(ddt && *ddt)) {
            std::string d = is_dict(vp) ? "ViewerPreferences /DisplayDocTitle is not true" : "no ViewerPreferences";
            report("EQX-UA1-7.1-10", -1, cat_ref_, d);
            report("EQX-UA2-8.11.2-1", -1, cat_ref_, d);
        }
        // GFPDDocument.getcontainsStructTreeRoot: PDCatalog.getStructTreeRoot needs a COS_DICT
        if (!is_dict(cos::get(cat_, "StructTreeRoot"))) {
            report("EQX-UA1-7.1-11", -1, cat_ref_, "no StructTreeRoot dictionary");
            report("EQX-UA2-8.2.1-1", -1, cat_ref_, "no StructTreeRoot dictionary");
        }
    }

    // ---------------------------------------------------------------------------------- COS graph walk
    // One pass over every indirect object (and the trailer) with the direct objects nested in it, the way
    // veraPDF's trailer/indirectObjects links reach them (references are not followed: each indirect object
    // is its own root). Collects GFCosVisitor.visitFromDictionary file specifications and the PDMetadata
    // objects GFCosDict.getMetadata creates for non-catalog dictionaries.
    void cos_walk() {
        Obj main_md = cos::get(cat_, "Metadata");
        ObjRef main_ref = is_metadata_object(main_md) ? ref_of(main_md) : ObjRef{};
        std::unordered_set<std::uint64_t> md_seen;
        // Only direct objects are followed; qpdf's parser bounds their nesting (about 500), so this recursion is safe.
        std::function<void(Obj)> visit = [&](Obj h) {
            if (h.isArray()) {
                for (auto& v : cos::array_items(h))
                    if (!v.isIndirect()) visit(v);
                return;
            }
            if (!is_dict_based(h)) return;
            Obj d = cos::dict_of(h);
            // The entries of the dictionary, taken once (a copy of its map), instead of a dictionary search for /Type,
            // for /Metadata and for every key: what cos::name_is, cos::get and getKeys/getKey read one by one, read
            // in the same order.
            const std::map<std::string, Obj> entries = d.getDictAsMap();
            const auto type_is = [&](const char* slash_name) {   // cos::name_is(d, "Type", name)
                auto type = entries.find("/Type");
                return type != entries.end() && type->second.isName() && type->second.getName() == slash_name;
            };
            if (h.isDictionary() && type_is("/Filespec")) cos_filespecs_.push_back(h);
            Obj md = Obj::newNull();   // cos::get(d, "Metadata")
            if (auto found = entries.find("/Metadata"); found != entries.end() && !found->second.isNull()) md = found->second;
            if (is_metadata_object(md) && !type_is("/Catalog")) {
                ObjRef r = ref_of(md);
                // veraPDF artefact: a shared PDMetadata ID makes it skip the main package; reproduced only in
                // --reference-dedup mode (the default evaluates the catalog's package regardless)
                if (r.valid() && r == main_ref && ctx_->reference_dedup) main_metadata_shadowed_ = true;
                if (!r.valid() || md_seen.insert(r.key()).second) other_metadata_.push_back(md);
            }
            // getKeys() gives the keys whose value is not null, having looked at every value: so here, before any direct
            // value is followed.
            std::vector<const Obj*> direct;
            for (const auto& item : entries) {
                const Obj& v = item.second;
                if (v.isNull()) continue;
                if (!v.isIndirect() && (v.isArray() || is_dict_based(v))) direct.push_back(&v);
            }
            for (const Obj* v : direct) visit(*v);
        };
        Obj trailer = ctx_->doc.trailer();
        visit(trailer);
        std::vector<Obj> objs = ctx_->doc.qpdf().getAllObjects();
        if (ctx_->reference_dedup) {
            // GFCosDocument.getIndirectObjects iterates a java.util.HashMap<COSKey, COSObject>: bucket order of
            // COSKey.hashCode (31 * number + generation) spread by h ^ (h >>> 16), in a table of the smallest
            // power of two >= 16 that holds the entries at load factor 0.75; ties keep the object-number order.
            // The order decides which package's property is "first" for 7.2-33 (Phase 3: 8 of 33 932 veraPDF
            // contexts of 300 real-world files named another XMP package than the number order did).
            std::uint32_t n = 16;
            while (objs.size() > std::size_t(n) * 3 / 4) n <<= 1;
            auto bucket = [n](const Obj& o) {
                std::uint32_t h = static_cast<std::uint32_t>(o.getObjectID()) * 31u + static_cast<std::uint32_t>(o.getGeneration());
                return (h ^ (h >> 16)) & (n - 1);
            };
            std::stable_sort(objs.begin(), objs.end(), [&](const Obj& a, const Obj& b) { return bucket(a) < bucket(b); });
        }
        for (auto& o : objs) visit(o);
    }

    // ----------------------------------------------------------------------------------- embedded files
    void check_filespec(const Obj& fs, bool in_embedded_files, ObjRef fallback) {
        ObjRef r = ref_or(fs, fallback);
        // GFCosFileSpecification (UA-1 7.11-1): containsEF == false || (F, UF non-empty strings);
        // PAC FSDictsContainsFAndUF ("F" or "UF" key missing) is contained in it.
        if (known(fs, "EF")) {
            auto f = string_key(fs, "F");
            auto uf = string_key(fs, "UF");
            if (!f || f->empty() || !uf || uf->empty())
                report("EQX-UA1-7.11-1", -1, r,
                       std::string("embedded file specification: ") + (!f ? "no F string" : f->empty() ? "empty F" : "F ok") +
                           ", " + (!uf ? "no UF string" : uf->empty() ? "empty UF" : "UF ok"));
        }
        // UA-2 8.14.1-1: containsDesc == true || presentInEmbeddedFiles == false
        if (in_embedded_files && !known(fs, "Desc")) report("EQX-UA2-8.14.1-1", -1, r, "file specification without /Desc");
    }

    void embedded_files() {
        // GFCosDocument.getEmbeddedFiles: Names (COS_DICT) -> EmbeddedFiles (COS_DICT) name tree values
        Obj names = cos::get(cat_, "Names");
        if (is_dict(names)) {
            Obj ef = cos::get(names, "EmbeddedFiles");
            if (is_dict(ef)) {
                std::vector<Obj> vals;
                std::unordered_set<std::uint64_t> seen;
                name_tree_values(ef, vals, seen);
                for (auto& v : vals)
                    if (is_dict_based(v)) check_filespec(v, true, ref_or(ef, cat_ref_));
            }
        }
        for (auto& fs : cos_filespecs_) check_filespec(fs, false, cat_ref_);
    }

    // ---------------------------------------------------------------------------------------- metadata
    void metadata() {
        Obj md = cos::get(cat_, "Metadata");
        bool contains = is_metadata_object(md);   // GFPDDocument.getcontainsMetadata
        if (!contains) {
            std::string d = md.isStream() ? "catalog /Metadata lacks /Type /Metadata or /Subtype /XML"
                                          : "no metadata stream in the catalog";
            report("EQX-UA1-7.1-8", -1, cat_ref_, d);
            report("EQX-UA2-8.11.1-2", -1, cat_ref_, d);
        }
        ObjRef md_ref = ref_or(md, cat_ref_);

        // The catalog packet (also read for PAC when /Type or /Subtype are wrong).
        std::optional<xmp::Packet> main;
        if (md.isStream()) {
            auto data = cos::stream_data(md);
            main = data ? xmp::Packet::parse(*data) : xmp::Packet::parse(std::string_view());
            if (main->valid() && main->contains_namespace(xmp::kNsPdfuaId))
                if (auto part = main->pdfua_part(); part && *part >= 1 && *part <= 9)
                    ctx_->declared_pdfua_part = static_cast<int>(*part);   // reported as input.pdfua_part
        }

        // veraPDF MainXMPPackage rules: only when the PDDocument metadata link yields the main package, i.e.
        // the stream is a metadata object that was not reached first through another dictionary's /Metadata
        // (then the shared PDMetadata ID makes veraPDF skip it; --reference-dedup only).
        if (contains && !main_metadata_shadowed_) {
            const xmp::Packet& pk = *main;
            bool ua = pk.valid() && pk.contains_namespace(xmp::kNsPdfuaId);
            if (!ua) {
                std::string d = pk.valid() ? "no PDF/UA identification schema" : "XMP not parsable: " + clip(pk.error());
                report("EQX-UA1-5-1", -1, md_ref, d);
                report("EQX-UA2-5-1", -1, md_ref, d);
            } else {
                auto part = pk.pdfua_part();
                std::string ps = part ? std::to_string(*part) : "null";
                if (!part || *part != 1) report("EQX-UA1-5-2", -1, md_ref, "pdfuaid:part " + ps);
                if (!part || *part != 2) report("EQX-UA2-5-2", -1, md_ref, "pdfuaid:part " + ps);
                auto prefix_bad = [&](const char* name) -> std::optional<std::string> {
                    const xmp::Property* p = pk.property(xmp::kNsPdfuaId, name);
                    if (p && p->prefix && *p->prefix != "pdfuaid") return *p->prefix;
                    return std::nullopt;
                };
                if (auto p = prefix_bad("part")) {
                    report("EQX-UA1-5-3", -1, md_ref, "part prefix '" + *p + "'");
                    report("EQX-UA2-5-3", -1, md_ref, "part prefix '" + *p + "'");
                }
                if (auto p = prefix_bad("amd")) report("EQX-UA1-5-4", -1, md_ref, "amd prefix '" + *p + "'");
                if (auto p = prefix_bad("corr")) report("EQX-UA1-5-5", -1, md_ref, "corr prefix '" + *p + "'");
                if (auto p = prefix_bad("rev")) report("EQX-UA2-5-4", -1, md_ref, "rev prefix '" + *p + "'");
                auto rev = pk.pdfua_rev();
                if (!rev || *rev != "2024") report("EQX-UA2-5-5", -1, md_ref, "pdfuaid:rev " + (rev ? "'" + clip(*rev) + "'" : std::string("null")));
            }
            if (!pk.dc_title()) {
                report("EQX-UA1-7.1-9", -1, md_ref, "no dc:title");
                report("EQX-UA2-8.11.1-1", -1, md_ref, "no dc:title");
            }
            auto decl = pk.declarations();
            if (!decl.count(kWtpdfReuse)) report("EQX-WTPDF-6.1.2-1", -1, md_ref, "no WTPDF reuse declaration");
            if (!decl.count(kWtpdfAccessibility))
                report("EQX-WTPDF-6.1.3-1", -1, md_ref, "no WTPDF accessibility declaration");
        }

        // PAC DocumentContainsPDFUAIdentification (merged into UA-1 5-1) and DocumentContainsTitle (merged into
        // UA-1 7.1-9): read from the catalog XMP whatever veraPDF's model does with it.
        std::optional<std::string> pac_title;
        if (main && main->valid()) {
            for (const auto& p : main->properties())
                if (p.ns && *p.ns == xmp::kNsDc && p.name == "title") {
                    pac_title = p.node->is_simple() ? std::optional<std::string>(p.node->value)
                                                    : xmp::language_alternative(*p.node);
                    break;
                }
        }
        if (ctx_->pac_applicable) {
            bool ua1 = main && main->valid() && main->pdfua_part() == std::optional<std::int64_t>(1);
            if (!ua1)
                report("EQX-UA1-5-1", -1, md_ref,
                       !md.isStream() ? "PAC: XMP metadata missing" : "PAC: PDF/UA identifier (pdfuaid:part 1) missing");
            if (!pac_title || trim_ascii(*pac_title).empty())
                report("EQX-UA1-7.1-9", -1, md_ref,
                       !md.isStream() ? "PAC: XMP metadata missing" : "PAC: title missing in XMP metadata");
            // PAC Quality: title looks like a file name or path (XMP title, else Info /Title)
            std::optional<std::string> title = pac_title;
            ObjRef tref = md_ref;
            if (!title || trim_ascii(*title).empty()) {
                Obj info = ctx_->doc.info();
                if (auto t = cos::text(info, "Title")) {
                    title = *t;
                    tref = ref_or(info, cat_ref_);
                }
            }
            if (title && pacq::looks_like_path_or_file_name(*title))
                report("EQX-PACQ-WarningDocumentTitleIsFilenameOrPath", -1, tref, "title '" + clip(*title) + "'");
        }

        // UA-1 7.2-33 (XMPLangAlt, deferred): xDefault == false || gContainsCatalogLang. Every XMP package
        // veraPDF visits takes part: those of non-catalog /Metadata first (COS walk), then the main package;
        // veraPDF validates a property ID ("ns - prefix:name") once, at its first visit (--reference-dedup);
        // the default mode evaluates the property in every package.
        if (!cos::get(cat_, "Lang").isString()) {
            std::set<std::string> ids;
            auto scan = [&](const xmp::Packet& pk, ObjRef where) {
                if (!pk.valid()) return;
                for (const auto& p : pk.properties()) {
                    const std::string id = (ctx_->reference_dedup ? std::string() : where.str() + " ") +
                                           xmp::Packet::property_id(p);
                    if (!ids.insert(id).second) continue;
                    if (p.ns && *p.ns == xmp::kNsXmpMM && p.name == "History") continue;
                    if (xmp::is_language_alternative(*p.node) && xmp::lang_alt_x_default(*p.node))
                        report("EQX-UA1-7.2-33", -1, where,
                               "x-default " + (p.prefix ? *p.prefix : std::string()) + ":" + p.name +
                                   " without a catalog Lang");
                }
            };
            for (auto& o : other_metadata_) {
                auto data = cos::stream_data(o);
                xmp::Packet pk = xmp::Packet::parse(data ? std::string_view(*data) : std::string_view());
                scan(pk, ref_of(o));
            }
            if (contains && !main_metadata_shadowed_) scan(*main, md_ref);
        }
    }

    // ---------------------------------------------------------------------------------------- languages
    void cos_lang(const std::string& raw, int page, ObjRef where, const char* what) {
        // GFCosLang: UA-1 7.2-29 / UA-2 8.4.4-2 regex on unicodeValue; it is a CosTextString (UA-2 8.6-1)
        std::u32string v = pdf_text_to_u32(raw);
        if (!doclang::verapdf_lang_ok(v)) {
            std::string d = std::string(what) + " Lang '" + clip(u8(v)) + "' is not a Language-Tag";
            report("EQX-UA1-7.2-29", page, where, d);
            report("EQX-UA2-8.4.4-2", page, where, d);
        }
        if (has_pua(v)) report("EQX-UA2-8.6-1", page, where, std::string(what) + " Lang uses the Unicode PUA");
    }

    // Lang of the structure element a /StructParent entry names (GFPDAnnot / GFPDFormField.getLang)
    std::optional<std::string> struct_parent_lang(const Obj& dict) {
        if (!is_dict(cos::get(cat_, "StructTreeRoot")) || !ctx_->st.has_parent_tree()) return std::nullopt;
        auto sp = cos::integer(dict, "StructParent");
        if (!sp) return std::nullopt;
        Obj se = ctx_->st.parent_tree_entry(*sp);
        if (!is_dict_based(se)) return std::nullopt;
        return cos::bytes(se, "Lang");
    }

    void languages() {
        Obj lang = cos::get(cat_, "Lang");
        bool catalog_lang = lang.isString();   // GFPDDocument.getcontainsLang (gContainsCatalogLang)
        if (!catalog_lang) report("EQX-UA2-8.4.4-1", -1, cat_ref_, "catalog has no Lang string");
        else cos_lang(lang.getStringValue(), -1, cat_ref_, "catalog");

        // PAC ValidLanguage on the document language
        if (ctx_->pac_applicable) {
            std::optional<std::u32string> v;
            if (catalog_lang) v = pdf_text_to_u32(lang.getStringValue());
            switch (doclang::pac_check(v)) {
                case doclang::PacLang::Missing:
                    report("EQX-UA1-7.2-29", -1, cat_ref_, "PAC: document language missing");
                    break;
                case doclang::PacLang::Syntax:
                    report("EQX-UA1-7.2-29", -1, cat_ref_, "PAC: document language '" + clip(u8(*v)) + "' syntax error");
                    break;
                case doclang::PacLang::PrivateUse:
                    report("EQX-PAC-FailValidLanguagePrimaryTagX", -1, cat_ref_, "document language '" + clip(u8(*v)) + "'");
                    break;
                case doclang::PacLang::IanaRegistered:
                    report("EQX-PAC-WarnValidLanguagePrimaryTagI", -1, cat_ref_, "document language '" + clip(u8(*v)) + "'");
                    break;
                case doclang::PacLang::Valid:
                    break;
            }
            // EQX-PAC-FailValidLanguagePrimaryTagOther: PAC 26.1 reports unknown primary subtags as syntax
            // errors (probes); this condition was never observed and is not raised.
        }

        // structure elements (GFPDStructElem.getLang)
        for (const auto& e : ctx_->st.elems())
            if (auto l = cos::bytes(e.get(StructElem::kLang, "Lang"))) cos_lang(*l, -1, ref_or(e.dict, cat_ref_), "structure element");
        // marked-content property lists (Op_BDC Lang link, string values only)
        for (const auto& f : ctx_->content.frames) {
            if (!f.bdc || !f.lang || f.stream < 0) continue;
            const auto& si = ctx_->content.streams[f.stream];
            if (ctx_->reference_dedup && si.repeat) continue;
            cos_lang(*f.lang, si.page, si.ref, "marked-content property list");
        }
        // annotations (pages' /Annots dictionaries) and form fields: Lang of their parent structure element
        for (const auto& pg : ctx_->doc.pages())
            for (auto& a : cos::array_items(cos::get(pg.dict, "Annots")))
                if (is_dict(a))
                    if (auto l = struct_parent_lang(a)) cos_lang(*l, pg.index, ref_or(a, pg.ref), "annotation");
        for_each_field([&](const Obj& f) {
            if (auto l = struct_parent_lang(f)) cos_lang(*l, -1, ref_or(f, cat_ref_), "form field");
        });

        // outlines: UA-1 7.2-2 (gContainsCatalogLang; PAC OutlineItemHasLanguage), UA-2 8.6-1 on titles
        collect_outlines();
        for (auto& [item, ref] : outlines_) {
            if (!catalog_lang) report("EQX-UA1-7.2-2", -1, ref, "outline item language cannot be determined");
            Obj t = cos::get(item, "Title");
            if (t.isString() && has_pua(pdf_text_to_u32(t.getStringValue())))
                report("EQX-UA2-8.6-1", -1, ref, "outline title uses the Unicode PUA");
        }
    }

    // GFPDAcroForm.getFormFields / GFPDFormField.getKids: dict-based /Fields entries, then Kids that are fields
    void for_each_field(const std::function<void(const Obj&)>& fn) {
        Obj af = cos::get(cat_, "AcroForm");
        if (!is_dict_based(af)) return;
        std::unordered_set<std::uint64_t> seen;
        std::vector<Obj> stack;   // preorder in /Kids order on an explicit stack: fields nest without a limit
        auto fields = cos::array_items(cos::get(af, "Fields"));
        for (auto it = fields.rbegin(); it != fields.rend(); ++it)
            if (is_dict_based(*it)) stack.push_back(*it);
        while (!stack.empty()) {
            Obj f = stack.back();
            stack.pop_back();
            if (f.isIndirect() && !seen.insert(ref_of(f).key()).second) continue;
            fn(f);
            auto kids = cos::array_items(cos::get(f, "Kids"));
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                if (is_dict_based(*it) && known(*it, "T")) stack.push_back(*it);
        }
    }

    // OutlinesHelper.getOutlines: items reached through First/Next, each once
    void collect_outlines() {
        if (!outlines_.empty()) return;
        Obj root = cos::get(cat_, "Outlines");
        if (!is_dict_based(root)) return;
        Obj first = cos::get(root, "First");
        if (!is_dict_based(first)) return;
        std::unordered_set<std::uint64_t> done;
        bool direct_done = false;
        auto is_done = [&](const Obj& o) { return o.isIndirect() ? done.count(ref_of(o).key()) > 0 : direct_done; };
        std::vector<Obj> stack{first};
        int direct_index = -1;
        while (!stack.empty()) {
            Obj item = stack.back();
            stack.pop_back();
            Obj next = cos::get(item, "Next");
            Obj kid = cos::get(item, "First");
            if (is_dict_based(next) && !is_done(next)) stack.push_back(next);
            if (is_dict_based(kid) && !is_done(kid)) stack.push_back(kid);
            if (item.isIndirect()) {
                if (done.insert(ref_of(item).key()).second) outlines_.emplace_back(item, ref_of(item));
            } else {   // direct items share the null ID: the last one is kept
                direct_done = true;
                if (direct_index < 0) {
                    direct_index = static_cast<int>(outlines_.size());
                    outlines_.emplace_back(item, cat_ref_);
                } else {
                    outlines_[direct_index].first = item;
                }
            }
        }
    }

    // -------------------------------------------------------------------------------- optional content
    void optional_content() {
        Obj ocp = cos::get(cat_, "OCProperties");   // PDCatalog.getOCProperties: COS_DICT
        if (!is_dict(ocp)) return;
        std::vector<Obj> configs;
        Obj d = cos::get(ocp, "D");
        if (is_dict(d)) configs.push_back(d);
        bool contains_configs = false;   // GFPDOCProperties.getcontainsConfigs (gContainsConfigs)
        for (auto& c : cos::array_items(cos::get(ocp, "Configs")))
            if (is_dict(c)) {
                configs.push_back(c);
                contains_configs = true;
            }
        std::unordered_set<std::uint64_t> seen;
        for (auto& c : configs) {
            if (c.isIndirect() && !seen.insert(ref_of(c).key()).second) continue;
            ObjRef r = ref_or(c, ref_or(ocp, cat_ref_));
            auto name = string_key(c, "Name");
            if (!name || name->empty()) {   // Name != null && Name.length() > 0
                report("EQX-UA1-7.10-1", -1, r, "optional content configuration without a non-empty Name");
                if (contains_configs)
                    report("EQX-UA2-8.7-1", -1, r, "optional content configuration without a non-empty Name");
            }
            if (known(c, "AS")) {   // AS == null
                report("EQX-UA1-7.10-2", -1, r, "AS key in an optional content configuration");
                report("EQX-UA2-8.7-2", -1, r, "AS key in an optional content configuration");
            }
        }
    }

    // ------------------------------------------------------------------------------------------- XFA
    static std::optional<std::string> dynamic_render(const Obj& xfa_in) {
        // GFPDAcroForm.getdynamicRender
        Obj obj = xfa_in;
        if (obj.isArray()) {
            Obj after;
            int n = obj.getArrayNItems();
            for (int i = 0; i < n - 1; ++i) {
                Obj e = obj.getArrayItem(i);
                if (e.isString() && pdf_text_to_utf8(e.getStringValue()) == "config") {
                    after = obj.getArrayItem(i + 1);
                    break;
                }
            }
            obj = after;
        }
        if (!obj.isInitialized() || !obj.isStream()) return std::nullopt;
        auto data = cos::stream_data(obj);
        if (!data) return std::nullopt;
        xml::Document doc;
        if (!xml::parse(*data, xml::Mode::Single, doc)) return std::nullopt;   // "Problems with parsing XFA"
        auto child = [](const std::vector<xml::Node>* nodes, const char* qname) -> const std::vector<xml::Node>* {
            if (!nodes) return nullptr;
            for (const auto& n : *nodes)
                if (n.is_element() && n.qname == qname) return &n.children;
            return nullptr;
        };
        const std::vector<xml::Node>* parent = child(&doc.children, "xdp:xdp");
        if (!parent) parent = &doc.children;
        const std::vector<xml::Node>* dr =
            child(child(child(child(parent, "config"), "acrobat"), "acrobat7"), "dynamicRender");
        if (!dr || dr->empty()) return std::nullopt;   // item(0) null -> exception -> null
        const xml::Node& first = dr->front();
        if (first.kind == xml::Node::Kind::Text || first.kind == xml::Node::Kind::CData ||
            first.kind == xml::Node::Kind::PI)
            return first.value;
        return std::nullopt;
    }

    void acroform() {
        Obj af = cos::get(cat_, "AcroForm");   // PDCatalog.getAcroForm: dictionary based
        if (!is_dict_based(af)) return;
        ObjRef r = ref_or(af, cat_ref_);
        if (known(af, "XFA")) {
            report("EQX-UA2-8.10.1-3", -1, r, "AcroForm contains XFA");   // containsXFA == false
            auto dr = dynamic_render(cos::get(af, "XFA"));
            if (dr && *dr == "required")   // dynamicRender != 'required'; PAC XFAIsNotDynamic
                report("EQX-UA1-7.15-1", -1, r, "dynamic XFA form (dynamicRender required)");
        }
    }

    // ------------------------------------------------------------------------------------- encryption
    void encryption() {
        // GFCosTrailer.getEncrypt (COS_DICT) -> GFPDEncryption.getP: P != null && (P & 512) == 512;
        // PAC AccessibleEncyptionSettings is the same condition.
        Obj enc = cos::get(ctx_->doc.trailer(), "Encrypt");
        if (!is_dict(enc)) return;
        Obj p = cos::get(enc, "P");
        std::optional<std::int64_t> pv;
        if (p.isInteger()) pv = p.getIntValue();
        else if (p.isReal()) pv = static_cast<std::int64_t>(p.getNumericValue());
        if (!pv || ((*pv & 512) != 512))
            report("EQX-UA1-7.16-1", -1, ref_or(enc, {}),
                   pv ? "P " + std::to_string(*pv) + " clears bit 10" : "no P entry");
    }

    // ----------------------------------------------------------------------------------- destinations
    // PDDestination.getIsStructDestination
    bool is_struct_destination(Obj dest) {
        if (dest.isString()) {
            Obj names = cos::get(cat_, "Names");
            if (!is_dict(names)) return false;
            Obj dests = cos::get(names, "Dests");
            if (is_dict(dests)) {
                const std::u32string key = pdf_text_to_u32(dest.getStringValue());
                // A plain tree through its decoded shape (core/tree_shape.h), any other by the walk over the objects.
                if (!dests_shape_) dests_shape_ = std::make_unique<TreeShape<std::u32string>>(dests, "Names", &text_limit, true);
                Obj r;
                const auto leaf = [&](const Obj& node, Obj& value) { return names_value(node, key, &dest_names_, value); };
                switch (dests_shape_->find(key, leaf, r)) {
                    case TreeShape<std::u32string>::Result::Found:
                        break;
                    case TreeShape<std::u32string>::Result::NotFound:
                        r = Obj();
                        break;
                    case TreeShape<std::u32string>::Result::NotPlain: {
                        std::unordered_set<std::uint64_t> seen;
                        r = name_tree_lookup(dests, key, seen, &dest_names_);
                        break;
                    }
                }
                if (!r.isInitialized()) return false;
                dest = r;
            }
        } else if (dest.isName()) {
            Obj dests = cos::get(cat_, "Dests");
            if (!is_dict_based(dests)) return false;
            dest = cos::get(dests, dest.getName().substr(1));
        }
        if (dest.isDictionary()) return known(dest, "SD");
        if (dest.isArray() && dest.getArrayNItems() > 0) return known(dest.getArrayItem(0), "S");
        return false;
    }

    std::unordered_set<std::uint64_t> actions_seen_;
    NamesIndex dest_names_;   // the /Dests name tree's /Names arrays, indexed on first lookup
    std::unique_ptr<TreeShape<std::u32string>> dests_shape_;   // its /Kids and /Limits, as lookups reach them

    // GFPDAction / GFPDGoToAction (UA-2 8.8-2: containsStructDestination) and the Next chain
    void action(const Obj& first, int page, ObjRef owner) {
        std::vector<Obj> stack{first};   // /Next chains have no length limit
        while (!stack.empty()) {
            Obj a = stack.back();
            stack.pop_back();
            if (!is_dict(a)) continue;
            if (a.isIndirect() && !actions_seen_.insert(ref_of(a).key()).second) continue;
            if (cos::name_is(a, "S", "GoTo")) {
                bool ok = known(a, "SD");
                if (!ok && known(a, "D")) {
                    Obj d = cos::get(a, "D");
                    if (d.isName() || d.isString()) ok = is_struct_destination(d);
                }
                if (!ok) report("EQX-UA2-8.8-2", page, ref_or(a, owner), "GoTo action without a structure destination");
            }
            Obj next = cos::get(a, "Next");
            if (is_dict(next)) {
                stack.push_back(next);
            } else if (next.isArray()) {
                auto items = cos::array_items(next);
                for (auto it = items.rbegin(); it != items.rend(); ++it) stack.push_back(*it);
            }
        }
    }

    void additional_actions(const Obj& aa, std::initializer_list<const char*> keys, int page, ObjRef owner) {
        if (!is_dict_based(aa)) return;
        for (const char* k : keys) action(cos::get(aa, k), page, owner);
    }

    void destination(const Obj& dest, int page, ObjRef owner) {
        // UA-2 8.8-1 on GFPDDestination
        if (!is_struct_destination(dest)) report("EQX-UA2-8.8-1", page, owner, "destination is not a structure destination");
    }

    void destinations() {
        // OpenAction: dictionary -> action, array -> destination
        Obj oa = cos::get(cat_, "OpenAction");
        if (is_dict(oa)) action(oa, -1, cat_ref_);
        else if (oa.isArray()) destination(oa, -1, cat_ref_);
        Obj caa = cos::get(cat_, "AA");
        if (is_dict(caa)) additional_actions(caa, {"WC", "WS", "DS", "WP", "DP"}, -1, cat_ref_);
        for (const auto& pg : ctx_->doc.pages()) {
            Obj paa = cos::get(pg.dict, "AA");
            if (is_dict(paa)) additional_actions(paa, {"O", "C"}, pg.index, pg.ref);
            for (auto& an : cos::array_items(cos::get(pg.dict, "Annots"))) {
                if (!is_dict(an)) continue;
                ObjRef ar = ref_or(an, pg.ref);
                action(cos::get(an, "A"), pg.index, ar);
                Obj aa = cos::get(an, "AA");
                if (is_dict(aa))
                    additional_actions(aa, {"E", "X", "D", "U", "Fo", "Bl", "PO", "PC", "PV", "PI"}, pg.index, ar);
                if (cos::name_is(an, "Subtype", "Link") && known(an, "Dest") && !known(an, "A"))
                    destination(cos::get(an, "Dest"), pg.index, ar);
            }
        }
        for_each_field([&](const Obj& f) {
            additional_actions(cos::get(f, "AA"), {"K", "F", "V", "C"}, -1, ref_or(f, cat_ref_));
        });
        collect_outlines();
        for (auto& [item, ref] : outlines_) {
            if (known(item, "A")) {
                action(cos::get(item, "A"), -1, ref);
            } else if (known(item, "Dest")) {
                destination(cos::get(item, "Dest"), -1, ref);
            }
        }
    }
};

}  // namespace

std::unique_ptr<RuleModule> make_document_module() { return std::make_unique<Module>(); }

}  // namespace eqx
