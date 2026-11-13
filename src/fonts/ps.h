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
// PostScript tokenizer and the small PostScript interpreter veraPDF 1.30.2 uses for embedded CMaps and the
// clear-text part of Type 1 font programs (org.verapdf.parser.BaseParser / NotSeekableBaseParser /
// SeekableBaseParser / COSParser and org.verapdf.parser.postscript.*). The port keeps veraPDF's quirks:
// the shared token keeps its integer value across keyword tokens (CMapParser reads list lengths from it),
// integers are looked ahead for "N G R" references, '<<' / '>>' are plain names, octal escapes in literal
// strings are dropped in PostScript mode, names are not #-decoded, procedures are pushed (not executed)
// when invoked by name, and so on.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eqx::ps {

// IOException / PostScriptException (and the Java runtime exceptions the parsers can raise on malformed
// input: empty stack, index out of bounds) - the caller treats the parse as failed.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Byte source. kBuffered: ASBufferedInFilter (readByte at EOF returns 0xFF without advancing);
// kSeekable: SeekableInputStream (readByte at EOF throws).
class Source {
public:
    enum class Kind : std::uint8_t { kBuffered, kSeekable };
    Source(std::string_view d, Kind k) : d_(d), kind_(k) {}
    bool eof() const { return p_ >= d_.size(); }
    std::size_t pos() const { return p_; }
    std::size_t size() const { return d_.size(); }
    std::string_view data() const { return d_; }
    void seek(std::size_t p) {
        if (p > d_.size()) throw Error("seek beyond end");
        p_ = p;
    }
    // Java byte semantics: returns 0..255 ("(byte) -1" at EOF for buffered sources).
    int read_byte() {
        if (p_ >= d_.size()) {
            if (kind_ == Kind::kSeekable) throw Error("End of file is reached");
            return 0xFF;
        }
        return static_cast<unsigned char>(d_[p_++]);
    }
    // InputStream.read(): -1 at EOF.
    int read() { return p_ >= d_.size() ? -1 : static_cast<unsigned char>(d_[p_++]); }
    int peek() const { return p_ >= d_.size() ? -1 : static_cast<unsigned char>(d_[p_]); }
    void unread(std::size_t n = 1) {
        if (n > p_) throw Error("unread before start");
        p_ -= n;
    }
    std::size_t skip(std::size_t n) {
        std::size_t k = std::min(n, d_.size() - p_);
        p_ += k;
        return k;
    }

private:
    std::string_view d_;
    std::size_t p_ = 0;
    Kind kind_;
};

enum class TT : std::uint8_t {
    None, Keyword, Integer, Real, LitString, HexString, Name, OpenArray, CloseArray, OpenDict, CloseDict, Eof,
    StartProc, EndProc, Unset
};
enum class KW : std::uint8_t { Null_, None, Null, True, False, Stream, EndStream, Obj, EndObj, R, N, F, Xref,
                               StartXref, Trailer };

struct Token {
    TT type = TT::Unset;
    KW keyword = KW::Null_;   // Null_ = Java null (unknown keyword)
    std::int64_t integer = 0;
    double real = 0;
    std::string value;        // bytes
};

// BaseParser tokenizer.
class Lexer {
public:
    enum class Comments : std::uint8_t {
        kNotSeekable,     // NotSeekableBaseParser: comment ends at LF, CR (+LF)
        kNotSeekableCMap, // CMapBaseParser: also FF
        kSeekable         // SeekableBaseParser: isEOL
    };
    Lexer(Source& s, bool ps_mode, Comments c, bool type1_private_names = false)
        : src_(s), ps_(ps_mode), comments_(c), t1names_(type1_private_names) {}

    Token& token() { return tok_; }
    Source& source() { return src_; }
    void next_token();
    void skip_spaces(bool skip_comment);
    bool skip_single_space(bool skip_comment);
    bool ps_mode() const { return ps_; }

private:
    void skip_comment();
    void read_number();
    void read_hex_string();
    void read_lit_string();
    void read_name();
    void read_token();

    Source& src_;
    bool ps_;
    Comments comments_;
    bool t1names_;
    Token tok_;
};

bool is_space(int c);
bool is_delimiter(int c);   // CharTable.isTokenDelimiter (spaces and delimiters)

// --- PostScript objects --------------------------------------------------------------------------------
struct Obj;
using ObjVec = std::vector<Obj>;
using ObjMap = std::unordered_map<std::string, Obj>;

struct Obj {
    enum class T : std::uint8_t { Empty, Null, Bool, Int, Real, String, Name, Operator, Array, Proc, Dict, Mark };
    T t = T::Empty;
    bool b = false;
    std::int64_t i = 0;
    double r = 0;
    std::string s;                      // String bytes, Name / Operator text
    std::shared_ptr<ObjVec> a;          // Array / Proc (shared, mutable: Java reference semantics)
    std::shared_ptr<ObjMap> d;          // Dict

    bool empty() const { return t == T::Empty; }
    bool is_number() const { return t == T::Int || t == T::Real; }
    bool is_name_type() const { return t == T::Name || t == T::Operator; }   // COS_NAME
    bool is_array_type() const { return t == T::Array || t == T::Proc; }     // COS_ARRAY
    double real() const { return t == T::Int ? static_cast<double>(i) : r; }
    std::int64_t integer() const { return t == T::Int ? i : static_cast<std::int64_t>(r); }
    // COSObject.getString(): name / string value; nullptr-equivalent = false.
    bool get_string(std::string& out) const {
        if (t == T::Name || t == T::Operator || t == T::String) {
            out = s;
            return true;
        }
        return false;
    }
    static Obj make_int(std::int64_t v) { Obj o; o.t = T::Int; o.i = v; o.r = static_cast<double>(v); return o; }
    static Obj make_real(double v) { Obj o; o.t = T::Real; o.r = v; return o; }
    static Obj make_bool(bool v) { Obj o; o.t = T::Bool; o.b = v; return o; }
    static Obj make_name(std::string v) { Obj o; o.t = T::Name; o.s = std::move(v); return o; }
    static Obj make_op(std::string v) { Obj o; o.t = T::Operator; o.s = std::move(v); return o; }
    static Obj make_string(std::string v) { Obj o; o.t = T::String; o.s = std::move(v); return o; }
};

// COSParser.nextObject with the PSParser overrides.
class ObjectParser {
public:
    explicit ObjectParser(Lexer& lx) : lx_(lx) {}
    Obj next_object();
    Lexer& lexer() { return lx_; }

private:
    Obj get_array(int depth);
    Obj next_object_depth(int depth);

    Lexer& lx_;
    std::deque<Obj> objects_;
    std::deque<std::int64_t> integers_;
    bool flag_ = true;
};

// PSOperator / PSProcedure / PSLiteralObject execution on an operand stack and a user dictionary.
class Interpreter {
public:
    std::vector<Obj> stack;
    ObjMap user_dict;

    // PSObject.getPSObject(obj).execute(...)
    void execute(const Obj& o);
    // PSOperator.execute
    void execute_operator(const std::string& op);
    // PSProcedure.executeProcedure (for) / modifiedExecuteProcedure (if, ifelse)
    void execute_procedure(const Obj& proc);
    void modified_execute_procedure(const Obj& proc);
    const Obj* user(const std::string& key) const {
        auto it = user_dict.find(key);
        return it == user_dict.end() ? nullptr : &it->second;
    }

private:
    Obj pop();
    Obj& peek();
    Obj top_number();
    int depth_ = 0;
};

}  // namespace eqx::ps
