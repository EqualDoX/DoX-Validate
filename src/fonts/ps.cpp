// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/ (also: LICENSES/MPL-2.0.txt).
//
// This file is a C++ translation of parts of veraPDF 1.30.2 (veraPDF Parser and veraPDF Validation,
// https://github.com/veraPDF), Copyright (c) 2015-2026, veraPDF Consortium <info@verapdf.org>. veraPDF is offered
// under GPL-3.0-or-later or MPL-2.0-or-later; this file is used and distributed under MPL-2.0.
// Translation and modifications: Copyright (c) 2026 EqualDoX (https://equaldox.de).
// The EqualDoX Attribution License does not apply to this file (THIRD_PARTY_NOTICES.md).
//
#include "fonts/ps.h"

#include <cmath>
#include <limits>

#include "fonts/util.h"

namespace eqx::ps {

namespace {

constexpr std::size_t kMaxStringLength = 65535;   // BaseParser.MAX_STRING_LENGTH
constexpr int kMaxArrayDepth = 256;               // Java would overflow its stack far later; we stop here
constexpr int kMaxExecDepth = 256;
constexpr std::int64_t kMaxPsArraySize = 1 << 16; // PSOperator.MAX_PS_ARRAY_SIZE
constexpr std::int64_t kMaxForIterations = 10'000;

int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 127;   // COSFilterASCIIHexDecode.ER
}

KW keyword_of(const std::string& v) {
    if (v == "null") return KW::Null;
    if (v == "true") return KW::True;
    if (v == "false") return KW::False;
    if (v == "stream") return KW::Stream;
    if (v == "endstream") return KW::EndStream;
    if (v == "obj") return KW::Obj;
    if (v == "endobj") return KW::EndObj;
    if (v == "R") return KW::R;
    if (v == "n") return KW::N;
    if (v == "f") return KW::F;
    if (v == "xref") return KW::Xref;
    if (v == "startxref") return KW::StartXref;
    if (v == "trailer") return KW::Trailer;
    return KW::Null_;
}

}  // namespace

bool is_space(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }

bool is_delimiter(int c) {
    if (is_space(c)) return true;
    switch (c) {
        case '%': case '(': case ')': case '/': case '<': case '>': case '[': case ']': case '{': case '}':
            return true;
        default:
            return false;
    }
}

// --- Lexer -------------------------------------------------------------------------------------------
bool Lexer::skip_single_space(bool skip_comment_flag) {
    if (src_.eof()) return false;
    int ch = src_.read_byte();
    if (is_space(ch)) return true;
    if (ch == '%' && skip_comment_flag) {
        skip_comment();
        return true;
    }
    src_.unread();
    return false;
}

void Lexer::skip_spaces(bool skip_comment_flag) {
    while (skip_single_space(skip_comment_flag)) {
    }
}

void Lexer::skip_comment() {
    if (comments_ == Comments::kSeekable) {
        // SeekableBaseParser.skipComment / isEOL
        while (!src_.eof()) {
            int ch = src_.read_byte();
            if (ch == 10) return;
            if (ch == 13) {
                int c2 = src_.read_byte();
                if (c2 != 10) src_.unread();
                return;
            }
        }
        return;
    }
    while (!src_.eof()) {
        int ch = src_.read_byte();
        if (ch == 10) return;
        bool end = ch == 13 || (comments_ == Comments::kNotSeekableCMap && ch == 12);
        if (end) {
            int c2 = src_.read_byte();
            if (c2 != 10) src_.unread();
            return;
        }
    }
}

void Lexer::read_token() {
    tok_.value.clear();
    while (!src_.eof()) {
        int ch = src_.read_byte();
        if (is_delimiter(ch)) {
            src_.unread();
            break;
        }
        tok_.value.push_back(static_cast<char>(ch));
    }
}

void Lexer::read_name() {
    tok_.value.clear();
    while (!src_.eof()) {
        int ch = src_.read_byte();
        if (is_delimiter(ch)) {
            src_.unread();
            break;
        }
        if (ch == '#' && !ps_ && !t1names_) {
            int ch1 = src_.read_byte();
            if (!src_.eof() && hex_value(ch1) != 127) {
                int dc = hex_value(ch1);
                int ch2 = src_.read_byte();
                if (!src_.eof() && hex_value(ch2) != 127) {
                    tok_.value.push_back(static_cast<char>((dc << 4) + hex_value(ch2)));
                } else {
                    tok_.value.push_back(static_cast<char>(ch));
                    tok_.value.push_back(static_cast<char>(ch1));
                    src_.unread();
                }
            } else {
                tok_.value.push_back(static_cast<char>(ch));
                src_.unread();
            }
        } else {
            tok_.value.push_back(static_cast<char>(ch));
        }
    }
}

void Lexer::read_number() {
    tok_.value.clear();
    tok_.type = TT::Integer;
    int radix = 10;
    bool failed = false;
    while (!src_.eof()) {
        int ch = src_.read_byte();
        if (is_delimiter(ch)) {
            src_.unread();
            break;
        }
        if (ch >= '0' && ch <= '9') {
            tok_.value.push_back(static_cast<char>(ch));
        } else if (ch == '.') {
            tok_.type = TT::Real;
            tok_.value.push_back('.');
        } else if (ch == '#' && ps_) {
            if (tok_.type == TT::Integer) {
                // Integer.parseInt(value): NumberFormatException ends the number
                auto v = java_parse_long(tok_.value, 10);
                if (!v || *v > std::numeric_limits<std::int32_t>::max()) {
                    failed = true;
                    break;
                }
                radix = static_cast<int>(*v);
            }
            tok_.value.clear();
        } else {
            src_.unread();
            break;
        }
    }
    if (!failed) {
        if (tok_.type == TT::Integer) {
            auto v = java_parse_long(tok_.value, radix);
            if (v) {
                tok_.integer = *v;
                tok_.real = static_cast<double>(*v);
                return;
            }
        } else {
            auto v = java_parse_double_digits(tok_.value);
            if (v) {
                tok_.integer = java_round(*v);
                tok_.real = *v;
                return;
            }
        }
    }
    // NumberFormatException
    tok_.integer = std::numeric_limits<std::int64_t>::max();
    tok_.real = std::numeric_limits<double>::max();
}

void Lexer::read_hex_string() {
    tok_.value.clear();
    int uc = 0;
    bool odd = false;
    while (!src_.eof()) {
        int ch = src_.read_byte();
        if (ch == '>') {
            if (odd) tok_.value.push_back(static_cast<char>(uc << 4));
            return;
        }
        if (!is_space(ch)) {
            int hex = hex_value(ch);
            if (hex < 16) {
                if (odd) {
                    tok_.value.push_back(static_cast<char>((uc << 4) + hex));
                    uc = 0;
                } else {
                    uc = hex;
                }
                odd = !odd;
            }
        }
    }
}

void Lexer::read_lit_string() {
    tok_.value.clear();
    int depth = 0;
    bool append = true;
    auto put = [&](int c) {
        if (append) tok_.value.push_back(static_cast<char>(c));
    };
    int ch = src_.read_byte();
    while (!src_.eof()) {
        switch (ch) {
            default: put(ch); break;
            case '(':
                ++depth;
                put(ch);
                break;
            case ')':
                if (depth == 0) return;
                --depth;
                put(ch);
                break;
            case '\\': {
                ch = src_.read_byte();
                switch (ch) {
                    case '(': put('('); break;
                    case ')': put(')'); break;
                    case 'n': put(10); break;
                    case 'r': put(13); break;
                    case 't': put(9); break;
                    case 'b': put(8); break;
                    case 'f': put(12); break;
                    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                        if (!ps_) {
                            int v = ch - '0';
                            for (int i = 1; i < 3; ++i) {
                                ch = src_.read_byte();
                                if (ch < '0' || ch > '7') {
                                    src_.unread();
                                    break;
                                }
                                v = ((v << 3) + (ch - '0')) & 0xFFFF;
                            }
                            put(v & 0xFF);
                        }
                        break;
                    case 10: break;
                    case 13: {
                        int c2 = src_.read_byte();
                        if (c2 != 10) src_.unread();
                        break;
                    }
                    default: put(ch); break;
                }
                break;
            }
        }
        ch = src_.read_byte();
        if (append && tok_.value.size() > kMaxStringLength) append = false;
    }
}

void Lexer::next_token() {
    skip_spaces(true);
    if (src_.eof()) {
        tok_.type = TT::Eof;
        return;
    }
    tok_.type = TT::None;
    int ch = src_.read_byte();
    switch (ch) {
        case '(':
            tok_.type = TT::LitString;
            read_lit_string();
            break;
        case ')':
            break;
        case '<': {
            int c2 = src_.read_byte();
            if (c2 == '<') {
                tok_.type = TT::OpenDict;
            } else if (c2 == '~') {
                tok_.type = TT::HexString;
                // ASCII85 string: decoded leniently (veraPDF COSFilterASCII85Decode).
                std::string raw;
                int b = src_.read_byte();
                while (!src_.eof() && (b != '~' || src_.peek() != '>')) {
                    raw.push_back(static_cast<char>(b));
                    b = src_.read_byte();
                }
                src_.read_byte();
                tok_.value = ascii85_decode(raw);
            } else {
                src_.unread();
                tok_.type = TT::HexString;
                read_hex_string();
            }
            break;
        }
        case '>': {
            int c2 = src_.read_byte();
            if (c2 == '>') tok_.type = TT::CloseDict;
            else throw Error("Unknown symbol after '>'");
            break;
        }
        case '[': tok_.type = TT::OpenArray; break;
        case ']': tok_.type = TT::CloseArray; break;
        case '{':
            if (ps_) tok_.type = TT::StartProc;
            break;
        case '}':
            if (ps_) tok_.type = TT::EndProc;
            break;
        case '/':
            tok_.type = TT::Name;
            read_name();
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '.':
            src_.unread();
            read_number();
            break;
        case '+':
            read_number();
            break;
        case '-':
            read_number();
            tok_.integer = tok_.integer == std::numeric_limits<std::int64_t>::min()
                               ? tok_.integer
                               : -tok_.integer;
            tok_.real = -tok_.real;
            break;
        default:
            src_.unread();
            read_token();
            tok_.type = TT::Keyword;
            tok_.keyword = keyword_of(tok_.value);
            break;
    }
}

// --- ObjectParser (COSParser.nextObject) --------------------------------------------------------------
Obj ObjectParser::next_object() { return next_object_depth(0); }

Obj ObjectParser::next_object_depth(int depth) {
    for (;;) {
        if (!objects_.empty()) {
            Obj r = std::move(objects_.front());
            objects_.pop_front();
            return r;
        }
        if (flag_) lx_.next_token();
        flag_ = true;
        Token& tok = lx_.token();
        if (tok.type == TT::Integer) {   // looking for indirect reference
            integers_.push_back(tok.integer);
            if (integers_.size() == 3) {
                Obj r = Obj::make_int(integers_.front());
                integers_.pop_front();
                return r;
            }
            continue;   // return nextObject()
        }
        if (tok.type == TT::Keyword && tok.keyword == KW::R && integers_.size() == 2) {
            integers_.clear();
            Obj r;
            r.t = Obj::T::Null;   // COSIndirect: no object in a font/CMap program
            return r;
        }
        if (!integers_.empty()) {
            Obj r = Obj::make_int(integers_.front());
            integers_.pop_front();
            while (!integers_.empty()) {
                objects_.push_back(Obj::make_int(integers_.front()));
                integers_.pop_front();
            }
            flag_ = false;
            return r;
        }
        switch (tok.type) {
            case TT::Keyword:
                if (tok.keyword == KW::Null_) {
                    if (lx_.ps_mode()) return Obj::make_op(tok.value);
                    return Obj{};
                }
                if (tok.keyword == KW::Null) {
                    Obj r;
                    r.t = Obj::T::Null;
                    return r;
                }
                if (tok.keyword == KW::True) return Obj::make_bool(true);
                if (tok.keyword == KW::False) return Obj::make_bool(false);
                return Obj{};
            case TT::Real: return Obj::make_real(tok.real);
            case TT::LitString:
            case TT::HexString: return Obj::make_string(tok.value);
            case TT::Name: return Obj::make_name(tok.value);
            case TT::OpenArray:
                flag_ = false;
                return get_array(depth + 1);
            case TT::CloseArray: return Obj{};
            case TT::OpenDict:
                if (lx_.ps_mode()) {   // PSParser.getDictionary: '<<' is the name "<<"
                    flag_ = true;
                    return Obj::make_name("<<");
                }
                throw Error("dictionary in a PostScript program");
            case TT::CloseDict:
                if (lx_.ps_mode()) return Obj::make_name(">>");
                return Obj{};
            case TT::Eof: return Obj{};
            case TT::StartProc: {
                flag_ = false;
                Obj a = get_array(depth + 1);
                if (a.t == Obj::T::Array) a.t = Obj::T::Proc;
                return a;
            }
            default: return Obj{};
        }
    }
}

Obj ObjectParser::get_array(int depth) {
    if (depth > kMaxArrayDepth) throw Error("arrays nested too deeply");
    if (flag_) lx_.next_token();
    flag_ = true;
    Token& tok = lx_.token();
    if (tok.type != TT::OpenArray && !(lx_.ps_mode() && tok.type == TT::StartProc)) return Obj{};
    Obj arr;
    arr.t = Obj::T::Array;
    arr.a = std::make_shared<ObjVec>();
    Obj o = next_object_depth(depth);
    while (!o.empty()) {
        arr.a->push_back(std::move(o));
        o = next_object_depth(depth);
    }
    if (tok.type != TT::CloseArray && !(lx_.ps_mode() && tok.type == TT::EndProc)) throw Error("invalid array");
    return arr;
}

// --- Interpreter ---------------------------------------------------------------------------------------
Obj Interpreter::pop() {
    if (stack.empty()) throw Error("empty operand stack");
    Obj o = std::move(stack.back());
    stack.pop_back();
    return o;
}

Obj& Interpreter::peek() {
    if (stack.empty()) throw Error("empty operand stack");
    return stack.back();
}

Obj Interpreter::top_number() {
    Obj o = pop();
    if (!o.is_number()) throw Error("top element is not a number");
    return o;
}

namespace {

Obj ps_copy(const Obj& o) {
    // PSOperator.psCopyObject: scalars are copied, names lose their operator nature (COSName.construct).
    if (o.t == Obj::T::Operator) return Obj::make_name(o.s);
    return o;
}

}  // namespace

void Interpreter::execute(const Obj& o) {
    switch (o.t) {
        case Obj::T::Operator: execute_operator(o.s); break;
        case Obj::T::Empty:
        case Obj::T::Mark: break;   // PSLiteralObject of an empty object pushes nothing
        default: stack.push_back(o); break;
    }
}

void Interpreter::execute_procedure(const Obj& proc) {
    if (!proc.a) return;
    ObjVec items = *proc.a;   // the procedure may modify itself through put
    for (const Obj& o : items) execute(o);
}

void Interpreter::modified_execute_procedure(const Obj& proc) {
    if (!proc.a) return;
    ObjVec items = *proc.a;
    for (const Obj& o : items) {
        if (o.t == Obj::T::Operator) execute_operator(o.s);
        else stack.push_back(o);
    }
}

void Interpreter::execute_operator(const std::string& op) {
    struct Guard {
        int& d;
        explicit Guard(int& x) : d(x) {
            if (++d > kMaxExecDepth) {
                --d;
                throw Error("PostScript execution nested too deeply");
            }
        }
        ~Guard() { --d; }
    } guard(depth_);
    auto two_numbers = [&](Obj& a, Obj& b) {
        if (stack.size() > 1) {
            a = pop();
            if (a.is_number()) {
                b = pop();
                if (b.is_number()) return;
            }
        }
        throw Error("top two elements are not two numbers");
    };
    auto two_bools = [&](Obj& a, Obj& b) {
        if (stack.size() > 1) {
            a = pop();
            if (a.t == Obj::T::Bool) {
                b = pop();
                if (b.t == Obj::T::Bool) return;
            }
        }
        throw Error("top two elements are not two booleans");
    };
    if (op == "{" || op == "}") return;
    if (op == "if") {
        if (stack.size() < 2) throw Error("No procedures for if operator");
        Obj proc = pop();
        Obj b = pop();
        if (proc.t != Obj::T::Proc || b.t != Obj::T::Bool) throw Error("Can't execute if operator");
        if (b.b) modified_execute_procedure(proc);
        return;
    }
    if (op == "ifelse") {
        if (stack.size() < 3) throw Error("No procedures for ifelse operator");
        Obj fp = pop();
        Obj tp = pop();
        Obj b = pop();
        if (fp.t != Obj::T::Proc || tp.t != Obj::T::Proc || b.t != Obj::T::Bool)
            throw Error("Can't execute ifelse operator");
        modified_execute_procedure(b.b ? tp : fp);
        return;
    }
    if (op == "dup") {
        Obj c = ps_copy(peek());
        stack.push_back(std::move(c));
        return;
    }
    if (op == "exch") {
        Obj a = pop();
        Obj b = pop();
        stack.push_back(std::move(a));
        stack.push_back(std::move(b));
        return;
    }
    if (op == "pop") {
        pop();
        return;
    }
    if (op == "copy") {
        std::int64_t n = top_number().integer();
        std::int64_t size = static_cast<std::int64_t>(stack.size());
        if (size < n) throw Error("Stack does not contain enough elements to copy");
        if (n < 0) throw Error("negative copy count");
        std::vector<Obj> add;
        for (std::int64_t k = size - n; k < size; ++k) add.push_back(ps_copy(stack[static_cast<std::size_t>(k)]));
        for (auto& o : add) stack.push_back(std::move(o));
        return;
    }
    if (op == "index") {
        std::int64_t n = static_cast<std::int32_t>(top_number().integer());
        std::int64_t size = static_cast<std::int64_t>(stack.size());
        if (size < n) throw Error("Stack does not contain n elements");
        std::int64_t idx = size - n - 1;
        if (idx < 0 || idx >= size) throw Error("index out of bounds");
        stack.push_back(ps_copy(stack[static_cast<std::size_t>(idx)]));
        return;
    }
    if (op == "roll") {
        Obj a, b;
        two_numbers(a, b);
        std::int32_t j = static_cast<std::int32_t>(a.integer());
        std::int32_t n = static_cast<std::int32_t>(b.integer());
        if (j < 0) {
            if (n == 0) throw Error("division by zero in roll");
            j = n - std::abs(j) % n;
        }
        std::int64_t size = static_cast<std::int64_t>(stack.size());
        if (size < n) throw Error("Stack has less than n elements");
        std::vector<Obj> last;
        for (std::int32_t k = 0; k < n; ++k) last.push_back(pop());
        if (n == 0) throw Error("division by zero in roll");
        std::int32_t split = (j - 1) % n;
        for (std::int32_t k = split; k >= 0; --k) {
            if (k >= static_cast<std::int32_t>(last.size())) throw Error("roll index out of bounds");
            stack.push_back(last[static_cast<std::size_t>(k)]);
        }
        for (std::int32_t k = n - 1; k > split; --k) stack.push_back(last[static_cast<std::size_t>(k)]);
        return;
    }
    if (op == "clear") {
        stack.clear();
        return;
    }
    if (op == "count") {
        stack.push_back(Obj::make_int(static_cast<std::int64_t>(stack.size())));
        return;
    }
    if (op == "mark" || op == "<<") {
        Obj m;
        m.t = Obj::T::Mark;
        stack.push_back(m);
        return;
    }
    if (op == "cleartomark") {
        const Obj* top = &peek();
        while (!stack.empty() && top->t != Obj::T::Mark) {
            stack.pop_back();
            top = &peek();
        }
        return;
    }
    if (op == "counttomark") {
        std::int64_t res = 0;
        for (std::int64_t k = static_cast<std::int64_t>(stack.size()) - 1; k >= 0; --k) {
            if (stack[static_cast<std::size_t>(k)].t == Obj::T::Mark || k == 0) {
                stack.push_back(Obj::make_int(res));
                return;
            }
            ++res;
        }
        return;
    }
    if (op == "and" || op == "or" || op == "xor") {
        Obj a, b;
        two_bools(a, b);
        bool r = op == "and" ? (b.b && a.b) : op == "or" ? (b.b || a.b) : (b.b != a.b);
        stack.push_back(Obj::make_bool(r));
        return;
    }
    if (op == "not") {
        Obj a = pop();
        if (a.t != Obj::T::Bool) throw Error("top element is not a boolean");
        stack.push_back(Obj::make_bool(!a.b));
        return;
    }
    if (op == "true" || op == "false") {
        stack.push_back(Obj::make_bool(op == "true"));
        return;
    }
    if (op == "bitshift" || op == "abs" || op == "neg" || op == "ceiling" || op == "floor" || op == "round" ||
        op == "truncate" || op == "sqrt" || op == "cos" || op == "atan" || op == "sin" || op == "exp" ||
        op == "cvi" || op == "cvr" || op == "ln" || op == "log") {
        Obj a = top_number();
        double v = a.real();
        Obj r;
        if (op == "abs") r = Obj::make_real(std::fabs(v));
        else if (op == "neg") r = Obj::make_real(-v);
        else if (op == "ceiling") r = Obj::make_int(java_d2l(std::ceil(v)));
        else if (op == "floor") r = Obj::make_int(java_d2l(std::floor(v)));
        else if (op == "round") r = Obj::make_int(java_round(v));
        else if (op == "truncate") r = Obj::make_int(java_d2l(v));
        else if (op == "sqrt") r = Obj::make_real(std::sqrt(v));
        else if (op == "cos") r = Obj::make_real(std::cos(v));
        else if (op == "atan") r = Obj::make_real(std::atan(v));
        else if (op == "sin") r = Obj::make_real(std::sin(v));
        else if (op == "exp") r = Obj::make_real(std::exp(v));
        else if (op == "cvi") r = Obj::make_int(java_d2i(v));
        else if (op == "cvr") r = Obj::make_real(v);
        else if (op == "ln") r = Obj::make_real(std::log(v));
        else if (op == "log") r = Obj::make_real(std::log10(v));
        else r = Obj::make_int(a.integer() >> 1);   // bitshift (sic)
        stack.push_back(std::move(r));
        return;
    }
    if (op == "add" || op == "div" || op == "idiv" || op == "mod" || op == "mul" || op == "sub" || op == "eq" ||
        op == "ne" || op == "gt" || op == "ge" || op == "lt" || op == "le") {
        Obj a, b;
        two_numbers(a, b);
        double x = b.real(), y = a.real();
        Obj r;
        if (op == "add") r = Obj::make_real(x + y);
        else if (op == "div") r = Obj::make_real(x / y);
        else if (op == "mul") r = Obj::make_real(x * y);
        else if (op == "sub") r = Obj::make_real(x - y);
        else if (op == "idiv" || op == "mod") {
            std::int64_t p = b.integer(), q = a.integer();
            if (q == 0) throw Error("division by zero");
            if (p == std::numeric_limits<std::int64_t>::min() && q == -1) r = Obj::make_int(op == "idiv" ? p : 0);
            else r = Obj::make_int(op == "idiv" ? p / q : p % q);
        } else if (op == "eq") r = Obj::make_bool(java_double_equals(x, y));
        else if (op == "ne") r = Obj::make_bool(!java_double_equals(x, y));
        else if (op == "gt") r = Obj::make_bool(x > y);
        else if (op == "ge") r = Obj::make_bool(x >= y);
        else if (op == "lt") r = Obj::make_bool(x < y);
        else r = Obj::make_bool(x <= y);
        stack.push_back(std::move(r));
        return;
    }
    if (op == "dict") {
        top_number();
        Obj d;
        d.t = Obj::T::Dict;
        d.d = std::make_shared<ObjMap>();
        stack.push_back(std::move(d));
        return;
    }
    if (op == "begin") {
        if (!stack.empty()) stack.pop_back();
        return;
    }
    if (op == "length") {
        Obj o = pop();
        if (o.t == Obj::T::Dict) {
            stack.push_back(Obj::make_int(static_cast<std::int64_t>(o.d ? o.d->size() : 0)));
            return;
        }
        if (o.is_array_type()) {
            stack.push_back(Obj::make_int(static_cast<std::int64_t>(o.a ? o.a->size() : 0)));
            return;
        }
        throw Error("Can't execute length operator");
    }
    if (op == "def") {
        if (stack.size() > 1) {
            Obj value = pop();
            Obj key = pop();
            if (key.is_name_type() || key.t == Obj::T::String) user_dict[key.s] = std::move(value);
        }
        return;
    }
    if (op == "load") {
        Obj key = pop();
        if (key.is_name_type() || key.t == Obj::T::String) {
            auto it = user_dict.find(key.s);
            if (it != user_dict.end()) stack.push_back(it->second);
            return;
        }
        throw Error("Can't execute load operator");
    }
    if (op == ">>") {
        Obj d;
        d.t = Obj::T::Dict;
        d.d = std::make_shared<ObjMap>();
        while (stack.size() > 1) {
            Obj value = pop();
            if (value.t == Obj::T::Mark) {
                stack.push_back(std::move(d));
                return;
            }
            Obj key = pop();
            if (key.t == Obj::T::Mark) throw Error("Odd number of arguments between << and >>");
            if (key.is_name_type() || key.t == Obj::T::String) (*d.d)[key.s] = std::move(value);
        }
        if (!stack.empty()) {
            Obj m = pop();
            if (m.t == Obj::T::Mark) {
                stack.push_back(std::move(d));
                return;
            }
        }
        throw Error("Missing << operator before >> operator");
    }
    if (op == "array") {
        std::int64_t n = static_cast<std::int32_t>(top_number().integer());
        if (n < 0 || n > kMaxPsArraySize) throw Error("Array size out of range");
        Obj arr;
        arr.t = Obj::T::Array;
        arr.a = std::make_shared<ObjVec>(static_cast<std::size_t>(n));
        stack.push_back(std::move(arr));
        return;
    }
    if (op == "put") {
        if (stack.size() >= 3) {
            Obj to_put = pop();
            std::int64_t idx = static_cast<std::int32_t>(top_number().integer());
            Obj arr = pop();
            if (arr.is_array_type() && arr.a) {
                if (idx < 0 || idx >= static_cast<std::int64_t>(arr.a->size()))
                    throw Error("Index greater than array size or less than 0");
                (*arr.a)[static_cast<std::size_t>(idx)] = std::move(to_put);
                return;
            }
        }
        throw Error("Can't execute put operator");
    }
    if (op == "for") {
        if (stack.empty()) throw Error("Problem with stack");
        Obj proc = pop();
        if (proc.t != Obj::T::Proc) throw Error("Object is not a procedure");
        std::int64_t limit = top_number().integer();
        std::int64_t inc = top_number().integer();
        std::int64_t init = top_number().integer();
        if (inc == 0 || (inc > 0 && init > limit) || (inc < 0 && init < limit)) throw Error("Wrong increment value");
        std::int64_t iterations = std::abs((limit - init) / inc) + 1;
        if (iterations > kMaxForIterations) throw Error("Loop iteration count is too large");
        for (std::int64_t k = init; inc > 0 ? k <= limit : k >= limit; k += inc) {
            stack.push_back(Obj::make_int(k));
            execute_procedure(proc);
        }
        return;
    }
    if (op == "StandardEncoding") {
        if (stack.empty()) return;
        const Obj& last = stack.back();
        if (last.is_name_type() && last.s == "Encoding") {
            stack.pop_back();
            user_dict["Encoding"] = Obj::make_name("StandardEncoding");
        }
        return;
    }
    auto it = user_dict.find(op);
    if (it != user_dict.end()) {
        Obj entry = it->second;
        execute(entry);
    }
}

}  // namespace eqx::ps
