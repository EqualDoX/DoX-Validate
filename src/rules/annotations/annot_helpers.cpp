#include "rules/annotations/annot_helpers.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "util/text.h"
#include "util/unread.h"

namespace eqx::annot {

namespace {

constexpr int kMaxDepth = 256;   // tree depth guard (veraPDF recurses without a limit)

// Java's (long) cast of a double: NaN -> 0, saturating at the long range, truncation toward zero.
std::int64_t java_long(double d) {
    if (std::isnan(d)) return 0;
    if (d >= 9223372036854775807.0) return std::numeric_limits<std::int64_t>::max();
    if (d <= -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(d);
}

std::uint64_t node_key(const Obj& h) { return ref_of(h).key(); }

}  // namespace

std::optional<std::int64_t> vera_integer(const Obj& h) {
    if (h.isInteger()) return h.getIntValue();
    if (h.isReal()) return java_long(h.getNumericValue());
    if (h.isString()) return static_cast<std::int64_t>(h.getStringValue().size());
    return std::nullopt;
}

std::optional<double> vera_real(const Obj& h) {
    if (h.isInteger()) return static_cast<double>(h.getIntValue());
    if (h.isReal()) return h.getNumericValue();
    if (h.isString()) return static_cast<double>(h.getStringValue().size());
    return std::nullopt;
}

std::optional<std::string> vera_string_key(const Obj& dict, std::string_view key) {
    Obj v = cos::get(dict, key);
    if (!v.isString()) return std::nullopt;
    return pdf_text_to_utf8(v.getStringValue());
}

std::optional<std::string> vera_string(const Obj& h) {
    if (h.isString()) return pdf_text_to_utf8(h.getStringValue());
    if (h.isName()) return h.getName().substr(1);
    return std::nullopt;
}

std::optional<std::vector<double>> vera_real_array(const Obj& h) {
    if (!h.isArray()) return std::nullopt;
    std::vector<double> out;
    for (const Obj& item : cos::array_items(h)) {
        auto v = vera_real(item);
        if (!v) return std::nullopt;
        out.push_back(*v);
    }
    return out;
}

namespace {

// PDPage.getInheritedCOSBBox: first page-tree node (following /Parent) that has the key decides; a
// non-array value yields null.
Obj inherited_box(const Obj& page, std::string_view key) {
    Obj cur = page;
    std::unordered_set<std::uint64_t> seen;
    while (cur.isDictionary() || cur.isStream()) {
        if (cur.isIndirect() && !seen.insert(node_key(cur)).second) break;
        Obj v = cos::get(cur, key);
        if (!v.isNull()) return v.isArray() ? v : Obj::newNull();
        cur = cos::get(cur, "Parent");
    }
    return Obj::newNull();
}

// PDPage.getDoubleArrayForBox (an array longer than 4 makes veraPDF throw; the first four entries are used).
Box box_values(const Obj& arr) {
    Box b{0, 0, 0, 0};
    auto items = cos::array_items(arr);
    for (std::size_t i = 0; i < items.size() && i < 4; ++i) {
        if (items[i].isInteger() || items[i].isReal()) b[i] = items[i].getNumericValue();
    }
    return b;
}

}  // namespace

std::optional<Box> vera_crop_box(const Obj& page) {
    Obj media = inherited_box(page, "MediaBox");
    Obj crop = inherited_box(page, "CropBox");
    std::optional<Box> mb;
    if (media.isArray()) mb = box_values(media);
    if (crop.isArray()) {
        Box cb = box_values(crop);
        if (!mb) return cb;
        return Box{std::max(cb[0], (*mb)[0]), std::max(cb[1], (*mb)[1]), std::min(cb[2], (*mb)[2]),
                   std::min(cb[3], (*mb)[3])};
    }
    return mb;
}

std::optional<bool> outside_crop_box(const std::optional<Box>& crop, const std::optional<std::vector<double>>& rect) {
    if (!rect || rect->size() < 4 || !crop) return std::nullopt;
    const Box& c = *crop;
    const auto& r = *rect;
    return c[1] >= r[3] || c[0] >= r[2] || c[3] <= r[1] || c[2] <= r[0];
}

// --- number tree --------------------------------------------------------------------------------------

const NumberTree::Nums& NumberTree::nums_of(const Obj& node, Nums& scratch) {
    auto parse = [&](Nums& m) {
        Obj nums = cos::get(node, "Nums");
        if (!nums.isArray()) return;
        auto items = cos::array_items(nums);
        for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
            if (items[i].isInteger()) m[items[i].getIntValue()] = items[i + 1];   // HashMap.put: last wins
        }
    };
    if (node.isIndirect()) {
        auto it = cache_.find(node_key(node));
        if (it != cache_.end()) return it->second;
        Nums& m = cache_[node_key(node)];
        parse(m);
        return m;
    }
    if (node.isSameObjectAs(root_)) {
        if (!root_cache_) {
            root_cache_.emplace();
            parse(*root_cache_);
        }
        return *root_cache_;
    }
    scratch.clear();
    parse(scratch);
    return scratch;
}

// Search of the parser's PDNumberTreeNode / PDNameTreeNode on an explicit stack: nesting has no limit. `open` decides a
// node: true with `found` set when the node holds the key, else it may return the kids to search.
template <class Open>
static Obj search_tree(const Obj& root, Open open) {
    struct Level {
        std::uint64_t key;     // 0: direct node
        std::vector<Obj> kids;
        std::size_t next = 0;
    };
    std::vector<Level> stack;
    std::unordered_set<std::uint64_t> path;
    auto enter = [&](const Obj& node, Obj& found) {
        std::uint64_t k = 0;
        if (node.isIndirect()) {
            k = node_key(node);
            if (!path.insert(k).second) return false;   // LoopedException: the branch ends here
        }
        std::vector<Obj> kids;
        if (open(node, found, kids)) return true;
        if (kids.empty()) {
            if (k) path.erase(k);
            return false;
        }
        stack.push_back({k, std::move(kids), 0});
        return false;
    };
    Obj found = Obj::newNull();
    if (enter(root, found)) return found;
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            if (top.key) path.erase(top.key);
            stack.pop_back();
            continue;
        }
        Obj kid = top.kids[top.next++];
        if (enter(kid, found)) return found;
    }
    return Obj::newNull();
}

Obj NumberTree::get(std::int64_t key) {
    if (!(root_.isDictionary() || root_.isStream())) return Obj::newNull();
    // A plain tree through its decoded shape (core/tree_shape.h), any other by the walk over the objects below.
    if (!shape_)
        shape_ = std::make_unique<TreeShape<std::int64_t>>(
            root_, "Nums",
            [](const Obj& item, std::int64_t& out) {
                if (!item.isInteger()) return false;
                out = item.getIntValue();
                return true;
            },
            true);
    {
        Obj found = Obj::newNull();
        const auto leaf = [&](const Obj& node, Obj& value) {
            Nums scratch;
            const Nums& m = nums_of(node, scratch);
            auto it = m.find(key);
            if (it == m.end() || it->second.isNull()) return false;
            value = it->second;
            return true;
        };
        switch (shape_->find(key, leaf, found)) {
            case TreeShape<std::int64_t>::Result::Found: return found;
            case TreeShape<std::int64_t>::Result::NotFound: return Obj::newNull();
            case TreeShape<std::int64_t>::Result::NotPlain: break;
        }
    }
    return search_tree(root_, [&](const Obj& node, Obj& found, std::vector<Obj>& kids) {
        Obj limits = cos::get(node, "Limits");
        // PDNumberTreeNode.parseLimitsArray: an array of at least two integers, otherwise no limits.
        if (limits.isArray() && limits.getArrayNItems() >= 2 && limits.getArrayItem(0).isInteger() &&
            limits.getArrayItem(1).isInteger() &&
            (key < limits.getArrayItem(0).getIntValue() || key > limits.getArrayItem(1).getIntValue()))
            return false;
        if (cos::has(node, "Nums")) {
            Nums scratch;
            const Nums& m = nums_of(node, scratch);
            auto it = m.find(key);
            if (it == m.end() || it->second.isNull()) return false;
            found = it->second;
            return true;
        }
        if (cos::has(node, "Kids")) kids = cos::array_items(cos::get(node, "Kids"));
        return false;
    });
}

// --- name tree ----------------------------------------------------------------------------------------

const NameTree::Names& NameTree::names_of(const Obj& node, Names& scratch) {
    auto parse = [&](Names& m) {
        Obj names = cos::get(node, "Names");
        if (!names.isArray()) return;
        auto items = cos::array_items(names);
        for (std::size_t i = 0; i < items.size(); i += 2) {
            auto k = vera_string(items[i]);
            if (!k) continue;
            m[*k] = i + 1 < items.size() ? items[i + 1] : Obj::newNull();   // LinkedHashMap.put: last wins
        }
    };
    if (node.isIndirect()) {
        auto it = cache_.find(node_key(node));
        if (it != cache_.end()) return it->second;
        Names& m = cache_[node_key(node)];
        parse(m);
        return m;
    }
    if (node.isSameObjectAs(root_)) {
        if (!root_cache_) {
            root_cache_.emplace();
            parse(*root_cache_);
        }
        return *root_cache_;
    }
    scratch.clear();
    parse(scratch);
    return scratch;
}

Obj NameTree::get(const std::string& key) {
    if (!(root_.isDictionary() || root_.isStream())) return Obj::newNull();
    // A plain tree through its decoded shape (the limits as UTF-8, as compared below), any other by the walk below.
    if (!shape_)
        shape_ = std::make_unique<TreeShape<std::string>>(
            root_, "Names",
            [](const Obj& item, std::string& out) {
                if (!item.isString()) return false;
                out = pdf_text_to_utf8(item.getStringValue());
                return true;
            },
            true);
    {
        Obj found = Obj::newNull();
        const auto leaf = [&](const Obj& node, Obj& value) {
            Names scratch;
            const Names& m = names_of(node, scratch);
            auto it = m.find(key);
            if (it == m.end() || it->second.isNull()) return false;
            value = it->second;
            return true;
        };
        switch (shape_->find(key, leaf, found)) {
            case TreeShape<std::string>::Result::Found: return found;
            case TreeShape<std::string>::Result::NotFound: return Obj::newNull();
            case TreeShape<std::string>::Result::NotPlain: break;
        }
    }
    return search_tree(root_, [&](const Obj& node, Obj& found, std::vector<Obj>& kids) {
        if (!(node.isDictionary() || node.isStream())) return false;
        Obj limits = cos::get(node, "Limits");
        if (limits.isArray() && limits.getArrayNItems() >= 2 && limits.getArrayItem(0).isString() &&
            limits.getArrayItem(1).isString()) {
            // String.compareTo; UTF-8 byte order equals code point order (differs from UTF-16 order only
            // between supplementary characters and U+E000..U+FFFF).
            std::string lo = pdf_text_to_utf8(limits.getArrayItem(0).getStringValue());
            std::string hi = pdf_text_to_utf8(limits.getArrayItem(1).getStringValue());
            if (key < lo || key > hi) return false;
        }
        if (cos::has(node, "Names")) {
            Names scratch;
            const Names& m = names_of(node, scratch);
            auto it = m.find(key);
            if (it == m.end() || it->second.isNull()) return false;
            found = it->second;
            return true;
        }
        if (cos::has(node, "Kids")) kids = cos::array_items(cos::get(node, "Kids"));
        return false;
    });
}

// --- rich text ----------------------------------------------------------------------------------------

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Every '&' of unescaped character data must start a character reference or one of the five predefined
// entities (a document without an internal DTD subset declares no others; JAXP reports a fatal error).
bool references_ok(std::string_view s) {
    static const char* const kPredef[] = {"amp;", "lt;", "gt;", "apos;", "quot;"};
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') return false;
        if (s[i] != '&') continue;
        std::string_view rest = s.substr(i + 1);
        bool ok = false;
        for (const char* p : kPredef) {
            if (rest.substr(0, std::char_traits<char>::length(p)) == p) ok = true;
        }
        if (!ok && !rest.empty() && rest[0] == '#') {
            std::size_t j = 1;
            bool hex = rest.size() > 1 && rest[1] == 'x';
            if (hex) j = 2;
            std::size_t start = j;
            while (j < rest.size() && (hex ? std::isxdigit(static_cast<unsigned char>(rest[j]))
                                           : std::isdigit(static_cast<unsigned char>(rest[j]))))
                ++j;
            ok = j > start && j < rest.size() && rest[j] == ';';
        }
        if (!ok) return false;
    }
    return true;
}

std::string_view prefix_of(std::string_view qname) {
    auto c = qname.find(':');
    return c == std::string_view::npos ? std::string_view() : qname.substr(0, c);
}

// Namespace well-formedness (the JAXP builder is namespace aware) and the raw character data checks.
bool raw_node_ok(const pugi::xml_node& n, std::vector<std::string>& scope, int depth) {
    if (depth > kMaxDepth) {
        note_unread("annotation rich text nested beyond the walkers' limit");
        return false;
    }
    std::size_t mark = scope.size();
    for (const pugi::xml_attribute& a : n.attributes()) {
        std::string_view name = a.name();
        if (name.substr(0, 6) == "xmlns:") {
            if (*a.value() == '\0') return false;   // undeclaring a prefix is an error in XML 1.0 namespaces
            scope.emplace_back(name.substr(6));
        }
        if (!references_ok(a.value())) return false;
    }
    auto bound = [&](std::string_view p) {
        if (p.empty() || p == "xml" || p == "xmlns") return true;
        return std::find(scope.begin(), scope.end(), p) != scope.end();
    };
    if (!bound(prefix_of(n.name()))) return false;
    for (const pugi::xml_attribute& a : n.attributes()) {
        std::string_view name = a.name();
        if (name.substr(0, 6) != "xmlns:" && !bound(prefix_of(name))) return false;
    }
    for (const pugi::xml_node& c : n.children()) {
        if (c.type() == pugi::node_pcdata && !references_ok(c.value())) return false;
        if (c.type() == pugi::node_element && !raw_node_ok(c, scope, depth + 1)) return false;
    }
    scope.resize(mark);
    return true;
}

void collect_text(const pugi::xml_node& n, std::string& out, int depth) {
    if (depth > kMaxDepth) {
        note_unread("annotation rich text nested beyond the walkers' limit");
        return;
    }
    for (const pugi::xml_node& c : n.children()) {
        switch (c.type()) {
            case pugi::node_pcdata:
            case pugi::node_cdata:
            case pugi::node_pi:
                out += c.value();
                break;
            case pugi::node_element:
                collect_text(c, out, depth + 1);
                break;
            default:
                break;   // declaration, doctype, comments (ignored by the builder)
        }
    }
}

}  // namespace

std::optional<std::string> rich_text_plain(std::string_view xml) {
    const unsigned base = pugi::parse_cdata | pugi::parse_eol | pugi::parse_pi | pugi::parse_ws_pcdata |
                          pugi::parse_declaration | pugi::parse_doctype | pugi::parse_wconv_attribute;
    pugi::xml_document raw;
    if (!raw.load_buffer(xml.data(), xml.size(), base, pugi::encoding_auto)) return std::nullopt;
    int elements = 0;
    for (const pugi::xml_node& c : raw.children()) {
        switch (c.type()) {
            case pugi::node_element:
                ++elements;
                break;
            case pugi::node_pcdata: {
                std::string_view v = c.value();
                if (!std::all_of(v.begin(), v.end(), is_ws)) return std::nullopt;   // content in the prolog
                break;
            }
            case pugi::node_cdata:
                return std::nullopt;
            case pugi::node_doctype: {
                // An external DTD cannot be loaded (secure processing): the parse fails.
                std::string_view v = c.value();
                if (v.find("SYSTEM") != std::string_view::npos || v.find("PUBLIC") != std::string_view::npos)
                    return std::nullopt;
                // Entities of an internal subset are not expanded here: undecidable, treated as an error.
                if (v.find('[') != std::string_view::npos) return std::nullopt;
                break;
            }
            default:
                break;
        }
    }
    if (elements != 1) return std::nullopt;
    std::vector<std::string> scope;
    if (!raw_node_ok(raw.document_element(), scope, 0)) return std::nullopt;
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), base | pugi::parse_escapes, pugi::encoding_auto)) return std::nullopt;
    std::string out;
    for (const pugi::xml_node& c : doc.children()) {
        if (c.type() == pugi::node_pi) out += c.value();
        else if (c.type() == pugi::node_element) collect_text(c, out, 0);
    }
    return out;
}

std::optional<std::string> rich_text_entry(const Obj& dict, std::string_view key) {
    Obj v = cos::get(dict, key);
    if (v.isStream()) {
        auto data = cos::stream_data(v);
        if (!data) return std::nullopt;   // getData() failure: the XML parse fails
        return rich_text_plain(*data);
    }
    if (v.isString()) {
        // COSString.getString().getBytes() (UTF-8) is what the XML parser reads.
        std::string s = pdf_text_to_utf8(v.getStringValue());
        return rich_text_plain(s);
    }
    return std::nullopt;
}

std::optional<std::string> text_or_stream_entry(const Obj& dict, std::string_view key) {
    Obj v = cos::get(dict, key);
    if (v.isStream()) {
        auto data = cos::stream_data(v);
        // COSStream.getStringFromTextStream: an unreadable stream gives an empty COSString.
        return pdf_text_to_utf8(data ? *data : std::string());
    }
    if (v.isString()) return pdf_text_to_utf8(v.getStringValue());
    return std::nullopt;
}

bool media_clip_has_correct_alt(const Obj& clip) {
    Obj alt = cos::get(clip, "Alt");
    if (!alt.isArray()) return false;
    auto items = cos::array_items(alt);
    if (items.size() % 2 != 0) return false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].isString()) return false;
        if (i % 2 == 1 && pdf_text_to_utf8(items[i].getStringValue()).empty()) return false;
    }
    return true;
}

}  // namespace eqx::annot
