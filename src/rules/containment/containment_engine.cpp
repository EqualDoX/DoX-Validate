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
#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "rules/containment/containment.h"

namespace eqx::containment {

namespace {

const GenRule kRules[] = {
#include "rules/containment/containment_table.inc"
};

constexpr int kHead = 12;    // kid tokens kept verbatim (KIDS_EQ sequences are shorter; details)
constexpr int kCycle = -2;   // parentStandardType does not terminate

// ---- standard type names and their classes -----------------------------------------------------------

enum : std::uint8_t { TF_HN = 1, TF_INLINE = 2, TF_GROUPING = 4, TF_BOG = 8, TF_BGI = 16, TF_PT = 32 };

bool is_hn_name(std::string_view t) {
    // TaggedPDFConstants.HN_REGEXP ^H[1-9][0-9]*$
    if (t.size() < 2 || t[0] != 'H' || t[1] < '1' || t[1] > '9') return false;
    for (std::size_t i = 2; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

bool in_list(std::string_view t, std::initializer_list<std::string_view> l) {
    return std::find(l.begin(), l.end(), t) != l.end();
}

// TaggedPDFHelper inlineStructureTypes / groupingStructureTypes / blockOrGroupingStructureTypes /
// blockOrGroupingOrInlineStructureTypes; PDStructElem.isPassThroughTag.
std::uint8_t flags_of(std::string_view t) {
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

class Types {
public:
    int id(std::string_view s) {
        auto it = map_.find(std::string(s));
        if (it != map_.end()) return it->second;
        int i = static_cast<int>(names_.size());
        names_.emplace_back(s);
        flags_.push_back(flags_of(s));
        map_.emplace(std::string(s), i);
        return i;
    }
    int find(std::string_view s) const {
        auto it = map_.find(std::string(s));
        return it == map_.end() ? -1 : it->second;
    }
    std::uint8_t flags(int i) const { return i >= 0 && i < static_cast<int>(flags_.size()) ? flags_[i] : 0; }
    const std::string& name(int i) const { return names_[i]; }
    int size() const { return static_cast<int>(names_.size()); }

private:
    std::unordered_map<std::string, int> map_;
    std::vector<std::string> names_;
    std::vector<std::uint8_t> flags_;
};

// ---- compiled rules ------------------------------------------------------------------------------------

struct CAtom {
    AK kind = AK::GROUPING_IS;
    std::uint8_t pred = PRED_NONE;
    std::uint8_t cmp = CMP_EQ;
    int n = 0;
    int tok = -1;
    std::vector<int> set;   // distinct ids
    std::vector<int> seq;
};

struct CRule {
    std::size_t gen = 0;
    int flavour = 0;          // 0 = Pdf17, 1 = Pdf20
    enum Sel { TOK, HN, STAR, ROOT } sel = TOK;
    int sel_tok = -1;
    std::vector<CAtom> atoms;
    bool kids = false;        // uses kidsStandardTypes / hasContentItems / isGrouping
    bool parent = false;      // uses parentStandardType
    std::string error;        // non-empty: rule not supported by this evaluator
};

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = s.find(sep, start);
        out.emplace_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
    return out;
}

std::vector<int> id_set(Types& T, std::string_view arg) {
    std::vector<int> v;
    for (auto& s : split(arg, '|')) {
        int i = T.id(s);
        if (std::find(v.begin(), v.end(), i) == v.end()) v.push_back(i);
    }
    return v;
}

CRule compile_rule(const GenRule& g, std::size_t gen, Types& T) {
    CRule r;
    r.gen = gen;
    r.flavour = g.flavour == Flavor::Pdf17 ? 0 : 1;
    std::string_view obj = g.object;
    if (obj == "*") r.sel = CRule::STAR;
    else if (obj == "#root") r.sel = CRule::ROOT;
    else if (obj == "Hn") r.sel = CRule::HN;
    else r.sel_tok = T.id(obj);
    if (g.natoms == 0 || g.natoms > 3) r.error = "invalid atom count";
    for (int i = 0; i < g.natoms && i < 3; ++i) {
        const GenAtom& ga = g.atoms[i];
        CAtom a;
        a.kind = ga.kind;
        a.pred = ga.pred;
        a.cmp = ga.cmp;
        a.n = ga.n;
        std::string_view arg = ga.arg ? ga.arg : "";
        switch (ga.kind) {
            case AK::GROUPING_IS:
            case AK::NO_CONTENT_ITEMS:
                r.kids = true;
                break;
            case AK::PARENT_EQ:
            case AK::PARENT_NE:
                a.tok = T.id(arg);
                r.parent = true;
                break;
            case AK::PARENT_NOT_HN:
                r.parent = true;
                break;
            case AK::PARENT_IN:
                a.set = id_set(T, arg);
                r.parent = true;
                break;
            case AK::KID_COUNT:
                r.kids = true;
                if (ga.pred == PRED_EQ) a.tok = T.id(arg);
                else if (ga.pred == PRED_NOT_IN) a.set = id_set(T, arg);
                else if (ga.pred != PRED_HN) r.error = "invalid filter predicate";
                if (arg.empty() && ga.pred != PRED_HN) r.error = "empty filter literal";
                if (ga.cmp > CMP_GT) r.error = "invalid comparison";
                break;
            case AK::KIDS_EQ:
                r.kids = true;
                if (!arg.empty())
                    for (auto& s : split(arg, '&')) {
                        if (s.empty()) r.error = "empty token in kid sequence";
                        a.seq.push_back(T.id(s));
                    }
                if (a.seq.size() >= static_cast<std::size_t>(kHead)) r.error = "kid sequence too long";
                break;
            case AK::KIDS_NOT_CONTAINS:
                // Only '&X&' is supported: tokens never contain '&', so the substring occurs exactly when a
                // kid X is neither the first nor the last kid.
                r.kids = true;
                if (arg.size() < 3 || arg.front() != '&' || arg.back() != '&' ||
                    arg.substr(1, arg.size() - 2).find('&') != std::string_view::npos)
                    r.error = "unsupported indexOf literal";
                else
                    a.tok = T.id(arg.substr(1, arg.size() - 2));
                break;
            case AK::KIDS_NOT_AFTER_FIRST:
                r.kids = true;
                a.tok = T.id(arg);
                break;
            case AK::KIDS_ALL_IN:
                r.kids = true;
                a.set = id_set(T, arg);
                break;
            default:
                r.error = "unknown atom kind";
        }
        r.atoms.push_back(std::move(a));
    }
    return r;
}

struct Compiled {
    Types types;              // base interner (copied per document)
    std::vector<CRule> rules;
    int tok_root = -1, tok_mathml = -1;
    // per flavour
    std::vector<std::vector<int>> by_tok[2];
    std::vector<int> hn[2], star[2], root[2];
};

const Compiled& compiled() {
    static const Compiled c = [] {
        Compiled c;
        for (const char* t :
             {"Document", "Part", "Div", "Caption", "Sect", "NonStruct", "H", "P", "L", "LI", "Lbl", "LBody", "Table",
              "TR", "TH", "TD", "Span", "Link", "Figure", "Formula", "Form", "Art", "BlockQuote", "TOC", "TOCI",
              "Index", "Private", "Quote", "Note", "Reference", "BibEntry", "Code", "H1", "H2", "H3", "H4", "H5", "H6",
              "THead", "TBody", "TFoot", "Annot", "Ruby", "Warichu", "RB", "RT", "RP", "WT", "WP",
              "DocumentFragment", "Aside", "Title", "FENote", "Sub", "Em", "Strong", "Artifact"})
            c.types.id(t);
        c.tok_root = c.types.id("StructTreeRoot");
        c.tok_mathml = c.types.id("MathML");
        std::size_t n = sizeof(kRules) / sizeof(kRules[0]);
        for (std::size_t i = 0; i < n; ++i) c.rules.push_back(compile_rule(kRules[i], i, c.types));
        for (int f = 0; f < 2; ++f) c.by_tok[f].resize(c.types.size());
        for (std::size_t i = 0; i < c.rules.size(); ++i) {
            const CRule& r = c.rules[i];
            int f = r.flavour;
            switch (r.sel) {
                case CRule::TOK: c.by_tok[f][r.sel_tok].push_back(static_cast<int>(i)); break;
                case CRule::HN: c.hn[f].push_back(static_cast<int>(i)); break;
                case CRule::STAR: c.star[f].push_back(static_cast<int>(i)); break;
                case CRule::ROOT: c.root[f].push_back(static_cast<int>(i)); break;
            }
        }
        return c;
    }();
    return c;
}

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

// One atom with JavaScript semantics. `parent` = parentStandardType id (-1 = null).
bool eval_atom(const CAtom& a, const Summary& k, int parent, bool grouping, const Types& T, bool pass_through_kids) {
    switch (a.kind) {
        case AK::GROUPING_IS: return grouping == (a.n != 0);
        case AK::NO_CONTENT_ITEMS: return !k.content;
        case AK::PARENT_EQ: return parent >= 0 && parent == a.tok;
        case AK::PARENT_NE: return !(parent >= 0 && parent == a.tok);
        case AK::PARENT_NOT_HN: return !(parent >= 0 && (T.flags(parent) & TF_HN));
        case AK::PARENT_IN: return parent >= 0 && std::find(a.set.begin(), a.set.end(), parent) != a.set.end();
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
                for (int t : a.set) c -= k.count(t);
            }
            if (pass_through_kids && a.pred == PRED_EQ && (T.flags(a.tok) & TF_PT)) {
                const std::string& n = T.name(a.tok);
                if ((n == "Part" && k.has_part) || (n == "Div" && k.has_div)) ++c;
            }
            return compare(c, a.cmp, a.n);
        }
        case AK::KIDS_EQ:
            if (a.seq.empty()) return k.len == 0;
            if (k.len != static_cast<int>(a.seq.size())) return false;
            for (std::size_t i = 0; i < a.seq.size(); ++i)
                if (k.head[i] != a.seq[i]) return false;
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
            for (int t : a.set) c += k.count(t);
            return c == k.len;
        }
    }
    return false;
}

bool eval_rule(const CRule& r, const Summary& k, int parent, int own, const Types& T, bool pass_through_kids = false) {
    bool grouping = is_grouping(own, k, T);
    for (const CAtom& a : r.atoms)
        if (eval_atom(a, k, parent, grouping, T, pass_through_kids)) return true;
    return false;
}

// ---- document evaluation ---------------------------------------------------------------------------------

// One node of the evaluation: a structure element of the core tree (index = element index) or the
// StructTreeRoot (index = number of elements).
struct Node {
    Obj dict;
    ObjRef ref;
    bool own_content = false;            // MCID / MCR / OBJR directly in /K (TaggedPDFHelper.isContentItem)
    const std::vector<int>* kids = nullptr;   // every struct child in /K order, shared/revisited ones included
};

struct StdInfo {
    int tok = -1;             // token in kid lists / object type (-1 = null standard type)
    bool name_pt = false;     // standard type name is Part/Div/NonStruct (getparentStandardType loop)
    bool math = false;        // MathML namespace (Pdf20 only)
    bool root_type = false;   // /Type /StructTreeRoot
};

class Engine {
public:
    Engine(const StructTree& st, const Sink& sink, bool reference_dedup)
        : st_(st), sink_(sink), reference_dedup_(reference_dedup), C_(compiled()), T_(C_.types) {}

    void run() {
        build_nodes();
        for (int f = 0; f < 2; ++f) evaluate_flavour(f);
    }

private:
    const StructTree& st_;
    const Sink& sink_;
    bool reference_dedup_;
    const Compiled& C_;
    Types T_;
    std::vector<Node> nodes_;
    std::vector<int> root_kids_;
    int root_ = 0;
    std::vector<StdInfo> std_[2];
    std::unordered_map<std::uint64_t, StdInfo> std_memo_[2];   // /P targets
    std::unordered_map<std::uint64_t, int> parent_memo_[2];
    std::vector<int> pst_state_[2];                  // 0 new, 1 in progress, 2 done
    std::vector<int> pst_index_[2];
    std::vector<Summary> psums_[2];
    std::unordered_set<std::size_t> incomplete_;

    // -- tree (core StructTree: struct_kids = veraPDF getStructChildren, shared kids listed in every parent) --
    void build_nodes() {
        const auto& elems = st_.elems();
        nodes_.resize(elems.size() + 1);
        for (std::size_t i = 0; i < elems.size(); ++i) {
            const StructElem& e = elems[i];
            Node& n = nodes_[i];
            n.dict = e.dict;
            n.ref = e.ref;
            n.kids = &e.struct_kids;
            for (const StructKid& k : e.kids)
                if (k.kind != StructKid::Kind::Elem) n.own_content = true;
        }
        root_ = static_cast<int>(elems.size());
        Node& r = nodes_[root_];
        r.dict = st_.root();
        r.ref = st_.root_ref();
        for (const StructKid& k : st_.root_kids()) {
            if (k.kind != StructKid::Kind::Elem) r.own_content = true;
            else if (k.elem >= 0) root_kids_.push_back(k.elem);
        }
        r.kids = &root_kids_;
    }

    // Elements whose rules are evaluated. Default: every element of the tree (each indirect element once,
    // every direct element). Reference mode: exactly the elements veraPDF validates (its direct-element id
    // "0 0 obj <S>" makes it skip all but one direct element per /S name, a veraPDF defect).
    std::vector<int> evaluated() const {
        if (reference_dedup_) return st_.verapdf_order();
        std::vector<int> v(st_.elems().size());
        for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int>(i);
        return v;
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
                s.math = dt->ns_uri() == std::optional<std::string>(kNsMathML);
                s.name_pt = (flags_of(dt->type) & TF_PT) != 0;
                s.tok = s.math ? C_.tok_mathml : T_.id(dt->type);
            }
        }
        return s;
    }

    StdInfo std_of_obj(const Obj& d, int f) {
        ObjRef r = ref_of(d);
        if (!r.valid()) return compute_std(d, f);
        auto it = std_memo_[f].find(r.key());
        if (it != std_memo_[f].end()) return it->second;
        StdInfo s = compute_std(d, f);
        std_memo_[f].emplace(r.key(), s);
        return s;
    }

    // GFPDStructElem.getparentStandardType, starting at the element's /P value. -1 = null, kCycle = the
    // reference loops forever (pass-through /P cycle).
    int resolve_parent(Obj cur, int f) {
        std::vector<std::uint64_t> path;
        std::unordered_set<std::uint64_t> seen;
        int res = -1;
        for (;;) {
            if (cur.isNull()) { res = -1; break; }
            ObjRef r = ref_of(cur);
            if (r.valid()) {
                auto it = parent_memo_[f].find(r.key());
                if (it != parent_memo_[f].end()) { res = it->second; break; }
                if (!seen.insert(r.key()).second) { res = kCycle; break; }
                path.push_back(r.key());
            }
            StdInfo s = std_of_obj(cur, f);
            if (!s.name_pt) {
                res = s.root_type ? C_.tok_root : s.math ? C_.tok_mathml : s.tok;
                break;
            }
            cur = cos::get(cur, "P");
        }
        for (auto k : path) parent_memo_[f][k] = res;
        return res;
    }

    // -- kid summaries --
    bool tok_pt(int tok) const { return tok >= 0 && (T_.flags(tok) & TF_PT); }

    void note_pass_through(Summary& s, int tok) const {
        const std::string& n = T_.name(tok);
        if (n == "Part") s.has_part = true;
        else if (n == "Div") s.has_div = true;
    }

    // Summary of a pass-through node (memoised, iterative; cycles flagged).
    const Summary& pt_summary(int n, int f) {
        auto& state = pst_state_[f];
        auto& index = pst_index_[f];
        auto& sums = psums_[f];
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
            fr.sum.content = nodes_[node].own_content;
            stack.push_back(std::move(fr));
        };
        open(n);
        while (!stack.empty()) {
            std::size_t top = stack.size() - 1;
            int node = stack[top].node;
            const auto& kids = *nodes_[node].kids;
            if (stack[top].i < kids.size()) {
                int k = kids[stack[top].i++];
                int tk = std_[f][k].tok;
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
    Summary node_summary(int n, int f) {
        Summary s;
        s.content = nodes_[n].own_content;
        for (int k : *nodes_[n].kids) {
            int tk = std_[f][k].tok;
            if (tk < 0) continue;
            if (tok_pt(tk)) {
                note_pass_through(s, tk);
                s.append(pt_summary(k, f));
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
        if (auto S = cos::name(nodes_[n].dict, "S")) s = *S;
        else s = "(no /S name)";
        int tok = std_[f][n].tok;
        s += " [standard type ";
        s += tok >= 0 ? T_.name(tok) : std::string("null");
        s += f == 0 ? ", PDF 1.7 semantics]" : ", PDF 2.0 semantics]";
        return s;
    }

    void incomplete(const CRule& r, const char* reason) {
        if (incomplete_.insert(r.gen).second) sink_.incomplete(r.gen, reason);
    }

    void check(const CRule& r, int n, int f, bool is_root, const Summary* kids, int parent) {
        if (!r.error.empty()) {
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
        int own = is_root ? -1 : std_[f][n].tok;
        static const Summary empty;
        static const bool clause = [] {
            const char* v = std::getenv("EQX_TS32005_CLAUSE");
            return v && *v == '1';
        }();
        if (eval_rule(r, kids ? *kids : empty, parent, own, T_, clause && !reference_dedup_)) return;
        std::string detail = label(n, f, is_root);
        if (r.kids) {
            detail += ": kids [" + kid_text(*kids) + "]";
            if (kids->content) detail += " + content items";
        }
        if (r.parent) detail += ": parent standard type " + (parent >= 0 ? T_.name(parent) : std::string("null"));
        sink_.violation(r.gen, is_root ? st_.root_ref() : nodes_[n].ref, std::move(detail));
    }

    void evaluate_flavour(int f) {
        std_[f].resize(nodes_.size());
        const auto& elems = st_.elems();
        for (std::size_t i = 0; i < elems.size(); ++i) {
            const StructElem& e = elems[i];
            StdInfo s;
            s.root_type = cos::name_is(e.dict, "Type", "StructTreeRoot");
            if (f == 0) {
                if (e.std17) {
                    s.tok = T_.id(*e.std17);
                    s.name_pt = (T_.flags(s.tok) & TF_PT) != 0;
                }
            } else if (e.std20) {
                s.math = e.std20->ns_uri() == std::optional<std::string>(kNsMathML);
                s.name_pt = (flags_of(e.std20->type) & TF_PT) != 0;
                s.tok = s.math ? C_.tok_mathml : T_.id(e.std20->type);
            }
            std_[f][i] = s;
            if (e.ref.valid()) std_memo_[f].emplace(e.ref.key(), s);
        }
        std_[f][root_] = StdInfo{};
        pst_state_[f].assign(nodes_.size(), 0);
        pst_index_[f].assign(nodes_.size(), -1);

        std::vector<int> cand;
        for (int n : evaluated()) {
            int tok = std_[f][n].tok;
            cand.clear();
            cand.insert(cand.end(), C_.star[f].begin(), C_.star[f].end());
            if (tok >= 0 && tok < static_cast<int>(C_.by_tok[f].size()))
                cand.insert(cand.end(), C_.by_tok[f][tok].begin(), C_.by_tok[f][tok].end());
            if (tok >= 0 && (T_.flags(tok) & TF_HN)) cand.insert(cand.end(), C_.hn[f].begin(), C_.hn[f].end());
            bool need_kids = false, need_parent = false;
            for (int ri : cand) {
                need_kids |= C_.rules[ri].kids;
                need_parent |= C_.rules[ri].parent;
            }
            Summary kids;
            if (need_kids) kids = node_summary(n, f);
            int parent = need_parent ? resolve_parent(cos::get(nodes_[n].dict, "P"), f) : -1;
            for (int ri : cand) check(C_.rules[ri], n, f, false, &kids, parent);
        }
        if (!C_.root[f].empty()) {
            Summary kids = node_summary(root_, f);
            for (int ri : C_.root[f]) check(C_.rules[ri], root_, f, true, &kids, -1);
        }
    }
};

}  // namespace

const GenRule* generated_rules(std::size_t* n) {
    *n = sizeof(kRules) / sizeof(kRules[0]);
    return kRules;
}

int find_generated(std::string_view id) {
    static const std::unordered_map<std::string_view, int> index = [] {
        std::unordered_map<std::string_view, int> m;
        std::size_t n = sizeof(kRules) / sizeof(kRules[0]);
        for (std::size_t i = 0; i < n; ++i) m.emplace(kRules[i].id, static_cast<int>(i));
        return m;
    }();
    auto it = index.find(id);
    return it == index.end() ? -1 : it->second;
}

bool test_passes(const GenRule& rule, const ObjectView& v) {
    Types T = compiled().types;
    CRule r = compile_rule(rule, 0, T);
    Summary k;
    k.content = v.has_content_items;
    for (const auto& s : v.kids) {
        int t = T.id(s);
        k.add(t, T.flags(t));
    }
    int parent = v.parent_standard_type ? T.id(*v.parent_standard_type) : -1;
    int own = v.standard_type ? T.id(*v.standard_type) : -1;
    return eval_rule(r, k, parent, own, T);
}

void evaluate(const StructTree& st, const Sink& sink, bool reference_dedup) {
    if (!st.present()) return;   // veraPDF creates no structure objects without a StructTreeRoot
    Engine e(st, sink, reference_dedup);
    e.run();
}

}  // namespace eqx::containment
