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
// Embedded font programs with the semantics of veraPDF 1.30.2's own parsers (org.verapdf.pd.font.*:
// truetype.TrueTypeFontProgram / CIDFontType2Program, cff.CFFFontProgram / CFFType1FontProgram /
// CFFCIDFontProgram, type1.Type1FontProgram, opentype.OpenTypeFontProgram). veraPDF does not use a
// general font library; glyph presence, widths and charsets follow its table parsers exactly (cmap formats
// 0/4/6 only, CFF encoding quirks, Type 1 charstrings without an hsbw width are not glyphs, ...), so these
// facts are computed by ports of those parsers rather than by FreeType.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eqx::fontimpl {

class CMap;

// veraPDF FontProgram.
class FontProgram {
public:
    enum class Kind : std::uint8_t { TrueType, CIDType2, CFFType1, CFFCID, Type1, OpenType };
    virtual ~FontProgram() = default;
    virtual Kind kind() const = 0;
    virtual float width(int code) = 0;                       // getWidth(int); -1 = unknown
    virtual float width(std::string_view glyph_name) = 0;    // getWidth(String)
    virtual bool contains_code(int code) = 0;
    virtual bool contains_cid(int cid) = 0;
    virtual bool contains_glyph(std::string_view glyph_name) = 0;
    virtual std::optional<std::string> glyph_name(int code) = 0;
    virtual bool successful() const = 0;                     // isSuccessfulParsing
    // GF*Font.fontProgramParsed: parseFont() completed without an exception and isSuccessfulParsing()
    // (TrueTypeFontProgram can be "successful" and still throw while building its encoding table).
    virtual bool gf_parsed() const { return successful(); }
    virtual std::vector<int> cid_list() = 0;                 // getCIDList
    // OpenTypeFontProgram.getFont / CFFFontProgram.getFont: the program that does the work.
    virtual FontProgram* inner() { return this; }
};

// ---- TrueType ------------------------------------------------------------------------------------------
struct CmapSubtable {
    int platform = 0, encoding = 0;
    std::unordered_map<int, int> map;
    int sample = -1;
    void put(int k, int v) {
        if (sample == -1) sample = k;
        map[k] = v;
    }
    int glyph(int code) const {
        auto it = map.find(code);
        return it == map.end() ? 0 : it->second;
    }
    bool contains(int code) const { return map.count(code) != 0; }
};

// Parsed tables (TrueTypeFontParser); shared by every program over the same bytes.
struct TrueTypeTables {
    bool parsed = false;          // BaseTrueTypeProgram.parseFont completed
    int units_per_em = 2048;
    std::vector<float> widths;    // hmtx advances * (1000f / unitsPerEm)
    bool has_cmap = false;
    std::vector<CmapSubtable> cmaps;
    bool has_maxp = false;
    int num_glyphs = 0;
    bool has_post = false;
    std::unordered_map<std::string, int> post;   // glyph name -> gid
    const CmapSubtable* cmap(int platform, int encoding) const {
        for (const auto& c : cmaps)
            if (c.platform == platform && c.encoding == encoding) return &c;
        return nullptr;
    }
    int cmap_gid(int code) const {   // TrueTypeCmapTable.getGID
        for (const auto& c : cmaps)
            if (c.contains(code)) return c.glyph(code);
        return 0;
    }
    float width_with_check(int gid) const;
};
std::shared_ptr<const TrueTypeTables> parse_truetype(std::string_view data);

// ---- CFF -------------------------------------------------------------------------------------------------
struct CffData;   // cff.cpp
std::shared_ptr<CffData> parse_cff(std::string_view data, bool is_subset);

// ---- Type 1 ----------------------------------------------------------------------------------------------
struct Type1Data {
    bool parsed = false;
    std::vector<std::string> encoding;          // 256 entries; "" = unset array element
    std::vector<bool> encoding_set;             // false = Java null (no Encoding array element)
    std::unordered_map<std::string, int> widths;   // glyphWidths (charstrings with an hsbw/sbw width)
};
std::shared_ptr<const Type1Data> parse_type1(std::string_view data);

// ---- program factories ----------------------------------------------------------------------------------
// TrueTypeFontProgram (simple TrueType font): is_symbolic = descriptor flag; encoding = /Encoding entry.
struct SimpleEncodingInfo {
    bool present = false;          // encoding.getDirectBase() != null
    enum class Type : std::uint8_t { Name, Dict, Other } type = Type::Other;
    std::string name;              // Name: the encoding name
    bool base_known = false;       // Dict: knownKey(BaseEncoding)
    std::optional<std::string> base;   // Dict: BaseEncoding as a name
    std::vector<std::pair<int, std::string>> differences;   // Dict: parsed Differences (in array order)
};
std::unique_ptr<FontProgram> make_truetype_program(std::shared_ptr<const TrueTypeTables> t, bool is_symbolic,
                                                   const SimpleEncodingInfo& enc);
std::unique_ptr<FontProgram> make_cid_type2_program(std::shared_ptr<const TrueTypeTables> t,
                                                    std::shared_ptr<const CMap> cmap, bool cid_to_gid_identity,
                                                    std::vector<int> cid_to_gid);
std::unique_ptr<FontProgram> make_cff_program(std::shared_ptr<CffData> d, std::shared_ptr<const CMap> external_cmap);
std::unique_ptr<FontProgram> make_type1_program(std::shared_ptr<const Type1Data> d);
// OpenTypeFontProgram: wraps the inner program; nullptr inner = parse failed.
std::unique_ptr<FontProgram> make_opentype_program(std::unique_ptr<FontProgram> inner);
// OpenTypeFontProgram.getCFFTable; nullopt when there is no "CFF " table.
std::optional<std::string_view> opentype_cff_table(std::string_view data);

// CFF program facts used by the font rules.
struct CffFacts {
    bool cid_keyed = false;
    std::vector<std::string> charset_names;      // CFFType1FontProgram.getCharSet (CID-keyed: empty)
    int charstring_count = 0;
};
const CffFacts* cff_facts(FontProgram* p);      // nullptr when p is not a CFF program
// CFFType1FontProgram.getCFFType1 (through OpenType/CFF wrappers); nullptr otherwise.
FontProgram* cff_type1_of(FontProgram* p);
float cff_type1_width_from_gid(FontProgram* cff_type1, int gid);
// CharStringsWidths.getWidth of any CFF program (diagnostics / tests); -1 when p is not CFF
float cff_width_from_gid(FontProgram* p, int gid);
// raw bytes of a CFF program and its CharStrings count (tests)
int cff_charstring_count(FontProgram* p);
bool cff_type1_contains_gid(FontProgram* cff_type1, int gid);
const Type1Data* type1_data(FontProgram* p);
const TrueTypeTables* truetype_tables(FontProgram* p);   // TrueType / CIDType2 (through OpenType)

}  // namespace eqx::fontimpl
