// The inner structure of a number tree or name tree (ISO 32000-1 7.9.6, 7.9.7), decoded as lookups reach it.
//
// The lookups of this validator follow veraPDF's PDNumberTreeNode.getObject / PDNameTreeNode.getObject: a node whose
// /Limits exclude the key is passed over, a node with /Nums (/Names) decides alone, else its /Kids are searched in
// order, depth first, and the first hit wins. Done on the PDF objects, every lookup reads the /Limits of every kid it
// passes again: O(kids) dictionary and array accesses per lookup, O(n^2) for a document whose lookups grow with its
// trees. TreeShape keeps per node the decoded limits and the position of its kids, so a lookup compares keys and
// nothing else until it reaches a leaf.
//
// What is kept is exactly what the object walk reads, read when the first lookup gets there and in the same order (so
// damaged objects are met at the same moment as before, and nothing is read that a walk would not read). The object
// walks differ in what they do with a node met again (leave the list of kids, skip the node, end the branch), and none
// of that is modelled here: as soon as an indirect node turns up a second time anywhere in the decoded part, the shape
// gives up for good (find returns NotPlain) and the caller walks the objects, as it does for a tree with more than
// max_nodes nodes. Until then every lookup has seen distinct nodes only, where all those walks agree with find().
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/cos.h"

namespace eqx {

template <class Key>
class TreeShape {
public:
    enum class Result : std::uint8_t { Found, NotFound, NotPlain };
    // True with the key of a /Limits item when the item has the key type of the tree (an integer; a string).
    using LimitFn = bool (*)(const Obj& item, Key& out);

    struct Node {
        Obj obj;                    // the node as its parent's /Kids (or the caller, for the root) holds it
        bool typed = false;         // usable and stream are known
        bool usable = false;        // a dictionary, or a stream when the tree takes streams as nodes
        bool decoded = false;       // limits and leaf are known
        bool has_limits = false;    // /Limits is an array whose first two items are keys
        bool leaf = false;          // the leaf key (/Nums, /Names) is present and not null: the node decides alone
        bool kids_read = false;     // first_kid and kid_count are known
        Key lo{}, hi{};
        std::uint32_t first_kid = 0, kid_count = 0;   // its /Kids, consecutive in the node list
    };

    // `leaf_key`: "Nums" or "Names". `streams`: a stream counts as a node (else it is passed over like any other
    // non-dictionary, without a look at its dictionary).
    TreeShape(const Obj& root, const char* leaf_key, LimitFn limit, bool streams, std::size_t max_nodes = std::size_t(1) << 20)
        : leaf_key_(leaf_key), limit_(limit), streams_(streams), max_nodes_(max_nodes) {
        if (root.isIndirect()) met_.insert(ref_of(root).key());
        Node n;
        n.obj = root;
        nodes_.push_back(std::move(n));
    }

    bool plain() const { return plain_; }

    // The search described above. `leaf(node_object, found)` looks the key up in the leaf's own entries: true (with
    // `found` set) ends the search with that value, false lets it go on with the next node.
    template <class Leaf>
    Result find(const Key& key, Leaf&& leaf, Obj& found) {
        if (!plain_) return Result::NotPlain;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> open_kids;   // [next, end) per level, innermost last
        // 1 the node has the key, 0 it does not (its kids may have been opened), -1 not a plain tree
        auto open = [&](std::uint32_t i) -> int {
            if (!nodes_[i].typed) {
                const Obj& node = nodes_[i].obj;
                const bool stream = node.isStream();
                nodes_[i].usable = node.isDictionary() || (stream && streams_);
                nodes_[i].typed = true;
            }
            if (!nodes_[i].usable) return 0;
            if (!nodes_[i].decoded) decode(i);
            if (nodes_[i].has_limits && (key < nodes_[i].lo || nodes_[i].hi < key)) return 0;
            if (nodes_[i].leaf) {
                const Obj node = nodes_[i].obj;
                return leaf(node, found) ? 1 : 0;
            }
            if (!nodes_[i].kids_read && !read_kids(i)) return -1;
            if (nodes_[i].kid_count) open_kids.emplace_back(nodes_[i].first_kid, nodes_[i].first_kid + nodes_[i].kid_count);
            return 0;
        };
        int r = open(0);
        while (r == 0 && !open_kids.empty()) {
            auto& top = open_kids.back();
            if (top.first >= top.second) {
                open_kids.pop_back();
                continue;
            }
            const std::uint32_t kid = top.first++;
            r = open(kid);   // may add a level: `top` is not used after it
        }
        if (r < 0) {
            give_up();
            return Result::NotPlain;
        }
        return r > 0 ? Result::Found : Result::NotFound;
    }

private:
    void decode(std::uint32_t i) {
        const Obj node = nodes_[i].obj;
        Key lo{}, hi{};
        bool has_limits = false;
        Obj limits = cos::get(node, "Limits");
        if (limits.isArray() && limits.getArrayNItems() >= 2 && limit_(limits.getArrayItem(0), lo) && limit_(limits.getArrayItem(1), hi))
            has_limits = true;
        const bool leaf = cos::has(node, leaf_key_);
        Node& n = nodes_[i];   // nothing above changed the list; only now the node is marked decoded
        n.has_limits = has_limits;
        n.lo = std::move(lo);
        n.hi = std::move(hi);
        n.leaf = leaf;
        n.decoded = true;
    }

    // Reads the /Kids of node i; false when one of them was met before (or the tree is too large).
    bool read_kids(std::uint32_t i) {
        const Obj node = nodes_[i].obj;
        std::vector<Obj> items;
        if (cos::has(node, "Kids")) items = cos::array_items(cos::get(node, "Kids"));
        if (nodes_.size() + items.size() > max_nodes_) return false;
        for (const Obj& kid : items)
            if (kid.isIndirect() && !met_.insert(ref_of(kid).key()).second) return false;
        const std::size_t first = nodes_.size();
        for (Obj& kid : items) {
            Node n;
            n.obj = std::move(kid);
            nodes_.push_back(std::move(n));
        }
        nodes_[i].first_kid = static_cast<std::uint32_t>(first);
        nodes_[i].kid_count = static_cast<std::uint32_t>(nodes_.size() - first);
        nodes_[i].kids_read = true;
        return true;
    }

    void give_up() {
        plain_ = false;
        std::vector<Node>().swap(nodes_);
        std::unordered_set<std::uint64_t>().swap(met_);
    }

    const char* leaf_key_;
    LimitFn limit_;
    bool streams_;
    std::size_t max_nodes_;
    std::vector<Node> nodes_;
    std::unordered_set<std::uint64_t> met_;   // every indirect node of the decoded part
    bool plain_ = true;
};

}  // namespace eqx
