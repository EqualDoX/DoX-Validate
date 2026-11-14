// The structure tree keeps packed records (64 bytes per element, one word per kid, the facts of a type once per type)
// and hands out views. Every member of every view must be what a tree of plain records holds: the traversal is
// written out here once more with plain structs and vectors, and the two are compared member by member, for every kind
// of kid, shared and cyclic elements, direct elements, types with and without namespaces and role mapping.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/document.h"
#include "core/struct_store.h"
#include "core/structure.h"
#include "unit.h"

using namespace eqx;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

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

Obj array_of(std::initializer_list<Obj> items) {
    Obj a = QPDFObjectHandle::newArray();
    for (const Obj& o : items) a.appendItem(o);
    return a;
}

// ---- the tree as plain records ----------------------------------------------------------------------------------

struct PlainKid {
    StructKid::Kind kind = StructKid::Kind::Elem;
    int elem = -1;
    bool revisit = false;
    std::int64_t mcid = -1;
    Obj dict;
    ObjRef page;
    ObjRef stm;
    Obj obj;
};

struct PlainElem {
    int index = -1, parent = -1, depth = 0;
    Obj dict;
    ObjRef ref;
    std::optional<std::string> S;
    SType type;
    std::optional<std::string> std17;
    std::optional<SType> std20;
    std::vector<PlainKid> kids;
    std::vector<int> elems, struct_kids;
    bool verapdf_checked = true;
    ObjRef page, inherited_page;
};

struct PlainTree {
    const StructTree& st;   // for the role mapping only (standard_type17 / default_type20)
    std::vector<PlainElem> elems;
    std::vector<PlainKid> root_kids;
    std::vector<int> root_elems, verapdf_order;
    std::unordered_map<std::uint64_t, int> by_ref;

    void content_kid(std::vector<PlainKid>& kids, const Obj& item, const std::string& type, ObjRef inherited) {
        PlainKid kid;
        kid.dict = item;
        ObjRef pg = ref_of(cos::get(item, "Pg"));
        kid.page = pg.valid() ? pg : inherited;
        if (type == "MCR") {
            kid.kind = StructKid::Kind::MCR;
            if (auto m = cos::integer(item, "MCID")) kid.mcid = *m;
            kid.stm = ref_of(cos::get(item, "Stm"));
        } else {
            kid.kind = StructKid::Kind::OBJR;
            kid.obj = cos::get(item, "Obj");
        }
        kids.push_back(kid);
    }

    // The kids of `node` (-1 = the root), each new element with its subtree before the next kid.
    void walk(const Obj& node, int parent, ObjRef inherited, int depth) {
        Obj k = cos::get(node, "K");
        std::vector<Obj> items;
        if (k.isArray()) items = cos::array_items(k);
        else if (!k.isNull()) items.push_back(k);
        auto kids = [&]() -> std::vector<PlainKid>& { return parent < 0 ? root_kids : elems[std::size_t(parent)].kids; };
        for (const Obj& item : items) {
            if (item.isInteger()) {
                PlainKid kid;
                kid.kind = StructKid::Kind::MCID;
                kid.mcid = item.getIntValue();
                kid.page = inherited;
                kids().push_back(kid);
                continue;
            }
            if (!item.isDictionary() && !item.isStream()) continue;
            auto type = cos::name(item, "Type");
            if (type && (*type == "MCR" || *type == "OBJR")) {
                content_kid(kids(), item, *type, inherited);
                continue;
            }
            if (item.isStream() || (type && *type != "StructElem")) continue;
            PlainKid kid;
            kid.dict = item;
            ObjRef r = ref_of(item);
            if (r.valid() && by_ref.count(r.key())) {
                kid.elem = by_ref[r.key()];
                kid.revisit = true;
                kids().push_back(kid);
                if (parent >= 0) elems[std::size_t(parent)].struct_kids.push_back(kid.elem);
                continue;
            }
            PlainElem e;
            e.index = static_cast<int>(elems.size());
            e.parent = parent;
            e.depth = depth;
            e.dict = item;
            e.ref = r;
            e.S = cos::name(item, "S");
            if (auto t = st.stype_of(item)) {
                e.type = *t;
                e.std20 = st.default_type20(*t);
            } else {
                e.type.ns = Obj::newNull();
            }
            if (e.S) e.std17 = st.standard_type17(*e.S);
            e.page = ref_of(cos::get(item, "Pg"));
            e.inherited_page = e.page.valid() ? e.page : inherited;
            const int idx = e.index;
            const ObjRef ip = e.inherited_page;
            elems.push_back(e);
            if (r.valid()) by_ref[r.key()] = idx;
            kid.elem = idx;
            kids().push_back(kid);
            if (parent < 0) root_elems.push_back(idx);
            else {
                elems[std::size_t(parent)].elems.push_back(idx);
                elems[std::size_t(parent)].struct_kids.push_back(idx);
            }
            walk(item, idx, ip, depth + 1);
        }
    }

    // veraPDF's visits: children pushed last to first, an id registered when pushed, a registered id not pushed again.
    void simulate() {
        for (PlainElem& e : elems) e.verapdf_checked = false;
        std::unordered_set<std::string> ids;
        std::vector<int> stack;
        auto push = [&](const std::vector<int>& kids) {
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                const PlainElem& k = elems[std::size_t(*it)];
                if (k.ref.valid()) {
                    if (!ids.insert("R" + std::to_string(k.ref.key())).second) continue;
                } else if (k.S) {
                    if (!ids.insert("D" + *k.S).second) continue;
                }
                stack.push_back(*it);
            }
        };
        std::vector<int> root_struct;
        for (const PlainKid& k : root_kids)
            if (k.kind == StructKid::Kind::Elem) root_struct.push_back(k.elem);
        push(root_struct);
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            elems[std::size_t(i)].verapdf_checked = true;
            verapdf_order.push_back(i);
            push(elems[std::size_t(i)].struct_kids);
        }
    }
};

// The same object: both uninitialised, or the same indirect object, or one direct object.
bool same_handle(const Obj& a, const Obj& b) {
    if (!a || !b) return !a && !b;
    if (a.isIndirect() || b.isIndirect()) return ref_of(a) == ref_of(b);
    return a.isSameObjectAs(b) || (a.isNull() && b.isNull());
}

bool same_stype(const SType& a, const SType& b) {
    return a.type == b.type && a.ns.isDictionary() == b.ns.isDictionary() && (!a.ns.isDictionary() || same_handle(a.ns, b.ns)) &&
           a.ns_uri() == b.ns_uri();
}

bool same_opt_stype(const std::optional<SType>& a, const std::optional<SType>& b) {
    return a.has_value() == b.has_value() && (!a || same_stype(*a, *b));
}

void expect_same_kids(const StructKids& got, const std::vector<PlainKid>& want) {
    EXPECT_EQ(got.size(), want.size());
    EXPECT_EQ(got.empty(), want.empty());
    if (got.size() != want.size()) return;
    std::size_t i = 0;
    for (const StructKid& k : got) {   // by iteration and by index: the same kid
        const PlainKid& w = want[i];
        const StructKid at = got[i];
        ++i;
        EXPECT(k.kind == w.kind && at.kind == w.kind);
        EXPECT_EQ(k.elem, w.elem);
        EXPECT_EQ(at.elem, w.elem);
        EXPECT_EQ(k.revisit, w.revisit);
        EXPECT_EQ(k.mcid, w.mcid);
        EXPECT_EQ(at.mcid, w.mcid);
        EXPECT(same_handle(k.dict, w.dict));
        EXPECT(k.page == w.page && at.page == w.page);
        EXPECT(k.stm == w.stm);
        EXPECT(same_handle(k.obj, w.obj));
    }
    EXPECT_EQ(i, want.size());
}

void expect_same_ints(std::span<const int> got, const std::vector<int>& want) {
    EXPECT(std::vector<int>(got.begin(), got.end()) == want);
    EXPECT(std::vector<int>(got.rbegin(), got.rend()) == std::vector<int>(want.rbegin(), want.rend()));
    EXPECT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < want.size() && i < got.size(); ++i) EXPECT_EQ(got[i], want[i]);
}

void expect_same_tree(const StructTree& st, const PlainTree& plain) {
    const auto& elems = st.elems();
    EXPECT_EQ(elems.size(), plain.elems.size());
    if (elems.size() != plain.elems.size()) return;
    std::size_t n = 0;
    for (const StructElem& e : elems) {
        const PlainElem& w = plain.elems[n];
        const StructElem& at = elems[n];
        const StructElem by_call = st.elem(static_cast<int>(n));
        ++n;
        EXPECT_EQ(e.index, w.index);
        EXPECT_EQ(at.index, w.index);
        EXPECT_EQ(by_call.index, w.index);
        EXPECT_EQ(e.parent, w.parent);
        EXPECT_EQ(e.depth, w.depth);
        EXPECT(same_handle(e.dict, w.dict));
        EXPECT(e.ref == w.ref);
        EXPECT(e.S == w.S);
        EXPECT(same_stype(e.type, w.type));
        EXPECT(e.type.ns_ref() == w.type.ns_ref());
        EXPECT(e.std17 == w.std17);
        EXPECT(same_opt_stype(e.std20, w.std20));
        for (Flavor f : {Flavor::Pdf17, Flavor::Pdf20}) {
            // StructElem::standard / is_math, written out for the plain record
            std::string standard;
            bool math = false;
            if (f == Flavor::Pdf17) standard = w.std17.value_or(std::string());
            else if (w.std20) {
                math = w.std20->ns_uri() == std::optional<std::string>(kNsMathML);
                standard = math ? "MathML" : w.std20->type;
            }
            EXPECT_EQ(e.standard(f), standard);
            EXPECT_EQ(e.is_math(f), math);
        }
        expect_same_kids(e.kids, w.kids);
        expect_same_kids(by_call.kids, w.kids);
        expect_same_ints(e.elems, w.elems);
        expect_same_ints(e.struct_kids, w.struct_kids);
        expect_same_ints(at.struct_kids, w.struct_kids);
        EXPECT_EQ(e.verapdf_checked, w.verapdf_checked);
        EXPECT(e.page == w.page);
        EXPECT(e.inherited_page == w.inherited_page);
        if (w.ref.valid()) EXPECT_EQ(st.find(w.ref), plain.by_ref.at(w.ref.key()));
        // p_is_parent: /P as it stands in the dictionary is a reference to the object the element hangs from
        const Obj p = cos::get(w.dict, "P");
        const ObjRef from = w.parent >= 0 ? plain.elems[std::size_t(w.parent)].ref : st.root_ref();
        EXPECT_EQ(e.p_is_parent, p.isIndirect() && from.valid() && ref_of(p) == from);
        if (e.p_is_parent) EXPECT(e.has_key(StructElem::kP) && p.isDictionary());
        EXPECT(e.type_id < st.type_count());
        // k_is_kids: as many kids as /K has items
        const Obj k = cos::get(w.dict, "K");
        const std::size_t items = k.isArray() ? static_cast<std::size_t>(const_cast<Obj&>(k).getArrayNItems()) : k.isNull() ? 0 : 1;
        EXPECT_EQ(e.k_is_kids, items == w.kids.size());
        // the key bits against the dictionary itself
        const auto entries = const_cast<Obj&>(w.dict).getDictAsMap();
        const std::pair<StructElem::Key, const char*> keys[] = {
            {StructElem::kP, "/P"}, {StructElem::kAlt, "/Alt"}, {StructElem::kActualText, "/ActualText"}, {StructElem::kE, "/E"},
            {StructElem::kLang, "/Lang"}, {StructElem::kA, "/A"}, {StructElem::kC, "/C"}, {StructElem::kID, "/ID"},
            {StructElem::kRef, "/Ref"}, {StructElem::kT, "/T"}};
        for (const auto& [bit, name] : keys) EXPECT_EQ(e.has_key(bit), entries.count(name) > 0);
    }
    EXPECT_EQ(n, plain.elems.size());
    expect_same_kids(st.root_kids(), plain.root_kids);
    EXPECT(st.root_elems() == plain.root_elems);
    EXPECT(st.verapdf_order() == plain.verapdf_order);
}

// A tree with every kind of kid and of element.
void build_mixed_tree(QPDF& q) {
    Obj page1 = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    Obj page2 = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page1, false);
    q.addPage(page2, false);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot /RoleMap << /Para /P /Chain /Para /Loop /Loop /Odd 5 >> >>"));
    auto ind = [&](const std::string& body) { return q.makeIndirectObject(P("<< " + body + " >>")); };
    Obj ns_pdf2 = ind("/Type /Namespace /NS (http://iso.org/pdf2/ssn)");
    Obj ns_math = ind("/Type /Namespace /NS (http://www.w3.org/1998/Math/MathML)");
    Obj ns_other = ind("/Type /Namespace /NS (urn:example)");
    Obj role_map_ns = QPDFObjectHandle::newDictionary();
    role_map_ns.replaceKey("/Mine", array_of({P("/P"), ns_pdf2}));
    ns_other.replaceKey("/RoleMapNS", role_map_ns);
    Obj annot = ind("/Type /Annot /Subtype /Link /Rect [0 0 1 1]");
    Obj form = q.newStream("q Q");
    form.getDict().replaceKey("/Type", P("/XObject"));

    Obj doc = ind("/S /Document /Lang (en) /ID (d)");
    Obj sect1 = ind("/S /Sect /T (one)");
    sect1.replaceKey("/Pg", page1);
    Obj sect2 = ind("/S /Sect /Type /StructElem");
    Obj shared = ind("/S /P /Alt (shared) /ActualText (x)");       // a kid of sect1 and of sect2
    Obj para1 = ind("/S /Para"), para2 = ind("/S /Para /E (e)"), chain = ind("/S /Chain"), loop = ind("/S /Loop");
    Obj unknown = ind("/S /Unknown /A << /O /Layout >> /C /cls");
    Obj no_s = ind("/Type /StructElem"), string_s = ind("/S (P)");
    Obj in_pdf2_a = ind("/S /P"), in_pdf2_b = ind("/S /P"), in_pdf2_title = ind("/S /Title"), in_pdf2_h7 = ind("/S /H7");
    for (Obj* e : {&in_pdf2_a, &in_pdf2_b, &in_pdf2_title, &in_pdf2_h7}) e->replaceKey("/NS", ns_pdf2);
    Obj in_math = ind("/S /math"), in_other = ind("/S /Mine"), in_other2 = ind("/S /Mine"), in_other_p = ind("/S /P");
    in_math.replaceKey("/NS", ns_math);
    for (Obj* e : {&in_other, &in_other2, &in_other_p}) e->replaceKey("/NS", ns_other);
    Obj direct_ns1 = ind("/S /P /NS << /NS (http://iso.org/pdf2/ssn) >>");
    Obj direct_ns2 = ind("/S /P /NS << /NS (urn:example:direct) >>");
    Obj direct_ns3 = ind("/S /P /NS << /NS (http://iso.org/pdf2/ssn) >>");
    Obj bad_ns = ind("/S /P /NS /NotADictionary /Ref [ ]");
    Obj cyclic = ind("/S /Div"), inside_cycle = ind("/S /Span");
    Obj mcr_stream = q.newStream("x");
    mcr_stream.getDict().replaceKey("/Type", P("/MCR"));
    mcr_stream.getDict().replaceKey("/MCID", P("9"));
    mcr_stream.getDict().replaceKey("/Pg", page2);
    Obj objr_stream = q.newStream("y");
    objr_stream.getDict().replaceKey("/Type", P("/OBJR"));
    objr_stream.getDict().replaceKey("/Obj", annot);
    Obj other_stream = q.newStream("z");

    Obj mcr_full = P("<< /Type /MCR /MCID 7 >>");
    mcr_full.replaceKey("/Pg", page2);
    mcr_full.replaceKey("/Stm", form);
    Obj objr_pg = P("<< /Type /OBJR >>");
    objr_pg.replaceKey("/Obj", annot);
    objr_pg.replaceKey("/Pg", page2);
    Obj objr_plain = P("<< /Type /OBJR >>");
    objr_plain.replaceKey("/Obj", annot);

    shared.replaceKey("/K", array_of({P("0"), P("1"), mcr_full, P("<< /Type /MCR /MCID /NoInteger >>"), P("<< /Type /MCR >>")}));
    para1.replaceKey("/K", P("4"));   // a single integer
    para2.replaceKey("/K", array_of({P("2305843009213693952"), P("-5"), P("-4611686018427387904"), P("1152921504606846975"),
                                     P("1152921504606846976"), P("-1152921504606846976"), P("-1152921504606846977"),
                                     P("9223372036854775807"), P("3")}));
    chain.replaceKey("/K", array_of({objr_pg, objr_plain, mcr_stream, objr_stream, other_stream}));
    // direct elements: two with the same /S (veraPDF validates the first only), one without /S, one with kids of its own
    loop.replaceKey("/K", array_of({P("<< /S /Span /K 11 >>"), P("<< /S /Span /K [12 << /S /Em /K 13 >>] >>"), P("<< /K 14 >>"),
                                    P("<< /S /Span /Type /StructElem >>")}));
    // items that are no kids: a name, a string, a real, null, a dictionary of another type, an array
    unknown.replaceKey("/K", array_of({P("/Name"), P("(string)"), P("1.5"), P("null"), P("<< /Type /Foo /S /P >>"), P("[ 1 2 ]"),
                                       P("6"), P("true")}));
    sect1.replaceKey("/K", array_of({shared, para1, P("21"), para2, chain, loop}));
    // /P: the node the element hangs from, another node, a direct dictionary, an object that is no dictionary
    doc.replaceKey("/P", root);
    sect1.replaceKey("/P", doc);
    sect2.replaceKey("/P", root);        // hangs from doc
    shared.replaceKey("/P", sect2);      // first reached through sect1
    para1.replaceKey("/P", sect1);
    para2.replaceKey("/P", P("<< /S /Sect >>"));
    chain.replaceKey("/P", q.makeIndirectObject(P("[ 1 2 ]")));
    loop.replaceKey("/P", sect1);
    in_other2.replaceKey("/P", in_other);
    cyclic.replaceKey("/P", inside_cycle);   // hangs from sect2
    inside_cycle.replaceKey("/P", cyclic);
    cyclic.replaceKey("/K", array_of({inside_cycle, P("30")}));
    inside_cycle.replaceKey("/K", array_of({cyclic, shared, inside_cycle, P("31")}));   // its parent, a shared one, itself
    in_other.replaceKey("/Pg", page2);
    in_other.replaceKey("/K", array_of({P("40"), in_other2, P("<< /Type /MCR /MCID 41 >>"), objr_plain}));
    sect2.replaceKey("/K", array_of({shared, unknown, no_s, string_s, in_pdf2_a, in_pdf2_b, in_pdf2_title, in_pdf2_h7, in_math,
                                     in_other, in_other_p, direct_ns1, direct_ns2, direct_ns3, bad_ns, cyclic, shared, sect1}));
    doc.replaceKey("/K", array_of({sect1, sect2}));
    // the root: an element, content items without a page, the element again, a direct element
    root.replaceKey("/K", array_of({doc, P("50"), P("<< /Type /MCR /MCID 51 >>"), objr_plain, doc, P("<< /S /Part >>"), P("/NoKid")}));
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", P("<< /Marked true >>"));
}

}  // namespace

EQX_TEST(structure_storage_views_match_plain_records) {
    QPDF q;
    q.emptyPDF();
    build_mixed_tree(q);
    auto doc = write_and_open(q, "storage");
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    EXPECT(st.present());
    PlainTree plain{st, {}, {}, {}, {}, {}};
    plain.walk(st.root(), -1, ObjRef{}, 0);
    plain.simulate();
    EXPECT(plain.elems.size() > 30);
    expect_same_tree(st, plain);

    // what the plain tree itself must show, so that the comparison compares something
    std::size_t revisits = 0, mcid = 0, mcr = 0, objr = 0, direct = 0, unchecked = 0, wide = 0;
    for (const PlainElem& e : plain.elems) {
        direct += !e.ref.valid();
        unchecked += !e.verapdf_checked;
        for (const PlainKid& k : e.kids) {
            revisits += k.revisit;
            mcid += k.kind == StructKid::Kind::MCID;
            mcr += k.kind == StructKid::Kind::MCR;
            objr += k.kind == StructKid::Kind::OBJR;
            wide += k.kind == StructKid::Kind::MCID && (k.mcid >= (std::int64_t(1) << 60) || k.mcid < -(std::int64_t(1) << 60));
        }
    }
    EXPECT_EQ(revisits, std::size_t(6));   // shared (sect2 twice), cyclic and itself in inside_cycle, shared there, sect1
    EXPECT(mcid >= 20);
    EXPECT_EQ(mcr, std::size_t(5));
    EXPECT_EQ(objr, std::size_t(4));
    EXPECT_EQ(wide, std::size_t(5));
    EXPECT_EQ(direct, std::size_t(6));
    EXPECT(unchecked >= 1);
    EXPECT_EQ(plain.root_kids.size(), std::size_t(6));
    std::size_t by_p = 0, other_p = 0;
    for (const StructElem& e : st.elems()) {
        by_p += e.p_is_parent;
        other_p += e.has_key(StructElem::kP) && !e.p_is_parent;
    }
    std::size_t stray = 0;
    for (const StructElem& e : st.elems()) stray += !e.k_is_kids;
    EXPECT_EQ(stray, std::size_t(2));     // chain (a stream that is no kid), unknown (a name, a string, ...)
    EXPECT_EQ(by_p, std::size_t(6));      // doc, sect1, para1, loop, in_other2, inside_cycle
    EXPECT_EQ(other_p, std::size_t(5));   // sect2, shared, para2, chain, cyclic
}

EQX_TEST(structure_storage_type_facts_are_shared_per_type) {
    QPDF q;
    q.emptyPDF();
    build_mixed_tree(q);
    auto doc = write_and_open(q, "storage_types");
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    // Elements of one type name and namespace refer to one record; any other pair to another.
    struct Seen {
        const std::optional<std::string>* S;
        const SType* type;
        const std::optional<std::string>* std17;
        const std::optional<SType>* std20;
    };
    std::unordered_map<std::string, Seen> by_type;
    std::size_t shared = 0, direct_ns = 0;
    for (const StructElem& e : st.elems()) {
        if (e.type.ns.isDictionary() && !e.type.ns.isIndirect()) {
            ++direct_ns;   // a direct namespace dictionary belongs to its element alone
            for (const StructElem& o : st.elems())
                if (o.index != e.index) EXPECT(&o.type != &e.type);
            continue;
        }
        const std::string id = (e.S ? "N" + *e.S : std::string("-")) + "|" + e.type.ns_ref().str();
        auto [it, added] = by_type.try_emplace(id, Seen{&e.S, &e.type, &e.std17, &e.std20});
        if (added) continue;
        ++shared;
        EXPECT(it->second.S == &e.S && it->second.type == &e.type && it->second.std17 == &e.std17 && it->second.std20 == &e.std20);
    }
    EXPECT(shared >= 8);
    EXPECT_EQ(direct_ns, std::size_t(3));
    // type_id numbers the records: the same number, the same record
    std::unordered_map<std::uint32_t, const SType*> by_id;
    for (const StructElem& e : st.elems()) {
        auto [it, added] = by_id.try_emplace(e.type_id, &e.type);
        EXPECT(it->second == &e.type);
    }
    std::unordered_set<const SType*> numbered;
    for (const auto& [id, type] : by_id) EXPECT(numbered.insert(type).second);
    EXPECT(by_id.size() <= st.type_count());
    // different types never share
    std::unordered_set<const SType*> records;
    for (const auto& [id, seen] : by_type) EXPECT(records.insert(seen.type).second);
    // the values: the role mapping of the plain record
    for (const StructElem& e : st.elems()) {
        EXPECT(e.S == cos::name(e.dict, "S"));
        if (!e.S) {
            EXPECT(e.type.type.empty() && !e.type.ns.isDictionary() && !e.std17 && !e.std20);
            continue;
        }
        EXPECT_EQ(e.type.type, *e.S);
        EXPECT(e.std17 == st.standard_type17(*e.S));
        const auto std20 = st.default_type20(e.type);
        EXPECT(e.std20.has_value() == std20.has_value());
        if (e.std20 && std20) EXPECT(e.std20->type == std20->type && e.std20->ns_uri() == std20->ns_uri());
    }
}

EQX_TEST(structure_storage_deep_and_wide_trees) {
    // A chain deeper than any recursion limit, and a node with more kids than an arena block holds.
    QPDF q;
    q.emptyPDF();
    Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
    Obj top = q.makeIndirectObject(P("<< /S /Document >>"));
    top.replaceKey("/Pg", page);
    Obj cur = top;
    for (int i = 0; i < 3000; ++i) {
        Obj next = q.makeIndirectObject(P(i % 3 ? "<< /S /Div >>" : "<< /S /Sect >>"));
        cur.replaceKey("/K", array_of({P(std::to_string(i)), next}));
        cur = next;
    }
    Obj wide = QPDFObjectHandle::newArray();
    for (int i = 0; i < 40000; ++i) {
        if (i % 7 == 0) wide.appendItem(q.makeIndirectObject(P(i % 2 ? "<< /S /P >>" : "<< /S /Span /K 1 >>")));
        else if (i % 11 == 0) wide.appendItem(top);   // an ancestor again
        else wide.appendItem(P(std::to_string(i)));
    }
    cur.replaceKey("/K", wide);
    root.replaceKey("/K", top);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    auto doc = write_and_open(q, "storage_deep");
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    // the plain traversal recurses once per level: on a thread of its own stack it would be fine, here the depth is
    // modest enough for the default stack
    PlainTree plain{st, {}, {}, {}, {}, {}};
    plain.walk(st.root(), -1, ObjRef{}, 0);
    plain.simulate();
    EXPECT_EQ(plain.elems.size(), std::size_t(3001 + (40000 + 6) / 7));
    expect_same_tree(st, plain);
    const StructElem last = st.elem(3000);
    EXPECT_EQ(last.kids.size(), std::size_t(40000));
    EXPECT_EQ(last.depth, 3000);
    EXPECT(last.elems.size() < last.struct_kids.size());
    EXPECT(last.inherited_page.valid() && last.inherited_page == st.elem(0).page && !last.page.valid());
}

EQX_TEST(structure_storage_empty_and_absent_trees) {
    {
        QPDF q;
        q.emptyPDF();
        q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
        auto doc = write_and_open(q, "storage_none");
        if (!doc) return;
        StructTree st;
        st.build(*doc);
        EXPECT(!st.present());
        EXPECT_EQ(st.elems().size(), std::size_t(0));
        EXPECT(st.elems().empty());
        EXPECT(st.root_kids().empty());
        EXPECT(st.elems().begin() == st.elems().end());
        EXPECT_EQ(st.find(ObjRef{1, 0}), -1);
    }
    {
        QPDF q;
        q.emptyPDF();
        q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
        q.getRoot().replaceKey("/StructTreeRoot", q.makeIndirectObject(P("<< /Type /StructTreeRoot /K [ ] >>")));
        auto doc = write_and_open(q, "storage_empty");
        if (!doc) return;
        StructTree st;
        st.build(*doc);
        EXPECT(st.present());
        EXPECT_EQ(st.elems().size(), std::size_t(0));
        EXPECT_EQ(st.root_kids().size(), std::size_t(0));
        EXPECT(st.verapdf_order().empty());
    }
    {
        // one element without /K: no kids, three empty ranges
        QPDF q;
        q.emptyPDF();
        q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
        Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
        root.replaceKey("/K", q.makeIndirectObject(P("<< /S /Document >>")));
        q.getRoot().replaceKey("/StructTreeRoot", root);
        auto doc = write_and_open(q, "storage_one");
        if (!doc) return;
        StructTree st;
        st.build(*doc);
        EXPECT_EQ(st.elems().size(), std::size_t(1));
        const StructElem e = st.elem(0);
        EXPECT(e.kids.empty() && e.elems.empty() && e.struct_kids.empty());
        EXPECT(e.kids.begin() == e.kids.end());
        EXPECT(e.verapdf_checked && e.parent == -1 && e.depth == 0 && !e.page.valid() && !e.inherited_page.valid());
        EXPECT_EQ(st.root_kids().size(), std::size_t(1));
        EXPECT(st.root_kids()[0].kind == StructKid::Kind::Elem && st.root_kids()[0].elem == 0 && !st.root_kids()[0].revisit);
    }
}

EQX_TEST(structure_storage_ref_index_matches_a_map) {
    // RefIndex against unordered_map: sequential object numbers, scattered ones, generations, keys outside the table.
    std::mt19937_64 rng(20261114);
    for (int round = 0; round < 6; ++round) {
        RefIndex index;
        std::unordered_map<std::uint64_t, int> map;
        std::vector<std::uint64_t> keys;
        const int n = round == 0 ? 0 : round == 1 ? 1 : 20000 * round;
        for (int i = 0; i < n; ++i) {
            ObjRef r;
            switch (round % 3) {
                case 0: r.num = i + 1; break;                                             // dense
                case 1: r.num = static_cast<int>(rng() % 2000000000) + 1; break;          // scattered
                default: r.num = static_cast<int>(rng() % 50000) + 1; r.gen = static_cast<int>(rng() % 3); break;   // generations
            }
            std::uint64_t key = r.key();
            if (i % 997 == 0) key = rng();   // any 64-bit key (a number above 32 bits, a generation, number 0)
            if (i % 1499 == 0) key = (rng() % 3) << 16 >> 16;   // number 0
            keys.push_back(key);
            if (index.find(key) < 0) {
                EXPECT(map.find(key) == map.end());
                index.insert(key, i);
                map.emplace(key, i);
            } else {
                EXPECT(map.find(key) != map.end());
                index.insert(key, i + 1000000);   // keeps the first value, as emplace does
                map.emplace(key, i + 1000000);
            }
        }
        EXPECT_EQ(index.size(), map.size());
        for (std::uint64_t key : keys) EXPECT_EQ(index.find(key), map.at(key));
        for (int i = 0; i < 20000; ++i) {
            const std::uint64_t key = i % 2 ? rng() : (rng() % 4000000) << 16;
            auto it = map.find(key);
            EXPECT_EQ(index.find(key), it == map.end() ? -1 : it->second);
        }
    }
}

EQX_TEST(structure_storage_block_arena_keeps_arrays_apart) {
    BlockArena arena;
    EXPECT(arena.allocate(0) == nullptr);
    std::mt19937 rng(7);
    struct Piece {
        unsigned char* p;
        std::size_t n;
        unsigned char fill;
    };
    std::vector<Piece> pieces;
    for (int i = 0; i < 6000; ++i) {
        std::size_t n = 1 + rng() % 96;
        if (i % 500 == 499) n = 20000 + rng() % 200000;   // larger than a block
        if (i % 97 == 0) n = 8 * (1 + rng() % 2048);
        auto* p = static_cast<unsigned char*>(arena.allocate(n));
        EXPECT(p != nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 8, std::uintptr_t(0));
        const unsigned char fill = static_cast<unsigned char>(i);
        std::memset(p, fill, n);
        pieces.push_back({p, n, fill});
    }
    // nothing was overwritten by a later array
    bool intact = true;
    for (const Piece& piece : pieces)
        for (std::size_t j = 0; j < piece.n; ++j) intact = intact && piece.p[j] == piece.fill;
    EXPECT(intact);
    BlockArena moved = std::move(arena);
    EXPECT(pieces.front().p[0] == pieces.front().fill);
    auto* again = static_cast<unsigned char*>(arena.allocate(16));   // the moved-from arena starts over
    EXPECT(again != nullptr);
    moved.clear();
}
