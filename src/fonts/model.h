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
// veraPDF 1.30.2 font model (org.verapdf.pd.font.PDFont & subclasses, PDCMap, GFGlyph properties) behind the
// Font interface the content interpreter uses. The fonts rule module reads the veraPDF properties from here.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cos.h"
#include "fonts/cmap.h"
#include "fonts/font.h"
#include "fonts/programs.h"

namespace eqx {
class Document;
}

namespace eqx::fontimpl {

// Per-document caches (veraPDF StaticResources: CMaps by "CMap N G", font programs by stream).
struct DocCache {
    std::unordered_map<std::uint64_t, std::shared_ptr<const CMap>> cmap_streams;
    std::unordered_map<std::uint64_t, std::shared_ptr<const TrueTypeTables>> truetype;
    std::unordered_map<std::uint64_t, std::shared_ptr<CffData>> cff;
    std::unordered_map<std::uint64_t, std::shared_ptr<const Type1Data>> type1;
    std::unordered_map<std::string, std::shared_ptr<FontProgram>> programs;   // FontProgramIDGenerator ids
    std::unordered_map<std::uint64_t, std::shared_ptr<const CMap>> merged;    // CMap streams with /UseCMap
    std::unordered_map<std::uint64_t, bool> flate_damaged;   // font program streams: their Flate data fails (pdfont.cpp)
};

// veraPDF PDCMap over a COS object (name, stream or anything else).
class PdCMap {
public:
    PdCMap(DocCache& cache, Obj o);
    Obj obj;
    ObjRef ref;   // indirect object of the CMap (invalid for names / direct objects)
    bool is_name() const { return obj.isName(); }
    bool is_stream() const { return obj.isStream(); }
    std::string cmap_name() const;                 // getCMapName ("" when none)
    bool is_identity() const;                       // getCMapName().startsWith("Identity-")
    std::shared_ptr<const CMap> file();             // getCMapFile (with the dictionary /UseCMap merged)
    // CIDSystemInfo (pdf20: veraPDF's ISO 32000-2 flavour table for predefined names)
    std::optional<std::string> registry(bool pdf20) const;
    std::optional<std::string> ordering(bool pdf20) const;
    std::optional<std::int64_t> supplement(bool pdf20) const;
    Obj use_cmap() const;                           // getUseCMap (null handle when none)
    // PDCMap.toUnicode; nullptr when unmapped
    const std::u16string* to_unicode(std::int32_t code, std::u16string& scratch);
    bool use_cmap_unresolved() const { return use_unresolved_; }   // /UseCMap naming no CMap (veraPDF NPE)

private:
    DocCache& cache_;
    bool parsed_ = false;
    std::shared_ptr<const CMap> file_;
    std::unique_ptr<PdCMap> use_;
    bool use_unresolved_ = false;
    int depth_ = 0;
    friend class PdCMapAccess;
};

// GFGlyph properties of one character code (rendering mode, marked content and real-content flags are
// evaluated by the rule module).
struct GlyphFacts {
    std::optional<bool> present;              // isGlyphPresent
    std::optional<double> width_program;      // widthFromFontProgram
    std::optional<double> width_dictionary;   // widthFromDictionary
    enum class Name : std::uint8_t { Null, Notdef, Other } name = Name::Null;
    bool has_unicode = false;                 // toUnicode != null
    std::u16string unicode;
    bool unicode_invalid = false;             // contains U+0000, U+FFFE or U+FEFF
    bool unicode_pua = false;                 // PUAHelper.containPUA
    // veraPDF's predefined-CMap tables differ between the ISO 32000-1 and ISO 32000-2 flavours (Korean CMaps:
    // ordering Korea1 vs KR), so the UCS2 route can give another Unicode value for PDF/UA-2 / WTPDF.
    bool has_unicode20 = false, unicode_invalid20 = false, unicode_pua20 = false;
    std::int32_t cid = -1;                    // CID glyphs
};

// Facts of a CIDFont (descendant) as GFPDCIDFont exposes them.
struct CidFontFacts {
    bool present = false;
    Obj dict;
    ObjRef ref;
    std::string subtype;                        // getSubtype
    std::optional<std::string> font_name;       // getfontName
    std::optional<std::string> cid_to_gid_map;  // "Identity" / "Custom" / null
    bool contains_font_file = false;
    bool contains_cid_set = false;
};

enum class VeraKind : std::uint8_t { Type1, TrueType, Type3, Type0, None };

class VeraFont : public Font {
public:
    VeraKind vkind = VeraKind::None;           // None: veraPDF builds no font object (no glyphs, no rules)
    DocCache* cache = nullptr;
    std::shared_ptr<DocCache> cache_owner;     // keeps the document cache alive
    Obj descriptor;                             // FontDescriptor dictionary (null handle when none)
    std::optional<std::string> font_name;      // PDFont.getName (BaseFont; Type 3: /Name; Type 0: CIDFont's)
    bool symbolic = false;                      // descriptor flag 3 (std-14 metrics: FontSpecific)
    // getFontProgram (may be unparsed / failed); created on first use, in content order, so that fonts
    // sharing a cached program get the first font's program as in veraPDF. Call ensure_program() first.
    mutable std::shared_ptr<FontProgram> program;
    mutable bool program_parsed = false;       // GF fontProgramParsed / isSuccessfullyParsed
    mutable bool program_loaded = false;
    void ensure_program() const;
    bool has_font_file_entry = false;          // descriptor has FontFile/FontFile2/FontFile3 (PAC "embedded")
    bool program_stream_damaged = false;       // its Flate checksum fails (see flate_data_error)

    // --- simple fonts ---
    std::optional<std::string> encoding_prop;  // GFPDSimpleFont.getEncoding
    bool contains_differences = false;
    std::vector<std::string> difference_names;   // names of the /Differences array (any order)
    std::optional<std::string> charset;        // descriptor /CharSet string (Type 1)
    // --- Type 0 ---
    std::unique_ptr<PdCMap> cmap;               // /Encoding
    std::shared_ptr<const CMap> cmap_file;      // its CMap file
    mutable CidFontFacts cid;   // contains_font_file is set by ensure_program()
    std::optional<std::string> cidfont_registry, cidfont_ordering;
    std::int64_t cidfont_supplement = 0;

    // Glyph facts (cached per code).
    const GlyphFacts& glyph(std::uint32_t code) const;
    // veraPDF readCode semantics: codes of a shown string; false when veraPDF reads none (Type 0 without CMap).
    void vera_codes(std::string_view bytes, std::vector<CharCode>& out) const;

    // Rule properties computed on demand.
    bool charset_lists_all_glyphs(bool* undecidable) const;   // GFPDType1Font.getcharSetListsAllGlyphs
    // GFPDCIDFont.getcidSetListsAllGlyphs; verapdf_read: read the CIDSet the way veraPDF does (first short read)
    bool cid_set_lists_all_glyphs(bool verapdf_read = false) const;
    bool differences_unicode_compliant() const;               // GFPDTrueTypeFont
    bool all_differences_in_agl() const;

    // Font interface
    void codes(std::string_view bytes, std::vector<CharCode>& out) const override;
    double advance(std::uint32_t code) const override;
    double glyph_to_text() const override { return glyph_scale; }
    bool vertical() const override { return wmode_vertical; }
    const std::u32string* to_unicode(std::uint32_t code) const override;

    // implementation data
    double glyph_scale = 0.001;
    bool wmode_vertical = false;
    struct Impl;
    std::shared_ptr<Impl> impl;

private:
    GlyphFacts compute(std::uint32_t code) const;
    double compute_advance(std::uint32_t code) const;

    // What the font knows about one character code, each part computed on its first request (the glyph facts
    // load the font program, so their order follows the content, as in veraPDF). One record per code instead of one
    // node in each of three maps.
    struct CodeEntry {
        GlyphFacts glyph;
        std::optional<std::u32string> u32;
        double advance = 0;
        bool has_glyph = false, has_u32 = false, has_advance = false;
    };
    // The entry of a code. Every glyph asks three times (advance, Unicode, glyph facts): the entries asked for last
    // are found through a small table indexed by the low bits of the code, without hashing.
    CodeEntry& entry(std::uint32_t code) const {
        if (!recent_) recent_.reset(new Recent[kRecent]());
        Recent& r = recent_[code & (kRecent - 1)];
        if (r.entry && r.code == code) return *r.entry;
        CodeEntry& e = codes_[code];   // nodes of the map never move
        r.code = code;
        r.entry = &e;
        return e;
    }
    struct Recent {
        std::uint32_t code = 0;
        CodeEntry* entry = nullptr;
    };
    static constexpr std::uint32_t kRecent = 128;   // no two ASCII codes share a slot
    mutable std::unordered_map<std::uint32_t, CodeEntry> codes_;
    mutable std::unique_ptr<Recent[]> recent_;
};

std::unique_ptr<VeraFont> load_vera_font(Document& doc, std::shared_ptr<DocCache> cache, const Obj& font_dict);

// Java String helpers for the rules
bool u16_has_invalid(const std::u16string& s);
bool u16_has_pua(const std::u16string& s);

}  // namespace eqx::fontimpl
