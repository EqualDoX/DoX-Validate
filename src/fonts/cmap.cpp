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
#include "fonts/cmap.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "fonts/data.h"
#include "fonts/ps.h"
#include "fonts/util.h"

namespace eqx::fontimpl {

// --- CodeSpace ---------------------------------------------------------------------------------------
CodeSpace::CodeSpace(std::string b, std::string e) {
    if (b.size() != e.size()) return;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (static_cast<unsigned char>(b[i]) > static_cast<unsigned char>(e[i])) return;
    begin = std::move(b);
    end = std::move(e);
}

bool CodeSpace::contains(const unsigned char* code, std::size_t len) const {
    if (begin.size() != len) return false;
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char lo = static_cast<unsigned char>(begin[i]), hi = static_cast<unsigned char>(end[i]);
        if (code[i] < lo || code[i] > hi) return false;
    }
    return true;
}

bool CodeSpace::overlaps(const CodeSpace& o) const {
    std::size_t n = std::min(length(), o.length());
    for (std::size_t i = 0; i < n; ++i) {
        int b1 = static_cast<unsigned char>(begin[i]), b2 = static_cast<unsigned char>(o.begin[i]);
        int e1 = static_cast<unsigned char>(end[i]), e2 = static_cast<unsigned char>(o.end[i]);
        if ((b2 > e1 && e2 > e1) || (b2 < b1 && e2 < b1)) return false;
    }
    return true;
}

// --- helpers --------------------------------------------------------------------------------------------
std::int64_t number_from_bytes(std::string_view b) {
    std::uint64_t r = 0;   // Java long arithmetic wraps: accumulate unsigned (signed overflow is undefined in C++)
    for (std::size_t i = 0; i < b.size(); ++i) {
        unsigned shift = static_cast<unsigned>((b.size() - i - 1) * 8) & 63u;   // Java long shift
        r += static_cast<std::uint64_t>(static_cast<unsigned char>(b[i])) << shift;
    }
    return static_cast<std::int64_t>(r);
}

namespace {

// CMapParser.getBfrangeEndFromBytes (int arithmetic per byte, as in Java).
std::int64_t bfrange_end(std::string_view end, std::string_view begin) {
    std::size_t len = std::max(begin.size(), end.size());
    if (len == 0) return 0;
    std::string b(len, '\0'), e(len, '\0');
    std::copy(begin.begin(), begin.end(), b.begin() + static_cast<std::ptrdiff_t>(len - begin.size()));
    std::copy(end.begin(), end.end(), e.begin() + static_cast<std::ptrdiff_t>(len - end.size()));
    std::int64_t res = 0;
    std::size_t i = 0;
    for (; i < len - 1; ++i) {
        unsigned shift = static_cast<unsigned>((len - i - 1) * 8) & 31u;   // Java int shift
        std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << shift);
        res += v;
    }
    unsigned shift = static_cast<unsigned>((len - i - 1) * 8) & 31u;
    res += static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<unsigned char>(e[i])) << shift);
    return res;
}

}  // namespace

std::u16string unicode_from_hex(std::string_view token) {
    if (token.size() == 1) return latin1_to_u16(token);
    if (token.size() == 2 && static_cast<unsigned char>(token[0]) == 0xFF && static_cast<unsigned char>(token[1]) == 0xFE)
        return std::u16string(1, char16_t(0xFFFE));
    return utf16be_decode(token);
}

std::u16string UnicodeInterval::to_unicode(std::int32_t code) const {
    std::string u(static_cast<std::size_t>(length), '\0');
    // Java long arithmetic (wrapping)
    std::int64_t n = static_cast<std::int64_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(code)) -
                                               static_cast<std::uint64_t>(begin) + static_cast<std::uint64_t>(start));
    for (int k = length - 1; k >= 0; --k) {
        u[static_cast<std::size_t>(k)] = static_cast<char>(n & 0xFF);
        n >>= 8;
    }
    // getFFFEFromUnicode / getFEFFFromUnicode: both return U+FFFE
    for (std::size_t k = 0; k + 1 < u.size(); ++k) {
        unsigned char a = static_cast<unsigned char>(u[k]), b = static_cast<unsigned char>(u[k + 1]);
        if ((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF)) return std::u16string(1, char16_t(0xFFFE));
    }
    if (u.empty()) return {};
    if (u[0] == 0) return std::u16string(1, static_cast<char16_t>(static_cast<unsigned char>(u.back())));
    return utf16be_decode(u);
}

// --- CMap ------------------------------------------------------------------------------------------------
bool CMap::find_cid(std::int32_t code, int& cid) const {
    for (auto it = own_cid.rbegin(); it != own_cid.rend(); ++it) {
        int r = it->get_cid(code);
        if (r != -1) {
            cid = r;
            return true;
        }
    }
    for (const auto& u : used) {
        if (u && u->find_cid(code, cid)) return true;
    }
    return false;
}

int CMap::to_cid(std::int32_t code) const {
    int cid;
    if (find_cid(code, cid)) return cid;
    for (const CidRange& r : notdef) {
        int v = r.get_cid(code);
        if (v != -1) return v;
    }
    return 0;
}

bool CMap::contains_code(std::int32_t code) const {
    int cid;
    if (find_cid(code, cid)) return true;
    for (const CidRange& r : notdef)
        if (r.get_cid(code) != -1) return true;
    return false;
}

bool CMap::read_code(std::string_view bytes, std::size_t& pos, std::int32_t& code) const {
    unsigned char cc[5] = {0, 0, 0, 0, 0};
    long long prev_shortest = shortest_cs;
    for (int i = 0; i <= 4; ++i) {
        int b = pos < bytes.size() ? static_cast<unsigned char>(bytes[pos++]) : 0xFF;   // (byte) read() == -1
        cc[i] = static_cast<unsigned char>(b);
        std::size_t len = static_cast<std::size_t>(i) + 1;
        for (const CodeSpace& cs : codespaces) {
            if (cs.contains(cc, len)) {
                std::int64_t v = 0;
                for (std::size_t k = 0; k < len; ++k)
                    v += static_cast<std::int64_t>(static_cast<std::uint64_t>(cc[k]) << ((len - k - 1) * 8));
                std::int32_t res = static_cast<std::int32_t>(v);
                if (res != -1) {
                    code = res;
                    return true;
                }
            }
        }
        long long shortest = INT_MAX;
        for (const CodeSpace& cs : codespaces) {
            // CodeSpace.isPartialMatch throws for a position beyond the range length: the exception ends
            // the scan of the remaining ranges (caught outside the loop in CMap.getCodeFromStream).
            bool partial = true, thrown = false;
            for (int j = 0; j <= i; ++j) {
                if (static_cast<std::size_t>(j) >= cs.length()) {
                    thrown = true;
                    break;
                }
                unsigned char lo = static_cast<unsigned char>(cs.begin[static_cast<std::size_t>(j)]);
                unsigned char hi = static_cast<unsigned char>(cs.end[static_cast<std::size_t>(j)]);
                if (cc[j] < lo || cc[j] > hi) {
                    partial = false;
                    break;
                }
            }
            if (thrown) break;
            if (partial && shortest > static_cast<long long>(cs.length())) shortest = static_cast<long long>(cs.length());
        }
        if (shortest == INT_MAX && !codespaces.empty()) {
            long long n = prev_shortest - i - 1;
            if (n < 0) return false;   // NegativeArraySizeException
            std::size_t k = static_cast<std::size_t>(std::min<long long>(n, static_cast<long long>(bytes.size() - std::min(pos, bytes.size()))));
            pos += k;
            code = 0;
            return true;
        }
        prev_shortest = shortest;
    }
    code = 0;
    return true;
}

const std::u16string* CMap::unicode(std::int32_t code, std::u16string& scratch) const {
    auto it = to_unicode_map.find(code);
    if (it != to_unicode_map.end()) return &it->second;
    for (const UnicodeInterval& iv : intervals) {
        if (code >= iv.begin && code <= iv.end) {
            scratch = iv.to_unicode(code);
            return &scratch;
        }
    }
    return nullptr;
}

void CMap::use(const std::shared_ptr<const CMap>& other) {
    if (!other) return;
    used.push_back(other);
    codespaces.insert(codespaces.end(), other->codespaces.begin(), other->codespaces.end());
    notdef.insert(notdef.end(), other->notdef.begin(), other->notdef.end());
    for (const auto& [k, v] : other->to_unicode_map) to_unicode_map[k] = v;
}

void CMap::add_codespace(std::string b, std::string e) {
    std::size_t blen = b.size();
    CodeSpace cs(std::move(b), std::move(e));
    for (const CodeSpace& o : codespaces)
        if (o.overlaps(cs)) return;   // "overlapping codespace ranges": ignored
    codespaces.push_back(std::move(cs));
    if (static_cast<long long>(blen) < shortest_cs) shortest_cs = static_cast<int>(blen);
}

// --- CMapParser ------------------------------------------------------------------------------------------
namespace {

class CMapParser {
public:
    explicit CMapParser(std::string_view data)
        : src_(data, ps::Source::Kind::kBuffered), lx_(src_, true, ps::Lexer::Comments::kNotSeekableCMap), op_(lx_) {}

    std::shared_ptr<CMap> parse() {
        cmap_ = std::make_shared<CMap>();
        lx_.skip_spaces(true);
        ps::Obj o = op_.next_object();
        while (lx_.token().type != ps::TT::Eof) {
            process(o);
            o = op_.next_object();
        }
        auto str = [&](const char* key) -> std::optional<std::string> {
            const ps::Obj* v = in_.user(key);
            std::string s;
            if (v && v->get_string(s)) return s;
            return std::nullopt;
        };
        auto lng = [&](const char* key) -> std::int64_t {
            const ps::Obj* v = in_.user(key);
            if (!v || !v->is_number()) return 0;
            return v->integer();
        };
        cmap_->name = str("CMapName");
        cmap_->registry = str("Registry");
        cmap_->ordering = str("Ordering");
        cmap_->wmode = static_cast<int>(lng("WMode"));
        cmap_->supplement = static_cast<int>(lng("Supplement"));
        return cmap_;
    }

private:
    ps::Token& tok() { return lx_.token(); }
    void check(ps::TT t) {
        if (tok().type != t) throw ps::Error("CMap contains an invalid entry");
    }

    void process(const ps::Obj& o) {
        if (o.t == ps::Obj::T::Int) {
            std::int32_t n = static_cast<std::int32_t>(tok().integer);   // the shared token's integer
            ps::Obj next = op_.next_object();
            if (tok().type != ps::TT::Keyword || !process_list(n, tok().value)) {
                in_.execute(o);
                in_.execute(next);
            }
            return;
        }
        if (o.is_name_type()) {
            if (tok().value == "usecmap") {
                std::string nm;
                std::shared_ptr<const CMap> used;
                if (!last_name_.empty() && last_name_.get_string(nm)) used = predefined_cmap(nm);
                if (used) cmap_->use(used);
                else cmap_->uses_non_predefined = true;
            } else {
                in_.execute(o);
                last_name_ = o;
            }
            return;
        }
        in_.execute(o);
    }

    bool process_list(std::int32_t n, const std::string& type) {
        if (type.rfind("begin", 0) != 0) return false;
        std::string key = type.substr(5);
        for (std::int32_t i = 0; i < n; ++i) {
            if (key == "codespacerange") {
                lx_.next_token();
                check(ps::TT::HexString);
                std::string b = tok().value;
                lx_.next_token();
                check(ps::TT::HexString);
                cmap_->add_codespace(std::move(b), tok().value);
            } else if (key == "cidrange" || key == "notdefrange") {
                lx_.next_token();
                check(ps::TT::HexString);
                std::int64_t s = number_from_bytes(tok().value);
                lx_.next_token();
                check(ps::TT::HexString);
                std::int64_t e = number_from_bytes(tok().value);
                lx_.next_token();
                check(ps::TT::Integer);
                CidRange r{static_cast<std::int32_t>(s), static_cast<std::int32_t>(e),
                           static_cast<std::int32_t>(tok().integer), key == "notdefrange"};
                if (key == "cidrange") cmap_->own_cid.push_back(r);
                else cmap_->notdef.push_back(r);
            } else if (key == "cidchar" || key == "notdefchar") {
                lx_.next_token();
                check(ps::TT::HexString);
                std::int64_t c = number_from_bytes(tok().value);
                lx_.next_token();
                check(ps::TT::Integer);
                CidRange r{static_cast<std::int32_t>(c), static_cast<std::int32_t>(c),
                           static_cast<std::int32_t>(tok().integer), false};
                if (key == "cidchar") cmap_->own_cid.push_back(r);
                else cmap_->notdef.push_back(r);
            } else if (key == "bfchar") {
                lx_.next_token();
                check(ps::TT::HexString);
                std::int64_t c = number_from_bytes(tok().value);
                cmap_->to_unicode_map[static_cast<std::int32_t>(c)] = read_unicode();
            } else if (key == "bfrange") {
                lx_.next_token();
                check(ps::TT::HexString);
                std::string begin = tok().value;
                std::int64_t b = number_from_bytes(begin);
                lx_.next_token();
                check(ps::TT::HexString);
                std::int64_t e = bfrange_end(tok().value, begin);
                lx_.next_token();
                if (tok().type == ps::TT::OpenArray) {
                    for (std::int64_t k = b; k <= e; ++k)
                        cmap_->to_unicode_map[static_cast<std::int32_t>(k)] = read_unicode();
                    lx_.next_token();
                } else {
                    const std::string& v = tok().value;
                    if (!v.empty())
                        cmap_->intervals.push_back(
                            UnicodeInterval{b, e, static_cast<int>(v.size()), number_from_bytes(v)});
                }
            } else {
                return false;
            }
        }
        lx_.next_token();   // "end<key>" (a mismatch is only logged)
        return true;
    }

    std::u16string read_unicode() {
        lx_.next_token();
        if (tok().type == ps::TT::Name) return latin1_to_u16(tok().value);
        if (tok().type == ps::TT::HexString) return unicode_from_hex(tok().value);
        throw ps::Error("CMap contains invalid entry in bfchar");
    }

    ps::Source src_;
    ps::Lexer lx_;
    ps::ObjectParser op_;
    ps::Interpreter in_;
    std::shared_ptr<CMap> cmap_;
    ps::Obj last_name_;
};

// --- predefined CMaps: replay of the generated operation streams -------------------------------------
class Replay {
public:
    explicit Replay(std::string_view d) : d_(d) {}
    bool done() const { return p_ >= d_.size(); }
    unsigned byte() {
        if (p_ >= d_.size()) throw ps::Error("truncated CMap record");
        return static_cast<unsigned char>(d_[p_++]);
    }
    std::uint64_t varint() {
        std::uint64_t v = 0;
        int shift = 0;
        for (;;) {
            unsigned b = byte();
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
            if (shift > 63) throw ps::Error("bad varint");
        }
    }
    std::int64_t zz() {
        std::uint64_t v = varint();
        return (v & 1) ? -static_cast<std::int64_t>((v + 1) >> 1) : static_cast<std::int64_t>(v >> 1);
    }
    std::string bytes() {
        std::uint64_t n = varint();
        if (n > d_.size() - p_) throw ps::Error("truncated CMap record");
        std::string s(d_.substr(p_, static_cast<std::size_t>(n)));
        p_ += static_cast<std::size_t>(n);
        return s;
    }

private:
    std::string_view d_;
    std::size_t p_ = 0;
};

std::shared_ptr<CMap> replay_predefined(std::string_view ops) {
    auto cm = std::make_shared<CMap>();
    Replay r(ops);
    std::int64_t code = 0, cid = 0, uprev = 0;
    auto dcode = [&]() {
        code += r.zz();
        return code;
    };
    auto dcid = [&](std::int64_t span) {
        std::int64_t c = cid + r.zz();
        cid = c + span + 1;
        return c;
    };
    auto ustr = [&]() -> std::u16string {
        unsigned kind = r.byte();
        if (kind == 2) {   // U_HEX2
            uprev += r.zz();
            std::string b{static_cast<char>((uprev >> 8) & 0xFF), static_cast<char>(uprev & 0xFF)};
            return unicode_from_hex(b);
        }
        std::string b = r.bytes();
        return kind == 1 ? latin1_to_u16(b) : unicode_from_hex(b);
    };
    while (!r.done()) {
        unsigned op = r.byte();
        switch (op) {
            case 1: {
                std::string b = r.bytes();
                std::string e = r.bytes();
                cm->add_codespace(std::move(b), std::move(e));
                break;
            }
            case 2:
            case 4: {
                std::int64_t s = dcode();
                std::int64_t e = dcode();
                std::int64_t c = dcid(e - s);
                CidRange cr{static_cast<std::int32_t>(s), static_cast<std::int32_t>(e), static_cast<std::int32_t>(c), op == 4};
                (op == 2 ? cm->own_cid : cm->notdef).push_back(cr);
                break;
            }
            case 3:
            case 5: {
                std::int64_t s = dcode();
                std::int64_t c = dcid(0);
                CidRange cr{static_cast<std::int32_t>(s), static_cast<std::int32_t>(s), static_cast<std::int32_t>(c), false};
                (op == 3 ? cm->own_cid : cm->notdef).push_back(cr);
                break;
            }
            case 6: {
                std::int64_t s = dcode();
                cm->to_unicode_map[static_cast<std::int32_t>(s)] = ustr();
                break;
            }
            case 7: {
                std::string b = r.bytes();
                std::string e = r.bytes();
                std::uint64_t n = r.varint();
                std::int64_t k = number_from_bytes(b);
                for (std::uint64_t i = 0; i < n; ++i, ++k) cm->to_unicode_map[static_cast<std::int32_t>(k)] = ustr();
                (void)e;
                break;
            }
            case 8: {
                std::string b = r.bytes();
                std::string e = r.bytes();
                std::string s = r.bytes();
                if (!s.empty())
                    cm->intervals.push_back(UnicodeInterval{number_from_bytes(b), bfrange_end(e, b),
                                                            static_cast<int>(s.size()), number_from_bytes(s)});
                break;
            }
            case 9: {
                std::string nm = r.bytes();
                auto used = predefined_cmap(nm);
                if (used) cm->use(used);
                else cm->uses_non_predefined = true;
                break;
            }
            case 10: {
                cm->wmode = static_cast<int>(r.varint());
                cm->name = r.bytes();
                return cm;
            }
            default: throw ps::Error("bad CMap record");
        }
    }
    return cm;
}

}  // namespace

std::shared_ptr<const CMap> parse_cmap_program(std::string_view data) {
    try {
        CMapParser p(data);
        return p.parse();
    } catch (std::exception&) {
        return std::make_shared<CMap>();   // CMapFactory: "Can't parse CMap, using default"
    }
}

std::shared_ptr<const CMap> predefined_cmap(std::string_view name) {
    static std::recursive_mutex mu;
    static std::map<std::string, std::shared_ptr<const CMap>, std::less<>> cache;
    std::lock_guard<std::recursive_mutex> lock(mu);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    std::shared_ptr<const CMap> res;
    if (const fontdata::PredefinedCMap* pc = fontdata::predefined_cmap(name)) {
        cache.emplace(std::string(name), nullptr);   // recursion guard (usecmap loops)
        auto ops = inflate(std::string_view(pc->data, pc->size), pc->raw_size);
        if (ops) {
            try {
                res = replay_predefined(*ops);
            } catch (std::exception&) {
                res = std::make_shared<CMap>();
            }
        } else {
            res = std::make_shared<CMap>();
        }
    }
    cache[std::string(name)] = res;
    return res;
}

bool is_table118_cmap(std::string_view n) {
    static const char* const kNames[] = {
        "Identity-H", "Identity-V", "GB-EUC-H", "GB-EUC-V", "GBpc-EUC-H", "GBpc-EUC-V", "GBK-EUC-H", "GBK-EUC-V",
        "GBKp-EUC-H", "GBKp-EUC-V", "GBK2K-H", "GBK2K-V", "UniGB-UCS2-H", "UniGB-UCS2-V", "UniGB-UTF16-H",
        "UniGB-UTF16-V", "B5pc-H", "B5pc-V", "HKscs-B5-H", "HKscs-B5-V", "ETen-B5-H", "ETen-B5-V", "ETenms-B5-H",
        "ETenms-B5-V", "CNS-EUC-H", "CNS-EUC-V", "UniCNS-UCS2-H", "UniCNS-UCS2-V", "UniCNS-UTF16-H",
        "UniCNS-UTF16-V", "83pv-RKSJ-H", "90ms-RKSJ-H", "90ms-RKSJ-V", "90msp-RKSJ-H", "90msp-RKSJ-V",
        "90pv-RKSJ-H", "Add-RKSJ-H", "Add-RKSJ-V", "EUC-H", "EUC-V", "Ext-RKSJ-H", "Ext-RKSJ-V", "H", "V",
        "UniJIS-UCS2-H", "UniJIS-UCS2-V", "UniJIS-UCS2-HW-H", "UniJIS-UCS2-HW-V", "UniJIS-UTF16-H",
        "UniJIS-UTF16-V", "KSC-EUC-H", "KSC-EUC-V", "KSCms-UHC-H", "KSCms-UHC-V", "KSCms-UHC-HW-H",
        "KSCms-UHC-HW-V", "KSCpc-EUC-H", "UniKS-UCS2-H", "UniKS-UCS2-V", "UniKS-UTF16-H", "UniKS-UTF16-V"};
    for (const char* k : kNames)
        if (n == k) return true;
    return false;
}

}  // namespace eqx::fontimpl
