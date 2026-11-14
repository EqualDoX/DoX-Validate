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
// CMaps with the semantics of org.verapdf.pd.font.cmap (CMap, CMapParser, CodeSpace, CIDInterval,
// NotDefInterval, ToUnicodeInterval): embedded CMap / ToUnicode streams are parsed with the PostScript
// port of fonts/ps.h; predefined CMaps are replayed from the generated operation streams
// (generated/font_cmaps.cpp) through the same builder.
#pragma once

#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eqx::fontimpl {

struct CodeSpace {
    std::string begin, end;   // empty when veraPDF rejected the range (different lengths, begin > end)
    CodeSpace(std::string b, std::string e);
    std::size_t length() const { return begin.size(); }
    bool contains(const unsigned char* code, std::size_t len) const;
    bool overlaps(const CodeSpace& o) const;
};

struct CidRange {
    std::int32_t start = 0, end = 0, cid = 0;
    bool notdef_interval = false;   // NotDefInterval: every code maps to `cid`
    int get_cid(std::int32_t code) const {
        if (code < start || code > end) return -1;
        return notdef_interval ? cid : cid + code - start;
    }
};

struct UnicodeInterval {
    std::int64_t begin = 0, end = 0;
    int length = 0;
    std::int64_t start = 0;
    std::u16string to_unicode(std::int32_t code) const;
};

class CMap {
public:
    int wmode = 0;
    std::optional<std::string> name, registry, ordering;
    int supplement = 0;
    bool uses_non_predefined = false;
    int shortest_cs = INT_MAX;
    std::vector<CodeSpace> codespaces;               // chronological (used CMaps' ranges appended)
    std::vector<CidRange> own_cid;                   // chronological; looked up newest first
    std::vector<std::shared_ptr<const CMap>> used;   // looked up after own_cid, in order
    std::vector<CidRange> notdef;                    // chronological (used CMaps' entries appended)
    std::unordered_map<std::int32_t, std::u16string> to_unicode_map;
    std::vector<UnicodeInterval> intervals;

    int to_cid(std::int32_t code) const;
    bool contains_code(std::int32_t code) const;
    // CMap.getCodeFromStream on `bytes` starting at `pos` (advanced). Returns false when Java would throw
    // a runtime exception (the caller stops reading the string).
    bool read_code(std::string_view bytes, std::size_t& pos, std::int32_t& code) const;
    // The only code space is <0000> <FFFF>: read_code then takes two bytes per code (see there).
    bool plain_two_byte() const {
        if (codespaces.size() != 1) return false;
        const CodeSpace& cs = codespaces[0];
        return cs.begin.size() == 2 && cs.begin[0] == '\0' && cs.begin[1] == '\0' && cs.end[0] == '\xFF' && cs.end[1] == '\xFF';
    }
    // CMap.getUnicode
    const std::u16string* unicode(std::int32_t code, std::u16string& scratch) const;
    // CMap.useCMap
    void use(const std::shared_ptr<const CMap>& other);
    // largest CID of cidrange/cidchar mappings (CMapFile.getMaxCID; not needed by the rules)

    // Builder operations (CMapParser)
    void add_codespace(std::string b, std::string e);

private:
    bool find_cid(std::int32_t code, int& cid) const;
};

// Parses an embedded CMap program (CMapFactory.getCMap: a failure yields an empty CMap).
std::shared_ptr<const CMap> parse_cmap_program(std::string_view data);
// Predefined CMap resource by name (veraPDF parser/font/cmap/*); nullptr when veraPDF has none.
std::shared_ptr<const CMap> predefined_cmap(std::string_view name);
// Names of ISO 32000-1 Table 118 (the list in the profile tests of 7.21.3.3-1 / 7.21.3.3-3).
bool is_table118_cmap(std::string_view name);

// CMapParser.numberFromBytes
std::int64_t number_from_bytes(std::string_view b);
// Unicode string of a bfchar / bfrange array entry (readStringFromUnicodeSequenceToken).
std::u16string unicode_from_hex(std::string_view bytes);

}  // namespace eqx::fontimpl
