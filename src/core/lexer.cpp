#include "core/lexer.h"

#include "util/unread.h"

#include <cstdlib>
#include <cstring>

namespace eqx {

namespace {

inline bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
inline bool is_delim(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}
inline bool is_regular(char c) { return !is_ws(c) && !is_delim(c); }
inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_number(std::string_view t, Operand& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    std::int64_t ip = 0;
    bool digits = false, dot = false, overflow = false;
    double frac = 0, scale = 0.1;
    for (; i < t.size(); ++i) {
        char c = t[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            if (!dot) {
                if (ip > (INT64_MAX - 9) / 10) overflow = true;
                else ip = ip * 10 + (c - '0');
            } else {
                frac += (c - '0') * scale;
                scale *= 0.1;
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits) return false;
    if (dot || overflow) {
        out.type = Operand::Real;
        out.r = (overflow ? std::strtod(std::string(t).c_str(), nullptr) : (static_cast<double>(ip) + frac));
        if (neg && !overflow) out.r = -out.r;
    } else {
        out.type = Operand::Int;
        out.i = neg ? -ip : ip;
    }
    return true;
}

}  // namespace

Op op_code(std::string_view o) {
    switch (o.size()) {
        case 1:
            switch (o[0]) {
                case 'b': return Op::b;
                case 'B': return Op::B;
                case 'c': return Op::c;
                case 'd': return Op::d;
                case 'f': return Op::f;
                case 'F': return Op::F;
                case 'G': return Op::G;
                case 'g': return Op::g;
                case 'h': return Op::h;
                case 'i': return Op::i;
                case 'j': return Op::j;
                case 'J': return Op::J;
                case 'K': return Op::K;
                case 'k': return Op::k;
                case 'l': return Op::l;
                case 'm': return Op::m;
                case 'M': return Op::M;
                case 'n': return Op::n;
                case 'q': return Op::q;
                case 'Q': return Op::Q;
                case 's': return Op::s;
                case 'S': return Op::S;
                case 'v': return Op::v;
                case 'w': return Op::w;
                case 'W': return Op::W;
                case 'y': return Op::y;
                case '\'': return Op::quote;
                case '"': return Op::dquote;
                default: return Op::Unknown;
            }
        case 2:
            switch (o[0]) {
                case 'b': return o[1] == '*' ? Op::b_star : Op::Unknown;
                case 'B':
                    return o[1] == '*' ? Op::B_star : o[1] == 'I' ? Op::BI : o[1] == 'T' ? Op::BT : o[1] == 'X' ? Op::BX
                                                                                                                : Op::Unknown;
                case 'c': return o[1] == 'm' ? Op::cm : o[1] == 's' ? Op::cs : Op::Unknown;
                case 'C': return o[1] == 'S' ? Op::CS : Op::Unknown;
                case 'd': return o[1] == '0' ? Op::d0 : o[1] == '1' ? Op::d1 : Op::Unknown;
                case 'D': return o[1] == 'o' ? Op::Do : o[1] == 'P' ? Op::DP : Op::Unknown;
                case 'E':
                    return o[1] == 'I' ? Op::EI : o[1] == 'T' ? Op::ET : o[1] == 'X' ? Op::EX : Op::Unknown;
                case 'f': return o[1] == '*' ? Op::f_star : Op::Unknown;
                case 'g': return o[1] == 's' ? Op::gs : Op::Unknown;
                case 'I': return o[1] == 'D' ? Op::ID : Op::Unknown;
                case 'M': return o[1] == 'P' ? Op::MP : Op::Unknown;
                case 'r':
                    return o[1] == 'e' ? Op::re : o[1] == 'g' ? Op::rg : o[1] == 'i' ? Op::ri : Op::Unknown;
                case 'R': return o[1] == 'G' ? Op::RG : Op::Unknown;
                case 's': return o[1] == 'c' ? Op::sc : o[1] == 'h' ? Op::sh : Op::Unknown;
                case 'S': return o[1] == 'C' ? Op::SC : Op::Unknown;
                case 'T':
                    switch (o[1]) {
                        case '*': return Op::T_star;
                        case 'c': return Op::Tc;
                        case 'd': return Op::Td;
                        case 'D': return Op::TD;
                        case 'f': return Op::Tf;
                        case 'j': return Op::Tj;
                        case 'J': return Op::TJ;
                        case 'L': return Op::TL;
                        case 'm': return Op::Tm;
                        case 'r': return Op::Tr;
                        case 's': return Op::Ts;
                        case 'w': return Op::Tw;
                        case 'z': return Op::Tz;
                        default: return Op::Unknown;
                    }
                case 'W': return o[1] == '*' ? Op::W_star : Op::Unknown;
                default: return Op::Unknown;
            }
        case 3:
            if (o == "BDC") return Op::BDC;
            if (o == "BMC") return Op::BMC;
            if (o == "EMC") return Op::EMC;
            if (o == "SCN") return Op::SCN;
            if (o == "scn") return Op::scn;
            return Op::Unknown;
        default:
            return Op::Unknown;
    }
}

const Operand* Operand::get(std::string_view key) const {
    if (type != Dict) return nullptr;
    for (std::size_t k = 0; k + 1 < items.size(); k += 2)
        if (items[k].type == Name && items[k].s == key) return &items[k + 1];
    return nullptr;
}

void ContentParser::error(const char* msg) {
    if (errors_.size() < 1000) errors_.push_back({p_, msg});
}

void ContentParser::skip_ws() {
    while (p_ < d_.size()) {
        char c = d_[p_];
        if (is_ws(c)) {
            ++p_;
        } else if (c == '%') {
            while (p_ < d_.size() && d_[p_] != '\n' && d_[p_] != '\r') ++p_;
        } else {
            break;
        }
    }
}

std::string ContentParser::read_literal_string() {
    // p_ at '('
    std::string s;
    ++p_;
    int depth = 1;
    while (p_ < d_.size()) {
        char c = d_[p_++];
        if (c == '(') {
            ++depth;
            s.push_back(c);
        } else if (c == ')') {
            if (--depth == 0) return s;
            s.push_back(c);
        } else if (c == '\\') {
            if (p_ >= d_.size()) break;
            char e = d_[p_++];
            switch (e) {
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case '(': case ')': case '\\': s.push_back(e); break;
                case '\r':
                    if (p_ < d_.size() && d_[p_] == '\n') ++p_;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && p_ < d_.size() && d_[p_] >= '0' && d_[p_] <= '7'; ++k)
                            v = v * 8 + (d_[p_++] - '0');
                        s.push_back(static_cast<char>(v & 0xFF));
                    } else {
                        s.push_back(e);
                    }
            }
        } else if (c == '\r') {
            if (p_ < d_.size() && d_[p_] == '\n') ++p_;
            s.push_back('\n');
        } else {
            s.push_back(c);
        }
    }
    error("unterminated literal string");
    return s;
}

std::string ContentParser::read_hex_string() {
    // p_ at '<' (not '<<')
    std::string s;
    ++p_;
    int hi = -1;
    while (p_ < d_.size()) {
        char c = d_[p_++];
        if (c == '>') {
            if (hi >= 0) s.push_back(static_cast<char>(hi << 4));
            return s;
        }
        if (is_ws(c)) continue;
        int v = hexval(c);
        if (v < 0) {
            error("invalid character in hexadecimal string");
            continue;
        }
        if (hi < 0) hi = v;
        else {
            s.push_back(static_cast<char>((hi << 4) | v));
            hi = -1;
        }
    }
    error("unterminated hexadecimal string");
    return s;
}

bool ContentParser::parse_object(Operand& out, int depth) {
    skip_ws();
    if (p_ >= d_.size()) return false;
    if (depth > 64) {
        error("operand nesting too deep");
        note_unread("an operand of a content stream is nested beyond 64 levels");
        return false;
    }
    char c = d_[p_];
    out.items.clear();
    out.s.clear();
    if (c == '/') {
        ++p_;
        out.type = Operand::Name;
        while (p_ < d_.size() && is_regular(d_[p_])) {
            char ch = d_[p_++];
            if (ch == '#' && p_ + 1 < d_.size() && hexval(d_[p_]) >= 0 && hexval(d_[p_ + 1]) >= 0) {
                out.s.push_back(static_cast<char>(hexval(d_[p_]) * 16 + hexval(d_[p_ + 1])));
                p_ += 2;
            } else {
                if (ch == '#') error("name with an invalid # escape (ISO 32000-1 7.3.5)");
                out.s.push_back(ch);
            }
        }
        return true;
    }
    if (c == '(') {
        out.type = Operand::String;
        out.s = read_literal_string();
        return true;
    }
    if (c == '<') {
        if (p_ + 1 < d_.size() && d_[p_ + 1] == '<') {
            p_ += 2;
            out.type = Operand::Dict;
            while (true) {
                skip_ws();
                if (p_ >= d_.size()) {
                    error("unterminated dictionary");
                    return true;
                }
                if (d_[p_] == '>' && p_ + 1 < d_.size() && d_[p_ + 1] == '>') {
                    p_ += 2;
                    return true;
                }
                std::size_t before = p_;
                if (!parse_object(out.items.emplace_back(), depth + 1)) {
                    out.items.pop_back();
                    return true;
                }
                if (out.items.back().type != Operand::Name) error("dictionary key is not a name");
                skip_ws();
                if (p_ < d_.size() && d_[p_] == '>' && p_ + 1 < d_.size() && d_[p_ + 1] == '>') {
                    error("dictionary key without value");
                    out.items.emplace_back();
                    continue;
                }
                if (!parse_object(out.items.emplace_back(), depth + 1)) {
                    out.items.back() = Operand{};   // keep key/value pairing
                    return true;
                }
                if (p_ == before) ++p_;   // no progress: avoid looping on garbage
            }
        }
        out.type = Operand::String;
        out.s = read_hex_string();
        return true;
    }
    if (c == '[') {
        ++p_;
        out.type = Operand::Array;
        while (true) {
            skip_ws();
            if (p_ >= d_.size()) {
                error("unterminated array");
                return true;
            }
            if (d_[p_] == ']') {
                ++p_;
                return true;
            }
            std::size_t before = p_;
            if (!parse_object(out.items.emplace_back(), depth + 1)) {
                out.items.pop_back();
                return true;
            }
            if (out.items.back().type == Operand::Null && p_ == before) ++p_;
        }
    }
    if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
        ++p_;
        error("unexpected delimiter");
        out.type = Operand::Null;
        return true;
    }
    // Regular token inside an array / dictionary: number, true, false, null (operators are an error).
    std::size_t b = p_;
    while (p_ < d_.size() && is_regular(d_[p_])) ++p_;
    std::string_view t = d_.substr(b, p_ - b);
    if (parse_number(t, out)) return true;
    if (t == "true" || t == "false") {
        out.type = Operand::Bool;
        out.b = t == "true";
        return true;
    }
    if (t == "null") {
        out.type = Operand::Null;
        return true;
    }
    error("unexpected token inside array or dictionary");
    out.type = Operand::Null;
    return true;
}

bool ContentParser::read_inline_image(RawOp& out) {
    // Parameters up to ID.
    stack_.clear();
    Operand params;
    params.type = Operand::Dict;
    while (true) {
        skip_ws();
        if (p_ >= d_.size()) {
            error("inline image without ID");
            note_unread("a content stream was not read to its end (inline image without ID)");
            return false;
        }
        if (d_[p_] != '/' ) {
            std::size_t b = p_;
            while (p_ < d_.size() && is_regular(d_[p_])) ++p_;
            std::string_view t = d_.substr(b, p_ - b);
            if (t == "ID") break;
            if (t.empty()) ++p_;
            error("unexpected token in inline image parameters");
            continue;
        }
        Operand k, v;
        parse_object(k, 1);
        if (!parse_object(v, 1)) break;
        params.items.push_back(std::move(k));
        params.items.push_back(std::move(v));
    }
    // A single white-space character follows ID.
    if (p_ < d_.size() && is_ws(d_[p_])) ++p_;
    std::size_t data_begin = p_;
    std::size_t data_end = std::string_view::npos;
    const Operand* len = params.get("L");
    if (!len) len = params.get("Length");
    if (len && len->type == Operand::Int && len->i >= 0 && data_begin + len->i <= d_.size()) {
        std::size_t q = data_begin + static_cast<std::size_t>(len->i);
        std::size_t r = q;
        while (r < d_.size() && is_ws(d_[r])) ++r;
        if (r + 1 < d_.size() + 1 && d_.substr(r, 2) == "EI" && (r + 2 >= d_.size() || !is_regular(d_[r + 2]))) {
            data_end = q;
            p_ = r + 2;
        }
    }
    if (data_end == std::string_view::npos) {
        // Search white space + "EI" + (white space | end | delimiter).
        std::size_t q = data_begin;
        while (true) {
            q = d_.find("EI", q);
            if (q == std::string_view::npos) break;
            bool before = q == data_begin || is_ws(d_[q - 1]);
            bool after = q + 2 >= d_.size() || is_ws(d_[q + 2]) || is_delim(d_[q + 2]);
            if (before && after) {
                data_end = q > data_begin ? q - 1 : q;
                p_ = q + 2;
                break;
            }
            q += 2;
        }
        if (data_end == std::string_view::npos) {
            error("inline image without EI");
            data_end = d_.size();
            p_ = d_.size();
        }
    }
    stack_.push_back(std::move(params));
    out.op = "EI";
    out.operands = &stack_;
    out.inline_data = d_.substr(data_begin, data_end - data_begin);
    return true;
}

bool ContentParser::next(RawOp& out) {
    stack_.clear();
    out.inline_data = {};
    while (true) {
        skip_ws();
        if (p_ >= d_.size()) {
            if (!stack_.empty()) error("operands at end of content stream without operator");
            return false;
        }
        char c = d_[p_];
        if (is_regular(c)) {
            std::size_t b = p_;
            while (p_ < d_.size() && is_regular(d_[p_])) ++p_;
            std::string_view t = d_.substr(b, p_ - b);
            Operand& o = stack_.emplace_back();
            if (parse_number(t, o)) continue;
            if (t == "true" || t == "false") {
                o.type = Operand::Bool;
                o.b = t == "true";
                continue;
            }
            if (t == "null") continue;
            stack_.pop_back();
            out.offset = b;
            if (t == "BI") {
                if (!stack_.empty()) error("operands before BI");
                if (!read_inline_image(out)) return false;
                out.code = Op::EI;
                return true;
            }
            out.op = t;
            out.code = op_code(t);
            out.operands = &stack_;
            return true;
        }
        std::size_t before = p_;
        Operand& o = stack_.emplace_back();
        if (!parse_object(o, 0)) {
            stack_.pop_back();
            if (p_ == before) ++p_;
            continue;
        }
        if (p_ == before) ++p_;
    }
}

}  // namespace eqx
