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
// CFF programs: org.verapdf.pd.font.cff.{CFFFileBaseParser, CFFFontBaseParser, CFFFontProgram,
// CFFType1FontProgram, CFFCIDFontProgram, CharStringsWidths, CFFCharStringsHandler}.
#include <array>
#include <cmath>
#include <map>
#include <string>

#include "fonts/charstrings.h"
#include "fonts/cmap.h"
#include "fonts/data.h"
#include "fonts/programs.h"
#include "fonts/ps.h"
#include "fonts/util.h"

namespace eqx::fontimpl {

struct CffData {
    std::string bytes;            // owned copy of the program
    bool parsed = false;          // font.isSuccessfulParsing()
    bool font_created = false;    // CFFFontProgram.font != null
    bool cid = false;
    bool is_subset = false;
    // common
    int charstring_type = 2;
    CffIndex charstrings, global_subrs, defined_names;
    int n_glyphs = 0;
    // Type 1
    bool std_encoding = false, expert_encoding = false;
    int encoding[256] = {0};
    std::unordered_map<std::string, int> charset;        // name -> gid
    std::unordered_map<int, std::string> inverse;         // gid -> name
    std::vector<std::string> charset_order;               // getCharSet() iteration (names)
    // CID
    bool default_charset = false;
    std::unordered_map<int, int> cid_charset;             // cid -> gid
    std::vector<int> fd_select;
    // widths (CharStringsWidths)
    std::vector<std::array<float, 6>> matrices;
    std::vector<bool> default_matrix;
    std::vector<CffIndex> local_subrs;
    std::vector<bool> has_local;
    std::vector<int> bias, default_widths, nominal_widths;
    bool use_fd_select = false;
    std::vector<float> subset_widths;
    std::unordered_map<int, float> general_widths;
    CffFacts facts;

    float width_from_gid(int gid);
};

namespace {

constexpr std::array<float, 6> kDefaultMatrix = {0.001f, 0, 0, 0.001f, 0, 0};

class CffReader {
public:
    explicit CffReader(std::string_view d) : d_(d) {}
    std::size_t pos() const { return p_; }
    void seek(std::int64_t p) {
        if (p < 0 || static_cast<std::uint64_t>(p) > d_.size()) throw ps::Error("seek beyond end");
        p_ = static_cast<std::size_t>(p);
    }
    int peek() const { return p_ >= d_.size() ? -1 : static_cast<unsigned char>(d_[p_]); }
    int byte() {
        if (p_ >= d_.size()) throw ps::Error("End of file is reached");
        return static_cast<unsigned char>(d_[p_++]);
    }
    int card16() {
        int h = byte() << 8;
        return h | byte();
    }
    CffIndex index() {
        CffIndex ix;
        int count = card16();
        if (count == 0) return ix;
        int off_size = byte();
        if (off_size == 0) throw ps::Error("Bad offset size");
        ix.offsets.resize(static_cast<std::size_t>(count) + 1);
        for (int i = 0; i <= count; ++i) {
            std::int64_t r = 0;
            for (int k = 0; k < off_size - 1; ++k) {
                r |= byte();
                r <<= 8;
            }
            r |= byte();
            ix.offsets[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(r);
        }
        if (ix.offsets.back() < 1) throw ps::Error("Wrong index data offset");
        std::size_t len = static_cast<std::size_t>(ix.offsets.back() - 1);
        if (len > d_.size() - p_) throw ps::Error("End of stream is reached");
        ix.data = d_.substr(p_, len);
        p_ += len;
        ix.count = count;
        ix.offset_shift = 3 + off_size * (count + 1);
        return ix;
    }
    CffNumber number() {
        int first = byte();
        if (first == 0x1E) {
            std::string s;
            for (;;) {
                int b = byte();
                int h[2] = {b >> 4, b & 0x0F};
                bool done = false;
                for (int k = 0; k < 2 && !done; ++k) {
                    if (h[k] < 10) s.push_back(static_cast<char>('0' + h[k]));
                    else if (h[k] == 0xA) s.push_back('.');
                    else if (h[k] == 0xB) s.push_back('E');
                    else if (h[k] == 0xC) s += "E-";
                    else if (h[k] == 0xE) s.push_back('-');
                    else done = true;
                }
                if (done) break;
            }
            auto v = java_parse_float(s);
            if (!v) throw ps::Error("Invalid number format");
            return CffNumber::of_real(*v);
        }
        if (first > 31 && first < 247) return CffNumber::of_int(first - 139);
        if (first > 246 && first < 251) return CffNumber::of_int(((first - 247) << 8) + byte() + 108);
        if (first > 250 && first < 255) return CffNumber::of_int(-((first - 251) << 8) - byte() - 108);
        if (first == 28) return CffNumber::of_int(card16());
        if (first == 29) {
            int a = card16();
            int b = card16();
            return CffNumber::of_int(static_cast<std::int32_t>((static_cast<std::uint32_t>(a) << 16) | static_cast<std::uint32_t>(b)));
        }
        throw ps::Error("Can't read integer");
    }

private:
    std::string_view d_;
    std::size_t p_ = 0;
};

std::string sid_string(const CffData& d, std::int64_t sid) {
    if (sid >= 0 && sid < static_cast<std::int64_t>(fontdata::kCffStdStringsCount)) return fontdata::kCffStdStrings[sid];
    std::string s;
    if (sid < 0 || !d.defined_names.get(static_cast<int>(sid - static_cast<std::int64_t>(fontdata::kCffStdStringsCount)), s))
        throw ps::Error("Can't get string with given SID");
    return s;
}

// CFFFontBaseParser state shared by the top / private dictionary readers.
struct DictState {
    std::vector<CffNumber> stack;
    std::int64_t private_offset = 0, private_size = 0;
    std::int64_t charstrings_offset = 0, charset_offset = 0, encoding_offset = 0;
    std::int64_t fdarray_offset = 0, fdselect_offset = 0;
    std::int64_t subrs_offset = -1;
    int default_width = 0, nominal_width = 0;
    std::optional<std::array<float, 6>> font_matrix;
    bool contains_ros = false;
    int charstring_type = 2;
};

const CffNumber& last(DictState& s, std::size_t back = 1) {
    if (s.stack.size() < back) throw ps::Error("Error with stack in processing Top DICT in CFF file");
    return s.stack[s.stack.size() - back];
}

// readTopDictUnit; `cid` selects CFFCIDFontProgram's two-byte operators, else CFFType1's one-byte ones.
void top_dict_unit(CffReader& r, CffData& d, DictState& s, bool cid, bool base_only) {
    int next = r.peek();
    if ((next > 27 && next < 31) || (next > 31 && next < 255)) {
        s.stack.push_back(r.number());
        return;
    }
    r.byte();
    if (next >= 22) return;
    switch (next) {
        case 4:
            sid_string(d, last(s).integer);   // Weight: an invalid SID ends the parse
            s.stack.clear();
            break;
        case 15:
            s.charset_offset = last(s).integer;
            s.stack.clear();
            break;
        case 17:
            s.charstrings_offset = last(s).integer;
            s.stack.clear();
            break;
        case 18:
            s.private_size = last(s, 2).integer;
            s.private_offset = last(s).integer;
            s.stack.clear();
            break;
        case 12: {
            int n2 = r.byte();
            switch (n2) {
                case 7: {
                    std::array<float, 6> m{};
                    for (std::size_t i = 0; i < 6; ++i) {
                        if (i >= s.stack.size()) throw ps::Error("Error with stack in processing Top DICT");
                        m[i] = s.stack[i].real;
                    }
                    s.font_matrix = m;
                    s.stack.clear();
                    break;
                }
                case 6:
                    s.charstring_type = static_cast<int>(last(s).integer);
                    s.stack.clear();
                    break;
                case 30: s.contains_ros = true; break;
                default:
                    if (cid && !base_only) {
                        if (n2 == 36) s.fdarray_offset = last(s).integer;
                        else if (n2 == 37) s.fdselect_offset = last(s).integer;
                    }
                    s.stack.clear();
            }
            break;
        }
        default:
            if (!cid && !base_only && next == 16) s.encoding_offset = last(s).integer;
            s.stack.clear();
    }
}

void private_dict_unit(CffReader& r, DictState& s) {
    int next = r.peek();
    if ((next > 27 && next < 31) || (next > 31 && next < 255)) {
        s.stack.push_back(r.number());
        return;
    }
    r.byte();
    if (next >= 22) return;
    switch (next) {
        case 20:
            if (!s.stack.empty()) {
                s.default_width = static_cast<int>(s.stack.back().integer);
                s.stack.clear();
            }
            break;
        case 21:
            if (!s.stack.empty()) {
                s.nominal_width = static_cast<int>(s.stack.back().integer);
                s.stack.clear();
            }
            break;
        case 19:
            if (!s.stack.empty()) {
                s.subrs_offset = s.stack.back().integer + s.private_offset;
                s.stack.clear();
            }
            break;
        default: s.stack.clear();
    }
}

int subr_bias(int charstring_type, int n) {
    if (charstring_type == 1) return 0;
    if (n < 1240) return 107;
    if (n < 33900) return 1131;
    return 32768;
}

bool is_cid_font(const CffData& d, const CffIndex& top, std::int64_t tb, std::int64_t te) {
    std::string td;
    bool got = top.get(0, td);
    auto contains_ros = [&]() {
        try {
            CffReader r(d.bytes);
            CffData tmp;
            tmp.defined_names = d.defined_names;
            DictState s;
            r.seek(tb);
            while (static_cast<std::int64_t>(r.pos()) < te) top_dict_unit(r, tmp, s, false, true);
            return s.contains_ros;
        } catch (std::exception&) {
            return false;
        }
    };
    if (!got || td.size() <= 4) return contains_ros();
    int b = static_cast<unsigned char>(td[4]);
    std::size_t ros;
    if (b < 247 && b > 31) ros = 5;
    else if (b > 246 && b < 255) ros = 6;
    else if (b == 28) ros = 7;
    else if (b == 29) ros = 9;
    else return contains_ros();
    if (ros + 1 >= td.size()) return contains_ros();
    if (td[ros] == 12 && td[ros + 1] == 30) return true;
    return contains_ros();
}

void parse_type1(CffReader& r, CffData& d, std::int64_t tb, std::int64_t te) {
    DictState s;
    s.font_matrix = kDefaultMatrix;
    r.seek(tb);
    while (static_cast<std::int64_t>(r.pos()) < te) top_dict_unit(r, d, s, false, false);
    s.stack.clear();
    r.seek(s.private_offset);
    while (static_cast<std::int64_t>(r.pos()) < s.private_offset + s.private_size) private_dict_unit(r, s);
    CffIndex local;
    bool has_local = false;
    int bias = 0;
    if (s.subrs_offset != -1) {
        std::size_t save = r.pos();
        r.seek(s.subrs_offset);
        local = r.index();
        has_local = true;
        r.seek(static_cast<std::int64_t>(save));
        bias = subr_bias(s.charstring_type, local.count);
    }
    r.seek(s.charstrings_offset);
    d.charstrings = r.index();
    d.n_glyphs = d.charstrings.count;
    r.seek(s.encoding_offset);
    // readEncoding
    if (s.encoding_offset == 0) {
        d.std_encoding = true;
    } else if (s.encoding_offset == 1) {
        d.expert_encoding = true;
    } else {
        int format = r.byte();
        auto supplements = [&]() {
            int n = r.byte();
            for (int i = 0; i < n; ++i) {
                int code = r.byte();
                int glyph = r.card16();
                d.encoding[code] = glyph;
            }
        };
        if (format == 0 || format == 128) {
            int amount = r.byte();
            for (int i = 0; i < amount; ++i) d.encoding[r.byte()] = i;
            if (format == 128) supplements();
        } else if (format == 1 || format == 129) {
            int amount = r.byte();
            int ptr = 0;
            for (int i = 0; i < amount; ++i) {
                int first = r.byte();
                int nleft = r.byte();
                for (int j = 0; j <= nleft; ++j)
                    if (first + j < 256) d.encoding[first + j] = ptr++;
            }
            if (format == 129) supplements();
        }
    }
    r.seek(s.charset_offset);
    // readCharSet
    auto put = [&](const std::string& name, int gid) {
        if (!d.charset.count(name)) d.charset_order.push_back(name);
        d.charset[name] = gid;
        d.inverse[gid] = name;
    };
    put(sid_string(d, 0), 0);
    auto predefined = [&](const char* const* arr, std::size_t n) {
        for (int i = 0; i < d.n_glyphs; ++i) {
            if (static_cast<std::size_t>(i) >= n) throw ps::Error("predefined charset too short");
            put(arr[i], i);
        }
    };
    if (s.charset_offset == 0) predefined(fontdata::kCffIsoAdobeCharset, fontdata::kCffIsoAdobeCharsetCount);
    else if (s.charset_offset == 1) predefined(fontdata::kCffExpertCharset, fontdata::kCffExpertCharsetCount);
    else if (s.charset_offset == 2) predefined(fontdata::kCffExpertSubsetCharset, fontdata::kCffExpertSubsetCharsetCount);
    else {
        int format = r.byte();
        if (format == 0) {
            for (int i = 1; i < d.n_glyphs; ++i) put(sid_string(d, r.card16()), i);
        } else if (format == 1 || format == 2) {
            int ptr = 1;
            while (ptr < d.n_glyphs) {
                int first = r.card16();
                int nleft = format == 1 ? r.byte() : r.card16();
                for (int i = 0; i <= nleft; ++i) put(sid_string(d, first + i), ptr++);
            }
        } else {
            throw ps::Error("Can't process format of CharSet in CFF file");
        }
    }
    // readWidths
    d.charstring_type = s.charstring_type;
    d.matrices = {*s.font_matrix};
    d.default_matrix = {*s.font_matrix == kDefaultMatrix};
    d.local_subrs = {local};
    d.has_local = {has_local};
    d.bias = {bias};
    d.default_widths = {s.default_width};
    d.nominal_widths = {s.nominal_width};
    d.use_fd_select = false;
    d.facts.charset_names = d.charset_order;
}

std::array<float, 6> multiply(const std::array<float, 6>& a, const std::array<float, 6>& b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
}

void parse_cid(CffReader& r, CffData& d, std::int64_t tb, std::int64_t te) {
    DictState s;   // CFFCIDFontProgram: fontMatrix starts null
    r.seek(tb);
    while (static_cast<std::int64_t>(r.pos()) < te) top_dict_unit(r, d, s, true, false);
    s.stack.clear();
    r.seek(s.charstrings_offset);
    d.charstrings = r.index();
    d.n_glyphs = d.charstrings.count;
    r.seek(s.charset_offset);
    d.cid_charset[0] = 0;
    int format = r.byte();
    if (format == 0) {
        for (int i = 1; i < d.n_glyphs; ++i) d.cid_charset[r.card16()] = i;
    } else if (format == 1 || format == 2) {
        int ptr = 1;
        while (ptr < d.n_glyphs) {
            int first = r.card16();
            int nleft = format == 1 ? r.byte() : r.card16();
            if (ptr + nleft >= d.n_glyphs) nleft = d.n_glyphs - ptr - 1;
            for (int i = 0; i <= nleft; ++i) d.cid_charset[first + i] = ptr++;
        }
    } else {
        d.default_charset = true;
    }
    r.seek(s.fdselect_offset);
    int fformat = r.byte();
    d.fd_select.assign(static_cast<std::size_t>(d.n_glyphs), 0);
    if (fformat == 0) {
        for (int i = 0; i < d.n_glyphs; ++i) d.fd_select[static_cast<std::size_t>(i)] = r.byte();
    } else if (fformat == 3) {
        int nranges = r.card16();
        int first = r.card16();
        for (int i = 0; i < nranges; ++i) {
            int fd = r.byte();
            int after = r.card16();
            for (int j = first; j < after && j < static_cast<int>(d.fd_select.size()); ++j)
                if (j >= 0) d.fd_select[static_cast<std::size_t>(j)] = fd;
                else throw ps::Error("Can't parse FDSelect in CFF file");
            first = after;
        }
    } else {
        throw ps::Error("Can't parse format of FDSelect in CFF file");
    }
    r.seek(s.fdarray_offset);
    CffIndex fdi = r.index();
    std::size_t n = static_cast<std::size_t>(fdi.count);
    d.matrices.assign(n, kDefaultMatrix);
    d.default_matrix.assign(n, true);
    d.local_subrs.assign(n, CffIndex{});
    d.has_local.assign(n, false);
    d.bias.assign(n, 0);
    d.default_widths.assign(n, 0);
    d.nominal_widths.assign(n, 0);
    std::optional<std::array<float, 6>> top_matrix = s.font_matrix;
    for (std::size_t i = 0; i < n; ++i) {
        s.font_matrix.reset();
        std::int64_t from = fdi.offsets[i] + s.fdarray_offset + fdi.offset_shift - 1;
        std::int64_t to = fdi.offsets[i + 1] + s.fdarray_offset + fdi.offset_shift - 1;
        s.stack.clear();
        std::size_t save = r.pos();
        r.seek(from);
        while (static_cast<std::int64_t>(r.pos()) < to) top_dict_unit(r, d, s, true, false);
        r.seek(static_cast<std::int64_t>(save));
        // readPrivateDict
        s.stack.clear();
        s.nominal_width = 0;
        s.default_width = 0;
        save = r.pos();
        r.seek(s.private_offset);
        while (static_cast<std::int64_t>(r.pos()) < s.private_offset + s.private_size) private_dict_unit(r, s);
        r.seek(static_cast<std::int64_t>(save));
        if (s.subrs_offset != -1) {
            std::size_t sv = r.pos();
            r.seek(s.subrs_offset);
            d.local_subrs[i] = r.index();
            d.has_local[i] = true;
            r.seek(static_cast<std::int64_t>(sv));
            d.bias[i] = subr_bias(s.charstring_type, d.local_subrs[i].count);
        }
        d.nominal_widths[i] = s.nominal_width;
        d.default_widths[i] = s.default_width;
        std::array<float, 6> m;
        if (top_matrix && s.font_matrix) m = multiply(*top_matrix, *s.font_matrix);
        else if (top_matrix) m = *top_matrix;
        else if (s.font_matrix) m = *s.font_matrix;
        else m = kDefaultMatrix;
        d.matrices[i] = m;
        d.default_matrix[i] = m == kDefaultMatrix;
    }
    d.charstring_type = s.charstring_type;
    d.use_fd_select = true;
}

}  // namespace

float CffData::width_from_gid(int gid) {
    auto compute = [&](int g) -> float {
        std::optional<CffNumber> w;
        bool failed = false;
        try {
            std::string cs;
            if (!charstrings.get(g, cs)) cs.clear();
            if (charstring_type == 1) {
                w = type1_charstring_width(cs, nullptr);
            } else if (charstring_type == 2) {
                std::size_t fd = 0;
                if (use_fd_select) {
                    if (g < 0 || static_cast<std::size_t>(g) >= fd_select.size()) throw ps::Error("fdSelect");
                    fd = static_cast<std::size_t>(fd_select[static_cast<std::size_t>(g)]);
                }
                if (fd >= local_subrs.size()) throw ps::Error("local subrs");
                w = type2_charstring_width(cs, has_local[fd] ? &local_subrs[fd] : nullptr, bias[fd], &global_subrs,
                                           subr_bias(2, global_subrs.count));
            } else {
                failed = true;
            }
        } catch (std::exception&) {
            failed = true;
        }
        if (failed) w = CffNumber::of_real(-1.0f);
        // getActualWidth (ArrayIndexOutOfBoundsException -> -1)
        std::size_t fd = 0;
        if (use_fd_select) {
            if (g < 0 || static_cast<std::size_t>(g) >= fd_select.size()) return -1.0f;
            fd = static_cast<std::size_t>(fd_select[static_cast<std::size_t>(g)]);
        }
        if (fd >= matrices.size()) return -1.0f;
        float res;
        if (!w) res = static_cast<float>(default_widths[fd]);
        else res = (w->is_integer ? static_cast<float>(w->integer) : w->real) + static_cast<float>(nominal_widths[fd]);
        if (!default_matrix[fd]) res *= (matrices[fd][0] * 1000.0f);
        return res;
    };
    if (is_subset) {
        if (gid >= 0 && static_cast<std::size_t>(gid) < subset_widths.size()) return subset_widths[static_cast<std::size_t>(gid)];
        return -1.0f;
    }
    auto it = general_widths.find(gid);
    if (it != general_widths.end()) return it->second;
    float v = compute(gid);
    general_widths.emplace(gid, v);
    return v;
}

std::shared_ptr<CffData> parse_cff(std::string_view data, bool is_subset) {
    auto d = std::make_shared<CffData>();
    d->bytes.assign(data);
    d->is_subset = is_subset;
    CffReader r(d->bytes);
    try {
        r.byte();
        r.byte();
        int hdr = r.byte();
        r.seek(hdr);
        r.index();   // Name INDEX
        std::int64_t top_offset = static_cast<std::int64_t>(r.pos());
        CffIndex top = r.index();
        if (top.count == 0) throw ps::Error("top DICT INDEX is empty");
        d->defined_names = r.index();
        d->global_subrs = r.index();
        std::int64_t tb = top_offset + top.offsets[0] - 1 + top.offset_shift;
        std::int64_t te = top_offset + top.offsets[1] - 1 + top.offset_shift;
        d->cid = is_cid_font(*d, top, tb, te);
        d->font_created = true;
        if (d->cid) parse_cid(r, *d, tb, te);
        else parse_type1(r, *d, tb, te);
        if (is_subset) {
            // CharStringsWidths.parseSubsetWidths
            d->is_subset = false;
            std::vector<float> w(static_cast<std::size_t>(d->charstrings.count));
            for (int i = 0; i < d->charstrings.count; ++i) w[static_cast<std::size_t>(i)] = d->width_from_gid(i);
            d->general_widths.clear();
            d->subset_widths = std::move(w);
            d->is_subset = true;
        }
        d->parsed = true;
    } catch (std::exception&) {
        d->parsed = false;
    }
    d->facts.cid_keyed = d->cid;
    d->facts.charstring_count = d->charstrings.count;
    return d;
}

namespace {

class CffProgram final : public FontProgram {
public:
    CffProgram(std::shared_ptr<CffData> d, std::shared_ptr<const CMap> cmap) : d_(std::move(d)), cmap_(std::move(cmap)) {}
    Kind kind() const override { return d_->cid ? Kind::CFFCID : Kind::CFFType1; }
    bool successful() const override { return d_->font_created && d_->parsed; }

    float width(int code) override {
        if (!d_->parsed) return -1;
        if (d_->cid) {
            if (!cmap_) return -1;
            int cid = cmap_->to_cid(code);
            int gid;
            if (!gid_of_cid(cid, gid)) return -1;
            return d_->width_from_gid(gid);
        }
        if (cmap_) {
            int gid = cmap_->to_cid(code);
            float r = d_->width_from_gid(gid);
            if (r != -1.0f) return r;
            return d_->width_from_gid(0);
        }
        if (!d_->std_encoding && !d_->expert_encoding) {
            if (code >= 0 && code < 256) return width_by_gid(d_->encoding[code] + 1, true);
        }
        auto nm = glyph_name(code);
        return width(nm ? std::string_view(*nm) : std::string_view());
    }
    float width(std::string_view name) override {
        if (d_->cid) return 0;
        if (!d_->parsed) return -1;
        auto it = d_->charset.find(std::string(name));
        return width_by_gid(it == d_->charset.end() ? 0 : it->second, it != d_->charset.end());
    }
    std::optional<std::string> glyph_name(int code) override {
        if (d_->cid || !d_->font_created) return std::nullopt;
        if (code < 0) return std::string(".notdef");
        if (d_->std_encoding) {
            if (code < static_cast<int>(fontdata::kCffStdEncodingCount))
                return std::string(fontdata::kCffStdStrings[fontdata::kCffStdEncoding[code]]);
        } else if (d_->expert_encoding) {
            if (code < static_cast<int>(fontdata::kCffExpertEncodingCount))
                return std::string(fontdata::kCffStdStrings[fontdata::kCffExpertEncoding[code]]);
        } else if (code < 256) {
            int gid = d_->encoding[code] + 1;
            if (gid < static_cast<int>(d_->inverse.size())) {
                auto it = d_->inverse.find(gid);
                if (it != d_->inverse.end()) return it->second;
                return std::nullopt;
            }
        }
        return std::string(".notdef");
    }
    bool contains_code(int code) override {
        if (d_->cid) {
            if (!cmap_) return false;
            return cmap_->contains_code(code) && contains_cid(cmap_->to_cid(code));
        }
        auto nm = glyph_name(code);
        return nm && d_->charset.count(*nm);
    }
    bool contains_glyph(std::string_view name) override {
        if (d_->cid) return false;
        return d_->charset.count(std::string(name)) != 0;
    }
    bool contains_cid(int cid) override {
        if (!d_->cid) return false;
        auto it = d_->cid_charset.find(cid);
        return it != d_->cid_charset.end() && it->second != 0;
    }
    std::vector<int> cid_list() override {
        std::vector<int> r;
        if (!d_->cid) return r;
        for (const auto& [cid, gid] : d_->cid_charset) r.push_back(cid);
        return r;
    }
    CffData& data() { return *d_; }

private:
    bool gid_of_cid(int cid, int& gid) const {
        if (d_->default_charset) {
            gid = cid;
            return true;
        }
        auto it = d_->cid_charset.find(cid);
        if (it == d_->cid_charset.end()) return false;
        gid = it->second;
        return true;
    }
    float width_by_gid(int gid, bool known) {
        if (!known || gid < 0 || gid >= d_->charstrings.count) return d_->width_from_gid(0);
        return d_->width_from_gid(gid);
    }
    std::shared_ptr<CffData> d_;
    std::shared_ptr<const CMap> cmap_;
};

}  // namespace

std::unique_ptr<FontProgram> make_cff_program(std::shared_ptr<CffData> d, std::shared_ptr<const CMap> external_cmap) {
    return std::make_unique<CffProgram>(std::move(d), std::move(external_cmap));
}

const CffFacts* cff_facts(FontProgram* p) {
    if (!p) return nullptr;
    auto* c = dynamic_cast<CffProgram*>(p->inner());
    return c ? &c->data().facts : nullptr;
}

FontProgram* cff_type1_of(FontProgram* p) {
    if (!p) return nullptr;
    auto* c = dynamic_cast<CffProgram*>(p->inner());
    if (!c || !c->data().font_created || c->data().cid) return nullptr;
    return c;
}

float cff_width_from_gid(FontProgram* p, int gid) {
    auto* c = p ? dynamic_cast<CffProgram*>(p->inner()) : nullptr;
    return c ? c->data().width_from_gid(gid) : -1.0f;
}

int cff_charstring_count(FontProgram* p) {
    auto* c = p ? dynamic_cast<CffProgram*>(p->inner()) : nullptr;
    return c ? c->data().charstrings.count : 0;
}

float cff_type1_width_from_gid(FontProgram* p, int gid) {
    auto* c = dynamic_cast<CffProgram*>(p);
    return c ? c->data().width_from_gid(gid) : -1.0f;
}

bool cff_type1_contains_gid(FontProgram* p, int gid) {
    auto* c = dynamic_cast<CffProgram*>(p);
    return c && gid >= 0 && c->data().charstrings.count > gid;
}

std::optional<std::string_view> opentype_cff_table(std::string_view data) {
    // OpenTypeFontProgram.getCFFTable (reads past the end yield 0xFF bytes, never an exception)
    std::size_t p = 0;
    auto rd = [&]() -> int { return p < data.size() ? static_cast<unsigned char>(data[p++]) : (p++, 0xFF); };
    auto ushort = [&]() { int h = (rd() & 0xFF) << 8; return h | (rd() & 0xFF); };
    auto ulong = [&]() { std::int64_t r = ushort(); return (r << 16) | ushort(); };
    p = std::min<std::size_t>(4, data.size());
    int n = ushort();
    p = std::min<std::size_t>(p + 6, std::max(p, data.size()));
    for (int i = 0; i < n; ++i) {
        std::int64_t name = ulong();
        ulong();
        std::int64_t off = ulong();
        std::int64_t len = ulong();
        if (name == 1128678944) {
            if (off < 0 || static_cast<std::uint64_t>(off) > data.size()) return std::string_view();
            std::size_t l = static_cast<std::size_t>(std::min<std::int64_t>(len, static_cast<std::int64_t>(data.size()) - off));
            return data.substr(static_cast<std::size_t>(off), l);
        }
    }
    return std::nullopt;
}

}  // namespace eqx::fontimpl
