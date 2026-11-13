// Unit tests of the containment module: generated table, atom semantics (JavaScript semantics of the
// veraPDF profile tests) and the document-level reproduction of veraPDF's structure traversal.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <set>

#include "core/document.h"
#include "core/structure.h"
#include "rules/containment/containment.h"
#include "unit.h"

using namespace eqx;
using namespace eqx::containment;

namespace {

const GenRule& rule(const char* id) {
    std::size_t n = 0;
    const GenRule* r = generated_rules(&n);
    int i = find_generated(id);
    if (i < 0) {
        eqx_test::fail(__FILE__, __LINE__, std::string("unknown rule ") + id);
        return r[0];
    }
    return r[i];
}

ObjectView view(std::optional<std::string> own, std::vector<std::string> kids, bool content = false,
                std::optional<std::string> parent = std::nullopt) {
    ObjectView v;
    v.standard_type = std::move(own);
    v.kids = std::move(kids);
    v.has_content_items = content;
    v.parent_standard_type = std::move(parent);
    return v;
}

bool passes(const char* id, const ObjectView& v) { return test_passes(rule(id), v); }

// ---- synthetic documents --------------------------------------------------------------------------------

struct TreeDoc {
    QPDF q;
    Obj root;
    Obj page;
    TreeDoc() {
        q.emptyPDF();
        page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
        q.addPage(page, true);
        root = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructTreeRoot /K [] >>"));
        Obj cat = q.getRoot();
        cat.replaceKey("/StructTreeRoot", root);
    }
    Obj elem(const char* S, Obj parent, bool indirect = true) {
        Obj d = QPDFObjectHandle::newDictionary();
        d.replaceKey("/Type", QPDFObjectHandle::newName("/StructElem"));
        d.replaceKey("/S", QPDFObjectHandle::newName(std::string("/") + S));
        d.replaceKey("/P", parent);
        d.replaceKey("/K", QPDFObjectHandle::newArray());
        return indirect ? q.makeIndirectObject(d) : d;
    }
    static void add(Obj parent, Obj kid) { parent.getKey("/K").appendItem(kid); }
};

struct Result {
    std::map<std::string, std::set<std::string>> violations;   // rule id -> object refs ("N G" or "direct")
    std::set<std::string> incomplete;
    std::map<std::string, int> occurrences;   // rule id -> number of failed objects
    std::size_t total = 0;
    bool has(const std::string& id) const { return violations.count(id) > 0; }
};

Result run(TreeDoc& t, bool reference_dedup = false) {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_containment_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    {
        QPDFWriter w(t.q, path.string().c_str());
        w.setStaticID(true);
        w.write();
    }
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path.string(), &status, &err);
    Result res;
    if (!doc) {
        eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
        return res;
    }
    StructTree st;
    st.build(*doc);
    std::size_t n = 0;
    const GenRule* rules = generated_rules(&n);
    Sink sink;
    sink.violation = [&](std::size_t gen, ObjRef obj, std::string) {
        res.violations[rules[gen].id].insert(obj.valid() ? obj.str() : "direct");
        ++res.occurrences[rules[gen].id];
        ++res.total;
    };
    sink.incomplete = [&](std::size_t gen, std::string) { res.incomplete.insert(rules[gen].id); };
    evaluate(st, sink, reference_dedup);
    std::filesystem::remove(path);
    return res;
}

std::string ref(const Obj& o) { return std::to_string(o.getObjectID()) + " " + std::to_string(o.getGeneration()); }

}  // namespace

// ---- table ------------------------------------------------------------------------------------------------

EQX_TEST(containment_table_complete) {
    std::size_t n = 0;
    const GenRule* r = generated_rules(&n);
    EXPECT_EQ(n, std::size_t(1667));
    std::set<std::string> ids;
    std::map<std::string, int> prefix;
    for (std::size_t i = 0; i < n; ++i) {
        ids.insert(r[i].id);
        std::string id = r[i].id;
        prefix[id.substr(0, id.find('-', 4))]++;
        EXPECT(r[i].natoms >= 1 && r[i].natoms <= 3);
        // ua1 rules use PDF 1.7 semantics, every other rule PDF 2.0 semantics
        EXPECT((id.rfind("EQX-UA1-", 0) == 0) == (r[i].flavour == Flavor::Pdf17));
        // every rule evaluates without error on trivial objects
        (void)test_passes(r[i], view(std::nullopt, {}));
        (void)test_passes(r[i], view("P", {"Span", "Caption"}, true, "Div"));
    }
    EXPECT_EQ(ids.size(), n);
    EXPECT_EQ(prefix["EQX-TS32005"], 1636);
    EXPECT_EQ(prefix["EQX-UA1"], 26);
    EXPECT_EQ(prefix["EQX-UA2"], 5);
}

// ---- atoms ------------------------------------------------------------------------------------------------

EQX_TEST(containment_forbidden_child_non_grouping) {
    // isGrouping == true || kids.filter(== 'Aside').length == 0
    EXPECT(!passes("EQX-TS32005-Annot-Aside-1", view("Annot", {"Aside"}, true)));     // content: non-grouping
    EXPECT(!passes("EQX-TS32005-Annot-Aside-1", view("Annot", {"Aside", "Span"})));    // inline kid: non-grouping
    EXPECT(passes("EQX-TS32005-Annot-Aside-1", view("Annot", {"Aside"})));             // grouping
    EXPECT(passes("EQX-TS32005-Annot-Aside-1", view("Annot", {"P"}, true)));
    // Hn filter
    EXPECT(!passes("EQX-TS32005-Link-Hn-1", view("Link", {"H12"}, true)));
    EXPECT(passes("EQX-TS32005-Link-Hn-1", view("Link", {"H"}, true)));
}

EQX_TEST(containment_grouping_max_count) {
    // isGrouping == false || Caption count <= 1
    EXPECT(!passes("EQX-TS32005-Annot-Caption-1", view("Annot", {"Caption", "P", "Caption"})));
    EXPECT(passes("EQX-TS32005-Annot-Caption-1", view("Annot", {"Caption", "P", "Caption"}, true)));
    // Art is always grouping: plain max count
    EXPECT(!passes("EQX-TS32005-Art-H-1", view("Art", {"H", "H"})));
    EXPECT(passes("EQX-TS32005-Art-H-1", view("Art", {"H"})));
}

EQX_TEST(containment_parent_rules) {
    // parentStandardType != 'Annot' (object: the child)
    EXPECT(!passes("EQX-TS32005-Annot-Document-1", view("Document", {}, false, "Annot")));
    EXPECT(passes("EQX-TS32005-Annot-Document-1", view("Document", {}, false, std::nullopt)));
    EXPECT(!passes("EQX-TS32005-StructTreeRoot-P-1", view("P", {}, true, "StructTreeRoot")));
    // Hn regex on the parent; null never matches
    EXPECT(!passes("EQX-TS32005-Hn-P-1", view("P", {}, true, "H3")));
    EXPECT(passes("EQX-TS32005-Hn-P-1", view("P", {}, true, "H")));
    EXPECT(passes("EQX-TS32005-Hn-P-1", view("P", {}, true, std::nullopt)));
    // required parent: null fails
    EXPECT(!passes("EQX-UA1-7.2-5", view("THead", {"TR"}, false, std::nullopt)));
    EXPECT(passes("EQX-UA1-7.2-5", view("THead", {"TR"}, false, "Table")));
    EXPECT(passes("EQX-UA1-7.2-4", view("TR", {"TD"}, false, "TBody")));
    EXPECT(!passes("EQX-UA1-7.2-4", view("TR", {"TD"}, false, "Div")));
    EXPECT(!passes("EQX-UA1-7.2-4", view("TR", {"TD"}, false, std::nullopt)));
}

EQX_TEST(containment_allowed_children) {
    EXPECT(passes("EQX-UA1-7.2-3", view("Table", {})));
    EXPECT(passes("EQX-UA1-7.2-3", view("Table", {"Caption", "THead", "TBody", "TR"})));
    EXPECT(!passes("EQX-UA1-7.2-3", view("Table", {"TR", "P"})));
    EXPECT(passes("EQX-UA1-7.2-10", view("TR", {})));
    EXPECT(passes("EQX-UA1-7.2-10", view("TR", {"TH", "TD"})));
    EXPECT(!passes("EQX-UA1-7.2-10", view("TR", {"TH", "P"})));
    EXPECT(passes("EQX-UA1-7.2-20", view("LI", {"Lbl", "LBody"})));
    EXPECT(!passes("EQX-UA1-7.2-20", view("LI", {"P"})));
}

EQX_TEST(containment_caption_position) {
    // indexOf('&Caption&') < 0: Caption neither first nor last
    EXPECT(passes("EQX-UA1-7.2-16", view("Table", {"Caption", "TR"})));
    EXPECT(passes("EQX-UA1-7.2-16", view("Table", {"TR", "Caption"})));
    EXPECT(passes("EQX-UA1-7.2-16", view("Table", {"Caption"})));
    EXPECT(passes("EQX-UA1-7.2-16", view("Table", {"Caption", "Caption"})));
    EXPECT(!passes("EQX-UA1-7.2-16", view("Table", {"TR", "Caption", "TR"})));
    EXPECT(!passes("EQX-UA1-7.2-16", view("Table", {"Caption", "Caption", "TR"})));
    EXPECT(!passes("EQX-UA2-8.2.5.27-1", view("Figure", {"P", "Caption", "P"})));
    // slice(1).findIndex(Caption) < 0: Caption only first
    EXPECT(passes("EQX-UA1-7.2-28", view("TOC", {})));
    EXPECT(passes("EQX-UA1-7.2-28", view("TOC", {"Caption", "TOCI"})));
    EXPECT(!passes("EQX-UA1-7.2-28", view("TOC", {"TOCI", "Caption"})));
    EXPECT(!passes("EQX-UA1-7.2-40", view("L", {"Caption", "Caption"})));
}

EQX_TEST(containment_sequences_and_counts) {
    EXPECT(passes("EQX-UA2-8.2.5.23-1", view("Ruby", {"RB", "RT"})));
    EXPECT(passes("EQX-UA2-8.2.5.23-1", view("Ruby", {"RB", "RP", "RT", "RP"})));
    EXPECT(!passes("EQX-UA2-8.2.5.23-1", view("Ruby", {"RB"})));
    EXPECT(!passes("EQX-UA2-8.2.5.23-1", view("Ruby", {})));
    EXPECT(passes("EQX-UA2-8.2.5.24-1", view("Warichu", {"WP", "WT", "WP"})));
    EXPECT(!passes("EQX-UA2-8.2.5.24-1", view("Warichu", {"WP", "WT"})));
    EXPECT(!passes("EQX-TS32005-StructTreeRoot-Document-1", view(std::nullopt, {})));
    EXPECT(passes("EQX-TS32005-StructTreeRoot-Document-1", view(std::nullopt, {"Document"})));
    EXPECT(!passes("EQX-TS32005-StructTreeRoot-Document-1", view(std::nullopt, {"Document", "Document"})));
    EXPECT(!passes("EQX-UA2-8.2.5.2-1", view(std::nullopt, {"Document", "P"})));
    // dependency: TFoot needs TBody
    EXPECT(!passes("EQX-UA1-7.2-13", view("Table", {"TFoot", "TR"})));
    EXPECT(passes("EQX-UA1-7.2-13", view("Table", {"TBody", "TFoot"})));
    EXPECT(passes("EQX-UA1-7.2-13", view("Table", {})));
    EXPECT(!passes("EQX-UA1-7.2-39", view("Table", {"Caption", "TR", "Caption"})));
    EXPECT(!passes("EQX-UA1-7.4.4-1", view("Sect", {"H", "P", "H"})));
    EXPECT(passes("EQX-UA1-7.4.4-1", view("Sect", {"H", "H1"})));
    // content items
    EXPECT(!passes("EQX-UA2-8.2.5.25-2", view("LI", {"Lbl"}, true)));
    EXPECT(passes("EQX-UA2-8.2.5.25-2", view("LI", {"Lbl", "LBody"})));
}

// ---- documents ----------------------------------------------------------------------------------------------

EQX_TEST(containment_doc_pass_through_flattening) {
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj sect = t.elem("Sect", doc);
    TreeDoc::add(doc, sect);
    Obj div = t.elem("Div", sect);
    TreeDoc::add(sect, div);
    Obj h1 = t.elem("H", div), h2 = t.elem("H", div);
    TreeDoc::add(div, h1);
    TreeDoc::add(div, h2);
    Obj part = t.elem("Part", sect);   // a Part kid of an Annot is never visible (pass-through)
    TreeDoc::add(sect, part);
    Obj annot = t.elem("Annot", part);
    TreeDoc::add(part, annot);
    annot.getKey("/K").appendItem(QPDFObjectHandle::newInteger(0));
    Obj inner = t.elem("Part", annot);
    TreeDoc::add(annot, inner);
    Result r = run(t);
    // the two H kids of the Div count for the Sect (and for the Div itself)
    EXPECT(r.has("EQX-UA1-7.4.4-1"));
    EXPECT(r.violations["EQX-UA1-7.4.4-1"].count(ref(sect)) == 1);
    EXPECT(r.violations["EQX-UA1-7.4.4-1"].count(ref(div)) == 1);
    EXPECT(r.has("EQX-TS32005-Sect-H-1"));
    // the Annot's parentStandardType skips the Part: Sect
    EXPECT(!r.has("EQX-TS32005-Annot-Part-1"));
    // the inner Part's parent is the Annot (not pass-through): Annot-Part-1 is a kid rule and never fails,
    // but the Part element itself is checked against its parent
    EXPECT(r.has("EQX-TS32005-Annot-Part-1") == false);
    // root has exactly one Document
    EXPECT(!r.has("EQX-TS32005-StructTreeRoot-Document-1"));
    EXPECT(r.incomplete.empty());
}

EQX_TEST(containment_doc_shared_kid_counted_twice) {
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj sect = t.elem("Sect", doc);
    TreeDoc::add(doc, sect);
    Obj h = t.elem("H", sect);
    TreeDoc::add(sect, h);
    TreeDoc::add(sect, h);   // same indirect element listed twice
    Result r = run(t);
    EXPECT(r.violations["EQX-UA1-7.4.4-1"].count(ref(sect)) == 1);
}

EQX_TEST(containment_doc_direct_duplicates_checked_once) {
    // veraPDF ids "0 0 obj <S>" for direct elements: of two direct THead with the same /S, veraPDF checks only
    // the one pushed first (reference mode); by default eqx evaluates both.
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj d1 = t.elem("THead", doc, false);
    Obj d2 = t.elem("THead", doc, false);
    TreeDoc::add(doc, d1);
    TreeDoc::add(doc, d2);
    Obj d3 = t.elem("TFoot", doc, false);   // different /S: own id, checked
    TreeDoc::add(doc, d3);
    Result ref_mode = run(t, true);
    EXPECT_EQ(ref_mode.occurrences["EQX-UA1-7.2-5"], 1);
    EXPECT_EQ(ref_mode.occurrences["EQX-UA1-7.2-7"], 1);
    Result r = run(t);
    EXPECT_EQ(r.occurrences["EQX-UA1-7.2-5"], 2);
    EXPECT_EQ(r.occurrences["EQX-UA1-7.2-7"], 1);
    // two indirect THead are both checked
    TreeDoc t2;
    Obj doc2 = t2.elem("Document", t2.root);
    TreeDoc::add(t2.root, doc2);
    TreeDoc::add(doc2, t2.elem("THead", doc2));
    TreeDoc::add(doc2, t2.elem("THead", doc2));
    Result r2 = run(t2);
    EXPECT_EQ(r2.occurrences["EQX-UA1-7.2-5"], 2);
}

EQX_TEST(containment_doc_cycles_are_incomplete) {
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj a = t.elem("Div", doc);
    Obj b = t.elem("Div", a);
    TreeDoc::add(doc, a);
    TreeDoc::add(a, b);
    TreeDoc::add(b, a);                  // pass-through cycle in /K
    Obj p = t.elem("P", b);
    TreeDoc::add(b, p);
    Obj x = t.elem("NonStruct", doc);    // pass-through cycle in /P
    Obj y = t.elem("NonStruct", x);
    x.replaceKey("/P", y);
    Obj span = t.elem("TR", y);
    TreeDoc::add(doc, span);
    Result r = run(t);
    // kidsStandardTypes of the Document does not terminate in veraPDF
    EXPECT(r.incomplete.count("EQX-UA1-7.4.4-1") == 1);
    EXPECT(r.incomplete.count("EQX-TS32005-Document-content-1") == 1);
    // parentStandardType of the TR loops (TR -> y -> x -> y ...)
    EXPECT(r.incomplete.count("EQX-UA1-7.2-4") == 1);
}

EQX_TEST(containment_doc_deep_tree_no_crash) {
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj cur = doc;
    for (int i = 0; i < 20000; ++i) {
        Obj d = t.elem(i % 2 ? "Div" : "Sect", cur);
        TreeDoc::add(cur, d);
        cur = d;
    }
    Obj h = t.elem("H", cur), h2 = t.elem("H", cur);
    TreeDoc::add(cur, h);
    TreeDoc::add(cur, h2);
    Result r = run(t);
    EXPECT(r.has("EQX-UA1-7.4.4-1"));
    EXPECT(r.incomplete.empty());
}

EQX_TEST(containment_doc_malformed_entries) {
    TreeDoc t;
    Obj doc = t.elem("Document", t.root);
    TreeDoc::add(t.root, doc);
    Obj odd = t.elem("P", doc);
    odd.replaceKey("/S", QPDFObjectHandle::newInteger(5));         // non-name /S: null standard type
    odd.replaceKey("/K", QPDFObjectHandle::newString("garbage"));  // malformed /K
    TreeDoc::add(doc, odd);
    Obj p = t.elem("P", doc);
    p.replaceKey("/P", QPDFObjectHandle::newInteger(3));           // non-dictionary /P: null parent
    TreeDoc::add(doc, p);
    Obj th = t.elem("TH", doc);
    TreeDoc::add(doc, th);
    Result r = run(t);
    EXPECT(r.has("EQX-UA1-7.2-8"));   // TH parent is Document, not TR
    EXPECT(r.incomplete.empty());
}
