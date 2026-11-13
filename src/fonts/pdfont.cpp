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
// veraPDF 1.30.2 PD font layer: org.verapdf.pd.font.{PDFont, PDSimpleFont, PDCIDFont, PDType0Font,
// PDFontDescriptor, Encoding}, type1.PDType1Font, truetype.PDTrueTypeFont, type3.PDType3Font, cmap.PDCMap,
// cmap.CharacterCollections and the GFGlyph property computation.
#include <algorithm>
#include <cmath>
#include <cstring>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <zlib.h>

#include "core/document.h"
#include "fonts/data.h"
#include "fonts/model.h"
#include "fonts/ps.h"
#include "fonts/util.h"

namespace eqx::fontimpl {

namespace {

std::optional<std::string> string_key(const Obj& d, std::string_view key) {   // COSDictionary.getStringKey
    Obj v = cos::get(d, key);
    if (v.isString()) return cos::text(v);
    return std::nullopt;
}

std::optional<std::int64_t> integer_key(const Obj& d, std::string_view key) {   // getIntegerKey
    Obj v = cos::get(d, key);
    if (v.isInteger()) return v.getIntValue();
    if (v.isReal()) return java_d2l(v.getNumericValue());
    return std::nullopt;
}

std::optional<double> real_value(const Obj& v) {   // COSObject.getReal
    if (v.isInteger()) return static_cast<double>(v.getIntValue());
    if (v.isReal()) return v.getNumericValue();
    return std::nullopt;
}

bool known(const Obj& d, std::string_view key) { return !cos::get(d, key).isNull(); }

// cmap.CharacterCollections: [PDF 1.4, ISO 32000-1, ISO 32000-2]
struct Collection {
    const char* name;
    const char* ordering[3];
    int supplement[3];
};
const Collection kCollections[] = {
    {"GB-EUC-H", {"GB1", "GB1", "GB1"}, {0, 0, 5}},       {"GB-EUC-V", {"GB1", "GB1", "GB1"}, {0, 0, 5}},
    {"GBpc-EUC-H", {"GB1", "GB1", "GB1"}, {0, 0, 5}},     {"GBpc-EUC-V", {"GB1", "GB1", "GB1"}, {0, 0, 5}},
    {"GBK-EUC-H", {"GB1", "GB1", "GB1"}, {2, 2, 5}},      {"GBK-EUC-V", {"GB1", "GB1", "GB1"}, {2, 2, 5}},
    {"GBKp-EUC-H", {"GB1", "GB1", "GB1"}, {2, 2, 5}},     {"GBKp-EUC-V", {"GB1", "GB1", "GB1"}, {2, 2, 5}},
    {"GBK2K-H", {"GB1", "GB1", "GB1"}, {4, 4, 5}},        {"GBK2K-V", {"GB1", "GB1", "GB1"}, {4, 4, 5}},
    {"UniGB-UCS2-H", {"GB1", "GB1", "GB1"}, {4, 4, 5}},   {"UniGB-UCS2-V", {"GB1", "GB1", "GB1"}, {4, 4, 5}},
    {"UniGB-UTF16-H", {nullptr, "GB1", "GB1"}, {0, 4, 5}}, {"UniGB-UTF16-V", {nullptr, "GB1", "GB1"}, {0, 4, 5}},
    {"B5pc-H", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},      {"B5pc-V", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},
    {"HKscs-B5-H", {"CNS1", "CNS1", "CNS1"}, {3, 3, 7}},  {"HKscs-B5-V", {"CNS1", "CNS1", "CNS1"}, {3, 3, 7}},
    {"ETen-B5-H", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},   {"ETen-B5-V", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},
    {"ETenms-B5-H", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}}, {"ETenms-B5-V", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},
    {"CNS-EUC-H", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},   {"CNS-EUC-V", {"CNS1", "CNS1", "CNS1"}, {0, 0, 7}},
    {"UniCNS-UCS2-H", {"CNS1", "CNS1", "CNS1"}, {3, 3, 7}}, {"UniCNS-UCS2-V", {"CNS1", "CNS1", "CNS1"}, {3, 3, 7}},
    {"UniCNS-UTF16-H", {nullptr, "CNS1", "CNS1"}, {0, 4, 7}}, {"UniCNS-UTF16-V", {nullptr, "CNS1", "CNS1"}, {0, 4, 7}},
    {"83pv-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}}, {"90ms-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}},
    {"90ms-RKSJ-V", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}}, {"90msp-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}},
    {"90msp-RKSJ-V", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}}, {"90pv-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},
    {"Add-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}}, {"Add-RKSJ-V", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},
    {"EUC-H", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},  {"EUC-V", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},
    {"Ext-RKSJ-H", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}}, {"Ext-RKSJ-V", {"Japan1", "Japan1", "Japan1"}, {2, 2, 7}},
    {"H", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},      {"V", {"Japan1", "Japan1", "Japan1"}, {1, 1, 7}},
    {"UniJIS-UCS2-H", {"Japan1", "Japan1", "Japan1"}, {4, 4, 7}}, {"UniJIS-UCS2-V", {"Japan1", "Japan1", "Japan1"}, {4, 4, 7}},
    {"UniJIS-UCS2-HW-H", {"Japan1", "Japan1", "Japan1"}, {4, 4, 7}},
    {"UniJIS-UCS2-HW-V", {"Japan1", "Japan1", "Japan1"}, {4, 4, 7}},
    {"UniJIS-UTF16-H", {nullptr, "Japan1", "Japan1"}, {0, 5, 7}},
    {"UniJIS-UTF16-V", {nullptr, "Japan1", "Japan1"}, {0, 5, 7}},
    {"KSC-EUC-H", {"Korea1", "Korea1", "KR"}, {0, 0, 9}},  {"KSC-EUC-V", {"Korea1", "Korea1", "KR"}, {0, 0, 9}},
    {"KSCms-UHC-H", {"Korea1", "Korea1", "KR"}, {1, 1, 9}}, {"KSCms-UHC-V", {"Korea1", "Korea1", "KR"}, {1, 1, 9}},
    {"KSCms-UHC-HW-H", {"Korea1", "Korea1", "KR"}, {1, 1, 9}}, {"KSCms-UHC-HW-V", {"Korea1", "Korea1", "KR"}, {1, 1, 9}},
    {"KSCpc-EUC-H", {"Korea1", "Korea1", "KR"}, {0, 0, 9}}, {"UniKS-UCS2-H", {"Korea1", "Korea1", "KR"}, {1, 1, 9}},
    {"UniKS-UCS2-V", {"Korea1", "Korea1", "KR"}, {1, 1, 9}}, {"UniKS-UTF16-H", {nullptr, "Korea1", "KR"}, {0, 2, 9}},
    {"UniKS-UTF16-V", {nullptr, "Korea1", "KR"}, {0, 2, 9}},
    {"Identity-H", {"Identity", "Identity", "Identity"}, {0, 0, 0}},
    {"Identity-V", {"Identity", "Identity", "Identity"}, {0, 0, 0}},
};

const Collection* collection(std::string_view name) {
    for (const auto& c : kCollections)
        if (name == c.name) return &c;
    return nullptr;
}

// org.verapdf.pd.font.Encoding
struct PdfEncoding {
    const char* const* predefined = nullptr;   // 256 entries or none
    bool has_differences = false;
    std::unordered_map<int, std::string> differences;

    int size() const { return predefined ? 256 : 0; }
    std::optional<std::string> name(int code) const {
        if (code < 0) return std::nullopt;
        if (!has_differences) {
            if (code < size()) return predefined[code] ? std::optional<std::string>(predefined[code]) : std::nullopt;
            return size() != 0 ? std::optional<std::string>(".notdef") : std::nullopt;
        }
        auto it = differences.find(code);
        if (it != differences.end()) return it->second;
        if (size() != 0) {
            if (code < size()) return predefined[code] ? std::optional<std::string>(predefined[code]) : std::nullopt;
            return std::string(".notdef");
        }
        return std::nullopt;
    }
};

const char* const* predefined_encoding(std::string_view n) {
    if (n == "MacRomanEncoding") return fontdata::kMacRoman;
    if (n == "MacExpertEncoding") return fontdata::kMacExpert;
    if (n == "WinAnsiEncoding") return fontdata::kWinAnsi;
    return nullptr;
}

// PDFont.getDifferencesFromCosEncoding (diffIndex starts at 0)
std::vector<std::pair<int, std::string>> parse_differences(const Obj& enc) {
    std::vector<std::pair<int, std::string>> out;
    Obj d = cos::get(enc, "Differences");
    if (!d.isArray()) return out;
    int idx = 0;
    for (const Obj& o : cos::array_items(d)) {
        if (o.isInteger()) idx = static_cast<int>(o.getIntValue());
        else if (o.isName() && idx != -1) out.emplace_back(idx++, *cos::name(o));
    }
    return out;
}

PdfEncoding encoding_from(const Obj& enc) {   // getEncodingMappingFromCOSObject
    PdfEncoding e;
    if (enc.isName()) {
        e.predefined = predefined_encoding(*cos::name(enc));
    } else if (enc.isDictionary()) {
        if (auto b = cos::name(enc, "BaseEncoding")) e.predefined = predefined_encoding(*b);
        e.has_differences = true;
        for (auto& [k, v] : parse_differences(enc)) e.differences[k] = v;
    }
    return e;
}

bool is_std14(std::string_view n) {
    static const char* const k[] = {"Courier-Bold", "Courier-BoldOblique", "Courier", "Courier-Oblique",
                                     "Helvetica", "Helvetica-Bold", "Helvetica-BoldOblique", "Helvetica-Oblique",
                                     "Symbol", "Times-Bold", "Times-BoldItalic", "Times-Italic", "Times-Roman",
                                     "ZapfDingbats"};
    for (const char* s : k)
        if (n == s) return true;
    return false;
}

// AdobeGlyphList.get(name).getUnicodeString (nullopt = EMPTY)
std::optional<std::u16string> agl_unicode(std::string_view name) {
    const fontdata::AglEntry* e = fontdata::agl_find(name);
    if (!e) return std::nullopt;
    std::u16string s;
    for (std::uint32_t i = 0; i < e->count; ++i) {
        std::uint32_t cp = fontdata::kAglPool[e->pool + i];
        if (cp >= 0x10000) {
            cp -= 0x10000;
            s.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            s.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            s.push_back(static_cast<char16_t>(cp));
        }
    }
    return s;
}

// A Flate stream whose checksum fails: qpdf hands out the data without a word. veraPDF's inflater throws once its parser
// reads to the end, and the program counts as not embedded. The bytes are damaged by proof, so the embedding rule fails
// whatever a parser reads; the program stays parsed, so the rules on its contents still run.
bool flate_data_error(const Obj& stream) {
    Obj filter = cos::get(stream, "Filter");
    if (filter.isArray() && filter.getArrayNItems() == 1) filter = filter.getArrayItem(0);
    if (!filter.isName() || filter.getName() != "/FlateDecode") return false;
    std::shared_ptr<Buffer> raw;
    try {
        raw = const_cast<Obj&>(stream).getRawStreamData();
    } catch (std::exception&) {
        return false;  // cos::stream_data decides
    }
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = raw->getBuffer();
    zs.avail_in = static_cast<uInt>(raw->getSize());
    unsigned char out[16384];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = out;
        zs.avail_out = sizeof out;
        rc = ::inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_BUF_ERROR) break;
    }
    inflateEnd(&zs);
    return rc == Z_DATA_ERROR;
}

std::optional<std::string> decoded(const Obj& stream) {
    if (!stream.isStream()) return std::nullopt;
    return cos::stream_data(stream);
}

bool is_subset_name(const std::optional<std::string>& n) {   // PDFont.isSubset (null name: NPE)
    if (!n) return false;
    std::size_t p = n->find('+');
    std::size_t len = p == std::string::npos ? n->size() : p;
    return len == 6;
}

}  // namespace

// --- PdCMap -------------------------------------------------------------------------------------------------
PdCMap::PdCMap(DocCache& cache, Obj o) : obj(std::move(o)), ref(ref_of(obj)), cache_(cache) {}

std::string PdCMap::cmap_name() const {
    if (obj.isName()) return *cos::name(obj);
    if (obj.isStream()) {
        Obj n = cos::get(obj, "CMapName");
        if (n.isName()) return *cos::name(n);
        if (n.isString()) return cos::text(n).value_or("");
    }
    return "";
}

bool PdCMap::is_identity() const { return cmap_name().rfind("Identity-", 0) == 0; }

std::shared_ptr<const CMap> PdCMap::file() {
    if (parsed_) return file_;
    parsed_ = true;
    if (obj.isStream()) {
        ObjRef r = ref_of(obj);
        auto it = r.valid() ? cache_.cmap_streams.find(r.key()) : cache_.cmap_streams.end();
        if (it != cache_.cmap_streams.end()) {
            file_ = it->second;
        } else {
            auto data = decoded(obj);
            if (data) {
                file_ = parse_cmap_program(*data);
                if (r.valid()) cache_.cmap_streams.emplace(r.key(), file_);
            }
        }
    } else if (obj.isName()) {
        file_ = predefined_cmap(*cos::name(obj));
    } else {
        return nullptr;
    }
    Obj u = use_cmap();
    if (!u.isNull()) {
        use_ = std::make_unique<PdCMap>(cache_, u);
        use_->depth_ = depth_ + 1;
        std::shared_ptr<const CMap> used;
        if (depth_ < 16 && !(u.isIndirect() && ref_of(u) == ref)) used = use_->file();
        if (!used || !file_) {
            use_unresolved_ = true;   // veraPDF: NullPointerException in CMap.useCMap / LoopedException
        } else if (ref.valid() && cache_.merged.count(ref.key())) {
            file_ = cache_.merged[ref.key()];   // one CMap object per stream (program cache ids use it)
        } else {
            auto merged = std::make_shared<CMap>(*file_);
            merged->use(used);
            file_ = merged;
            if (ref.valid()) cache_.merged.emplace(ref.key(), file_);
        }
    }
    return file_;
}

Obj PdCMap::use_cmap() const {
    if (obj.isDictionary() || obj.isStream()) return cos::get(obj, "UseCMap");
    return Obj::newNull();
}

namespace {

Obj cid_system_info(const Obj& o) {
    Obj c = cos::get(o, "CIDSystemInfo");
    if (c.isDictionary()) return c;
    if (c.isArray() && c.getArrayNItems() > 0 && c.getArrayItem(0).isDictionary()) return c.getArrayItem(0);
    return Obj::newNull();
}

}  // namespace

std::optional<std::string> PdCMap::registry(bool pdf20) const {
    if (obj.isName()) {
        const Collection* c = collection(*cos::name(obj));
        if (!c || !c->ordering[pdf20 ? 2 : 1]) return std::nullopt;
        return std::string("Adobe");
    }
    Obj d = cid_system_info(obj);
    return d.isDictionary() ? string_key(d, "Registry") : std::nullopt;
}

std::optional<std::string> PdCMap::ordering(bool pdf20) const {
    if (obj.isName()) {
        const Collection* c = collection(*cos::name(obj));
        if (!c || !c->ordering[pdf20 ? 2 : 1]) return std::nullopt;
        return std::string(c->ordering[pdf20 ? 2 : 1]);
    }
    Obj d = cid_system_info(obj);
    return d.isDictionary() ? string_key(d, "Ordering") : std::nullopt;
}

std::optional<std::int64_t> PdCMap::supplement(bool pdf20) const {
    if (obj.isName()) {
        const Collection* c = collection(*cos::name(obj));
        if (!c || !c->ordering[pdf20 ? 2 : 1]) return std::nullopt;
        return c->supplement[pdf20 ? 2 : 1];
    }
    Obj d = cid_system_info(obj);
    return d.isDictionary() ? integer_key(d, "Supplement") : std::nullopt;
}

const std::u16string* PdCMap::to_unicode(std::int32_t code, std::u16string& scratch) {
    auto f = file();
    if (!f) return nullptr;
    const std::u16string* r = f->unicode(code, scratch);
    if (!r && use_) r = use_->to_unicode(code, scratch);
    return r;
}

// --- helpers -------------------------------------------------------------------------------------------------
bool u16_has_invalid(const std::u16string& s) {
    for (char16_t c : s)
        if (c == 0 || c == 0xFFFE || c == 0xFEFF) return true;
    return false;
}

bool u16_has_pua(const std::u16string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
        if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD))
            return true;
    }
    return false;
}

// --- font implementation data ----------------------------------------------------------------------------
struct VeraFont::Impl {
    Obj font_dict;          // simple: the font; Type 0: the Type 0 dictionary
    Obj dict;               // PDFont.dictionary (Type 0: the descendant CIDFont)
    // widths (PDFont.getWidth)
    bool widths_known = false;
    std::int64_t first_char = 0, last_char = 0;
    std::vector<std::optional<double>> widths;
    bool widths_is_array = false;
    bool missing_width_known = false;
    double missing_width = 0;
    const fontdata::StdMetrics* metrics = nullptr;   // PDType1Font.fontMetrics (std-14 without descriptor)
    const fontdata::StdMetrics* advance_metrics = nullptr;   // renderer metrics for the interpreter advance
    PdfEncoding encoding;
    bool ignore_encoding_for_names = false;          // symbolic TrueType: Encoding.empty()
    std::unique_ptr<PdCMap> to_unicode;
    // Type 3
    Obj char_procs;
    // Type 0
    std::unordered_map<int, double> w_single;
    struct WRange { int a, b; double w; };
    std::vector<WRange> w_ranges;
    bool w_array = false;
    double dw = 1000;
    bool ucs_parsed = false;
    std::unique_ptr<PdCMap> ucs;
    bool ucs20_parsed = false;
    std::unique_ptr<PdCMap> ucs20;
    bool cff_type1_special = false;

    std::optional<double> pdfont_width(VeraFont& f, int code) const;   // PDFont/PDType1Font/PDCIDFont.getWidth
};

std::optional<double> VeraFont::Impl::pdfont_width(VeraFont& f, int code) const {
    if (f.vkind == VeraKind::Type0) {
        if (!w_array) return dw;
        int cid = f.cmap_file ? f.cmap_file->to_cid(code) : 0;
        auto it = w_single.find(cid);
        if (it != w_single.end()) return it->second;
        for (const auto& r : w_ranges)
            if (cid >= r.a && cid <= r.b) return r.w;
        return dw;
    }
    if (widths_known) {
        if (widths_is_array && !widths.empty() && code >= first_char && code <= last_char) {
            std::size_t i = static_cast<std::size_t>(code - first_char);
            return i < widths.size() ? widths[i] : std::nullopt;
        }
    }
    if (missing_width_known) return missing_width;
    if (f.vkind == VeraKind::Type3 || f.vkind == VeraKind::Type1) {
        if (f.vkind == VeraKind::Type1 && metrics) {
            auto nm = encoding.name(code);
            return static_cast<double>(fontdata::std_width(*metrics, nm ? std::string_view(*nm) : std::string_view("\x01null")));
        }
        return std::nullopt;
    }
    return 0.0;
}

// --- loading -------------------------------------------------------------------------------------------------
namespace {

SimpleEncodingInfo encoding_info(const Obj& font) {
    SimpleEncodingInfo e;
    Obj enc = cos::get(font, "Encoding");
    e.present = !enc.isNull();
    if (enc.isName()) {
        e.type = SimpleEncodingInfo::Type::Name;
        e.name = *cos::name(enc);
    } else if (enc.isDictionary()) {
        e.type = SimpleEncodingInfo::Type::Dict;
        Obj b = cos::get(enc, "BaseEncoding");
        e.base_known = !b.isNull();
        if (b.isName()) e.base = *cos::name(b);
        e.differences = parse_differences(enc);
    }
    return e;
}

std::shared_ptr<const TrueTypeTables> truetype_of(DocCache& c, const Obj& stream, std::string_view data_or_empty,
                                                  bool use_data) {
    ObjRef r = ref_of(stream);
    if (r.valid()) {
        auto it = c.truetype.find(r.key());
        if (it != c.truetype.end()) return it->second;
    }
    std::shared_ptr<const TrueTypeTables> t;
    if (use_data) t = parse_truetype(data_or_empty);
    if (r.valid()) c.truetype.emplace(r.key(), t);
    return t;
}

// Font program cache (veraPDF StaticResources.cacheFontProgram with FontProgramIDGenerator ids). Note that an
// /Encoding or /CIDToGIDMap that is not an indirect object contributes "direct" to the id, so fonts sharing a
// font file and a direct encoding share the first font's program (and its encoding table).
struct ProgramRef {
    std::shared_ptr<FontProgram> program;
    bool created = false;   // this font created (and parsed) the program
};

std::string obj_id(const Obj& o) {
    ObjRef r = ref_of(o);
    return r.valid() ? r.str() : std::string("direct");
}

std::string cmap_id(const CMap* c) {
    return c ? std::to_string(reinterpret_cast<std::uintptr_t>(c)) : std::string("null");
}

template <class Make>
ProgramRef cached_program(DocCache& cache, const std::string& id, Make make) {
    auto it = cache.programs.find(id);
    if (it != cache.programs.end()) return {it->second, false};
    std::shared_ptr<FontProgram> p = make();
    if (p) cache.programs.emplace(id, p);
    return {p, true};
}

ProgramRef load_simple_program(VeraFont& f, DocCache& cache) {
    Obj fd = f.descriptor;
    auto stream_of = [&](const char* key) -> Obj {
        Obj s = cos::get(fd, key);
        return s.isStream() ? s : Obj::newNull();
    };
    if (f.vkind == VeraKind::Type1) {
        if (Obj ff = stream_of("FontFile"); !ff.isNull()) {
            return cached_program(cache, "Type1FontProgram " + ref_of(ff).str(), [&]() -> std::shared_ptr<FontProgram> {
                auto data = decoded(ff);
                if (!data) return nullptr;
                return make_type1_program(parse_type1(*data));
            });
        }
        if (Obj ff = stream_of("FontFile3"); !ff.isNull()) {
            auto subtype = cos::name(ff, "Subtype");
            if (!f.font_name) return {};   // isSubset(): NullPointerException -> no program
            bool subset = is_subset_name(f.font_name);
            if (subtype && *subtype == "Type1C") {
                return cached_program(cache, "CFFFontProgram " + ref_of(ff).str() + " null " + (subset ? "true" : "false"),
                                      [&]() -> std::shared_ptr<FontProgram> {
                                          auto data = decoded(ff);
                                          if (!data) return nullptr;
                                          return make_cff_program(parse_cff(*data, subset), nullptr);
                                      });
            }
            if (subtype && *subtype == "OpenType") {
                std::string id = "OpenTypeFontProgram " + ref_of(ff).str() + " null true " + (f.symbolic ? "true" : "false") +
                                 " " + obj_id(cos::get(f.impl->font_dict, "Encoding")) + " " + (subset ? "true" : "false");
                return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
                    auto data = decoded(ff);
                    if (!data) return nullptr;
                    auto cff = opentype_cff_table(*data);
                    if (!cff) return make_opentype_program(nullptr);
                    return make_opentype_program(make_cff_program(parse_cff(*cff, subset), nullptr));
                });
            }
            return {};
        }
        return {};
    }
    if (f.vkind == VeraKind::TrueType) {
        SimpleEncodingInfo enc = encoding_info(f.impl->font_dict);
        std::string enc_id = obj_id(cos::get(f.impl->font_dict, "Encoding"));
        if (Obj ff = stream_of("FontFile2"); !ff.isNull()) {
            std::string id = "TrueTypeFontProgram " + ref_of(ff).str() + " " + (f.symbolic ? "true" : "false") + " " + enc_id;
            return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
                auto data = decoded(ff);
                if (!data) return nullptr;
                return make_truetype_program(truetype_of(cache, ff, *data, true), f.symbolic, enc);
            });
        }
        if (Obj ff = stream_of("FontFile3"); !ff.isNull()) {
            if (!f.font_name) return {};
            bool subset = is_subset_name(f.font_name);
            std::string id = "OpenTypeFontProgram " + ref_of(ff).str() + " null false " + (f.symbolic ? "true" : "false") +
                             " " + enc_id + " " + (subset ? "true" : "false");
            return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
                auto data = decoded(ff);
                if (!data) return nullptr;
                return make_opentype_program(make_truetype_program(truetype_of(cache, ff, *data, true), f.symbolic, enc));
            });
        }
    }
    return {};
}

ProgramRef load_cid_program(VeraFont& f, DocCache& cache) {
    Obj fd = f.descriptor;
    std::string sub = cos::name(f.impl->dict, "Subtype").value_or("");
    Obj ff2 = cos::get(fd, "FontFile2");
    Obj ff3 = cos::get(fd, "FontFile3");
    Obj c2g = cos::get(f.impl->dict, "CIDToGIDMap");
    auto cid_to_gid = [&](bool& identity, std::vector<int>& map) -> bool {
        identity = true;
        if (c2g.isStream()) {
            auto data = decoded(c2g);
            if (!data) return false;   // IOException -> no program
            identity = false;
            const std::string& d = *data;
            for (std::size_t i = 0; i < d.size(); i += 2) {
                int v = static_cast<unsigned char>(d[i]) << 8;
                if (i + 1 < d.size()) v += static_cast<unsigned char>(d[i + 1]);
                map.push_back(v);
            }
        }
        return true;
    };
    const std::string cm = cmap_id(f.cmap_file.get());
    if (ff2.isStream() && sub == "CIDFontType2") {
        std::string id = "CIDFontType2Program " + ref_of(ff2).str() + " " + cm + " " + obj_id(c2g);
        return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
            auto data = decoded(ff2);
            if (!data) return nullptr;
            bool identity;
            std::vector<int> map;
            if (!cid_to_gid(identity, map)) return nullptr;
            return make_cid_type2_program(truetype_of(cache, ff2, *data, true), f.cmap_file, identity, std::move(map));
        });
    }
    if (ff3.isStream()) {
        Obj st = cos::get(ff3, "Subtype");
        std::optional<std::string> subtype = st.isName() ? cos::name(st) : std::nullopt;
        if (!f.font_name) return {};
        bool subset = is_subset_name(f.font_name);
        if (subtype && *subtype == "CIDFontType0C") {
            std::string id = "CFFFontProgram " + ref_of(ff3).str() + " " + cm + " " + (subset ? "true" : "false");
            return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
                auto data = decoded(ff3);
                if (!data) return nullptr;
                return make_cff_program(parse_cff(*data, subset), f.cmap_file);
            });
        }
        if (subtype && *subtype == "OpenType") {
            bool is_cff = sub != "TrueType" && sub != "CIDFontType2";
            std::string id = "OpenTypeFontProgram " + ref_of(ff3).str() + " " + cm + " " + (is_cff ? "true" : "false") +
                             " " + (f.symbolic ? "true" : "false") + " " + obj_id(cos::get(f.impl->dict, "Encoding")) +
                             " " + (subset ? "true" : "false");
            return cached_program(cache, id, [&]() -> std::shared_ptr<FontProgram> {
                auto data = decoded(ff3);
                if (!data) return nullptr;
                if (sub == "CIDFontType2") {
                    bool identity;
                    std::vector<int> map;
                    if (!cid_to_gid(identity, map)) return nullptr;
                    return make_opentype_program(make_cid_type2_program(truetype_of(cache, ff3, *data, true),
                                                                        f.cmap_file, identity, std::move(map)));
                }
                if (is_cff) {
                    auto cff = opentype_cff_table(*data);
                    if (!cff) return make_opentype_program(nullptr);
                    return make_opentype_program(make_cff_program(parse_cff(*cff, subset), f.cmap_file));
                }
                SimpleEncodingInfo enc = encoding_info(f.impl->dict);
                return make_opentype_program(make_truetype_program(truetype_of(cache, ff3, *data, true), f.symbolic, enc));
            });
        }
    }
    return {};
}

void read_descriptor(VeraFont& f, const Obj& dict) {
    Obj fd = cos::get(dict, "FontDescriptor");
    f.descriptor = fd.isDictionary() ? fd : Obj::newNull();
    auto flags = integer_key(f.descriptor, "Flags");
    f.symbolic = flags && ((static_cast<std::int32_t>(*flags) & (1 << 2)) != 0);
    f.has_font_file_entry = cos::get(f.descriptor, "FontFile").isStream() ||
                            cos::get(f.descriptor, "FontFile2").isStream() ||
                            cos::get(f.descriptor, "FontFile3").isStream();
    for (const char* key : {"FontFile", "FontFile2", "FontFile3"}) {
        Obj ff = cos::get(f.descriptor, key);
        if (ff.isStream() && flate_data_error(ff)) f.program_stream_damaged = true;
    }
    f.impl->missing_width_known = known(f.descriptor, "MissingWidth");
    f.impl->missing_width = real_value(cos::get(f.descriptor, "MissingWidth")).value_or(0);
}

}  // namespace

std::unique_ptr<VeraFont> load_vera_font(Document& doc, std::shared_ptr<DocCache> cache_ptr, const Obj& d) {
    (void)doc;
    auto f = std::make_unique<VeraFont>();
    DocCache& cache = *cache_ptr;
    f->cache = cache_ptr.get();
    f->cache_owner = cache_ptr;
    f->impl = std::make_shared<VeraFont::Impl>();
    VeraFont::Impl& im = *f->impl;
    f->dict = d;
    f->ref = ref_of(d);
    f->subtype = cos::name(d, "Subtype").value_or("");
    f->base_font = cos::name(d, "BaseFont").value_or("");
    im.font_dict = d;
    im.dict = d;
    const std::string& st = f->subtype;
    f->kind = st == "TrueType" ? FontKind::TrueType
              : st == "Type1" ? FontKind::Type1
              : st == "MMType1" ? FontKind::MMType1
              : st == "Type3" ? FontKind::Type3
              : st == "Type0" ? FontKind::Type0
                              : FontKind::Unknown;

    if (f->kind == FontKind::Type0) {
        Obj desc = cos::get(d, "DescendantFonts");
        Obj cid = desc.isArray() && desc.getArrayNItems() > 0 ? desc.getArrayItem(0) : Obj::newNull();
        if (!cid.isDictionary()) cid = Obj::newNull();
        im.dict = cid;
        Obj enc = cos::get(d, "Encoding");
        if (enc.isNull()) {
            // PDType0Font: "There is no Encoding entry in Type0 font dictionary" (exception): no font object
            f->vkind = VeraKind::None;
            return f;
        }
        f->vkind = VeraKind::Type0;
        read_descriptor(*f, cid);
        if (auto n = cos::name(cid, "BaseFont")) f->font_name = *n;
        f->cmap = std::make_unique<PdCMap>(cache, enc);
        f->cmap_file = f->cmap->file();
        f->wmode_vertical = f->cmap_file && f->cmap_file->wmode == 1;
        Obj w = cos::get(cid, "W");
        if (w.isArray()) {
            im.w_array = true;
            auto items = cos::array_items(w);
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (!items[i].isInteger()) break;   // Java NullPointerException
                std::int64_t a = items[i].getIntValue();
                if (++i >= items.size()) break;
                const Obj& o = items[i];
                if (o.isInteger()) {
                    if (++i >= items.size()) break;
                    auto wv = real_value(items[i]);
                    if (!wv) break;
                    im.w_ranges.push_back({static_cast<int>(a), static_cast<int>(o.getIntValue()), *wv});
                } else if (o.isArray()) {
                    auto ws = cos::array_items(o);
                    for (std::size_t k = 0; k < ws.size(); ++k) {
                        auto wv = real_value(ws[k]);
                        if (!wv) continue;
                        im.w_single[static_cast<int>(a + static_cast<std::int64_t>(k))] = *wv;
                    }
                }
            }
        }
        im.dw = real_value(cos::get(cid, "DW")).value_or(1000.0);
        // CIDFont facts
        if (cid.isDictionary()) {
            f->cid.present = true;
            f->cid.dict = cid;
            f->cid.ref = ref_of(cid);
            f->cid.subtype = cos::name(cid, "Subtype").value_or("");
            f->cid.font_name = f->font_name;
            Obj m = cos::get(cid, "CIDToGIDMap");
            if (m.isStream()) f->cid.cid_to_gid_map = "Custom";
            else if (m.isName() && *cos::name(m) == "Identity") f->cid.cid_to_gid_map = "Identity";
            f->cid.contains_cid_set = cos::get(f->descriptor, "CIDSet").isStream();
        }
        Obj csi = cos::get(cid, "CIDSystemInfo");
        f->cidfont_registry = string_key(csi, "Registry");
        f->cidfont_ordering = string_key(csi, "Ordering");
        f->cidfont_supplement = integer_key(csi, "Supplement").value_or(0);
        im.to_unicode = std::make_unique<PdCMap>(cache, cos::get(d, "ToUnicode"));
        return f;
    }

    if (f->kind == FontKind::Unknown) {
        f->vkind = VeraKind::None;   // PDFontFactory: no font for this subtype
        return f;
    }

    // simple fonts
    f->vkind = f->kind == FontKind::TrueType ? VeraKind::TrueType
               : f->kind == FontKind::Type3  ? VeraKind::Type3
                                             : VeraKind::Type1;
    read_descriptor(*f, d);
    if (f->vkind == VeraKind::Type3) {
        if (auto n = cos::name(d, "Name")) f->font_name = *n;
    } else if (auto n = cos::name(d, "BaseFont")) {
        f->font_name = *n;
    }
    im.widths_known = known(d, "Widths") && known(d, "FirstChar") && known(d, "LastChar") &&
                      cos::integer(d, "FirstChar") && cos::integer(d, "LastChar");
    im.first_char = cos::integer(d, "FirstChar").value_or(0);
    im.last_char = cos::integer(d, "LastChar").value_or(0);
    Obj w = cos::get(d, "Widths");
    im.widths_is_array = w.isArray();
    for (const Obj& x : cos::array_items(w)) im.widths.push_back(real_value(x));
    im.encoding = encoding_from(cos::get(d, "Encoding"));
    if (f->vkind == VeraKind::Type1 && f->font_name && is_std14(*f->font_name)) {
        im.advance_metrics = fontdata::std_metrics(*f->font_name);
        Obj fd = cos::get(d, "FontDescriptor");
        bool empty = !fd.isDictionary() || fd.getKeys().empty();
        if (empty) {
            // PDType1Font: descriptor from the standard metrics
            im.metrics = im.advance_metrics;
            if (im.metrics) {
                f->descriptor = Obj::newNull();
                f->symbolic = std::string_view(im.metrics->encoding_scheme ? im.metrics->encoding_scheme : "") == "FontSpecific";
                im.missing_width_known = false;
                im.missing_width = 0;
                if (im.encoding.size() == 0) {
                    PdfEncoding e;
                    std::string scheme = im.metrics->encoding_scheme ? im.metrics->encoding_scheme : "";
                    if (scheme == "AdobeStandardEncoding") e.predefined = fontdata::kStandard;
                    e.has_differences = true;
                    for (auto& [k, v] : parse_differences(cos::get(d, "Encoding"))) e.differences[k] = v;
                    im.encoding = std::move(e);
                }
            }
        }
    }
    if (f->vkind == VeraKind::Type3) {
        Obj cp = cos::get(d, "CharProcs");
        im.char_procs = cp.isDictionary() ? cp : Obj::newNull();
        auto fm = cos::array_items(cos::get(d, "FontMatrix"));
        if (fm.size() == 6) f->glyph_scale = real_value(fm[0]).value_or(0.001);
    }
    // GFPDSimpleFont properties
    Obj enc = cos::get(d, "Encoding");
    if (enc.isName()) f->encoding_prop = *cos::name(enc);
    else if (!enc.isNull()) {
        Obj b = cos::get(enc, "BaseEncoding");
        if (b.isName()) f->encoding_prop = *cos::name(b);
        else if (b.isString()) f->encoding_prop = cos::text(b);
    }
    f->contains_differences = enc.isDictionary() && known(enc, "Differences");
    if (enc.isDictionary())
        for (const Obj& o : cos::array_items(cos::get(enc, "Differences")))
            if (o.isName()) f->difference_names.push_back(*cos::name(o));
    f->charset = string_key(f->descriptor, "CharSet");
    im.ignore_encoding_for_names = f->vkind == VeraKind::TrueType && f->symbolic;
    if (f->vkind == VeraKind::Type3) {
        f->program_loaded = true;
        f->program_parsed = true;   // PDType3Font: setSuccessfullyParsed(true)
    }
    im.to_unicode = std::make_unique<PdCMap>(cache, cos::get(d, "ToUnicode"));
    return f;
}

void VeraFont::ensure_program() const {
    if (program_loaded) return;
    program_loaded = true;
    if (vkind != VeraKind::Type1 && vkind != VeraKind::TrueType && vkind != VeraKind::Type0) return;
    VeraFont& self = const_cast<VeraFont&>(*this);
    ProgramRef pr = vkind == VeraKind::Type0 ? load_cid_program(self, *cache) : load_simple_program(self, *cache);
    program = pr.program;
    // GF*Font: parseFont() only when not attempted yet; fontProgramParsed = isSuccessfulParsing() (false when
    // that first parse threw)
    program_parsed = program && (pr.created ? program->gf_parsed() : program->successful());
    if (vkind == VeraKind::Type0) cid.contains_font_file = program && program_parsed;
}

// --- codes / advances / Unicode ---------------------------------------------------------------------------
void VeraFont::vera_codes(std::string_view bytes, std::vector<CharCode>& out) const {
    if (vkind == VeraKind::Type0) {
        if (!cmap_file) return;   // PDCIDFont.readCode: "No CMap for Type 0 font" (IOException)
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            std::size_t start = pos;
            std::int32_t code;
            if (!cmap_file->read_code(bytes, pos, code)) return;
            std::size_t len = std::min<std::size_t>(pos - start, 255);
            out.push_back({static_cast<std::uint32_t>(code), static_cast<std::uint8_t>(len == 0 ? 1 : len)});
            if (pos == start) break;
        }
        return;
    }
    for (unsigned char c : bytes) out.push_back({c, 1});
}

void VeraFont::codes(std::string_view bytes, std::vector<CharCode>& out) const {
    if (vkind == VeraKind::None && kind == FontKind::Type0) {
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
            out.push_back({(std::uint32_t(static_cast<unsigned char>(bytes[i])) << 8) | static_cast<unsigned char>(bytes[i + 1]), 2});
        return;
    }
    vera_codes(bytes, out);
}

double VeraFont::advance(std::uint32_t code) const {
    auto it = adv_.find(code);
    if (it != adv_.end()) return it->second;
    double w = compute_advance(code);
    adv_.emplace(code, w);
    return w;
}

double VeraFont::compute_advance(std::uint32_t code) const {
    const Impl& im = *impl;
    if (vkind == VeraKind::Type0) {
        auto w = im.pdfont_width(const_cast<VeraFont&>(*this), static_cast<int>(code));
        return w.value_or(im.dw);
    }
    if (vkind == VeraKind::None) return 0;
    if (im.widths_is_array && cos::integer(dict, "FirstChar")) {
        std::int64_t i = static_cast<std::int64_t>(code) - im.first_char;
        if (i >= 0 && static_cast<std::size_t>(i) < im.widths.size()) return im.widths[static_cast<std::size_t>(i)].value_or(0);
    }
    if (im.advance_metrics && !im.widths_is_array) {
        auto nm = im.encoding.name(static_cast<int>(code));
        if (!nm) {
            for (std::size_t k = 0; k < im.advance_metrics->code_count; ++k)
                if (im.advance_metrics->codes[k].code == static_cast<int>(code)) {
                    nm = std::string(im.advance_metrics->codes[k].name);
                    break;
                }
        }
        if (nm) return fontdata::std_width(*im.advance_metrics, *nm);
    }
    if (vkind != VeraKind::Type3 && !im.widths_is_array) ensure_program();
    if (vkind != VeraKind::Type3 && program && program_parsed && !im.widths_is_array) {
        float w = program->width(static_cast<int>(code));
        if (w >= 0) return w;
    }
    return im.missing_width_known ? im.missing_width : 0;
}

const std::u32string* VeraFont::to_unicode(std::uint32_t code) const {
    if (vkind == VeraKind::None) return nullptr;
    auto it = u32_.find(code);
    if (it == u32_.end()) {
        const GlyphFacts& g = glyph(code);
        std::optional<std::u32string> r;
        if (g.has_unicode) r = u16_to_u32(g.unicode);
        it = u32_.emplace(code, std::move(r)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const GlyphFacts& VeraFont::glyph(std::uint32_t code) const {
    auto it = glyphs_.find(code);
    if (it != glyphs_.end()) return it->second;
    return glyphs_.emplace(code, compute(code)).first->second;
}

GlyphFacts VeraFont::compute(std::uint32_t ucode) const {
    VeraFont& self = const_cast<VeraFont&>(*this);
    Impl& im = *impl;
    GlyphFacts g;
    const int code = static_cast<int>(ucode);
    ensure_program();
    FontProgram* prog = program.get();
    // --- toUnicode ---
    std::u16string scratch;
    std::optional<std::u16string> uni;
    auto cmap_to_unicode = [&]() -> std::optional<std::u16string> {   // PDFont.cMapToUnicode
        if (im.to_unicode->is_identity()) return std::u16string(1, static_cast<char16_t>(code & 0xFFFF));
        const std::u16string* s = im.to_unicode->to_unicode(code, scratch);
        return s ? std::optional<std::u16string>(*s) : std::nullopt;
    };
    std::optional<std::u16string> uni20;
    bool split = false;
    if (vkind == VeraKind::Type0) {
        uni = cmap_to_unicode();
        if (!uni && cmap) {
            // PDType0Font.toUnicode: the UCS2 CMap of the character collection
            bool identity = cmap->cmap_name() == "Identity-H" || cmap->cmap_name() == "Identity-V";
            auto ucs_of = [&](bool pdf20, bool& parsed, std::unique_ptr<PdCMap>& ucs) {
                if (parsed) return;
                parsed = true;
                if (identity) {
                    if (cidfont_registry == "Adobe" && cidfont_ordering &&
                        (*cidfont_ordering == "Japan1" || *cidfont_ordering == "CNS1" || *cidfont_ordering == "GB1" ||
                         *cidfont_ordering == "Korea1" || *cidfont_ordering == "KR")) {
                        ucs = std::make_unique<PdCMap>(*cache, Obj::newName("/Adobe-" + *cidfont_ordering + "-UCS2"));
                    }
                } else {
                    auto reg = cmap->registry(pdf20), ord = cmap->ordering(pdf20);
                    std::string nm = (reg ? *reg : "null") + "-" + (ord ? *ord : "null") + "-UCS2";
                    auto pc = std::make_unique<PdCMap>(*cache, Obj::newName("/" + nm));
                    if (pc->file()) ucs = std::move(pc);
                }
            };
            auto lookup = [&](PdCMap* ucs) -> std::optional<std::u16string> {
                if (!ucs) return std::nullopt;
                if (identity) {
                    const std::u16string* s = ucs->to_unicode(code, scratch);
                    return s ? std::optional<std::u16string>(*s) : std::nullopt;
                }
                if (!cmap_file) return std::nullopt;
                int cid = cmap_file->to_cid(code);
                auto f = ucs->file();
                const std::u16string* s = f ? f->unicode(cid, scratch) : nullptr;
                return s ? std::optional<std::u16string>(*s) : std::nullopt;
            };
            ucs_of(false, im.ucs_parsed, im.ucs);
            uni = lookup(im.ucs.get());
            if (!identity && cmap->is_name() && cmap->ordering(false) != cmap->ordering(true)) {
                ucs_of(true, im.ucs20_parsed, im.ucs20);
                uni20 = lookup(im.ucs20.get());
                split = true;
            }
        }
    } else {
        uni = cmap_to_unicode();
        if (!uni) {   // PDSimpleFont.toUnicode
            std::optional<std::string> name = im.encoding.name(code);
            if (!name && prog) name = prog->glyph_name(code);
            if (name) {
                if (auto a = agl_unicode(*name)) uni = *a;
                else if (fontdata::zapf_dingbats_has(*name) || fontdata::symbol_set_has(*name)) uni = std::u16string(u" ");
            }
        }
    }
    if (uni) {
        g.has_unicode = true;
        g.unicode = *uni;
        g.unicode_invalid = u16_has_invalid(g.unicode);
        g.unicode_pua = u16_has_pua(g.unicode);
    }
    if (!split) uni20 = uni;
    if (uni20) {
        g.has_unicode20 = true;
        g.unicode_invalid20 = u16_has_invalid(*uni20);
        g.unicode_pua20 = u16_has_pua(*uni20);
    }

    // --- presence and widths (GFGlyph.initForNotType3 / initForType3) ---
    if (vkind == VeraKind::Type3) {
        std::optional<std::string> nm = im.encoding.name(code);
        Obj cp = nm && im.char_procs.isDictionary() ? cos::get(im.char_procs, *nm) : Obj::newNull();
        g.present = !cp.isNull();
        double wp = -1;
        if (cp.isStream()) {
            if (auto data = decoded(cp)) {
                try {
                    ps::Source src(*data, ps::Source::Kind::kBuffered);
                    ps::Lexer lx(src, false, ps::Lexer::Comments::kNotSeekable);
                    double w = -1;
                    lx.next_token();
                    if (lx.token().type == ps::TT::Integer || lx.token().type == ps::TT::Real) w = lx.token().real;
                    lx.next_token();
                    lx.next_token();
                    bool ok = true;
                    if (!(lx.token().type == ps::TT::Keyword && lx.token().value == "d0")) {
                        for (int k = 0; k < 4; ++k) lx.next_token();
                        if (lx.token().type != ps::TT::Keyword || lx.token().value != "d1") ok = false;
                    }
                    wp = ok ? w : -1;
                } catch (std::exception&) {
                    wp = -1;
                }
            }
        }
        if (wp == -1) wp = im.missing_width_known ? im.missing_width : 0;
        g.width_program = wp;
        g.width_dictionary = im.pdfont_width(self, code).value_or(0);
    } else {
        bool invalid = !prog || !program_parsed;
        if (!invalid) {
            bool present = false;
            double wp = -1;
            if (vkind == VeraKind::Type1) {
                auto nm = im.encoding.name(code);
                present = nm ? prog->contains_glyph(*nm) : prog->contains_code(code);
                wp = nm ? prog->width(*nm) : prog->width(code);
            } else if (vkind == VeraKind::TrueType) {
                present = prog->contains_code(code);
                wp = prog->width(code);
            } else {   // PDCIDFont
                FontProgram* c1 = cff_type1_of(prog);
                int cid = cmap_file ? cmap_file->to_cid(code) : 0;
                if (c1 && cid != 0) {
                    present = cff_type1_contains_gid(c1, cid);
                    wp = cff_type1_width_from_gid(c1, cid);
                } else {
                    present = prog->contains_code(code);
                    wp = prog->width(code);
                }
            }
            g.present = code == 0 || present;
            if (wp == -1) {
                if (vkind == VeraKind::Type0) wp = im.dw;   // PDCIDFont.getDefaultWidth
                else wp = im.missing_width_known ? im.missing_width : 0;
            }
            g.width_program = wp;
            g.width_dictionary = im.pdfont_width(self, code).value_or(0);
        }
    }

    // --- glyph name ---
    if (vkind == VeraKind::Type0) {
        if (prog && prog->successful()) {
            FontProgram* c1 = cff_type1_of(prog);
            int cid = cmap_file ? cmap_file->to_cid(code) : 0;
            bool present = (c1 && cid != 0) ? cff_type1_contains_gid(c1, cid) : prog->contains_code(code);
            g.name = present ? GlyphFacts::Name::Null : GlyphFacts::Name::Notdef;
        }
        if (cmap_file) g.cid = cmap_file->to_cid(code);
    } else {
        std::optional<std::string> nm;
        if (!im.ignore_encoding_for_names) nm = im.encoding.name(code);
        if (!nm && vkind == VeraKind::TrueType) {
            if (prog) {
                nm = prog->glyph_name(code);
                if (!nm) nm = prog->contains_code(code) ? std::nullopt : std::optional<std::string>(".notdef");
            } else if (code == 0) {
                nm = ".notdef";
            }
        }
        g.name = !nm ? GlyphFacts::Name::Null : *nm == ".notdef" ? GlyphFacts::Name::Notdef : GlyphFacts::Name::Other;
    }
    return g;
}

// --- rule properties --------------------------------------------------------------------------------------------
bool VeraFont::charset_lists_all_glyphs(bool* undecidable) const {
    if (undecidable) *undecidable = false;
    ensure_program();
    if (!program_parsed) return false;
    // descriptor CharSet parsed as PDF objects (names only, parsing stops at the first non-object)
    std::vector<std::string> desc;
    if (charset) {
        try {
            ps::Source src(*charset, ps::Source::Kind::kSeekable);
            ps::Lexer lx(src, false, ps::Lexer::Comments::kSeekable);
            ps::ObjectParser op(lx);
            ps::Obj o = op.next_object();
            while (!o.empty()) {
                if (o.t == ps::Obj::T::Name) desc.push_back(o.s);
                o = op.next_object();
            }
        } catch (std::exception&) {
            desc.clear();
        }
    }
    std::sort(desc.begin(), desc.end());
    desc.erase(std::unique(desc.begin(), desc.end()), desc.end());
    std::vector<std::string> prog;
    if (const Type1Data* t1 = type1_data(program.get())) {
        for (const auto& [k, v] : t1->widths) prog.push_back(k);
    } else if (const CffFacts* cf = cff_facts(program.get())) {
        if (cf->cid_keyed) {   // Java ClassCastException
            if (undecidable) *undecidable = true;
            return true;
        }
        prog = cf->charset_names;
    }
    std::sort(prog.begin(), prog.end());
    prog.erase(std::unique(prog.begin(), prog.end()), prog.end());
    if (!(desc.size() == prog.size() || desc.size() + 1 == prog.size())) return false;
    for (const auto& n : desc)
        if (n != ".notdef" && !std::binary_search(prog.begin(), prog.end(), n)) return false;
    for (const auto& n : prog)
        if (n != ".notdef" && !std::binary_search(desc.begin(), desc.end(), n)) return false;
    return true;
}

// The CIDSet bytes veraPDF 1.30.2 actually reads (GFPDCIDFont.getCIDsFromCIDSet stops at the first read that
// returns less than 2048 bytes). That first read is short for an AES-encrypted Flate stream of two or more
// cipher blocks: COSFilterAESDecryptionDefault decrypts with Cipher.update (AES/CBC/PKCS5Padding), which holds
// back the last block, so the first inflate (COSFilterFlateDecode) sees only the first 16 * floor(L / 16)
// compressed bytes (at most 2032). Other cases read the whole stream (nullopt: no difference modelled).
static std::optional<std::string> verapdf_cidset_read(const Obj& cs) {
    QPDF* q = cs.getOwningQPDF();
    if (!q) return std::nullopt;
    int R = 0, P = 0, V = 0;
    QPDF::encryption_method_e stream_m = QPDF::e_none, string_m = QPDF::e_none, file_m = QPDF::e_none;
    if (!q->isEncrypted(R, P, V, stream_m, string_m, file_m)) return std::nullopt;
    if (stream_m != QPDF::e_aes && stream_m != QPDF::e_aesv3) return std::nullopt;
    Obj dict = cs.getDict();
    Obj filter = cos::get(dict, "Filter");
    if (filter.isArray()) filter = filter.getArrayNItems() == 1 ? filter.getArrayItem(0) : Obj();
    if (!cos::name_is(dict, "Filter", "FlateDecode") && !(filter.isName() && filter.getName() == "/FlateDecode"))
        return std::nullopt;
    Obj parms = cos::get(dict, "DecodeParms");
    if (parms.isArray()) parms = parms.getArrayNItems() == 1 ? parms.getArrayItem(0) : Obj();
    if (parms.isDictionary() && cos::integer(parms, "Predictor").value_or(1) > 1) return std::nullopt;
    std::shared_ptr<Buffer> raw;
    try {
        raw = Obj(cs).getRawStreamData();   // decrypted, still Flate-compressed
    } catch (std::exception&) {
        return std::nullopt;
    }
    const std::size_t L = raw->getSize();
    const std::size_t first = std::min<std::size_t>(16 * (L / 16), 2032);
    if (first == 0 || first >= L) return std::nullopt;
    std::string out = inflate_prefix(std::string_view(reinterpret_cast<const char*>(raw->getBuffer()), first), 2048);
    if (out.size() >= 2048) return std::nullopt;   // veraPDF would read on; not modelled
    return out;
}

bool VeraFont::cid_set_lists_all_glyphs(bool verapdf_read) const {
    ensure_program();
    if (!program_parsed) return false;
    Obj cs = cos::get(descriptor, "CIDSet");
    if (!cs.isStream()) return true;
    std::optional<std::string> data;
    if (verapdf_read) data = verapdf_cidset_read(cs);
    if (!data) data = decoded(cs);
    if (!data) return false;
    std::string bytes = data->substr(0, std::min<std::size_t>(data->size(), 16384));
    auto bit = [&](int i) -> bool {
        if (i < 0 || static_cast<std::size_t>(i / 8) >= bytes.size()) return false;
        return (static_cast<unsigned char>(bytes[static_cast<std::size_t>(i / 8)]) & (0x80 >> (i % 8))) != 0;
    };
    for (int cid : program->cid_list())
        if (cid != 0 && !bit(cid)) return false;
    int length = 0;
    for (int i = static_cast<int>(bytes.size()) * 8 - 1; i >= 0; --i)
        if (bit(i)) {
            length = i + 1;
            break;
        }
    for (int i = 1; i < length; ++i)
        if (bit(i) && !program->contains_cid(i)) return false;
    return true;
}

bool VeraFont::all_differences_in_agl() const {
    for (const auto& n : difference_names)
        if (!fontdata::agl_find(n)) return false;
    return true;
}

bool VeraFont::differences_unicode_compliant() const {
    ensure_program();
    if (!program) return true;
    if (!program_parsed) return false;
    const TrueTypeTables* t = truetype_tables(program.get());
    if (!t || !t->cmap(3, 1)) return false;
    Obj enc = cos::get(impl->font_dict, "Encoding");
    if (!enc.isNull() && !enc.isName()) {
        if (cos::get(enc, "Differences").isArray() && !all_differences_in_agl()) return false;
    }
    return true;
}

}  // namespace eqx::fontimpl
