// Unit tests of the content-stream lexer. The parser hands the storage of an operator's strings and arrays to the
// operands of the next operators: nothing of an earlier operand may show in a later one.
#include <string>
#include <string_view>
#include <vector>

#include "core/lexer.h"
#include "unit.h"

using namespace eqx;

namespace {

// A readable form of an operand: type and value, elements in brackets.
std::string show(const Operand& o) {
    switch (o.type) {
        case Operand::Null: return "null";
        case Operand::Bool: return o.b ? "true" : "false";
        case Operand::Int: return "i" + std::to_string(o.i);
        case Operand::Real: return "r" + std::to_string(o.r);
        case Operand::Name: return "/" + o.s;
        case Operand::String: return "(" + o.s + ")";
        case Operand::Array:
        case Operand::Dict: {
            std::string s = o.type == Operand::Array ? "[" : "<<";
            for (const Operand& e : o.items) s += show(e) + " ";
            return s + (o.type == Operand::Array ? "]" : ">>");
        }
    }
    return "?";
}

// Every field an operand does not use must be as in a new operand.
bool clean(const Operand& o) {
    const bool has_string = o.type == Operand::Name || o.type == Operand::String;
    const bool has_items = o.type == Operand::Array || o.type == Operand::Dict;
    if (!has_string && !o.s.empty()) return false;
    if (!has_items && !o.items.empty()) return false;
    if (o.type != Operand::Bool && o.b) return false;
    if (o.type != Operand::Int && o.i != 0) return false;
    if (o.type != Operand::Real && o.r != 0) return false;
    for (const Operand& e : o.items)
        if (!clean(e)) return false;
    return true;
}

struct Parsed {
    std::vector<std::string> ops;      // "operand operand ... operator"
    std::vector<LexError> errors;
    bool all_clean = true;
};

Parsed parse(std::string_view data) {
    Parsed out;
    ContentParser p(data);
    RawOp op;
    while (p.next(op)) {
        std::string s;
        for (const Operand& o : *op.operands) {
            s += show(o) + " ";
            out.all_clean = out.all_clean && clean(o);
        }
        out.ops.push_back(s + std::string(op.op));
    }
    out.errors = p.errors();
    return out;
}

}  // namespace

EQX_TEST(content_lexer_literal_strings) {
    const Parsed p = parse("(plain) Tj (a\\(b\\)c\\\\d) Tj (nested (parens (deep)) end) Tj (\\101\\60\\7x\\400) Tj\n"
                           "(line\\\nbreak\\\r\ncr\rlf\r\nend\\q) Tj (tab\\tnl\\nret\\rbs\\bff\\f) Tj () Tj");
    EXPECT_EQ(p.ops.size(), std::size_t(7));
    EXPECT_EQ(p.ops[0], std::string("(plain) Tj"));
    EXPECT_EQ(p.ops[1], std::string("(a(b)c\\d) Tj"));
    EXPECT_EQ(p.ops[2], std::string("(nested (parens (deep)) end) Tj"));
    EXPECT_EQ(p.ops[3], std::string("(A0\ax") + '\0' + ") Tj");       // \400 keeps its low eight bits
    EXPECT_EQ(p.ops[4], std::string("(linebreakcr\nlf\nendq) Tj"));    // escaped line ends vanish, CR and CRLF read as LF
    EXPECT_EQ(p.ops[5], std::string("(tab\tnl\nret\rbs\bff\f) Tj"));
    EXPECT_EQ(p.ops[6], std::string("() Tj"));
    EXPECT(p.errors.empty());
    EXPECT(p.all_clean);

    const Parsed open = parse("(never closed \\");
    EXPECT(open.ops.empty());
    EXPECT_EQ(open.errors.size(), std::size_t(2));
    EXPECT_EQ(open.errors[0].message, std::string("unterminated literal string"));
    EXPECT_EQ(open.errors[0].offset, std::size_t(15));
    EXPECT_EQ(open.errors[1].message, std::string("operands at end of content stream without operator"));
}

EQX_TEST(content_lexer_hex_strings_names_numbers) {
    const Parsed p = parse("<48 65 6C6c 6F7> Tj <> Tj /A#42#2fC /D#zz /E# Do 12 -3 +4 1.5 -.25 4. 00012 0.000 99999999999999999999 w");
    EXPECT_EQ(p.ops.size(), std::size_t(4));
    EXPECT_EQ(p.ops[0], std::string("(Hellop) Tj"));                   // an odd last digit is the high half of a byte
    EXPECT_EQ(p.ops[1], std::string("() Tj"));
    EXPECT_EQ(p.ops[2], std::string("/AB/C /D#zz /E# Do"));
    EXPECT_EQ(p.ops[3], std::string("i12 i-3 i4 r1.500000 r-0.250000 r4.000000 i12 r0.000000 r100000000000000000000.000000 w"));
    EXPECT_EQ(p.errors.size(), std::size_t(2));
    EXPECT_EQ(p.errors[0].message, std::string("name with an invalid # escape (ISO 32000-1 7.3.5)"));
    EXPECT(p.all_clean);

    const Parsed bad = parse("<4Z1 Tj");
    EXPECT(bad.ops.empty());
    EXPECT_EQ(bad.errors.size(), std::size_t(5));   // Z, T, j, the missing ">", the operand left at the end
    EXPECT_EQ(bad.errors.back().message, std::string("operands at end of content stream without operator"));
}

EQX_TEST(content_lexer_storage_is_reused_without_traces) {
    // Each operator's operands differ in shape from those of the operator before it.
    const Parsed p = parse(
        "[(a long string, longer than any small-string buffer) -120.5 (second) [1 2 [3 4]] <</K /V /L [true null]>>] TJ\n"
        "[7] TJ\n"
        "[(x) 1 (y) 2 (z) 3 (w)] TJ\n"
        "[[9] /N <</A 1>> (s)] TJ\n"
        "(another string that is long enough to need the heap for its bytes) Tj\n"
        "(short) Tj\n"
        "/Span <</MCID 3 /Lang (en-US) /ActualText (some replacement text of some length)>> BDC\n"
        "/P <</MCID 4>> BDC\n"
        "[] TJ\n"
        "<<>> DP\n"
        "<</Only>> DP\n"
        "[true false null 1 2.5 /n (s) <41>] TJ\n"
        "1 0 0 1 72 720 Tm\n"
        "BT ET\n"
        "[(after the empty operators) 5] TJ");
    const std::vector<std::string> expect = {
        "[(a long string, longer than any small-string buffer) r-120.500000 (second) [i1 i2 [i3 i4 ] ] <</K /V /L [true null ] >> ] TJ",
        "[i7 ] TJ",
        "[(x) i1 (y) i2 (z) i3 (w) ] TJ",
        "[[i9 ] /N <</A i1 >> (s) ] TJ",
        "(another string that is long enough to need the heap for its bytes) Tj",
        "(short) Tj",
        "/Span <</MCID i3 /Lang (en-US) /ActualText (some replacement text of some length) >> BDC",
        "/P <</MCID i4 >> BDC",
        "[] TJ",
        "<<>> DP",
        "<</Only null >> DP",
        "[true false null i1 r2.500000 /n (s) (A) ] TJ",
        "i1 i0 i0 i1 i72 i720 Tm",
        "BT",
        "ET",
        "[(after the empty operators) i5 ] TJ",
    };
    EXPECT_EQ(p.ops.size(), expect.size());
    for (std::size_t i = 0; i < expect.size() && i < p.ops.size(); ++i) EXPECT_EQ(p.ops[i], expect[i]);
    EXPECT(p.all_clean);
    EXPECT_EQ(p.errors.size(), std::size_t(1));
    EXPECT_EQ(p.errors[0].message, std::string("dictionary key without value"));
}

EQX_TEST(content_lexer_damaged_operands) {
    // Unterminated containers, a key without value at the end of the data, stray delimiters.
    const Parsed a = parse("[(x) [1 2 TJ");
    EXPECT(a.ops.empty());
    EXPECT(!a.errors.empty());
    const Parsed b = parse("[(first array with a few elements) 1 2 3 4 5] TJ <</K");
    EXPECT_EQ(b.ops.size(), std::size_t(1));
    const Parsed c = parse("[1 2 3 4 5 6] TJ [ ) > } ] TJ /N <</A [1 2] /B>> BDC [(s)] TJ");
    EXPECT_EQ(c.ops.size(), std::size_t(4));
    EXPECT_EQ(c.ops[1], std::string("[null null null ] TJ"));
    EXPECT_EQ(c.ops[2], std::string("/N <</A [i1 i2 ] /B null >> BDC"));
    EXPECT_EQ(c.ops[3], std::string("[(s) ] TJ"));
    EXPECT(a.all_clean && b.all_clean && c.all_clean);
}

EQX_TEST(content_lexer_inline_image) {
    const std::string data = std::string("q BI /W 2 /H 1 /BPC 8 /CS /G /L 2 ID ") + "\x01\xff" + " EI Q (t) Tj";
    ContentParser p(data);
    RawOp op;
    EXPECT(p.next(op) && op.op == "q");
    EXPECT(p.next(op) && op.op == "EI" && op.code == Op::EI);
    EXPECT_EQ(op.inline_data.size(), std::size_t(2));
    EXPECT(op.operands->size() == 1 && op.operands->back().type == Operand::Dict && op.operands->back().items.size() == 10);
    const Operand* w = op.operands->back().get("W");
    EXPECT(w && w->type == Operand::Int && w->i == 2);
    EXPECT(p.next(op) && op.op == "Q" && op.operands->empty());
    EXPECT(p.next(op) && op.op == "Tj" && op.operands->size() == 1 && op.operands->back().s == "t");
    EXPECT(!p.next(op));
    EXPECT(p.errors().empty());
}
