// GFPDAnnot.getisArtifact asks whether the struct parent of an annotation or any object on its /P chain is an
// Artifact. The module keeps the answer per element of the tree and steps along the tree where /P names the element's
// parent. The rule that reads it (an annotation with the Invisible flag that is tagged and no Artifact) must fail for
// exactly the annotations the plain walk over the dictionaries finds: chains through the real parent, through another
// element, through a dictionary outside the tree, chains that end, and chains that loop.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/document.h"
#include "core/structure.h"
#include "engine/validate.h"
#include "rules/catalog.h"
#include "unit.h"

using namespace eqx;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

std::string write_pdf(std::uint32_t seed) {
    QPDF q;
    q.emptyPDF();
    std::mt19937 rng(seed);
    const auto pick = [&](std::size_t bound) { return static_cast<std::size_t>(rng() % bound); };
    Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] /Annots [ ] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot /K [ ] >>"));
    Obj ns = q.makeIndirectObject(P("<< /Type /Namespace /NS (http://iso.org/pdf2/ssn) >>"));
    const auto make = [&](bool artifact) {
        static const char* const kTypes[] = {"P", "Sect", "Div", "Span", "Link", "NonStruct"};
        Obj d = QPDFObjectHandle::newDictionary();
        d.replaceKey("/S", QPDFObjectHandle::newName(artifact ? "/Artifact" : std::string("/") + kTypes[pick(std::size(kTypes))]));
        if (artifact || pick(5) == 0) d.replaceKey("/NS", ns);
        d.replaceKey("/K", QPDFObjectHandle::newArray());
        return q.makeIndirectObject(d);
    };
    std::vector<Obj> elems;
    std::vector<std::size_t> parent_of;
    for (int i = 0; i < 140; ++i) {
        Obj e = make(i > 0 && pick(14) == 0);
        const std::size_t at = i == 0 ? 0 : pick(elems.size());
        Obj parent = i == 0 ? root : elems[at];
        parent.getKey("/K").appendItem(e);
        const std::size_t kind = pick(20);
        if (i == 0 || kind < 13) {
            e.replaceKey("/P", parent);
        } else if (kind < 15) {
            e.replaceKey("/P", elems[pick(elems.size())]);   // another element
        } else if (kind < 16) {
            // no /P
        } else if (kind < 18) {
            // a dictionary that is no element of the tree, an Artifact or not, leading on
            Obj outside = make(pick(3) == 0);
            outside.replaceKey("/P", pick(4) ? elems[pick(elems.size())] : root);
            e.replaceKey("/P", outside);
        } else if (kind < 19) {
            e.replaceKey("/P", e);                            // itself
        } else {
            e.replaceKey("/P", P("<< /S /Div >>"));           // a direct dictionary that leads nowhere
        }
        elems.push_back(e);
        parent_of.push_back(at);
    }
    // loops over several elements: /P of an element names one further down its own subtree
    for (std::size_t i = 20; i < elems.size(); i += 17) elems[parent_of[parent_of[i]]].replaceKey("/P", elems[i]);
    Obj nums = QPDFObjectHandle::newArray();
    for (std::size_t j = 0; j < elems.size(); j += 2) {
        Obj annot = q.makeIndirectObject(P("<< /Type /Annot /Subtype /Link /Rect [ 10 10 50 50 ] /F 1 /Contents (c) >>"));
        annot.replaceKey("/StructParent", QPDFObjectHandle::newInteger(static_cast<long long>(j)));
        annot.replaceKey("/P", page);
        page.getKey("/Annots").appendItem(annot);
        nums.appendItem(QPDFObjectHandle::newInteger(static_cast<long long>(j)));
        nums.appendItem(elems[j]);
    }
    Obj parent_tree = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
    parent_tree.replaceKey("/Nums", nums);
    root.replaceKey("/ParentTree", parent_tree);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", P("<< /Marked true >>"));
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_artifact_chain_" + std::to_string(::getpid()) + "_" + std::to_string(seed) + ".pdf");
    QPDFWriter w(q, path.string().c_str());
    w.setStaticID(true);
    w.setObjectStreamMode(seed % 2 ? qpdf_o_generate : qpdf_o_disable);
    w.write();
    return path.string();
}

// The walk over the dictionaries.
bool plain_is_artifact(const StructTree& st, const Obj& parent) {
    std::unordered_set<std::uint64_t> seen;
    Obj e = parent;
    while (!e.isNull()) {
        if (e.isIndirect() && !seen.insert(ref_of(e).key()).second) break;
        if (e.isDictionary())
            if (auto t = st.stype_of(e))
                if (auto def = st.default_type20(*t); def && def->type == "Artifact") return true;
        e = cos::get(e, "P");
    }
    return false;
}

}  // namespace

EQX_TEST(annotations_artifact_chain_matches_the_walk) {
    std::size_t annotations = 0, artifacts = 0;
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        const std::string path = write_pdf(seed);
        std::set<std::string> expected;
        {
            OpenStatus status;
            std::string err;
            auto doc = Document::open(path, &status, &err);
            EXPECT(doc != nullptr);
            if (!doc) return;
            StructTree st;
            st.build(*doc);
            for (const Obj& annot : cos::array_items(cos::get(doc->pages()[0].dict, "Annots"))) {
                const auto key = cos::integer(annot, "StructParent");
                EXPECT(key.has_value());
                if (!key) continue;
                const Obj parent = st.parent_tree_entry(*key);
                EXPECT(parent.isDictionary());
                ++annotations;
                if (plain_is_artifact(st, parent)) ++artifacts;
                else expected.insert(ref_of(annot).str());
            }
        }
        Result r = validate_file(path, Options{});
        std::filesystem::remove(path);
        EXPECT(r.processed);
        std::set<std::string> reported;
        for (const FindingRecord* f : r.findings.sorted())
            if (std::string_view(Catalog::get().rule(f->rule).id) == "EQX-UA2-8.9.2.2-1") {
                EXPECT_EQ(f->occurrences, std::uint64_t(f->locations.size()));
                for (const Location& l : f->locations) reported.insert(l.obj.str());
            }
        EXPECT(reported == expected);
    }
    EXPECT_EQ(annotations, std::size_t(8 * 70));
    EXPECT(artifacts > 60 && artifacts < annotations - 60);
}
