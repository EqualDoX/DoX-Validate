// ParentTree number-tree lookups (StructTree::parent_tree_entry): the semantics of veraPDF's
// PDNumberTreeNode.getObject on adversarial trees, for indexed /Nums arrays (indirect array or indirect node) and the
// linear scan of a direct array in a direct node. Expected values follow from the construction.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "core/document.h"
#include "core/structure.h"
#include "unit.h"

using namespace eqx;

namespace {

// Writes a one-page tagged PDF whose StructTreeRoot gets /ParentTree `tree` (built by `make` on the QPDF), opens it
// and returns the parent-tree entries of keys 0..max_key as integers (-1 = null, -2 = not an integer).
std::vector<long long> lookups(const std::function<Obj(QPDF&)>& make, int max_key) {
    static int counter = 0;
    QPDF q;
    q.emptyPDF();
    Obj page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructTreeRoot /K [] >>"));
    root.replaceKey("/ParentTree", make(q));
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", QPDFObjectHandle::parse("<< /Marked true >>"));
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_parent_tree_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    {
        QPDFWriter w(q, path.string().c_str());
        w.setStaticID(true);
        w.setObjectStreamMode(qpdf_o_disable);
        w.write();
    }
    std::vector<long long> out;
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path.string(), &status, &err);
    if (!doc) {
        eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
        std::filesystem::remove(path);
        return out;
    }
    StructTree st;
    st.build(*doc);
    for (int pass = 0; pass < 2; ++pass) {   // the second pass reads the per-key cache
        std::vector<long long> got;
        for (int k = 0; k <= max_key; ++k) {
            Obj v = st.parent_tree_entry(k);
            got.push_back(v.isNull() ? -1 : v.isInteger() ? v.getIntValue() : -2);
        }
        if (pass == 0) out = got;
        else EXPECT(got == out);
    }
    std::filesystem::remove(path);
    return out;
}

using V = std::vector<long long>;

}  // namespace

EQX_TEST(parent_tree_flat_nums_last_duplicate_wins) {
    // pairs (0,100) (1,101) (1,111) (2.0,102) (3,103) (4,null) and a trailing key 5 without a value
    const char* nums = "[0 100 1 101 1 111 2.0 102 3 103 4 null 5]";
    for (int variant = 0; variant < 3; ++variant) {
        V got = lookups([&](QPDF& q) {
            Obj arr = QPDFObjectHandle::parse(nums);
            if (variant == 0) arr = q.makeIndirectObject(arr);                              // indirect array
            Obj node = QPDFObjectHandle::newDictionary();
            node.replaceKey("/Nums", arr);
            return variant == 2 ? node : q.makeIndirectObject(node);                       // 2: direct node, direct array
        }, 6);
        EXPECT((got == V{100, 111, -1, 103, -1, -1, -1}));
    }
}

EQX_TEST(parent_tree_kids_limits_null_values_and_shared_arrays) {
    V got = lookups([](QPDF& q) {
        Obj shared = q.makeIndirectObject(QPDFObjectHandle::parse("[40 140 41 141]"));
        Obj k1 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Limits [0 9] /Nums [0 100 9 109] >>"));
        // key 15: null, then 125 (last wins); key 16: only null, so the next kid decides; key 17 is inside the
        // Limits but absent: the next kid decides too
        Obj k2 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Limits [10 19] /Nums [10 110 15 null 15 125 16 null] >>"));
        Obj k3 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Limits [16 30] /Nums [16 136 17 137 30 130] >>"));
        // Limits exclude key 0 although the array has it: never found here
        Obj k4 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Limits [31 50] /Nums [0 999 31 131] >>"));
        Obj k5 = q.makeIndirectObject(QPDFObjectHandle::newDictionary());   // the same indirect array as k6
        k5.replaceKey("/Limits", QPDFObjectHandle::parse("[40 40]"));
        k5.replaceKey("/Nums", shared);
        Obj k6 = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
        k6.replaceKey("/Limits", QPDFObjectHandle::parse("[41 41]"));
        k6.replaceKey("/Nums", shared);
        Obj kids = QPDFObjectHandle::newArray();
        for (Obj k : {k1, k2, k3, k4, k5, k6}) kids.appendItem(k);
        Obj root = QPDFObjectHandle::newDictionary();   // a direct root with kids
        root.replaceKey("/Kids", kids);
        return root;
    }, 42);
    V want(43, -1);
    want[0] = 100;
    want[9] = 109;
    want[10] = 110;
    want[15] = 125;
    want[16] = 136;
    want[17] = 137;
    want[30] = 130;
    want[31] = 131;
    want[40] = 140;
    want[41] = 141;
    EXPECT(got == want);
}

EQX_TEST(parent_tree_kid_loop_ends_its_list) {
    // k1's kids: k2 (key 1) then k1 again (a loop: veraPDF ends that list of kids), then k3 (never reached from k1).
    // The root's next kid k4 still answers key 2; key 3 (only in k3, behind the loop) stays null.
    V got = lookups([](QPDF& q) {
        Obj k1 = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
        Obj k2 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Nums [1 101] >>"));
        Obj k3 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Nums [3 103] >>"));
        Obj k4 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Nums [2 102] >>"));
        Obj k1kids = QPDFObjectHandle::newArray();
        k1kids.appendItem(k2);
        k1kids.appendItem(k1);
        k1kids.appendItem(k3);
        k1.replaceKey("/Kids", k1kids);
        Obj kids = QPDFObjectHandle::newArray();
        kids.appendItem(k1);
        kids.appendItem(k4);
        Obj root = QPDFObjectHandle::newDictionary();
        root.replaceKey("/Kids", kids);
        return q.makeIndirectObject(root);
    }, 3);
    EXPECT((got == V{-1, 101, 102, -1}));
}
