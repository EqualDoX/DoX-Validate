// StructTree::reaches_root keeps its answer per dictionary and StructElem::keys notes which keys an element has: both
// must give what the plain walk over /P and the plain dictionary lookup give, for chains through elements, through
// dictionaries outside the tree and direct dictionaries, chains that end, loops, and in any order of asking.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/document.h"
#include "core/structure.h"
#include "unit.h"

using namespace eqx;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

// The walk as it was before the answers were kept.
bool walk_reaches_root(const Obj& d) {
    std::unordered_set<std::uint64_t> seen;
    Obj e = d;
    while (e.isDictionary()) {
        if (cos::name_is(e, "Type", "StructTreeRoot")) return true;
        if (e.isIndirect() && !seen.insert(ref_of(e).key()).second) break;
        e = cos::get(e, "P");
    }
    return false;
}

std::unique_ptr<Document> write_and_open(QPDF& q, const char* tag) {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    {
        QPDFWriter w(q, path.string().c_str());
        w.setStaticID(true);
        w.setObjectStreamMode(qpdf_o_disable);
        w.write();
    }
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path.string(), &status, &err);
    if (!doc) eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
    std::filesystem::remove(path);   // the open file stays readable
    return doc;
}

// A tree whose elements have every kind of /P chain. Returns the dictionaries outside the tree that chains pass.
std::vector<Obj> build_tree(QPDF& q) {
    Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
    auto elem = [&](const char* s) { return q.makeIndirectObject(P(std::string("<< /S /") + s + " >>")); };
    Obj doc = elem("Document");
    doc.replaceKey("/P", root);
    Obj reaches = elem("P");            // /P the Document
    reaches.replaceKey("/P", doc);
    Obj no_p = elem("P");               // no /P at all
    Obj outside1 = q.makeIndirectObject(P("<< /S /Sect >>")), outside2 = q.makeIndirectObject(P("<< /S /Sect >>"));
    outside1.replaceKey("/P", outside2);
    outside2.replaceKey("/P", root);
    Obj via_outside = elem("P");        // through two dictionaries that are no kids of anything
    via_outside.replaceKey("/P", outside1);
    Obj loop1 = q.makeIndirectObject(P("<< /S /Sect >>")), loop2 = q.makeIndirectObject(P("<< /S /Sect >>"));
    loop1.replaceKey("/P", loop2);
    loop2.replaceKey("/P", loop1);
    Obj into_loop = elem("P");          // into a loop of two dictionaries
    into_loop.replaceKey("/P", loop1);
    Obj self = elem("P");               // its own parent
    self.replaceKey("/P", self);
    Obj not_dict = elem("P");           // /P is an integer
    not_dict.replaceKey("/P", P("5"));
    Obj direct = elem("P");             // /P is a direct dictionary that names the root
    Obj direct_parent = QPDFObjectHandle::newDictionary();
    direct_parent.replaceKey("/P", root);
    direct.replaceKey("/P", direct_parent);
    Obj other_root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
    Obj to_other_root = elem("P");      // another dictionary typed as a root counts
    to_other_root.replaceKey("/P", other_root);
    Obj child_ok = elem("Span"), child_lost = elem("Span"), grandchild_lost = elem("Span");
    child_ok.replaceKey("/P", reaches);
    child_lost.replaceKey("/P", no_p);
    grandchild_lost.replaceKey("/P", child_lost);
    Obj elem_in_loop = elem("Span");    // an element whose parent chain comes back to itself through another element
    Obj elem_in_loop2 = elem("Span");
    elem_in_loop.replaceKey("/P", elem_in_loop2);
    elem_in_loop2.replaceKey("/P", elem_in_loop);
    auto kids = [](std::initializer_list<Obj> items) {
        Obj a = QPDFObjectHandle::newArray();
        for (const Obj& o : items) a.appendItem(o);
        return a;
    };
    child_lost.replaceKey("/K", grandchild_lost);
    reaches.replaceKey("/K", child_ok);
    no_p.replaceKey("/K", child_lost);
    elem_in_loop.replaceKey("/K", elem_in_loop2);
    doc.replaceKey("/K", kids({reaches, no_p, via_outside, into_loop, self, not_dict, direct, to_other_root, elem_in_loop}));
    root.replaceKey("/K", doc);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", P("<< /Marked true >>"));
    return {outside1, outside2, loop1, loop2, other_root};
}

}  // namespace

EQX_TEST(structure_reaches_root_matches_the_walk_in_any_order) {
    for (int order = 0; order < 4; ++order) {
        QPDF q;
        q.emptyPDF();
        build_tree(q);
        auto doc = write_and_open(q, "reaches_root");
        if (!doc) return;
        StructTree st;
        st.build(*doc);
        const auto& elems = st.elems();
        EXPECT_EQ(elems.size(), std::size_t(14));
        // the dictionaries the chains pass that are no elements: reached through /P of the elements
        std::vector<Obj> asked;
        for (const StructElem& e : elems) asked.push_back(e.dict);
        for (const StructElem& e : elems) {
            Obj p = cos::get(e.dict, "P");
            for (int hop = 0; hop < 3 && p.isDictionary(); ++hop) {
                asked.push_back(p);
                p = cos::get(p, "P");
            }
        }
        if (order == 1) std::reverse(asked.begin(), asked.end());
        if (order == 2) std::rotate(asked.begin(), asked.begin() + std::ptrdiff_t(asked.size() / 2), asked.end());
        if (order == 3)   // the parents before the elements
            std::stable_partition(asked.begin(), asked.end(), [&](const Obj& o) { return !o.isIndirect() || st.find(ref_of(o)) < 0; });
        int yes = 0;
        for (int pass = 0; pass < 2; ++pass)   // the second pass reads only kept answers
            for (const Obj& o : asked) {
                const bool want = walk_reaches_root(o);
                EXPECT_EQ(st.reaches_root(o), want);
                yes += want ? 1 : 0;
            }
        EXPECT(yes > 0 && yes < 2 * int(asked.size()));   // both answers occur
        EXPECT(!st.reaches_root(Obj::newNull()));
        EXPECT(st.reaches_root(st.root()));
    }
}

EQX_TEST(structure_element_keys_tell_what_the_dictionary_holds) {
    QPDF q;
    q.emptyPDF();
    Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
    Obj all = q.makeIndirectObject(P("<< /S /P /Alt (a) /ActualText (b) /E (c) /Lang (en) /A << /O /Layout >> /C /Cls /ID (i) /Ref [] /T (t) >>"));
    all.replaceKey("/P", root);
    Obj none = q.makeIndirectObject(P("<< /S /Span /Type /StructElem /Pg 3 /K 7 >>"));
    // other keys of the same lengths, and a key whose value is an indirect null object
    Obj near = q.makeIndirectObject(P("<< /S /Span /Q 1 /IX 1 /Alx 1 /Lanx 1 /ActualTexx 1 >>"));
    near.replaceKey("/Alt", q.makeIndirectObject(QPDFObjectHandle::newNull()));
    Obj kids = QPDFObjectHandle::newArray();
    for (const Obj& o : {all, none, near}) kids.appendItem(o);
    root.replaceKey("/K", kids);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    auto doc = write_and_open(q, "element_keys");
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    EXPECT_EQ(st.elems().size(), std::size_t(3));
    struct K {
        StructElem::Key bit;
        const char* name;
    };
    const K keys[] = {{StructElem::kP, "P"}, {StructElem::kAlt, "Alt"}, {StructElem::kActualText, "ActualText"}, {StructElem::kE, "E"},
                      {StructElem::kLang, "Lang"}, {StructElem::kA, "A"}, {StructElem::kC, "C"}, {StructElem::kID, "ID"},
                      {StructElem::kRef, "Ref"}, {StructElem::kT, "T"}};
    for (const StructElem& e : st.elems()) {
        const auto entries = e.dict.getDictAsMap();
        for (const K& k : keys) {
            // the bit: the key is in the dictionary; get(): what cos::get gives
            EXPECT_EQ(e.has_key(k.bit), entries.count(std::string("/") + k.name) > 0);
            Obj a = e.get(k.bit, k.name), b = cos::get(e.dict, k.name);
            EXPECT_EQ(a.isNull(), b.isNull());
            EXPECT_EQ(a.getTypeCode(), b.getTypeCode());
        }
    }
    EXPECT_EQ(st.elem(0).keys, std::uint16_t(0x3ff));   // every key
    EXPECT_EQ(st.elem(1).keys, std::uint16_t(0));
    // keys that only resemble the noted ones set no bit (the /Alt naming a null object is judged by the loop above:
    // whether or not the entry is in the file, the bit follows the dictionary and get() gives the null cos::get gives)
    EXPECT(!st.elem(2).has_key(StructElem::kActualText) && !st.elem(2).has_key(StructElem::kLang) && !st.elem(2).has_key(StructElem::kID) &&
           !st.elem(2).has_key(StructElem::kP) && !st.elem(2).has_key(StructElem::kE));
}
