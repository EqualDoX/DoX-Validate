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
// Type 1 programs: org.verapdf.pd.font.type1.{Type1FontProgram, Type1PrivateParser, EexecFilterDecode}.
#include <functional>
#include <array>
#include <unordered_set>

#include "fonts/charstrings.h"
#include "fonts/data.h"
#include "fonts/programs.h"
#include "fonts/ps.h"
#include "fonts/util.h"

namespace eqx::fontimpl {

namespace {

constexpr std::array<double, 6> kDefaultMatrix = {0.001, 0, 0, 0.001, 0, 0};

// EexecFilterDecode
std::string eexec_decode(std::string_view in, int r, int discard) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        int c = static_cast<unsigned char>(in[i]);
        int p = c ^ (r >> 8);
        if (static_cast<long long>(i) >= discard) out.push_back(static_cast<char>(p));
        r = static_cast<int>((static_cast<std::uint32_t>(c + r) * 52845u + 22719u) & 0xFFFFu);   // Java int: wraps
    }
    return out;
}

bool is_keyword_operator(const std::string& op) {
    static const std::unordered_set<std::string> kOps = {
        "abs", "floor", "mod", "add", "idiv", "mul", "div", "neg", "sub", "ceiling", "round", "copy", "exch", "pop",
        "dup", "index", "roll", "clear", "count", "mark", "cleartomark", "counttomark", "dict", "begin", "length",
        "def", "load", "array", "put", "for", "StandardEncoding", "<<", ">>"};
    return kOps.count(op) != 0;
}

class PrivateParser {
public:
    PrivateParser(std::string_view data, const std::array<double, 6>& m)
        : src_(data, ps::Source::Kind::kSeekable), lx_(src_, false, ps::Lexer::Comments::kSeekable, true), matrix_(m),
          default_matrix_(m == kDefaultMatrix) {}

    // Returns glyphWidths (nullopt = CharStrings never decoded, Java null).
    std::optional<std::unordered_map<std::string, int>> parse() {
        lx_.skip_spaces(true);
        auto& tok = lx_.token();
        while (tok.type != ps::TT::Eof && (!charstrings_found_ || tok.value.rfind("closefile", 0) != 0)) {
            lx_.next_token();
            process();
        }
        return widths_;
    }

private:
    void process() {
        auto& tok = lx_.token();
        if (tok.type != ps::TT::Name) return;
        if (tok.value == "CharStrings") {
            charstrings_found_ = true;
            lx_.next_token();
            if (tok.type != ps::TT::Integer) return;
            std::int32_t amount = static_cast<std::int32_t>(tok.integer);
            lx_.next_token();
            lx_.next_token();
            lx_.next_token();
            for (std::int32_t i = 0; i < amount; ++i)
                if (!decode_charstring()) break;
        } else if (tok.value == "lenIV") {
            lx_.next_token();
            if (tok.type == ps::TT::Integer) len_iv_ = static_cast<std::int32_t>(tok.integer);
        } else if (tok.value == "Subrs") {
            lx_.next_token();
            std::int32_t amount = static_cast<std::int32_t>(tok.integer);
            lx_.next_token();   // array
            for (std::int32_t i = 0; i < amount; ++i) {
                lx_.next_token();
                if (tok.value != "dup") break;
                lx_.next_token();
                std::int64_t number = tok.integer;
                lx_.next_token();
                std::int64_t to_skip = tok.integer;
                skip_rd();
                lx_.skip_spaces(false);
                std::size_t begin = src_.pos();
                if (to_skip < 0) throw ps::Error("negative subroutine length");
                src_.skip(static_cast<std::size_t>(to_skip));
                auto w = charstring_width(src_.data().substr(begin, src_.pos() - begin));
                if (w) subr_widths_[static_cast<std::int32_t>(number)] = *w;
                lx_.next_token();   // NP
                if (tok.value == "noaccess") lx_.next_token();
            }
        }
    }

    bool decode_charstring() {
        auto& tok = lx_.token();
        if (!widths_) widths_.emplace();
        lx_.next_token();
        if (tok.type != ps::TT::Name) {
            if (tok.type == ps::TT::Keyword && tok.value == "end") return false;
            throw ps::Error("Error in parsing Private dictionary of font 1 file");
        }
        std::string name = tok.value;
        lx_.next_token();
        if (tok.type != ps::TT::Integer) throw ps::Error("Error in parsing Private dictionary of font 1 file");
        std::int64_t len = tok.integer;
        skip_rd();
        lx_.skip_single_space(false);
        std::size_t begin = src_.pos();
        if (len < 0) throw ps::Error("negative charstring length");
        src_.skip(static_cast<std::size_t>(len));
        auto w = charstring_width(src_.data().substr(begin, src_.pos() - begin));
        if (w) {
            if (!default_matrix_) (*widths_)[name] = java_d2i(static_cast<double>(w->real) * (matrix_[0] * 1000.0));
            else (*widths_)[name] = static_cast<std::int32_t>(w->integer);
        }
        lx_.next_token();
        return true;
    }

    std::optional<CffNumber> charstring_width(std::string_view enc) {
        if (len_iv_ < 0) throw ps::Error("unsupported lenIV");   // Java: broken EexecFilterDecode state
        std::string dec = eexec_decode(enc, 4330, len_iv_);
        return type1_charstring_width(dec, &subr_widths_);
    }

    void skip_rd() {
        lx_.skip_spaces(false);
        int c = src_.read();
        if (c == 45) {
            src_.read();
        } else {
            src_.unread();   // also after read() == -1 at the end (Java seeks one byte back)
            lx_.next_token();
        }
    }

    ps::Source src_;
    ps::Lexer lx_;
    std::array<double, 6> matrix_;
    bool default_matrix_;
    bool charstrings_found_ = false;
    std::int32_t len_iv_ = 4;
    std::optional<std::unordered_map<std::string, int>> widths_;
    std::unordered_map<int, CffNumber> subr_widths_;
};

}  // namespace

std::shared_ptr<const Type1Data> parse_type1(std::string_view data) {
    auto d = std::make_shared<Type1Data>();
    d->encoding.assign(256, std::string());
    d->encoding_set.assign(256, false);
    try {
        ps::Source src(data, ps::Source::Kind::kBuffered);
        ps::Lexer lx(src, true, ps::Lexer::Comments::kNotSeekable);
        ps::ObjectParser op(lx);
        ps::Interpreter in;
        std::optional<std::unordered_map<std::string, int>> widths;
        int depth = 0;
        std::function<void(const ps::Obj&)> to_execute = [&](const ps::Obj& o) {
            if (++depth > 64) throw ps::Error("Type 1 font program exceeded toExecute recursion depth");
            if (o.t == ps::Obj::T::Operator) {
                if (!is_keyword_operator(o.s)) {
                    if (const ps::Obj* e = in.user(o.s)) {
                        ps::Obj copy = *e;
                        to_execute(copy);
                    }
                } else {
                    in.execute_operator(o.s);
                }
            } else {
                in.execute(o);
            }
            --depth;
        };
        lx.skip_spaces(true);
        ps::Obj o = op.next_object();
        while (lx.token().type != ps::TT::Eof) {
            std::string sv;
            if (o.is_name_type() && o.get_string(sv) && sv == "eexec") {
                // skipSpacesExceptNullByte
                while (!src.eof()) {
                    int ch = src.read_byte();
                    if (ps::is_space(ch) && ch != 0) continue;
                    src.unread();
                    break;
                }
                // ASBufferedInFilter.getStreamUntilToken("cleartomark")
                static const std::string kTok = "cleartomark";
                std::string buf;
                std::size_t avail = src.size() - src.pos();
                std::size_t first = std::min(avail, kTok.size());
                if (first != kTok.size()) throw ps::Error("Stream is shorter than finishing token");
                buf.assign(src.data().substr(src.pos(), first));
                src.skip(first);
                int rb = src.read_byte();
                while (!src.eof() && buf.compare(buf.size() - kTok.size(), kTok.size(), kTok) != 0) {
                    buf.push_back(static_cast<char>(rb));
                    rb = src.read_byte();
                }
                std::size_t cap = 10240;
                while (cap <= buf.size()) cap *= 2;
                buf.resize(cap, '\0');
                // matrix from the user dictionary (getFontMatrix)
                std::array<double, 6> m = kDefaultMatrix;
                if (const ps::Obj* fm = in.user("FontMatrix"); fm && fm->t == ps::Obj::T::Array && fm->a) {
                    m = {0, 0, 0, 0, 0, 0};
                    std::size_t k = 0;
                    for (const ps::Obj& x : *fm->a) {
                        if (!x.is_number()) continue;
                        if (k >= 6) throw ps::Error("FontMatrix with more than 6 numbers");
                        m[k++] = x.real();
                    }
                }
                std::string plain = eexec_decode(buf, 55665, 4);
                PrivateParser pp(plain, m);
                widths = pp.parse();
            } else {
                to_execute(o);
            }
            o = op.next_object();
        }
        // initializeEncoding
        if (const ps::Obj* e = in.user("Encoding")) {
            if (e->t == ps::Obj::T::Array && e->a) {
                std::size_t k = 0;
                for (const ps::Obj& x : *e->a) {
                    if (k >= 256) break;
                    std::string s;
                    d->encoding[k] = x.get_string(s) ? s : std::string();
                    d->encoding_set[k] = true;
                    ++k;
                }
            } else if (e->is_name_type() && e->s == "StandardEncoding") {
                for (int k = 0; k < 256; ++k) {
                    const char* n = fontdata::kStandard[k];
                    d->encoding[static_cast<std::size_t>(k)] = n ? n : "";
                    d->encoding_set[static_cast<std::size_t>(k)] = n != nullptr;
                }
            }
        }
        if (!widths) throw ps::Error("Type 1 font doesn't contain charstrings.");
        d->widths = std::move(*widths);
        d->parsed = true;
    } catch (std::exception&) {
        d->parsed = false;
    }
    return d;
}

namespace {

class Type1Program final : public FontProgram {
public:
    explicit Type1Program(std::shared_ptr<const Type1Data> d) : d_(std::move(d)) {}
    Kind kind() const override { return Kind::Type1; }
    bool successful() const override { return d_->parsed; }
    float width(int code) override {
        if (!d_->parsed) return -1;
        auto g = glyph(code);
        if (!g) return -1;
        auto it = d_->widths.find(*g);
        return it == d_->widths.end() ? -1.0f : static_cast<float>(it->second);
    }
    float width(std::string_view name) override {
        auto it = d_->widths.find(std::string(name));
        return it == d_->widths.end() ? -1.0f : static_cast<float>(it->second);
    }
    bool contains_code(int code) override {
        auto g = glyph(code);
        return g && contains_glyph(*g);
    }
    bool contains_glyph(std::string_view name) override {
        return d_->parsed && d_->widths.count(std::string(name)) && name != ".notdef";
    }
    bool contains_cid(int) override { return false; }
    std::optional<std::string> glyph_name(int code) override {
        if (code >= 0 && code < 256 && d_->encoding_set[static_cast<std::size_t>(code)])
            return d_->encoding[static_cast<std::size_t>(code)];
        return std::nullopt;
    }
    std::vector<int> cid_list() override { return {}; }
    const Type1Data& data() const { return *d_; }

private:
    std::optional<std::string> glyph(int code) const {   // getGlyph
        if (code < 256) {
            if (code >= 0 && d_->encoding_set[static_cast<std::size_t>(code)]) return d_->encoding[static_cast<std::size_t>(code)];
            return std::nullopt;
        }
        return std::string(".notdef");
    }
    std::shared_ptr<const Type1Data> d_;
};

}  // namespace

std::unique_ptr<FontProgram> make_type1_program(std::shared_ptr<const Type1Data> d) {
    return std::make_unique<Type1Program>(std::move(d));
}

const Type1Data* type1_data(FontProgram* p) {
    if (!p) return nullptr;
    auto* t = dynamic_cast<Type1Program*>(p->inner());
    return t ? &t->data() : nullptr;
}

}  // namespace eqx::fontimpl
