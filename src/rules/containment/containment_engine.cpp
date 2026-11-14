// Evaluation of the generated containment rules (see containment.h) with the exact semantics of the
// veraPDF 1.30.2 validation model:
//
// * Evaluated structure elements: by default every element of the core tree (StructTree::elems: each
//   indirect element once, every direct element). With reference_dedup exactly the elements veraPDF
//   validates (StructTree::verapdf_order): veraPDF's BaseValidator registers the model id of a pushed object
//   and a direct GFPDStructElem has the id "0 0 obj <S>", so of several direct elements with the same /S it
//   validates only the one pushed first (with its subtree) - a veraPDF defect that skips real elements.
// * Struct children: StructElem::struct_kids (TaggedPDFHelper.getStructChildren; a child listed twice or
//   shared by two parents counts in every parent's kidsStandardTypes); content items: the non-element kids.
// * Standard types: Pdf17 = TaggedPDFRoleMapHelper.getStandardType (ua1 profile), Pdf20 =
//   TaggedPDFHelper.getDefaultStructureType (ua2/wt1a/wt1r); a Pdf20 type in the MathML namespace is
//   "MathML" (GFSEFactory -> GFSEMathMLStructElem) for kid lists and object selection, while the
//   pass-through test of getparentStandardType uses the type name itself.
// * Pass-through elements (PDStructElem.isPassThroughTag: Part, Div, NonStruct) are flattened in
//   kidsStandardTypes and hasContentItems; kid lists are summarised bottom-up (counts, length, first/last,
//   head), so a document is evaluated in time linear in elements x distinct types.
// * The five rules that forbid a pass-through type in a non-grouping element (Annot/Form/Link/Reference-Part-1,
//   Quote-Div-1) can never fail in veraPDF: getChildrenStandardTypes replaces every Part/Div/NonStruct child by its own
//   children, so the literal is never in kidsStandardTypes. ISO/TS 32005 Table 5 states the rule (Part is not
//   permitted inside Link when Link is used as a non-grouping element). With EQX_TS32005_CLAUSE=1 these children themselves count,
//   through further pass-through elements (never with --reference-dedup). Off by default: the reference expectations do not
//   list such nestings yet.
// * Reference non-termination: a cycle of pass-through elements makes getChildrenStandardTypes recurse
//   without end, a cycle of pass-through /P parents makes getparentStandardType loop; the affected rules are
//   reported incomplete for that document instead of being guessed.
//
// The rule table is compiled by the compiler (kCompiled): type names interned, atoms resolved, rules grouped by the
// structure type they select. A process starts with the finished tables in its read-only data.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "rules/containment/containment.h"
#include "rules/containment/containment_rules.h"

namespace eqx::containment {

namespace {

constexpr int kHead = 12;    // kid tokens kept verbatim (KIDS_EQ sequences are shorter; details)
constexpr int kCycle = -2;   // parentStandardType does not terminate

// ---- standard type names and their classes -----------------------------------------------------------

enum : std::uint8_t { TF_HN = 1, TF_INLINE = 2, TF_GROUPING = 4, TF_BOG = 8, TF_BGI = 16, TF_PT = 32 };

constexpr bool is_hn_name(std::string_view t) {
    // TaggedPDFConstants.HN_REGEXP ^H[1-9][0-9]*$
    if (t.size() < 2 || t[0] != 'H' || t[1] < '1' || t[1] > '9') return false;
    for (std::size_t i = 2; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

constexpr bool in_list(std::string_view t, std::initializer_list<std::string_view> l) {
    for (std::string_view x : l)
        if (x == t) return true;
    return false;
}

// TaggedPDFHelper inlineStructureTypes / groupingStructureTypes / blockOrGroupingStructureTypes /
// blockOrGroupingOrInlineStructureTypes; PDStructElem.isPassThroughTag.
constexpr std::uint8_t flags_of(std::string_view t) {
    std::uint8_t f = 0;
    if (is_hn_name(t)) f |= TF_HN;
    if (in_list(t, {"Span", "Em", "Strong", "Sub", "Ruby", "RB", "RP", "RT", "Warichu", "WT", "WP"})) f |= TF_INLINE;
    if (in_list(t, {"Art", "Part", "TOC", "TOCI", "Index", "Sect", "Div", "Aside", "NonStruct"})) f |= TF_GROUPING;
    if (in_list(t, {"Title", "BlockQuote", "Caption"})) f |= TF_BOG;
    if (in_list(t, {"Link", "Artifact", "Reference", "BibEntry", "Note", "Quote", "Formula", "Private", "Figure",
                    "Form", "Code", "Annot", "FENote"}))
        f |= TF_BGI;
    if (in_list(t, {"Part", "Div", "NonStruct"})) f |= TF_PT;
    return f;
}

// The type names of the rule table, interned while the table is compiled: the type ids 0 .. n-1 of every document.
constexpr int kMaxBaseTypes = 96;
struct BaseTypes {
    std::array<std::string_view, kMaxBaseTypes> names{};
    int n = 0;
    constexpr int id(std::string_view s) {
        for (int i = 0; i < n; ++i)
            if (names[i] == s) return i;
        if (n == kMaxBaseTypes) throw "containment table: more type names than kMaxBaseTypes";
        names[n] = s;
        return n++;
    }
};

// ---- compiled rules ------------------------------------------------------------------------------------

// Type ids of the sets and sequences of the atoms (CAtom::first, CAtom::count).
struct IdPool {
    std::array<std::int16_t, 256> ids{};
    int n = 0;
    constexpr bool add(int id) {
        if (n == static_cast<int>(ids.size()) || id > 0x7fff) return false;
        ids[n++] = static_cast<std::int16_t>(id);
        return true;
    }
};

struct CAtom {
    AK kind = AK::GROUPING_IS;
    std::uint8_t pred = PRED_NONE;
    std::uint8_t cmp = CMP_EQ;
    std::uint8_t count = 0;    // ids of the set (distinct) or of the sequence
    std::uint16_t first = 0;   // their position in the pool
    std::int16_t tok = -1;
    std::int32_t n = 0;
};

struct CRule {
    enum Sel : std::uint8_t { TOK, HN, STAR, ROOT };
    std::uint32_t gen = 0;
    std::uint8_t flavour = 0;     // 0 = Pdf17, 1 = Pdf20
    std::uint8_t sel = TOK;
    std::uint8_t natoms = 0;
    bool kids = false;            // uses kidsStandardTypes / hasContentItems / isGrouping
    bool parent = false;          // uses parentStandardType
    bool unsupported = false;     // rule not supported by this evaluator
    bool only_parent_ne = false;  // one PARENT_NE atom: the test passes unless parentStandardType is atoms[0].tok
    std::int16_t sel_tok = -1;
    CAtom atoms[3];
};

template <class Interner>
constexpr std::int16_t tok_id(Interner& T, std::string_view s, CRule& r) {
    const int i = T.id(s);
    if (i > 0x7fff) {
        r.unsupported = true;
        return -1;
    }
    return static_cast<std::int16_t>(i);
}

// Interns the parts of `arg` between `sep` characters (''.split(sep) is ['']) and appends their ids to the pool.
// `distinct`: a set, each id once.
template <class Interner>
constexpr void add_ids(Interner& T, IdPool& pool, std::string_view arg, char sep, bool distinct, bool empty_is_error,
                       CAtom& a, CRule& r) {
    a.first = static_cast<std::uint16_t>(pool.n);
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = arg.find(sep, start);
        const std::string_view part = arg.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start);
        if (empty_is_error && part.empty()) r.unsupported = true;   // empty token in a kid sequence
        const int id = T.id(part);
        bool known = false;
        for (int k = 0; distinct && k < a.count; ++k) known = known || pool.ids[a.first + k] == id;
        if (!known) {
            if (a.count == 0xff || !pool.add(id)) r.unsupported = true;   // more ids than the tables hold
            else ++a.count;
        }
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
}

// Run by the compiler for the generated table, and at run time for a single rule (test_passes).
template <class Interner>
constexpr CRule compile_rule(const GenRule& g, std::size_t gen, Interner& T, IdPool& pool) {
    CRule r;
    r.gen = static_cast<std::uint32_t>(gen);
    r.flavour = g.flavour == Flavor::Pdf17 ? 0 : 1;
    const std::string_view obj = g.object;
    if (obj == "*") r.sel = CRule::STAR;
    else if (obj == "#root") r.sel = CRule::ROOT;
    else if (obj == "Hn") r.sel = CRule::HN;
    else r.sel_tok = tok_id(T, obj, r);
    if (g.natoms == 0 || g.natoms > 3) r.unsupported = true;   // invalid atom count
    for (int i = 0; i < g.natoms && i < 3; ++i) {
        const GenAtom& ga = g.atoms[i];
        CAtom a;
        a.kind = ga.kind;
        a.pred = ga.pred;
        a.cmp = ga.cmp;
        a.n = ga.n;
        const std::string_view arg = ga.arg ? std::string_view(ga.arg) : std::string_view();
        switch (ga.kind) {
            case AK::GROUPING_IS:
            case AK::NO_CONTENT_ITEMS:
                r.kids = true;
                break;
            case AK::PARENT_EQ:
            case AK::PARENT_NE:
                a.tok = tok_id(T, arg, r);
                r.parent = true;
                break;
            case AK::PARENT_NOT_HN:
                r.parent = true;
                break;
            case AK::PARENT_IN:
                add_ids(T, pool, arg, '|', true, false, a, r);
                r.parent = true;
                break;
            case AK::KID_COUNT:
                r.kids = true;
                if (ga.pred == PRED_EQ) a.tok = tok_id(T, arg, r);
                else if (ga.pred == PRED_NOT_IN) add_ids(T, pool, arg, '|', true, false, a, r);
                else if (ga.pred != PRED_HN) r.unsupported = true;             // invalid filter predicate
                if (arg.empty() && ga.pred != PRED_HN) r.unsupported = true;   // empty filter literal
                if (ga.cmp > CMP_GT) r.unsupported = true;                     // invalid comparison
                break;
            case AK::KIDS_EQ:
                r.kids = true;
                if (!arg.empty()) add_ids(T, pool, arg, '&', false, true, a, r);
                if (a.count >= kHead) r.unsupported = true;                    // kid sequence too long
                break;
            case AK::KIDS_NOT_CONTAINS:
                // Only '&X&' is supported: tokens never contain '&', so the substring occurs exactly when a
                // kid X is neither the first nor the last kid.
                r.kids = true;
                if (arg.size() < 3 || arg.front() != '&' || arg.back() != '&' ||
                    arg.substr(1, arg.size() - 2).find('&') != std::string_view::npos)
                    r.unsupported = true;                                      // unsupported indexOf literal
                else
                    a.tok = tok_id(T, arg.substr(1, arg.size() - 2), r);
                break;
            case AK::KIDS_NOT_AFTER_FIRST:
                r.kids = true;
                a.tok = tok_id(T, arg, r);
                break;
            case AK::KIDS_ALL_IN:
                r.kids = true;
                add_ids(T, pool, arg, '|', true, false, a, r);
                break;
            default:
                r.unsupported = true;                                          // unknown atom kind
        }
        r.atoms[r.natoms++] = a;
    }
    r.only_parent_ne = !r.unsupported && r.natoms == 1 && r.atoms[0].kind == AK::PARENT_NE;
    return r;
}

enum : std::uint8_t { NEED_KIDS = 1, NEED_PARENT = 2 };

// A run of rule indices in Compiled::order, and the properties its rules read.
struct Group {
    std::uint16_t first = 0, last = 0;
    std::uint8_t needs = 0;
};

struct Compiled {
    std::array<CRule, kGenRuleCount> rules{};
    IdPool pool;
    BaseTypes types;
    std::array<std::uint8_t, kMaxBaseTypes> flags{};
    int tok_root = -1, tok_mathml = -1;
    // Rule indices grouped by selector, each group in table order. Per flavour: the rules of one standard type, the
    // Hn rules, the rules of every element, the StructTreeRoot rules.
    std::array<std::uint16_t, kGenRuleCount> order{};
    std::array<std::array<Group, kMaxBaseTypes>, 2> by_tok{};
    std::array<Group, 2> hn{}, star{}, root{};
};

consteval Compiled compile_table() {
    Compiled c;
    for (std::string_view t :
         {"Document", "Part", "Div", "Caption", "Sect", "NonStruct", "H", "P", "L", "LI", "Lbl", "LBody", "Table",
          "TR", "TH", "TD", "Span", "Link", "Figure", "Formula", "Form", "Art", "BlockQuote", "TOC", "TOCI",
          "Index", "Private", "Quote", "Note", "Reference", "BibEntry", "Code", "H1", "H2", "H3", "H4", "H5", "H6",
          "THead", "TBody", "TFoot", "Annot", "Ruby", "Warichu", "RB", "RT", "RP", "WT", "WP",
          "DocumentFragment", "Aside", "Title", "FENote", "Sub", "Em", "Strong", "Artifact"})
        c.types.id(t);
    c.tok_root = c.types.id("StructTreeRoot");
    c.tok_mathml = c.types.id("MathML");
    for (std::size_t i = 0; i < kGenRuleCount; ++i) c.rules[i] = compile_rule(kGenRules[i], i, c.types, c.pool);
    for (int t = 0; t < c.types.n; ++t) c.flags[t] = flags_of(c.types.names[t]);
    const auto group_of = [&c](const CRule& r) -> Group& {
        switch (r.sel) {
            case CRule::TOK: return c.by_tok[r.flavour][r.sel_tok];
            case CRule::HN: return c.hn[r.flavour];
            case CRule::STAR: return c.star[r.flavour];
            default: return c.root[r.flavour];
        }
    };
    // Group sizes, then group positions, then the rules in table order.
    for (const CRule& r : c.rules) ++group_of(r).last;
    std::uint16_t pos = 0;
    const auto place = [&pos](Group& g) {
        const std::uint16_t size = g.last;
        g.first = g.last = pos;
        pos = static_cast<std::uint16_t>(pos + size);
    };
    for (int f = 0; f < 2; ++f) {
        for (int t = 0; t < c.types.n; ++t) place(c.by_tok[f][t]);
        place(c.hn[f]);
        place(c.star[f]);
        place(c.root[f]);
    }
    for (std::size_t i = 0; i < kGenRuleCount; ++i) {
        const CRule& r = c.rules[i];
        Group& g = group_of(r);
        c.order[g.last++] = static_cast<std::uint16_t>(i);
        g.needs |= (r.kids ? NEED_KIDS : 0) | (r.parent ? NEED_PARENT : 0);
    }
    return c;
}

static_assert(kGenRuleCount <= 0xffff);
constexpr Compiled kCompiled = compile_table();

// Hash index of the base type names (open addressing; a slot holds a type id, -1 = free).
constexpr std::uint32_t name_hash(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (char ch : s) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return h;
}
constexpr std::uint32_t kSlotMask = 255;
constexpr std::array<std::int8_t, kSlotMask + 1> kBaseSlots = []() consteval {
    std::array<std::int8_t, kSlotMask + 1> slots{};
    for (auto& s : slots) s = -1;
    for (int i = 0; i < kCompiled.types.n; ++i) {
        std::uint32_t h = name_hash(kCompiled.types.names[i]) & kSlotMask;
        while (slots[h] >= 0) h = (h + 1) & kSlotMask;
        slots[h] = static_cast<std::int8_t>(i);
    }
    return slots;
}();

// Type names of one document: the base types (ids as compiled) and, after them, every other name the document uses.
class Types {
public:
    int id(std::string_view s) {
        for (std::uint32_t h = name_hash(s) & kSlotMask; kBaseSlots[h] >= 0; h = (h + 1) & kSlotMask)
            if (kCompiled.types.names[kBaseSlots[h]] == s) return kBaseSlots[h];
        auto it = extra_.find(s);
        if (it != extra_.end()) return it->second;
        const int i = kCompiled.types.n + static_cast<int>(names_.size());
        names_.emplace_back(s);
        flags_.push_back(flags_of(s));
        extra_.emplace(std::string_view(names_.back()), i);
        return i;
    }
    std::uint8_t flags(int i) const {
        if (i < 0) return 0;
        if (i < kCompiled.types.n) return kCompiled.flags[i];
        i -= kCompiled.types.n;
        return i < static_cast<int>(flags_.size()) ? flags_[i] : 0;
    }
    std::string_view name(int i) const {
        return i < kCompiled.types.n ? kCompiled.types.names[i] : std::string_view(names_[i - kCompiled.types.n]);
    }

private:
    std::deque<std::string> names_;   // stable addresses: extra_ refers to them
    std::vector<std::uint8_t> flags_;
    std::unordered_map<std::string_view, int> extra_;
};

// ---- kid statistics ------------------------------------------------------------------------------------

// Summary of a flattened kid-type sequence (kidsStandardTypes) plus hasContentItems.
struct Summary {
    int len = 0, first = -1, last = -1;
    int head[kHead] = {};
    int head_len = 0;
    int hn = 0, inl = 0;
    bool content = false, cycle = false;
    bool has_part = false, has_div = false;    // a pass-through child of that type, also below further pass-through ones
    std::vector<std::pair<int, int>> counts;   // sparse (type id, count), sorted by id

    void add(int tok, std::uint8_t flags) {
        if (len == 0) first = tok;
        last = tok;
        if (head_len < kHead) head[head_len++] = tok;
        ++len;
        if (flags & TF_HN) ++hn;
        if (flags & TF_INLINE) ++inl;
        auto it = std::lower_bound(counts.begin(), counts.end(), std::make_pair(tok, 0));
        if (it != counts.end() && it->first == tok) ++it->second;
        else counts.insert(it, {tok, 1});
    }
    void append(const Summary& o) {
        if (o.len > 0) {
            if (len == 0) first = o.first;
            last = o.last;
            for (int i = 0; i < o.head_len && head_len < kHead; ++i) head[head_len++] = o.head[i];
            len += o.len;
            hn += o.hn;
            inl += o.inl;
            std::vector<std::pair<int, int>> merged;
            merged.reserve(counts.size() + o.counts.size());
            auto a = counts.cbegin();
            auto b = o.counts.cbegin();
            while (a != counts.cend() || b != o.counts.cend()) {
                if (b == o.counts.cend() || (a != counts.cend() && a->first < b->first)) merged.push_back(*a++);
                else if (a == counts.cend() || b->first < a->first) merged.push_back(*b++);
                else merged.push_back({a->first, (a++)->second + (b++)->second});
            }
            counts.swap(merged);
        }
        content = content || o.content;
        cycle = cycle || o.cycle;
        has_part = has_part || o.has_part;
        has_div = has_div || o.has_div;
    }
    int count(int tok) const {
        auto it = std::lower_bound(counts.begin(), counts.end(), std::make_pair(tok, 0));
        return it != counts.end() && it->first == tok ? it->second : 0;
    }
};

bool compare(int v, std::uint8_t cmp, int n) {
    switch (cmp) {
        case CMP_EQ: return v == n;
        case CMP_LE: return v <= n;
        case CMP_LT: return v < n;
        case CMP_GT: return v > n;
    }
    return false;
}

// GFPDStructElem.getisGrouping.
bool is_grouping(int own, const Summary& k, const Types& T) {
    if (own < 0) return false;
    std::uint8_t f = T.flags(own);
    if (f & TF_GROUPING) return true;
    if (!(f & (TF_BOG | TF_BGI))) return false;
    if (k.content) return false;
    return k.inl == 0;
}

// One atom with JavaScript semantics. `parent` = parentStandardType id (-1 = null); `ids` = the pool of the rule's
// sets and sequences.
bool eval_atom(const CAtom& a, const std::int16_t* ids, const Summary& k, int parent, bool grouping, const Types& T,
               bool pass_through_kids) {
    const std::int16_t* set = ids + a.first;
    const std::int16_t* set_end = set + a.count;
    switch (a.kind) {
        case AK::GROUPING_IS: return grouping == (a.n != 0);
        case AK::NO_CONTENT_ITEMS: return !k.content;
        case AK::PARENT_EQ: return parent >= 0 && parent == a.tok;
        case AK::PARENT_NE: return !(parent >= 0 && parent == a.tok);
        case AK::PARENT_NOT_HN: return !(parent >= 0 && (T.flags(parent) & TF_HN));
        case AK::PARENT_IN: return parent >= 0 && std::find(set, set_end, parent) != set_end;
        case AK::KID_COUNT: {
            int c = 0;
            if (k.len == 0) {
                // ''.split('&') == ['']: only a "not one of" filter keeps the empty token
                c = a.pred == PRED_NOT_IN ? 1 : 0;
            } else if (a.pred == PRED_EQ) {
                c = k.count(a.tok);
            } else if (a.pred == PRED_HN) {
                c = k.hn;
            } else {
                c = k.len;
                for (const std::int16_t* t = set; t != set_end; ++t) c -= k.count(*t);
            }
            if (pass_through_kids && a.pred == PRED_EQ && (T.flags(a.tok) & TF_PT)) {
                const std::string_view n = T.name(a.tok);
                if ((n == "Part" && k.has_part) || (n == "Div" && k.has_div)) ++c;
            }
            return compare(c, a.cmp, a.n);
        }
        case AK::KIDS_EQ:
            if (a.count == 0) return k.len == 0;
            if (k.len != static_cast<int>(a.count)) return false;
            for (int i = 0; i < a.count && i < kHead; ++i)
                if (k.head[i] != set[i]) return false;
            return true;
        case AK::KIDS_NOT_CONTAINS: {
            int c = k.count(a.tok);
            int ends = (k.len >= 1 && k.first == a.tok ? 1 : 0) + (k.len >= 2 && k.last == a.tok ? 1 : 0);
            return c <= ends;
        }
        case AK::KIDS_NOT_AFTER_FIRST:
            if (k.len == 0) return true;
            return k.count(a.tok) - (k.first == a.tok ? 1 : 0) == 0;
        case AK::KIDS_ALL_IN: {
            if (k.len == 0) return false;
            int c = 0;
            for (const std::int16_t* t = set; t != set_end; ++t) c += k.count(*t);
            return c == k.len;
        }
    }
    return false;
}

bool eval_rule(const CRule& r, const std::int16_t* ids, const Summary& k, int parent, int own, const Types& T,
               bool pass_through_kids = false) {
    bool grouping = is_grouping(own, k, T);
    for (int i = 0; i < r.natoms; ++i)
        if (eval_atom(r.atoms[i], ids, k, parent, grouping, T, pass_through_kids)) return true;
    return false;
}

// ---- document evaluation ---------------------------------------------------------------------------------

struct StdInfo {
    int tok = -1;             // token in kid lists / object type (-1 = null standard type)
    bool name_pt = false;     // standard type name is Part/Div/NonStruct (getparentStandardType loop)
    bool math = false;        // MathML namespace (Pdf20 only)
    bool root_type = false;   // /Type /StructTreeRoot
};

// SType::ns_uri() == kNsMathML, without building the URI of a type that has no namespace dictionary.
bool in_mathml_namespace(const SType& t) {
    if (!t.ns.isDictionary()) return false;   // ns_uri() is kNsPdf then
    const std::optional<std::string> uri = t.ns_uri();
    return uri && *uri == kNsMathML;
}

// A node of the evaluation is a structure element of the core tree (its element index) or the StructTreeRoot
// (index = number of elements). One flavour is evaluated after the other; the per-flavour state is reused.
class Engine {
public:
    Engine(const StructTree& st, const Sink& sink, bool reference_dedup)
        : st_(st), sink_(sink), reference_dedup_(reference_dedup), C_(kCompiled), elems_(st.elems()),
          root_(static_cast<int>(st.elems().size())), root_dict_(st.root()) {}

    void run() {
        build_nodes();
        for (int f = 0; f < 2; ++f) evaluate_flavour(f);
    }

private:
    const StructTree& st_;
    const Sink& sink_;
    bool reference_dedup_;
    const Compiled& C_;
    const StructElems elems_;
    const int root_;
    const Obj root_dict_;
    Types T_;
    std::vector<char> own_content_;      // per node: MCID / MCR / OBJR directly in /K (TaggedPDFHelper.isContentItem)
    std::vector<char> root_type_;        // per element: /Type /StructTreeRoot
    std::vector<int> root_kids_;
    // state of the flavour being evaluated
    std::vector<StdInfo> std_;                             // per node
    std::unordered_map<std::uint64_t, StdInfo> std_memo_;   // /P targets that are no element of the tree
    std::unordered_map<std::uint64_t, int> parent_memo_;
    std::vector<int> pst_state_;                            // 0 new, 1 in progress, 2 done
    std::vector<int> pst_index_;
    std::vector<Summary> psums_;
    std::unordered_set<std::size_t> incomplete_;

    // -- tree (core StructTree: struct_kids = veraPDF getStructChildren, shared kids listed in every parent) --
    // Every struct child of a node in /K order, shared/revisited ones included.
    std::span<const int> kids_of(int n) const { return n == root_ ? std::span<const int>(root_kids_) : elems_[n].struct_kids; }
    const Obj& dict_of(int n) const { return n == root_ ? root_dict_ : elems_[n].dict; }

    void build_nodes() {
        own_content_.assign(elems_.size() + 1, 0);
        root_type_.assign(elems_.size(), 0);
        for (std::size_t i = 0; i < elems_.size(); ++i) {
            for (const StructKid& k : elems_[i].kids)
                if (k.kind != StructKid::Kind::Elem) {
                    own_content_[i] = 1;
                    break;
                }
            // the same for both flavours: read once, in element order
            root_type_[i] = cos::name_is(elems_[i].dict, "Type", "StructTreeRoot");
        }
        for (const StructKid& k : st_.root_kids()) {
            if (k.kind != StructKid::Kind::Elem) own_content_[root_] = 1;
            else if (k.elem >= 0) root_kids_.push_back(k.elem);
        }
    }

    // -- standard types --
    StdInfo compute_std(const Obj& d, int f) {
        StdInfo s;
        s.root_type = cos::name_is(d, "Type", "StructTreeRoot");
        if (f == 0) {
            if (auto t = st_.standard_of(d, Flavor::Pdf17)) {
                s.tok = T_.id(*t);
                s.name_pt = (T_.flags(s.tok) & TF_PT) != 0;
            }
        } else if (auto ty = st_.stype_of(d)) {
            if (auto dt = st_.default_type20(*ty)) {
                s.math = in_mathml_namespace(*dt);
                s.name_pt = (flags_of(dt->type) & TF_PT) != 0;
                s.tok = s.math ? C_.tok_mathml : T_.id(dt->type);
            }
        }
        return s;
    }

    // An element of the tree has its standard type in std_ (each indirect element is in the tree once).
    StdInfo std_of_obj(const Obj& d, int f) {
        ObjRef r = ref_of(d);
        if (!r.valid()) return compute_std(d, f);
        if (const int i = st_.find(r); i >= 0) return std_[i];
        auto it = std_memo_.find(r.key());
        if (it != std_memo_.end()) return it->second;
        StdInfo s = compute_std(d, f);
        std_memo_.emplace(r.key(), s);
        return s;
    }

    // GFPDStructElem.getparentStandardType, starting at the element's /P value. -1 = null, kCycle = the
    // reference loops forever (pass-through /P cycle).
    int resolve_parent(Obj cur, int f) {
        // The objects on the way: a short chain is searched as it is, a long one through a set.
        constexpr std::size_t kLinear = 16;
        std::uint64_t first[kLinear];
        std::size_t n_first = 0;
        std::vector<std::uint64_t> more;
        std::unordered_set<std::uint64_t> seen;
        int res = -1;
        for (;;) {
            if (cur.isNull()) { res = -1; break; }
            ObjRef r = ref_of(cur);
            if (r.valid()) {
                const std::uint64_t key = r.key();
                auto it = parent_memo_.find(key);
                if (it != parent_memo_.end()) { res = it->second; break; }
                bool again = false;
                if (n_first < kLinear) {
                    again = std::find(first, first + n_first, key) != first + n_first;
                    if (!again) first[n_first++] = key;
                } else {
                    if (seen.empty()) seen.insert(first, first + n_first);
                    again = !seen.insert(key).second;
                    if (!again) more.push_back(key);
                }
                if (again) { res = kCycle; break; }
            }
            StdInfo s = std_of_obj(cur, f);
            if (!s.name_pt) {
                res = s.root_type ? C_.tok_root : s.math ? C_.tok_mathml : s.tok;
                break;
            }
            cur = cos::get(cur, "P");
        }
        for (std::size_t i = 0; i < n_first; ++i) parent_memo_[first[i]] = res;
        for (auto k : more) parent_memo_[k] = res;
        return res;
    }

    // -- kid summaries --
    bool tok_pt(int tok) const { return tok >= 0 && (T_.flags(tok) & TF_PT); }

    void note_pass_through(Summary& s, int tok) const {
        const std::string_view n = T_.name(tok);
        if (n == "Part") s.has_part = true;
        else if (n == "Div") s.has_div = true;
    }

    // Summary of a pass-through node (memoised, iterative; cycles flagged).
    const Summary& pt_summary(int n) {
        if (pst_state_.empty()) {   // first pass-through node of this flavour
            pst_state_.assign(elems_.size() + 1, 0);
            pst_index_.assign(elems_.size() + 1, -1);
        }
        auto& state = pst_state_;
        auto& index = pst_index_;
        auto& sums = psums_;
        if (state[n] == 2) return sums[index[n]];
        struct Frame {
            int node;
            std::size_t i;
            Summary sum;
        };
        std::vector<Frame> stack;
        auto open = [&](int node) {
            state[node] = 1;
            Frame fr{node, 0, Summary{}};
            fr.sum.content = own_content_[node] != 0;
            stack.push_back(std::move(fr));
        };
        open(n);
        while (!stack.empty()) {
            std::size_t top = stack.size() - 1;
            int node = stack[top].node;
            const auto& kids = kids_of(node);
            if (stack[top].i < kids.size()) {
                int k = kids[stack[top].i++];
                int tk = std_[k].tok;
                if (tk < 0) continue;
                if (!tok_pt(tk)) {
                    stack[top].sum.add(tk, T_.flags(tk));
                    continue;
                }
                note_pass_through(stack[top].sum, tk);
                if (state[k] == 2) {
                    stack[top].sum.append(sums[index[k]]);
                } else if (state[k] == 1) {
                    stack[top].sum.cycle = true;
                } else {
                    open(k);
                }
                continue;
            }
            index[node] = static_cast<int>(sums.size());
            sums.push_back(std::move(stack[top].sum));
            state[node] = 2;
            stack.pop_back();
            if (!stack.empty()) stack.back().sum.append(sums[index[node]]);
        }
        return sums[index[n]];
    }

    // Summary of any node's kid list (flattening its pass-through children).
    Summary node_summary(int n) {
        Summary s;
        s.content = own_content_[n] != 0;
        for (int k : kids_of(n)) {
            int tk = std_[k].tok;
            if (tk < 0) continue;
            if (tok_pt(tk)) {
                note_pass_through(s, tk);
                s.append(pt_summary(k));
            } else {
                s.add(tk, T_.flags(tk));
            }
        }
        return s;
    }

    // Kid sequence for finding details: the first kHead tokens of the flattened list (O(1)).
    std::string kid_text(const Summary& k) const {
        std::string out;
        for (int i = 0; i < k.head_len; ++i) {
            if (i) out += '&';
            out += T_.name(k.head[i]);
        }
        if (k.len > k.head_len) out += "&... (" + std::to_string(k.len) + " kids)";
        return out;
    }

    std::string label(int n, int f, bool is_root) {
        if (is_root) return "StructTreeRoot";
        std::string s;
        if (auto S = cos::name(dict_of(n), "S")) s = *S;
        else s = "(no /S name)";
        int tok = std_[n].tok;
        s += " [standard type ";
        s += tok >= 0 ? T_.name(tok) : std::string_view("null");
        s += f == 0 ? ", PDF 1.7 semantics]" : ", PDF 2.0 semantics]";
        return s;
    }

    void incomplete(const CRule& r, const char* reason) {
        if (incomplete_.insert(r.gen).second) sink_.incomplete(r.gen, reason);
    }

    void check(const CRule& r, int n, int f, bool is_root, const Summary* kids, int parent) {
        if (r.unsupported) {
            incomplete(r, "rule test not supported by the containment evaluator");
            return;
        }
        if (r.kids && kids->cycle) {
            incomplete(r, "pass-through structure elements (Part/Div/NonStruct) form a cycle: veraPDF's "
                          "kidsStandardTypes/hasContentItems do not terminate");
            return;
        }
        if (r.parent && parent == kCycle) {
            incomplete(r, "the /P chain loops through pass-through elements: veraPDF's parentStandardType does "
                          "not terminate");
            return;
        }
        int own = is_root ? -1 : std_[n].tok;
        static const Summary empty;
        static const bool clause = [] {
            const char* v = std::getenv("EQX_TS32005_CLAUSE");
            return v && *v == '1';
        }();
        if (eval_rule(r, C_.pool.ids.data(), kids ? *kids : empty, parent, own, T_, clause && !reference_dedup_)) return;
        std::string detail = label(n, f, is_root);
        if (r.kids) {
            detail += ": kids [" + kid_text(*kids) + "]";
            if (kids->content) detail += " + content items";
        }
        if (r.parent) {
            detail += ": parent standard type ";
            detail += parent >= 0 ? T_.name(parent) : std::string_view("null");
        }
        sink_.violation(r.gen, is_root ? st_.root_ref() : elems_[n].ref, std::move(detail));
    }

    // The rules of one group on a node, in table order.
    void check_group(const Group& g, int n, int f, const Summary& kids, int parent) {
        for (std::uint16_t i = g.first; i < g.last; ++i) {
            const CRule& r = C_.rules[C_.order[i]];
            // Nine rules in ten test nothing but "the parent is not X": such a rule passes unless the parent is X
            // (a /P chain that does not terminate goes through check(), which reports the rule incomplete).
            if (r.only_parent_ne && parent != kCycle && parent != r.atoms[0].tok) continue;
            check(r, n, f, false, &kids, parent);
        }
    }

    // Element n: the rules of every element, of its standard type, and the Hn rules; kidsStandardTypes and
    // parentStandardType are computed only when one of these rules reads them.
    void evaluate_element(int n, int f) {
        const int tok = std_[n].tok;
        const Group& star = C_.star[f];
        const Group* typed = tok >= 0 && tok < C_.types.n ? &C_.by_tok[f][tok] : nullptr;
        const Group* hn = tok >= 0 && (T_.flags(tok) & TF_HN) ? &C_.hn[f] : nullptr;
        const std::uint8_t needs = star.needs | (typed ? typed->needs : 0) | (hn ? hn->needs : 0);
        Summary kids;
        if (needs & NEED_KIDS) kids = node_summary(n);
        const int parent = (needs & NEED_PARENT) ? resolve_parent(cos::get(elems_[n].dict, "P"), f) : -1;
        check_group(star, n, f, kids, parent);
        if (typed) check_group(*typed, n, f, kids, parent);
        if (hn) check_group(*hn, n, f, kids, parent);
    }

    void evaluate_flavour(int f) {
        std_.assign(elems_.size() + 1, StdInfo{});
        std_memo_.clear();
        parent_memo_.clear();
        pst_state_.clear();
        pst_index_.clear();
        psums_.clear();
        for (std::size_t i = 0; i < elems_.size(); ++i) {
            const StructElem& e = elems_[i];
            StdInfo s;
            s.root_type = root_type_[i] != 0;
            if (f == 0) {
                if (e.std17) {
                    s.tok = T_.id(*e.std17);
                    s.name_pt = (T_.flags(s.tok) & TF_PT) != 0;
                }
            } else if (e.std20) {
                s.math = in_mathml_namespace(*e.std20);
                s.name_pt = (flags_of(e.std20->type) & TF_PT) != 0;
                s.tok = s.math ? C_.tok_mathml : T_.id(e.std20->type);
            }
            std_[i] = s;
        }

        // Elements whose rules are evaluated. Default: every element of the tree (each indirect element once,
        // every direct element). Reference mode: exactly the elements veraPDF validates (its direct-element id
        // "0 0 obj <S>" makes it skip all but one direct element per /S name, a veraPDF defect).
        if (reference_dedup_) {
            for (int n : st_.verapdf_order()) evaluate_element(n, f);
        } else {
            for (int n = 0; n < root_; ++n) evaluate_element(n, f);
        }
        const Group& root = C_.root[f];
        if (root.first != root.last) {
            Summary kids = node_summary(root_);
            for (std::uint16_t i = root.first; i < root.last; ++i) check(C_.rules[C_.order[i]], root_, f, true, &kids, -1);
        }
    }
};

}  // namespace

bool test_passes(const GenRule& rule, const ObjectView& v) {
    Types T;
    IdPool pool;
    CRule r = compile_rule(rule, 0, T, pool);
    Summary k;
    k.content = v.has_content_items;
    for (const auto& s : v.kids) {
        int t = T.id(s);
        k.add(t, T.flags(t));
    }
    int parent = v.parent_standard_type ? T.id(*v.parent_standard_type) : -1;
    int own = v.standard_type ? T.id(*v.standard_type) : -1;
    return eval_rule(r, pool.ids.data(), k, parent, own, T);
}

void evaluate(const StructTree& st, const Sink& sink, bool reference_dedup) {
    if (!st.present()) return;   // veraPDF creates no structure objects without a StructTreeRoot
    Engine e(st, sink, reference_dedup);
    e.run();
}

}  // namespace eqx::containment
