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
// TrueType programs: org.verapdf.pd.font.truetype.{TrueTypeFontParser, TrueTypeCmapTable, TrueTypeHmtxTable,
// TrueTypePostTable, BaseTrueTypeProgram, TrueTypeFontProgram, CIDFontType2Program}.
#include <algorithm>
#include <cmath>

#include "fonts/cmap.h"
#include "fonts/data.h"
#include "fonts/programs.h"
#include "fonts/ps.h"

namespace eqx::fontimpl {

namespace {

// SeekableInputStream over the decoded program.
class In {
public:
    explicit In(std::string_view d) : d_(d) {}
    std::size_t pos() const { return p_; }
    std::size_t size() const { return d_.size(); }
    void seek(std::int64_t p) {
        if (p < 0 || static_cast<std::uint64_t>(p) > d_.size()) throw ps::Error("seek beyond end");
        p_ = static_cast<std::size_t>(p);
    }
    void skip(std::size_t n) { p_ += std::min(n, d_.size() - p_); }
    int byte() {
        if (p_ >= d_.size()) throw ps::Error("End of file is reached");
        return static_cast<unsigned char>(d_[p_++]);
    }
    int ushort() {
        int h = byte() << 8;
        return h | byte();
    }
    std::int64_t ulong() {
        std::int64_t r = ushort();
        return (r << 16) | ushort();
    }
    // read(buf, n): partial reads leave zeros
    std::string read(std::size_t n) {
        std::string s(n, '\0');
        std::size_t k = std::min(n, d_.size() - p_);
        for (std::size_t i = 0; i < k; ++i) s[i] = d_[p_ + i];
        p_ += k;
        return s;
    }

private:
    std::string_view d_;
    std::size_t p_ = 0;
};

constexpr std::int64_t tag(const char* t) {
    return (static_cast<std::int64_t>(static_cast<unsigned char>(t[0])) << 24) |
           (static_cast<std::int64_t>(static_cast<unsigned char>(t[1])) << 16) |
           (static_cast<std::int64_t>(static_cast<unsigned char>(t[2])) << 8) |
           static_cast<std::int64_t>(static_cast<unsigned char>(t[3]));
}

void read_cmap(In& in, std::int64_t offset, TrueTypeTables& t) {
    in.seek(offset);
    in.skip(2);
    int n = in.ushort();
    struct Rec {
        int plat, enc;
        std::int64_t off;
    };
    std::vector<Rec> recs;
    for (int i = 0; i < n; ++i) {
        int p = in.ushort();
        int e = in.ushort();
        std::int64_t o = in.ulong();
        recs.push_back({p, e, o});
        t.cmaps.push_back(CmapSubtable{p, e, {}, -1});
    }
    for (std::size_t r = 0; r < recs.size(); ++r) {
        CmapSubtable& cm = t.cmaps[r];
        in.seek(recs[r].off + offset);
        int format = in.ushort();
        if (format == 0) {
            in.skip(4);
            for (int i = 0; i < 256; ++i) cm.put(i, in.byte());
        } else if (format == 4) {
            in.skip(4);
            int seg = in.ushort() / 2;
            in.skip(6);
            std::vector<int> end_code(static_cast<std::size_t>(seg)), start_code(end_code), delta(end_code), range(end_code);
            for (int i = 0; i < seg; ++i) end_code[static_cast<std::size_t>(i)] = in.ushort();
            in.skip(2);
            for (int i = 0; i < seg; ++i) start_code[static_cast<std::size_t>(i)] = in.ushort();
            for (int i = 0; i < seg; ++i) delta[static_cast<std::size_t>(i)] = in.ushort();
            for (int i = 0; i < seg; ++i) range[static_cast<std::size_t>(i)] = in.ushort();
            std::int64_t range_begin = static_cast<std::int64_t>(in.pos());
            std::int64_t work = 0;
            for (int i = 0; i < seg; ++i) {
                std::size_t k = static_cast<std::size_t>(i);
                // Overlapping full-range segments would make veraPDF loop for billions of entries; a damaged
                // table beyond this bound is treated as unparsable (documented deviation).
                work += std::max(0, end_code[k] - start_code[k] + 1);
                if (work > 50'000'000) throw ps::Error("cmap format 4 table too large");
                if (range[k] == 0) {
                    for (int j = start_code[k]; j <= end_code[k]; ++j) cm.put(j, (delta[k] + j) % 65536);
                } else if (start_code[k] != 65535 && end_code[k] != 65535) {
                    for (int j = 0; j <= end_code[k] - start_code[k]; ++j) {
                        std::int64_t go = range_begin + (static_cast<std::int64_t>(range[k] / 2) + j + (i - seg)) * 2;
                        in.seek(go);
                        int g = in.ushort();
                        if (g != 0) g = (g + delta[k]) % 65536;
                        cm.put(j + start_code[k], g);
                    }
                }
            }
        } else if (format == 6) {
            in.skip(4);
            int first = in.ushort();
            int count = in.ushort();
            for (int i = 0; i < count; ++i) cm.put(first + i, in.ushort());
        }
        // format 2: readHighByteMapping is empty; other formats are ignored
    }
}

void read_post(In& in, std::int64_t offset, std::int64_t length, int num_glyphs, TrueTypeTables& t) {
    in.seek(offset);
    std::string fx = in.read(4);
    std::int32_t integer = 0;
    for (int i = 0; i < 3; ++i) {
        integer |= static_cast<unsigned char>(fx[static_cast<std::size_t>(i)]);
        integer = static_cast<std::int32_t>(static_cast<std::uint32_t>(integer) << 8);
    }
    integer |= static_cast<unsigned char>(fx[3]);
    float format = static_cast<float>(integer) / 65536.0f;
    in.skip(4 + 2 + 2 + 4 + 4 + 4 + 4 + 4);
    if (format == 2.0f) {
        int n = in.ushort();
        if (n != num_glyphs) num_glyphs = n;
        std::vector<int> idx(static_cast<std::size_t>(num_glyphs));
        for (int i = 0; i < num_glyphs; ++i) idx[static_cast<std::size_t>(i)] = in.ushort();
        std::vector<std::string> strings;
        while (static_cast<std::int64_t>(in.pos()) < offset + length) {
            int len = in.byte();
            strings.push_back(in.read(static_cast<std::size_t>(len)));
        }
        const int nmac = static_cast<int>(fontdata::kMacGlyphNamesCount);
        for (int i = 0; i < num_glyphs; ++i) {
            int g = idx[static_cast<std::size_t>(i)];
            if (g < nmac) {
                const char* nm = fontdata::kMacGlyphNames[g];
                if (nm) t.post[nm] = i;
            } else {
                int k = g - nmac;
                if (k >= 0 && k < static_cast<int>(strings.size())) t.post[strings[static_cast<std::size_t>(k)]] = i;
            }
        }
    } else if (format == 2.5f) {
        const int nmac = static_cast<int>(fontdata::kMacGlyphNamesCount);
        for (int i = 0; i < num_glyphs; ++i) {
            int off = static_cast<signed char>(in.byte());
            int k = off + i;
            if (k < 0 || k >= nmac) throw ps::Error("Error in reading post table");
            t.post[fontdata::kMacGlyphNames[k]] = i;
        }
    }
}

}  // namespace

float TrueTypeTables::width_with_check(int gid) const {
    if (gid >= 0 && static_cast<std::size_t>(gid) < widths.size()) return widths[static_cast<std::size_t>(gid)];
    if (widths.empty()) return 0;
    if (gid < num_glyphs) return widths.back();   // "case of monospaced fonts"
    return widths[0];
}

std::shared_ptr<const TrueTypeTables> parse_truetype(std::string_view data) {
    auto t = std::make_shared<TrueTypeTables>();
    In in(data);
    try {
        in.skip(4);
        int num_tables = in.ushort();
        in.skip(6);
        std::int64_t cmap_off = -1, hhea_off = -1, hmtx_off = -1, head_off = -1, post_off = -1, post_len = 0,
                     maxp_off = -1;
        for (int i = 0; i < num_tables; ++i) {
            std::int64_t name = in.ulong();
            in.ulong();
            std::int64_t off = in.ulong();
            std::int64_t len = in.ulong();
            if (name == tag("cmap")) cmap_off = off;
            else if (name == tag("hhea")) hhea_off = off;
            else if (name == tag("hmtx")) hmtx_off = off;
            else if (name == tag("head")) head_off = off;
            else if (name == tag("post")) {
                post_off = off;
                post_len = len;
            } else if (name == tag("maxp")) maxp_off = off;
        }
        if (head_off >= 0) {
            in.seek(head_off);
            in.skip(18);
            t->units_per_em = in.ushort();
        }
        if (hhea_off < 0) throw ps::Error("True type font doesn't contain hhea table.");
        in.seek(hhea_off);
        in.skip(4 + 2 + 2);
        in.skip(26);
        int n_hmetrics = in.ushort();
        if (hmtx_off < 0) throw ps::Error("True type font doesn't contain hmtx table.");
        in.seek(hmtx_off);
        std::vector<int> adv;
        adv.reserve(static_cast<std::size_t>(n_hmetrics));
        for (int i = 0; i < n_hmetrics; ++i) {
            adv.push_back(in.ushort());
            in.skip(2);
        }
        if (cmap_off >= 0) {
            t->has_cmap = true;
            read_cmap(in, cmap_off, *t);
        }
        if (maxp_off >= 0) {
            t->has_maxp = true;
            in.seek(maxp_off);
            in.skip(4);
            t->num_glyphs = in.ushort();
        } else {
            t->has_maxp = true;   // TrueTypeMaxpTable(numberOfHMetrics)
            t->num_glyphs = static_cast<int>(adv.size());
        }
        if (post_off >= 0) {
            t->has_post = true;
            read_post(in, post_off, post_len, t->num_glyphs, *t);
        }
        float quotient = 1000.0f / static_cast<float>(t->units_per_em);
        t->widths.resize(adv.size());
        for (std::size_t i = 0; i < adv.size(); ++i) t->widths[i] = static_cast<float>(adv[i]) * quotient;
        t->parsed = true;
    } catch (std::exception&) {
        t->parsed = false;
    }
    return t;
}

namespace {

int agl_symbol_code(std::string_view name) {
    const fontdata::AglEntry* e = fontdata::agl_find(name);
    return e ? static_cast<int>(fontdata::kAglPool[e->pool]) : -1;
}

class TrueTypeProgram final : public FontProgram {
public:
    TrueTypeProgram(std::shared_ptr<const TrueTypeTables> t, bool symbolic, const SimpleEncodingInfo& enc)
        : t_(std::move(t)), symbolic_(symbolic), enc_present_(enc.present) {
        // TrueTypeFontProgram.parseFont: createCIDToNameTable after a successful base parse.
        if (t_->parsed && !symbolic_ && enc_present_) {
            map_.assign(256, std::nullopt);
            has_map_ = true;
            auto copy = [&](const char* const* arr) {
                for (int i = 0; i < 256; ++i)
                    map_[static_cast<std::size_t>(i)] = arr[i] ? std::optional<std::string>(arr[i]) : std::nullopt;
            };
            if (enc.type == SimpleEncodingInfo::Type::Name) {
                if (enc.name == "MacRomanEncoding") copy(fontdata::kMacRoman);
                else if (enc.name == "WinAnsiEncoding") copy(fontdata::kWinAnsi);
                else threw_ = true;
            } else if (enc.type == SimpleEncodingInfo::Type::Dict) {
                if (enc.base_known) {
                    if (enc.base == "WinAnsiEncoding") copy(fontdata::kWinAnsi);
                    else if (enc.base == "MacRomanEncoding") copy(fontdata::kMacRoman);
                    else if (enc.base == "MacExpertEncoding") copy(fontdata::kMacExpert);
                    else copy(fontdata::kStandard);
                } else {
                    copy(fontdata::kStandard);
                }
                for (const auto& [k, v] : enc.differences) {
                    if (k < 0) {   // ArrayIndexOutOfBoundsException
                        threw_ = true;
                        break;
                    }
                    if (k < 256) map_[static_cast<std::size_t>(k)] = v;
                }
                if (!threw_)
                    for (int i = 0; i < 256; ++i)
                        if (map_[static_cast<std::size_t>(i)] == std::string(".notdef"))
                            map_[static_cast<std::size_t>(i)] = fontdata::kStandard[i]
                                                                    ? std::optional<std::string>(fontdata::kStandard[i])
                                                                    : std::nullopt;
            } else {
                threw_ = true;
            }
            if (threw_ && enc.type != SimpleEncodingInfo::Type::Dict) map_.assign(256, std::nullopt);
        }
    }
    Kind kind() const override { return Kind::TrueType; }
    bool successful() const override { return t_->parsed; }
    bool gf_parsed() const override { return t_->parsed && !threw_; }

    bool contains_code(int code) override {
        if (!symbolic_ && enc_present_) {
            std::optional<std::string> glyph;
            if (has_map_ && code >= 0 && code < static_cast<int>(map_.size())) glyph = map_[static_cast<std::size_t>(code)];
            else glyph = ".notdef";
            if (!glyph) return false;   // null glyph name: Java NPE in AGL lookup; treated as absent
            if (*glyph == ".notdef") return false;
            int gid = gid_from_cmaps(*glyph);
            if (gid == 0) gid = gid_from_post(*glyph);
            return gid > 0 && gid < t_->num_glyphs;
        }
        if (t_->cmap(3, 0)) return cmap30_contains(code) && gid_from_30(code) != 0;
        const CmapSubtable* c10 = t_->cmap(1, 0);
        return c10 && c10->contains(code) && c10->glyph(code) != 0;
    }

    float width(int code) override {
        if (symbolic_ || !enc_present_) return width_symbolic(code);
        float w;
        if (has_map_ && code >= 0 && code < static_cast<int>(map_.size()) && map_[static_cast<std::size_t>(code)])
            w = width(*map_[static_cast<std::size_t>(code)]);
        else if (has_map_ && code >= 0 && code < static_cast<int>(map_.size()))
            w = -1;   // null name
        else
            w = width(std::string_view(".notdef"));
        if (w == -1) {
            if (!t_->has_cmap) return 0;
            return t_->width_with_check(t_->cmap_gid(code));
        }
        return w;
    }

    std::optional<std::string> glyph_name(int code) override {
        if (!symbolic_ && enc_present_ && has_map_ && code >= 0 && code < static_cast<int>(map_.size()))
            return map_[static_cast<std::size_t>(code)];
        if (symbolic_ || !enc_present_) return std::string(" ");   // "toUnicode should not be checked"
        return std::nullopt;
    }

    float width(std::string_view glyph_name) override {
        if (symbolic_ || !enc_present_) return -1;
        int gid = gid_from_cmaps(glyph_name);
        if (gid == 0) gid = gid_from_post(glyph_name);
        bool mapping = (t_->has_cmap && !t_->cmaps.empty()) ||
                       (t_->has_post && t_->post.count(std::string(glyph_name)));
        if (mapping) return t_->width_with_check(gid);
        return -1;
    }

    bool contains_glyph(std::string_view glyph_name) override {
        if (!symbolic_ && enc_present_) {
            int gid = gid_from_cmaps(glyph_name);
            return gid >= 0 && gid < t_->num_glyphs;
        }
        return false;
    }
    bool contains_cid(int) override { return false; }
    std::vector<int> cid_list() override { return {}; }

    const TrueTypeTables& tables() const { return *t_; }
    bool symbolic() const { return symbolic_; }

private:
    int gid_from_cmaps(std::string_view name) const {
        int sym = agl_symbol_code(name);
        if (const CmapSubtable* c31 = t_->cmap(3, 1)) {
            int g = c31->glyph(sym);
            if (g != 0) return g;
        }
        if (const CmapSubtable* c10 = t_->cmap(1, 0)) {
            int code = fontdata::mac_os_roman_code(name);
            if (code >= 0) return c10->glyph(code);
        }
        return 0;
    }
    int gid_from_post(std::string_view name) const {
        if (!t_->has_post) return 0;
        auto it = t_->post.find(std::string(name));
        return it == t_->post.end() ? 0 : it->second;
    }
    int high_byte_mask() const {
        const CmapSubtable* c30 = t_->cmap(3, 0);
        if (!c30) return -1;
        int m = c30->sample & 0xFF00;
        if (m == 0 || m == 0xF000 || m == 0xF100 || m == 0xF200) return m;
        return -1;
    }
    int gid_from_30(int code) const {
        int m = high_byte_mask();
        if (m < 0) return 0;
        return t_->cmap(3, 0)->glyph(m + code);
    }
    bool cmap30_contains(int code) const {
        int m = high_byte_mask();
        if (m < 0) return false;
        return t_->cmap(3, 0)->contains(m + code);
    }
    float width_symbolic(int code) const {
        int gid = gid_from_30(code);
        if (gid != 0) return t_->width_with_check(gid);
        if (const CmapSubtable* c10 = t_->cmap(1, 0)) return t_->width_with_check(c10->glyph(code));
        if (!t_->has_cmap) return 0;
        return t_->width_with_check(t_->cmap_gid(code));
    }

    std::shared_ptr<const TrueTypeTables> t_;
    bool symbolic_;
    bool enc_present_;
    bool has_map_ = false;
    bool threw_ = false;
    std::vector<std::optional<std::string>> map_;
};

class CidType2Program final : public FontProgram {
public:
    CidType2Program(std::shared_ptr<const TrueTypeTables> t, std::shared_ptr<const CMap> cmap, bool identity,
                    std::vector<int> map)
        : t_(std::move(t)), cmap_(std::move(cmap)), identity_(identity), map_(std::move(map)) {}
    Kind kind() const override { return Kind::CIDType2; }
    bool successful() const override { return t_->parsed; }
    float width(int code) override {
        int cid = cmap_ ? cmap_->to_cid(code) : 0;
        return t_->width_with_check(gid(cid));
    }
    float width(std::string_view) override { return 0; }
    bool contains_code(int code) override {
        if (!cmap_ || !cmap_->contains_code(code)) return false;
        return contains_cid(cmap_->to_cid(code));
    }
    bool contains_glyph(std::string_view) override { return false; }
    std::optional<std::string> glyph_name(int) override { return std::nullopt; }
    bool contains_cid(int cid) override {
        bool in = identity_ || (cid >= 0 && static_cast<std::size_t>(cid) < map_.size());
        if (in && cid != 0) return gid(cid) < t_->num_glyphs;
        return false;
    }
    std::vector<int> cid_list() override {
        std::vector<int> r;
        if (!identity_) {
            for (std::size_t i = 0; i < map_.size(); ++i)
                if (contains_cid(static_cast<int>(i))) r.push_back(static_cast<int>(i));
        } else {
            for (std::size_t i = 0; i < t_->widths.size(); ++i) r.push_back(static_cast<int>(i));
        }
        return r;
    }
    const TrueTypeTables& tables() const { return *t_; }

private:
    int gid(int cid) const {
        if (identity_) return cid;
        if (cid >= 0 && static_cast<std::size_t>(cid) < map_.size()) return map_[static_cast<std::size_t>(cid)];
        return 0;
    }
    std::shared_ptr<const TrueTypeTables> t_;
    std::shared_ptr<const CMap> cmap_;
    bool identity_;
    std::vector<int> map_;
};

}  // namespace

std::unique_ptr<FontProgram> make_truetype_program(std::shared_ptr<const TrueTypeTables> t, bool is_symbolic,
                                                   const SimpleEncodingInfo& enc) {
    return std::make_unique<TrueTypeProgram>(std::move(t), is_symbolic, enc);
}

std::unique_ptr<FontProgram> make_cid_type2_program(std::shared_ptr<const TrueTypeTables> t,
                                                    std::shared_ptr<const CMap> cmap, bool identity,
                                                    std::vector<int> cid_to_gid) {
    return std::make_unique<CidType2Program>(std::move(t), std::move(cmap), identity, std::move(cid_to_gid));
}

const TrueTypeTables* truetype_tables(FontProgram* p) {
    if (!p) return nullptr;
    FontProgram* in = p->inner();
    if (!in) return nullptr;
    if (auto* tt = dynamic_cast<TrueTypeProgram*>(in)) return &tt->tables();
    if (auto* ct = dynamic_cast<CidType2Program*>(in)) return &ct->tables();
    return nullptr;
}

}  // namespace eqx::fontimpl
