// The containment engine against the model it implements, object by object.
//
// The engine works on the tree's indices: standard types per type record, kid summaries bottom-up, the parent standard
// type along StructElem::p_is_parent without reading /P, the rules of a group filtered per parent. The model is the
// plain definition: for every element and flavour, the veraPDF properties from the dictionaries (StructTree's
// kids_standard_types, has_content_items, parent_standard_type) and every rule that selects the element, evaluated by
// test_passes. Random trees with shared and direct elements, role-mapped and namespaced types, and /P entries that
// name the real parent, another element, the root, a dictionary outside the tree, a direct dictionary, or nothing.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/structure.h"
#include "rules/containment/containment.h"
#include "unit.h"

using namespace eqx;
using namespace eqx::containment;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

bool is_hn(const std::string& t) {
    if (t.size() < 2 || t[0] != 'H' || t[1] < '1' || t[1] > '9') return false;
    for (std::size_t i = 2; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

using Failures = std::map<std::pair<std::string, std::string>, int>;   // (rule id, object) -> occurrences

struct Outcome {
    Failures engine, model;
    std::set<std::string> incomplete;
    std::size_t elements = 0, by_p = 0, other_p = 0, shared = 0;
};

// A random tree of `n` elements; element 0 is the Document under the root.
void build_random_tree(QPDF& q, std::mt19937& rng, int n) {
    static const char* const kTypes[] = {
        "Part", "Div", "NonStruct", "Sect", "P", "P", "H", "H1", "H3", "L", "LI", "Lbl", "LBody", "Table", "TR", "TH", "TD",
        "THead", "TBody", "TFoot", "Span", "Link", "Figure", "Caption", "TOC", "TOCI", "Note", "Formula", "Annot", "Form",
        "Reference", "Quote", "Art", "BlockQuote", "Aside", "Document", "Custom", "Wrap", "Unknown", "Private", "Index"};
    const auto pick = [&](std::size_t bound) { return static_cast<std::size_t>(rng() % bound); };
    Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot /RoleMap << /Custom /P /Wrap /Div /Chain /Wrap >> /K [ ] >>"));
    Obj ns_pdf2 = q.makeIndirectObject(P("<< /Type /Namespace /NS (http://iso.org/pdf2/ssn) >>"));
    Obj ns_math = q.makeIndirectObject(P("<< /Type /Namespace /NS (http://www.w3.org/1998/Math/MathML) >>"));
    std::vector<Obj> elems;        // the elements made so far that take kids
    std::vector<char> indirect;    // parallel: an indirect object
    std::vector<Obj> targets;      // the indirect ones: what a /P may name (a direct object has no reference)
    const auto add_kid = [](Obj parent, const Obj& kid) { parent.getKey("/K").appendItem(kid); };
    for (int i = 0; i < n; ++i) {
        Obj d = QPDFObjectHandle::newDictionary();
        const char* type = i == 0 ? "Document" : kTypes[pick(std::size(kTypes))];
        d.replaceKey("/S", QPDFObjectHandle::newName(std::string("/") + type));
        if (pick(4) != 0) d.replaceKey("/Type", QPDFObjectHandle::newName("/StructElem"));
        if (pick(12) == 0) d.replaceKey("/NS", ns_pdf2);
        if (pick(30) == 0) {
            d.replaceKey("/S", QPDFObjectHandle::newName("/math"));
            d.replaceKey("/NS", ns_math);
        }
        d.replaceKey("/K", QPDFObjectHandle::newArray());
        if (pick(3) == 0) d.getKey("/K").appendItem(QPDFObjectHandle::newInteger(i));   // a content item
        // the parent in /K: the root for the first, else an element made before (so /K has no cycle)
        const std::size_t at = i == 0 ? 0 : pick(elems.size());
        Obj parent = i == 0 ? root : elems[at];
        const bool parent_indirect = i == 0 || indirect[at];
        // /P: mostly the parent, else something else made before (so /P chains end)
        const std::size_t kind = pick(20);
        if (i == 0 || (kind < 13 && parent_indirect)) {
            d.replaceKey("/P", parent);
        } else if (kind < 15) {
            // no /P
        } else if (kind < 16) {
            d.replaceKey("/P", root);
        } else if (kind < 18) {
            d.replaceKey("/P", targets[pick(targets.size())]);
        } else if (kind < 19) {
            // an indirect dictionary that is no element of the tree, pass-through or not, with a /P of its own
            Obj outside = q.makeIndirectObject(P(pick(2) ? "<< /S /NonStruct >>" : "<< /S /Sect >>"));
            outside.replaceKey("/P", pick(2) ? targets[pick(targets.size())] : root);
            d.replaceKey("/P", outside);
        } else {
            Obj direct = P(pick(2) ? "<< /S /Div >>" : "<< /S /Table >>");
            direct.replaceKey("/P", targets[pick(targets.size())]);
            d.replaceKey("/P", direct);
        }
        const bool as_indirect = i == 0 || pick(8) != 0;
        Obj e = as_indirect ? q.makeIndirectObject(d) : d;
        add_kid(parent, e);
        // an indirect element also under a second parent made before it (never one of its descendants)
        if (as_indirect && i > 2 && pick(15) == 0) add_kid(elems[pick(elems.size())], e);
        if (as_indirect) targets.push_back(e);
        if (as_indirect || pick(2)) {   // a direct element sometimes gets no kids of its own
            elems.push_back(e);
            indirect.push_back(as_indirect);
        }
    }
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", P("<< /Marked true >>"));
}

Outcome run(std::uint32_t seed, int n) {
    Outcome out;
    QPDF q;
    q.emptyPDF();
    std::mt19937 rng(seed);
    build_random_tree(q, rng, n);
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_containment_model_" + std::to_string(::getpid()) + "_" + std::to_string(seed) + ".pdf");
    {
        QPDFWriter w(q, path.string().c_str());
        w.setStaticID(true);
        w.setObjectStreamMode(seed % 2 ? qpdf_o_generate : qpdf_o_disable);
        w.write();
    }
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path.string(), &status, &err);
    std::filesystem::remove(path);
    if (!doc) {
        eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
        return out;
    }
    StructTree st;
    st.build(*doc);
    std::size_t n_rules = 0;
    const GenRule* rules = generated_rules(&n_rules);
    Sink sink;
    sink.violation = [&](std::size_t gen, ObjRef obj, std::string) {
        if (obj == st.root_ref()) return;   // the rules of the StructTreeRoot are not modelled here
        ++out.engine[{rules[gen].id, obj.valid() ? obj.str() : "direct"}];
    };
    sink.incomplete = [&](std::size_t gen, std::string) { out.incomplete.insert(rules[gen].id); };
    evaluate(st, sink, false);

    // The rules by the object they select.
    std::map<std::pair<int, std::string>, std::vector<const GenRule*>> by_object;
    for (std::size_t i = 0; i < n_rules; ++i)
        by_object[{rules[i].flavour == Flavor::Pdf17 ? 0 : 1, rules[i].object}].push_back(&rules[i]);
    for (const StructElem& e : st.elems()) {
        ++out.elements;
        out.by_p += e.p_is_parent;
        out.other_p += e.has_key(StructElem::kP) && !e.p_is_parent;
        out.shared += e.elems.size() != e.struct_kids.size();
        for (int f = 0; f < 2; ++f) {
            const Flavor flavour = f == 0 ? Flavor::Pdf17 : Flavor::Pdf20;
            ObjectView v;
            const std::string own = e.standard(flavour);
            if (!own.empty()) v.standard_type = own;
            v.kids = st.kids_standard_types(e.index, flavour);
            v.has_content_items = st.has_content_items(e.index, flavour);
            v.parent_standard_type = st.parent_standard_type(e.dict, flavour);
            std::vector<const GenRule*> selected = by_object[{f, "*"}];
            if (!own.empty()) {
                const auto& typed = by_object[{f, own}];
                selected.insert(selected.end(), typed.begin(), typed.end());
                if (is_hn(own)) {
                    const auto& hn = by_object[{f, "Hn"}];
                    selected.insert(selected.end(), hn.begin(), hn.end());
                }
            }
            for (const GenRule* r : selected)
                if (!test_passes(*r, v)) ++out.model[{r->id, e.ref.valid() ? e.ref.str() : "direct"}];
        }
    }
    EXPECT(!st.truncated());
    return out;
}

}  // namespace

EQX_TEST(containment_engine_matches_the_model_on_random_trees) {
    std::size_t elements = 0, by_p = 0, other_p = 0, shared = 0, failures = 0;
    for (std::uint32_t seed = 1; seed <= 10; ++seed) {
        const Outcome o = run(seed, seed <= 8 ? 160 : 900);
        // a rule the engine cannot decide is not compared (none is expected on these trees)
        EXPECT(o.incomplete.empty());
        Failures engine = o.engine, model = o.model;
        for (const std::string& id : o.incomplete) {
            std::erase_if(engine, [&](const auto& item) { return item.first.first == id; });
            std::erase_if(model, [&](const auto& item) { return item.first.first == id; });
        }
        EXPECT_EQ(engine.size(), model.size());
        EXPECT(engine == model);
        if (engine != model) {
            int shown = 0;
            for (const auto& [key, count] : engine)
                if ((!model.count(key) || model.at(key) != count) && shown++ < 5)
                    eqx_test::fail(__FILE__, __LINE__, "seed " + std::to_string(seed) + ": engine only or other count: " + key.first + " on " + key.second);
            for (const auto& [key, count] : model)
                if (!engine.count(key) && shown++ < 10)
                    eqx_test::fail(__FILE__, __LINE__, "seed " + std::to_string(seed) + ": model only: " + key.first + " on " + key.second);
        }
        elements += o.elements;
        by_p += o.by_p;
        other_p += o.other_p;
        shared += o.shared;
        for (const auto& item : o.engine) failures += static_cast<std::size_t>(item.second);
    }
    // the trees exercise what the test is about
    EXPECT(elements > 2500);
    EXPECT(by_p > elements / 2);
    EXPECT(other_p > elements / 12);
    EXPECT(shared > 30);
    EXPECT(failures > 2000);
}

EQX_TEST(containment_engine_saturated_rules_get_no_detail) {
    // A receiver that only counts further violations of a rule (Sink::saturated) gets the same violations in the same
    // order, with "PAC:" in place of a detail that starts with it and nothing in place of any other.
    QPDF q;
    q.emptyPDF();
    q.addPage(q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>")), true);
    Obj root = q.makeIndirectObject(P("<< /Type /StructTreeRoot /RoleMap << /PAC:Odd /P >> >>"));
    Obj doc = q.makeIndirectObject(P("<< /S /Document >>"));
    Obj span = q.makeIndirectObject(P("<< /S /Span >>"));
    doc.replaceKey("/P", root);
    span.replaceKey("/P", doc);
    Obj kids = QPDFObjectHandle::newArray();
    for (int i = 0; i < 60; ++i) {
        Obj p = q.makeIndirectObject(P(i % 3 == 0 ? "<< /S /PAC:Odd >>" : "<< /S /P >>"));
        p.replaceKey("/P", span);
        kids.appendItem(p);
    }
    span.replaceKey("/K", kids);
    doc.replaceKey("/K", span);
    root.replaceKey("/K", doc);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    auto path = std::filesystem::temp_directory_path() / ("eqx_containment_saturated_" + std::to_string(::getpid()) + ".pdf");
    {
        QPDFWriter w(q, path.string().c_str());
        w.setStaticID(true);
        w.write();
    }
    OpenStatus status;
    std::string err;
    auto document = Document::open(path.string(), &status, &err);
    std::filesystem::remove(path);
    EXPECT(document != nullptr);
    if (!document) return;
    StructTree st;
    st.build(*document);
    struct Seen {
        std::size_t gen;
        std::string obj, detail;
    };
    const auto run_with = [&](std::size_t limit) {
        std::vector<Seen> seen;
        std::map<std::size_t, std::size_t> count;
        Sink sink;
        sink.violation = [&](std::size_t gen, ObjRef obj, std::string detail) {
            seen.push_back({gen, obj.str(), std::move(detail)});
            ++count[gen];
        };
        sink.incomplete = [](std::size_t, std::string) {};
        if (limit) sink.saturated = [&count, limit](std::size_t gen) { return count[gen] >= limit; };
        evaluate(st, sink, false);
        return seen;
    };
    const std::vector<Seen> full = run_with(0), capped = run_with(5);
    EXPECT(full.size() >= 60);
    EXPECT_EQ(capped.size(), full.size());
    if (capped.size() != full.size()) return;
    std::map<std::size_t, std::size_t> count;
    std::size_t dropped = 0, pac = 0;
    for (std::size_t i = 0; i < full.size(); ++i) {
        EXPECT_EQ(capped[i].gen, full[i].gen);
        EXPECT_EQ(capped[i].obj, full[i].obj);
        const bool pac_detail = full[i].detail.rfind("PAC:", 0) == 0;
        if (count[full[i].gen]++ < 5) {
            EXPECT_EQ(capped[i].detail, full[i].detail);
        } else {
            EXPECT_EQ(capped[i].detail, std::string(pac_detail ? "PAC:" : ""));
            ++dropped;
            pac += pac_detail;
        }
    }
    EXPECT(dropped > 50);
    EXPECT(pac > 10 && pac < dropped);
}
