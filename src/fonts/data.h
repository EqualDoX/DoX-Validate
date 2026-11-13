// Static font data generated from the veraPDF 1.30.2 parser sources (tools/gen_font_data.py):
// Adobe Glyph List, encodings, CFF predefined tables, standard-14 metrics and predefined CMaps.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eqx::fontdata {

struct NameCode {
    const char* name;
    int code;
};
struct NameWidth {
    const char* name;
    int width;
};
struct CodeName {
    int code;
    const char* name;
};
struct AglEntry {
    const char* name;
    std::uint32_t pool;     // index of the first code point in kAglPool
    std::uint32_t count;    // number of code points (1 + diacritics)
};
struct StdMetrics {
    const char* font_name;          // file stem = PDF base font name ("Times-Roman")
    const char* encoding_scheme;    // AFM EncodingScheme ("AdobeStandardEncoding", "FontSpecific")
    const char* charset;            // AFM CharacterSet
    const NameWidth* widths;        // sorted by name (bytewise)
    std::size_t width_count;
    const CodeName* codes;          // AFM "C code" entries (built-in encoding), file order
    std::size_t code_count;
};
struct PredefinedCMap {
    const char* name;
    const char* data;               // zlib-compressed operation stream
    std::size_t size;
    std::size_t raw_size;
};

// org.verapdf.pd.font.truetype.TrueTypePredefined
extern const char* const kMacRoman[256];
extern const char* const kWinAnsi[256];
extern const char* const kStandard[256];
extern const char* const kMacExpert[256];
extern const char* const kMacGlyphNames[];
extern const std::size_t kMacGlyphNamesCount;
extern const NameCode kMacOsRoman[];
extern const std::size_t kMacOsRomanCount;

// org.verapdf.pd.font.cff.CFFPredefined
extern const char* const kCffStdStrings[];
extern const std::size_t kCffStdStringsCount;
extern const std::uint16_t kCffStdEncoding[];
extern const std::size_t kCffStdEncodingCount;
extern const std::uint16_t kCffExpertEncoding[];
extern const std::size_t kCffExpertEncodingCount;
extern const char* const kCffIsoAdobeCharset[];
extern const std::size_t kCffIsoAdobeCharsetCount;
extern const char* const kCffExpertCharset[];
extern const std::size_t kCffExpertCharsetCount;
extern const char* const kCffExpertSubsetCharset[];
extern const std::size_t kCffExpertSubsetCharsetCount;

// type1.SymbolSet / type1.ZapfDingbats (sorted)
extern const char* const kSymbolSetNames[];
extern const std::size_t kSymbolSetCount;
extern const char* const kZapfDingbatsNames[];
extern const std::size_t kZapfDingbatsCount;

// AdobeGlyphList.txt (sorted by name)
extern const std::uint32_t kAglPool[];
extern const AglEntry kAgl[];
extern const std::size_t kAglCount;

extern const StdMetrics kStdMetrics[];
extern const std::size_t kStdMetricsCount;

extern const PredefinedCMap kPredefinedCMaps[];
extern const std::size_t kPredefinedCMapsCount;

// --- lookups (data.cpp) -------------------------------------------------------------------------------
const AglEntry* agl_find(std::string_view name);
bool symbol_set_has(std::string_view name);
bool zapf_dingbats_has(std::string_view name);
const StdMetrics* std_metrics(std::string_view font_name);
// AFM width of a glyph name (veraPDF StandardFontMetrics.getWidth: the name, else .notdef, else 0).
int std_width(const StdMetrics& m, std::string_view glyph_name, bool* found = nullptr);
const PredefinedCMap* predefined_cmap(std::string_view name);
// MAC_OS_ROMAN_ENCODING_MAP lookup; -1 when absent.
int mac_os_roman_code(std::string_view glyph_name);

}  // namespace eqx::fontdata
