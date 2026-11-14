// TreeShape (core/tree_shape.h): the decoded /Kids and /Limits of number and name trees must give what the walks over
// the PDF objects give. The walks are written out here once more, straight from veraPDF's PDNumberTreeNode.getObject /
// PDNameTreeNode.getObject as this validator reads them (three variants that differ in what a node met again does), and
// compared with TreeShape::find and with the lookups built on it (StructTree::parent_tree_entry, annot::NumberTree,
// annot::NameTree) on generated trees: several levels, missing, wrong, malformed and reversed /Limits, unsorted and
// duplicate keys, null values, direct and shared nodes, loops, nodes that are no dictionaries, streams as nodes, nodes
// with /Nums and /Kids, /Nums and /Kids that are no arrays.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/document.h"
#include "core/structure.h"
#include "core/tree_shape.h"
#include "rules/annotations/annot_helpers.h"
#include "unit.h"
#include "util/text.h"

using namespace eqx;

namespace {

Obj P(const std::string& s) { return QPDFObjectHandle::parse(s); }

struct Rng {
    std::uint32_t s;
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    bool chance(int percent) { return below(100) < percent; }
};

// A generated tree over the keys 0..kMaxKey. `names`: a name tree, key k is the string "kNN".
constexpr int kMaxKey = 39;

std::string name_of(int k) {
    char b[8];
    std::snprintf(b, sizeof b, "k%02d", k);
    return b;
}

struct Gen {
    QPDF& q;
    Rng rng;
    bool names;
    bool streams;                 // stream nodes may occur
    bool irregular;               // shared nodes and loops may occur
    std::vector<Obj> made;        // indirect nodes so far
    int value = 1000;

    std::string key(int k) const { return names ? "(" + name_of(k) + ")" : std::to_string(k); }

    Obj limits(int lo, int hi) {
        const int r = rng.below(100);
        if (r < 60) return P("[" + key(lo) + " " + key(hi) + "]");
        if (r < 70) return Obj();                                             // none
        if (r < 78) return P("[" + key(std::min(kMaxKey, lo + 2)) + " " + key(std::max(0, hi - 2)) + "]");   // too narrow
        if (r < 84) return P("[" + key(hi) + " " + key(lo) + "]");            // reversed
        if (r < 88) return P("[" + key(lo) + "]");                             // one item
        if (r < 92) return P(names ? "[1 2]" : "[(a) (b)]");                   // the other tree's key type
        if (r < 95) return P("[" + key(lo) + " /N]");
        if (r < 97) return P("[" + key(lo) + " " + key(hi) + " " + key(lo) + "]");   // three items: the first two count
        return P("/NotAnArray");
    }

    Obj leaf_entries(int lo, int hi) {
        const int r = rng.below(100);
        if (r < 4) return P("null");          // counts as absent
        if (r < 8) return P("7");             // present, no array
        std::string a = "[";
        const int n = rng.below(7);
        for (int i = 0; i < n; ++i) {
            const int k = lo + rng.below(std::max(1, hi - lo + 1));
            const int kind = rng.below(100);
            if (kind < 5) a += names ? "5 " : "(x) ";              // a key of the wrong type
            else if (kind < 8 && !names) a += std::to_string(k) + ".0 ";   // a real is no key
            else if (kind < 11 && names) a += "/" + name_of(k) + " ";      // a name key (annot::NameTree takes it)
            else a += key(k) + " ";
            a += rng.chance(8) ? "null " : std::to_string(value++) + " ";
        }
        if (rng.chance(10)) a += key(lo) + " ";   // a trailing key without a value
        return P(a + "]");
    }

    Obj wrap(Obj dict) {
        // indirect dictionary, direct dictionary, or (with `streams`) a stream with that dictionary
        const int r = rng.below(100);
        if (streams && r < 8) {
            Obj s = q.newStream();
            for (const auto& k : dict.getKeys()) s.getDict().replaceKey(k, dict.getKey(k));
            made.push_back(s);
            return s;
        }
        if (r < 85) {
            Obj o = q.makeIndirectObject(dict);
            made.push_back(o);
            return o;
        }
        return dict;
    }

    Obj node(int depth, int lo, int hi) {
        Obj d = QPDFObjectHandle::newDictionary();
        if (Obj l = limits(lo, hi); l.isInitialized()) d.replaceKey("/Limits", l);
        const char* leaf_key = names ? "/Names" : "/Nums";
        const int r = rng.below(100);
        const bool leaf = depth >= 3 || r < 45;
        if (leaf || r >= 95) d.replaceKey(leaf_key, leaf_entries(lo, hi));
        if (!leaf || r >= 95) {
            if (rng.chance(4)) {
                d.replaceKey("/Kids", P("3"));   // no array
            } else {
                Obj kids = QPDFObjectHandle::newArray();
                const int n = rng.below(5);
                const int span = std::max(1, (hi - lo + 1) / std::max(1, n));
                for (int i = 0; i < n; ++i) {
                    const int klo = std::min(hi, lo + i * span), khi = i + 1 == n ? hi : std::min(hi, klo + span - 1 + rng.below(3));
                    const int kind = rng.below(100);
                    if (kind < 5) kids.appendItem(P(kind < 2 ? "17" : kind < 4 ? "null" : "[1 2]"));   // no node
                    else if (irregular && kind < 12 && !made.empty()) kids.appendItem(made[static_cast<std::size_t>(rng.below(int(made.size())))]);
                    else kids.appendItem(node(depth + 1, klo, khi));
                }
                d.replaceKey("/Kids", kids);
            }
        }
        return wrap(d);
    }

    // The whole tree; with `irregular`, some trees also get a kid that is the root or another node made before
    // (an ancestor: a loop; anything else: a shared node).
    Obj tree() {
        Obj root = node(0, 0, kMaxKey);
        if (!irregular || made.empty()) return root;
        for (int n = rng.below(3); n > 0; --n) {
            Obj holder = made[static_cast<std::size_t>(rng.below(int(made.size())))];
            Obj kids = cos::get(holder, "Kids");
            if (!kids.isArray()) continue;
            Obj target = rng.chance(40) && root.isIndirect() ? root : made[static_cast<std::size_t>(rng.below(int(made.size())))];
            if (rng.chance(50)) kids.appendItem(target);
            else kids.insertItem(0, target);
        }
        return root;
    }
};

long long as_number(const Obj& v) { return !v.isInitialized() ? -3 : v.isNull() ? -1 : v.isInteger() ? v.getIntValue() : -2; }

// --- the walks over the objects ---------------------------------------------------------------------------

template <class Key>
struct Walk {
    const char* leaf_key;
    std::function<bool(const Obj&, Key&)> key_of;   // the key of an entry / limit item of the tree's key type
    bool name_keys = false;                         // annot::NameTree: names are keys too (vera_string)

    bool excluded(const Obj& node, const Key& key) const {
        Obj limits = cos::get(node, "Limits");
        Key lo{}, hi{};
        return limits.isArray() && limits.getArrayNItems() >= 2 && key_of(limits.getArrayItem(0), lo) &&
               key_of(limits.getArrayItem(1), hi) && (key < lo || key > hi);
    }
    // The last value of `key` among the complete pairs (number trees) / all keys (name trees, a missing value is null).
    bool entry(const Obj& node, const Key& key, bool pairs_only, bool with_names, Obj& found) const {
        Obj arr = cos::get(node, leaf_key);
        bool hit = false;
        if (!arr.isArray()) return false;
        const int n = arr.getArrayNItems();
        for (int i = 0; i < n; i += 2) {
            if (pairs_only && i + 1 >= n) break;
            Obj k = arr.getArrayItem(i);
            Key kk{};
            bool is_key = key_of(k, kk);
            if (!is_key && with_names && k.isName()) {
                is_key = true;
                assign_name(kk, k);
            }
            if (!is_key || !(kk == key)) continue;
            found = i + 1 < n ? arr.getArrayItem(i + 1) : Obj::newNull();
            hit = true;
        }
        return hit;
    }
    static void assign_name(std::string& out, const Obj& name) { out = name.getName().substr(1); }
    static void assign_name(std::u32string&, const Obj&) {}
    static void assign_name(std::int64_t&, const Obj&) {}

    // StructTree::parent_tree_entry: dictionaries only; a node met again (in the whole lookup) ends its list of kids.
    Obj struct_tree(const Obj& root, const Key& key) const {
        if (!root.isDictionary()) return Obj::newNull();
        std::unordered_set<std::uint64_t> visited;
        if (root.isIndirect()) visited.insert(ref_of(root).key());
        Obj found = Obj::newNull();
        std::function<bool(const Obj&)> open = [&](const Obj& node) -> bool {
            if (!node.isDictionary()) return false;
            if (excluded(node, key)) return false;
            if (cos::has(node, leaf_key)) {
                Obj v = Obj::newNull();
                entry(node, key, true, false, v);
                found = v;
                return !found.isNull();
            }
            Obj kids = cos::get(node, "Kids");
            if (!kids.isArray()) return false;
            for (const Obj& kid : cos::array_items(kids)) {
                if (kid.isIndirect() && !visited.insert(ref_of(kid).key()).second) break;
                if (open(kid)) return true;
            }
            return false;
        };
        return open(root) ? found : Obj::newNull();
    }
    // annot::NumberTree / NameTree: dictionaries and streams; a node on the current path ends that branch.
    Obj annot_tree(const Obj& root, const Key& key) const {
        if (!(root.isDictionary() || root.isStream())) return Obj::newNull();
        std::unordered_set<std::uint64_t> path;
        Obj found = Obj::newNull();
        std::function<bool(const Obj&)> enter = [&](const Obj& node) -> bool {
            std::uint64_t k = 0;
            if (node.isIndirect()) {
                k = ref_of(node).key();
                if (!path.insert(k).second) return false;
            }
            bool hit = false;
            if ((node.isDictionary() || node.isStream()) && !excluded(node, key)) {
                if (cos::has(node, leaf_key)) {
                    Obj v;
                    hit = entry(node, key, !name_keys, name_keys, v) && !v.isNull();
                    if (hit) found = v;
                } else if (cos::has(node, "Kids")) {
                    for (const Obj& kid : cos::array_items(cos::get(node, "Kids")))
                        if (enter(kid)) {
                            hit = true;
                            break;
                        }
                }
            }
            if (k) path.erase(k);
            return hit;
        };
        return enter(root) ? found : Obj::newNull();
    }
    // The document module's /Dests lookup: dictionaries and streams; a node met again (in the whole lookup) is passed
    // over; a key without a value is found (a null object). Not initialised: not found.
    Obj document_tree(const Obj& root, const Key& key) const {
        std::unordered_set<std::uint64_t> seen;
        Obj found;
        std::function<bool(const Obj&)> open = [&](const Obj& node) -> bool {
            if (!(node.isDictionary() || node.isStream())) return false;
            if (node.isIndirect() && !seen.insert(ref_of(node).key()).second) return false;
            if (excluded(node, key)) return false;
            if (cos::has(node, leaf_key)) return entry(node, key, false, false, found);
            if (cos::has(node, "Kids"))
                for (const Obj& kid : cos::array_items(cos::get(node, "Kids")))
                    if (open(kid)) return true;
            return false;
        };
        return open(root) ? found : Obj();
    }
};

bool integer_key(const Obj& item, std::int64_t& out) {
    if (!item.isInteger()) return false;
    out = item.getIntValue();
    return true;
}
bool utf8_key(const Obj& item, std::string& out) {
    if (!item.isString()) return false;
    out = pdf_text_to_utf8(item.getStringValue());
    return true;
}

template <class Key>
Key test_key(int k);
template <>
std::int64_t test_key<std::int64_t>(int k) { return k; }
template <>
std::string test_key<std::string>(int k) { return name_of(k); }

// TreeShape::find against the three walks, on `count` generated trees. Returns how many trees stayed plain.
template <class Key>
int shape_agrees(bool names, bool irregular, std::uint32_t seed, int count, bool (*key_fn)(const Obj&, Key&)) {
    int plain = 0;
    for (int t = 0; t < count; ++t) {
        QPDF q;
        q.emptyPDF();
        Gen g{q, Rng{seed + static_cast<std::uint32_t>(t) * 7919u}, names, true, irregular, {}, 1000};
        Obj root = g.tree();
        Walk<Key> walk{names ? "Names" : "Nums", key_fn, false};
        for (int with_streams = 0; with_streams < 2; ++with_streams) {
            TreeShape<Key> shape(root, walk.leaf_key, key_fn, with_streams != 0);
            bool gave_up = false;
            Rng order{seed ^ 0x5bd1e995u};
            for (int n = 0; n < 3 * (kMaxKey + 4); ++n) {
                const int k = order.below(kMaxKey + 4) - 2;   // also keys outside every limit
                const Key key = test_key<Key>(k);
                // (1) with the leaf rule of StructTree / annot: a null value is not a hit
                Obj found = Obj::newNull();
                const auto leaf = [&](const Obj& node, Obj& value) {
                    Obj v;
                    if (!walk.entry(node, key, true, false, v) || v.isNull()) return false;
                    value = v;
                    return true;
                };
                const auto r = shape.find(key, leaf, found);
                if (r == TreeShape<Key>::Result::NotPlain) {
                    gave_up = true;
                    continue;
                }
                EXPECT(!gave_up);   // once given up, for good
                if (r == TreeShape<Key>::Result::NotFound) found = Obj::newNull();
                if (with_streams) EXPECT_EQ(as_number(found), as_number(walk.annot_tree(root, key)));
                else EXPECT_EQ(as_number(found), as_number(walk.struct_tree(root, key)));
                // (2) with the leaf rule of the document module: any occurrence is a hit, a missing value is null
                if (with_streams) {
                    Obj dfound;
                    const auto dleaf = [&](const Obj& node, Obj& value) { return walk.entry(node, key, false, false, value); };
                    const auto dr = shape.find(key, dleaf, dfound);
                    EXPECT(dr != TreeShape<Key>::Result::NotPlain);   // the same nodes were decoded just before
                    if (dr == TreeShape<Key>::Result::NotFound) dfound = Obj();
                    EXPECT_EQ(as_number(dfound), as_number(walk.document_tree(root, key)));
                }
            }
            if (!gave_up) ++plain;
        }
    }
    return plain;
}

}  // namespace

EQX_TEST(tree_shape_plain_number_trees_match_the_object_walks) {
    const int plain = shape_agrees<std::int64_t>(false, false, 12345u, 300, &integer_key);
    EXPECT_EQ(plain, 600);   // without shared nodes every tree is plain, with and without streams as nodes
}

EQX_TEST(tree_shape_plain_name_trees_match_the_object_walks) {
    const int plain = shape_agrees<std::string>(true, false, 777u, 300, &utf8_key);
    EXPECT_EQ(plain, 600);
}

EQX_TEST(tree_shape_gives_up_on_shared_nodes_and_loops) {
    // With shared nodes and loops the shape must either agree (as long as no lookup has reached a node met before) or
    // give up; both happen on these trees.
    const int plain_numbers = shape_agrees<std::int64_t>(false, true, 4242u, 300, &integer_key);
    const int plain_names = shape_agrees<std::string>(true, true, 99u, 300, &utf8_key);
    EXPECT(plain_numbers > 0 && plain_numbers < 600);
    EXPECT(plain_names > 0 && plain_names < 600);
}

EQX_TEST(tree_shape_limits_and_size) {
    QPDF q;
    q.emptyPDF();
    // Limits exclude key 5 from the only leaf that has it; the second kid has no /Limits and answers.
    Obj k1 = q.makeIndirectObject(P("<< /Limits [10 20] /Nums [5 105 10 110] >>"));
    Obj k2 = q.makeIndirectObject(P("<< /Nums [7 107 5 205 5 305] >>"));
    Obj root = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
    Obj kids = QPDFObjectHandle::newArray();
    kids.appendItem(k1);
    kids.appendItem(k2);
    root.replaceKey("/Kids", kids);
    Walk<std::int64_t> walk{"Nums", &integer_key, false};
    auto lookup = [&](TreeShape<std::int64_t>& shape, std::int64_t key, TreeShape<std::int64_t>::Result* result) {
        Obj found = Obj::newNull();
        *result = shape.find(key, [&](const Obj& node, Obj& value) {
            Obj v;
            if (!walk.entry(node, key, true, false, v) || v.isNull()) return false;
            value = v;
            return true;
        }, found);
        return as_number(found);
    };
    TreeShape<std::int64_t>::Result r;
    TreeShape<std::int64_t> shape(root, "Nums", &integer_key, false);
    EXPECT_EQ(lookup(shape, 5, &r), 305);   // the last duplicate of the first leaf that takes the key
    EXPECT(r == TreeShape<std::int64_t>::Result::Found);
    EXPECT_EQ(lookup(shape, 10, &r), 110);
    EXPECT_EQ(lookup(shape, 7, &r), 107);
    EXPECT_EQ(lookup(shape, 8, &r), -1);
    EXPECT(r == TreeShape<std::int64_t>::Result::NotFound);
    EXPECT(shape.plain());
    // A tree larger than the limit is left to the object walk.
    TreeShape<std::int64_t> small(root, "Nums", &integer_key, false, 2);
    lookup(small, 5, &r);
    EXPECT(r == TreeShape<std::int64_t>::Result::NotPlain);
    EXPECT(!small.plain());
    // The root among its own kids: a loop.
    kids.appendItem(root);
    TreeShape<std::int64_t> looped(root, "Nums", &integer_key, false);
    lookup(looped, 5, &r);
    EXPECT(r == TreeShape<std::int64_t>::Result::NotPlain);
}

EQX_TEST(tree_shape_annot_trees_match_the_object_walks) {
    // annot::NumberTree and annot::NameTree (plain trees through the shape, the others through their own walk).
    for (int t = 0; t < 300; ++t) {
        for (int names = 0; names < 2; ++names) {
            QPDF q;
            q.emptyPDF();
            Gen g{q, Rng{31337u + static_cast<std::uint32_t>(t) * 104729u + static_cast<std::uint32_t>(names)}, names != 0, true, t % 3 == 0, {}, 1000};
            Obj root = g.tree();
            if (names) {
                Walk<std::string> walk{"Names", &utf8_key, true};
                annot::NameTree tree(root);
                for (int pass = 0; pass < 2; ++pass)
                    for (int k = -2; k <= kMaxKey + 2; ++k)
                        EXPECT_EQ(as_number(tree.get(name_of(k))), as_number(walk.annot_tree(root, name_of(k))));
            } else {
                Walk<std::int64_t> walk{"Nums", &integer_key, false};
                annot::NumberTree tree(root);
                for (int pass = 0; pass < 2; ++pass)
                    for (int k = -2; k <= kMaxKey + 2; ++k)
                        EXPECT_EQ(as_number(tree.get(k)), as_number(walk.annot_tree(root, k)));
            }
        }
    }
}

EQX_TEST(tree_shape_parent_tree_matches_the_object_walk) {
    // StructTree::parent_tree_entry on generated ParentTrees, read back from a written file.
    for (int t = 0; t < 40; ++t) {
        QPDF q;
        q.emptyPDF();
        Obj page = q.makeIndirectObject(P("<< /Type /Page /MediaBox [0 0 612 792] >>"));
        q.addPage(page, true);
        Obj sroot = q.makeIndirectObject(P("<< /Type /StructTreeRoot /K [] >>"));
        Gen g{q, Rng{2024u + static_cast<std::uint32_t>(t) * 15485863u}, false, false, t % 2 == 0, {}, 1000};
        Obj tree = g.tree();
        if (!tree.isDictionary()) continue;
        sroot.replaceKey("/ParentTree", tree);
        q.getRoot().replaceKey("/StructTreeRoot", sroot);
        auto path = std::filesystem::temp_directory_path() /
                    ("eqx_tree_shape_" + std::to_string(::getpid()) + "_" + std::to_string(t) + ".pdf");
        {
            QPDFWriter w(q, path.string().c_str());
            w.setStaticID(true);
            w.setObjectStreamMode(qpdf_o_disable);
            w.write();
        }
        OpenStatus status;
        std::string err;
        auto doc = Document::open(path.string(), &status, &err);
        if (!doc) {
            eqx_test::fail(__FILE__, __LINE__, "cannot open synthetic document: " + err);
            std::filesystem::remove(path);
            continue;
        }
        StructTree st;
        st.build(*doc);
        Walk<std::int64_t> walk{"Nums", &integer_key, false};
        for (int pass = 0; pass < 2; ++pass)
            for (int k = -2; k <= kMaxKey + 2; ++k)
                EXPECT_EQ(as_number(st.parent_tree_entry(k)), as_number(walk.struct_tree(st.parent_tree(), k)));
        std::filesystem::remove(path);
    }
}
