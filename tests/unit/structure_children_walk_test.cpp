// se::significance_children / children_standard_types walk the records of the tree where these say what the
// dictionaries say, and the dictionaries otherwise. Both must give what the walk over the dictionaries gives, written
// out here once more: the children in order with their standard types, pass-through elements (Part, Div, NonStruct)
// replaced by their own children, a pass-through element met again on its own path reported through `ok`.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/document.h"
#include "core/structure.h"
#include "rules/structure/se_common.h"
#include "unit.h"

using namespace eqx;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

std::unique_ptr<Document> write_and_open(QPDF& q, const char* tag, bool object_streams) {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    {
        QPDFWriter w(q, path.string().c_str());
        w.setStaticID(true);
        w.setObjectStreamMode(object_streams ? qpdf_o_generate : qpdf_o_disable);
        w.write();
    }
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path.string(), &status, &err);
    if (!doc) eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
    std::filesystem::remove(path);
    return doc;
}

// The walk over the dictionaries.
std::vector<se::TypedChild> plain_children(const StructTree& st, const Obj& node, Flavor f, bool* ok) {
    std::vector<se::TypedChild> out;
    struct Level {
        const void* id;
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    std::vector<Level> stack;
    std::unordered_set<const void*> path;
    path.insert(se::identity(node));
    stack.push_back({se::identity(node), se::struct_children(node), 0});
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            path.erase(top.id);
            stack.pop_back();
            continue;
        }
        Obj child = top.kids[top.next++];
        std::string t = se::standard_type(st, child, f);
        if (!se::is_pass_through(t)) {
            out.push_back({child, std::move(t)});
            continue;
        }
        const void* id = se::identity(child);
        if (!path.insert(id).second) {
            *ok = false;
            continue;
        }
        stack.push_back({id, se::struct_children(child), 0});
    }
    return out;
}

struct Counts {
    std::size_t nodes = 0, children = 0, not_ok = 0;
};

void compare_all(const StructTree& st, Counts& counts) {
    std::vector<Obj> nodes{st.root()};
    for (const StructElem& e : st.elems()) nodes.push_back(e.dict);
    for (const Obj& node : nodes)
        for (Flavor f : {Flavor::Pdf17, Flavor::Pdf20}) {
            bool ok = true, plain_ok = true, types_ok = true;
            const auto got = se::significance_children(st, node, f, &ok);
            const auto want = plain_children(st, node, f, &plain_ok);
            const auto types = se::children_standard_types(st, node, f, &types_ok);
            ++counts.nodes;
            counts.children += want.size();
            counts.not_ok += !plain_ok;
            EXPECT_EQ(ok, plain_ok);
            EXPECT_EQ(types_ok, plain_ok);
            EXPECT_EQ(got.size(), want.size());
            EXPECT_EQ(types.size(), want.size());
            if (got.size() != want.size() || types.size() != want.size()) continue;
            for (std::size_t i = 0; i < want.size(); ++i) {
                EXPECT_EQ(got[i].type, want[i].type);
                EXPECT_EQ(types[i], want[i].type);
                EXPECT(se::identity(got[i].dict) == se::identity(want[i].dict));
            }
        }
}

// Random trees: pass-through and other types, role-mapped ones, direct elements, shared elements, and with `cycles`
// pass-through elements that list one of their ancestors.
void build_tree(QPDF& q, std::mt19937& rng, int n, bool cycles) {
    static const char* const kTypes[] = {"Part", "Div", "NonStruct", "Part", "Div", "Sect", "P", "L", "LI", "Lbl", "LBody",
                                         "Table", "TR", "TH", "TD", "THead", "TBody", "Span", "Wrap", "Custom", "Unknown"};
    const auto pick = [&](std::size_t bound) { return static_cast<std::size_t>(rng() % bound); };
    q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot /RoleMap << /Custom /P /Wrap /Div >> /K [ ] >>"));
    std::vector<Obj> elems;
    std::vector<std::size_t> parent_of;
    for (int i = 0; i < n; ++i) {
        Obj d = QPDFObjectHandle::newDictionary();
        d.replaceKey("/S", QPDFObjectHandle::newName(std::string("/") + (i == 0 ? "Document" : kTypes[pick(std::size(kTypes))])));
        d.replaceKey("/K", QPDFObjectHandle::newArray());
        if (pick(4) == 0) d.getKey("/K").appendItem(QPDFObjectHandle::newInteger(i));
        const std::size_t at = i == 0 ? 0 : pick(elems.size());
        Obj parent = i == 0 ? root : elems[at];
        const bool indirect = i == 0 || pick(6) != 0;
        Obj e = indirect ? q.makeIndirectObject(d) : d;
        parent.getKey("/K").appendItem(e);
        if (indirect && i > 2 && pick(12) == 0) elems[pick(elems.size())].getKey("/K").appendItem(e);   // shared
        if (!indirect) continue;   // a direct element stays a leaf of the generator (its dictionary is embedded now)
        elems.push_back(e);
        parent_of.push_back(at);
        if (cycles && i > 3 && pick(10) == 0) {
            // an ancestor again, some levels up
            std::size_t up = elems.size() - 1;
            for (std::size_t hops = 1 + pick(3); hops > 0 && up != 0; --hops) up = parent_of[up];
            e.getKey("/K").appendItem(elems[up]);
        }
    }
    q.getRoot().replaceKey("/StructTreeRoot", root);
}

}  // namespace

EQX_TEST(structure_children_walk_matches_the_dictionaries) {
    Counts counts;
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        QPDF q;
        q.emptyPDF();
        std::mt19937 rng(seed * 7919u);
        build_tree(q, rng, 250, seed > 6);
        auto doc = write_and_open(q, "children_walk", seed % 2 == 0);
        if (!doc) return;
        StructTree st;
        st.build(*doc);
        EXPECT(st.complete());
        compare_all(st, counts);
    }
    EXPECT(counts.nodes > 5000);
    EXPECT(counts.children > 5000);
    EXPECT(counts.not_ok > 50);            // the trees with cycles
    EXPECT(counts.not_ok < counts.nodes / 2);
}

EQX_TEST(structure_children_walk_direct_pass_through_children) {
    // Direct pass-through dictionaries with kids of their own, one of them holding its indirect ancestor: the walk
    // goes over the dictionaries for these.
    QPDF q;
    q.emptyPDF();
    q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot >>"));
    Obj doc = q.makeIndirectObject(P("<< /S /Document >>"));
    Obj sect = q.makeIndirectObject(P("<< /S /Div >>"));
    Obj p1 = q.makeIndirectObject(P("<< /S /P >>")), p2 = q.makeIndirectObject(P("<< /S /P >>"));
    Obj inner = P("<< /S /NonStruct >>");
    Obj inner_kids = QPDFObjectHandle::newArray();
    inner_kids.appendItem(p2);
    inner_kids.appendItem(P("<< /S /Span >>"));
    inner_kids.appendItem(sect);   // its ancestor: a cycle through a direct element
    inner.replaceKey("/K", inner_kids);
    Obj direct_div = P("<< /S /Div /K [ << /S /H1 >> << /S /Part /K [ << /S /LI >> ] >> ] >>");
    Obj sect_kids = QPDFObjectHandle::newArray();
    sect_kids.appendItem(p1);
    sect_kids.appendItem(direct_div);
    sect_kids.appendItem(inner);
    sect.replaceKey("/K", sect_kids);
    doc.replaceKey("/K", sect);
    root.replaceKey("/K", doc);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    auto document = write_and_open(q, "children_walk_direct", false);
    if (!document) return;
    StructTree st;
    st.build(*document);
    Counts counts;
    compare_all(st, counts);
    EXPECT(counts.not_ok >= 2);   // from the Document and from the Div, in both flavours
    bool ok = true;
    const auto kids = se::children_standard_types(st, st.elem(0).dict, Flavor::Pdf17, &ok);
    EXPECT(!ok);
    EXPECT(kids == std::vector<std::string>({"P", "H1", "LI", "P", "Span"}));
}
