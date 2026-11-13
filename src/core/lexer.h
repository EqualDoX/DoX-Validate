// Content-stream lexer and operand parser (ISO 32000-2 7.2, 7.8.2). Allocation-light: names and
// short strings use std::string's small-buffer storage; the operand stack is reused between operators.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eqx {

struct Operand {
    enum Type : std::uint8_t { Null, Bool, Int, Real, Name, String, Array, Dict };
    Type type = Null;
    bool b = false;
    std::int64_t i = 0;
    double r = 0;
    std::string s;                 // Name (without '/', #xx decoded) or String bytes
    std::vector<Operand> items;    // Array elements; Dict as key, value, key, value ... (keys are Names)

    bool is_number() const { return type == Int || type == Real; }
    double num() const { return type == Int ? static_cast<double>(i) : r; }
    // Dict lookup (nullptr when absent / not a dict).
    const Operand* get(std::string_view key) const;
};

// Content-stream operators of ISO 32000-2 Annex A (Unknown: anything else).
enum class Op : std::uint8_t {
    Unknown, b, B, b_star, B_star, BDC, BI, BMC, BT, BX, c, cm, CS, cs, d, d0, d1, Do, DP, EI, EMC, ET, EX,
    f, F, f_star, G, g, gs, h, i, ID, j, J, K, k, l, m, M, MP, n, q, Q, re, RG, rg, ri, s, S, SC, sc, SCN,
    scn, sh, T_star, Tc, Td, TD, Tf, Tj, TJ, TL, Tm, Tr, Ts, Tw, Tz, v, w, W, W_star, y, quote, dquote,
    Count
};
Op op_code(std::string_view op);

struct LexError {
    std::size_t offset;
    std::string message;
};

// One operator with its operands, as delivered by ContentParser::next.
struct RawOp {
    std::string_view op;              // operator keyword (points into the stream data)
    Op code = Op::Unknown;            // op_code(op)
    std::vector<Operand>* operands;   // operands in order
    std::size_t offset = 0;           // byte offset of the operator
    // Inline images: BI operands are the parameter dictionary entries; ID ... EI is delivered as one
    // operator "EI" whose operands are [Dict params] and whose data is in inline_data.
    std::string_view inline_data;
};

class ContentParser {
public:
    explicit ContentParser(std::string_view data) : d_(data) {}
    // Next operator; false at end of data.
    bool next(RawOp& out);
    const std::vector<LexError>& errors() const { return errors_; }

private:
    bool parse_object(Operand& out, int depth);
    bool read_token(std::string_view& tok, char& kind);
    void skip_ws();
    std::string read_literal_string();
    std::string read_hex_string();
    void error(const char* msg);
    bool read_inline_image(RawOp& out);

    std::string_view d_;
    std::size_t p_ = 0;
    std::vector<Operand> stack_;
    std::vector<LexError> errors_;
};

}  // namespace eqx
