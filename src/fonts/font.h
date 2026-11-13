// Font interface used by the content interpreter (character codes, advances, Unicode) and by the font
// rules. Implementations live in src/fonts/ (owned by the font module); the interpreter depends only on
// this header.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/cos.h"

namespace eqx {

class Document;
namespace fontimpl {
struct DocCache;
}

enum class FontKind : std::uint8_t { Type1, MMType1, TrueType, Type3, Type0, Unknown };

struct CharCode {
    std::uint32_t code = 0;   // character code (big-endian value of its bytes)
    std::uint8_t len = 1;     // number of bytes in the shown string
};

class Font {
public:
    virtual ~Font() = default;

    Obj dict;                 // font dictionary
    ObjRef ref;               // its object reference (invalid for direct dictionaries)
    FontKind kind = FontKind::Unknown;
    std::string subtype;      // /Subtype name
    std::string base_font;    // /BaseFont name (empty when absent)

    // Splits the bytes of a text-showing string into character codes (one byte for simple fonts,
    // CMap code space ranges for Type 0 fonts).
    virtual void codes(std::string_view bytes, std::vector<CharCode>& out) const = 0;
    // Horizontal advance of a code in glyph-space units of the font dictionary (/Widths, /W, /DW,
    // /MissingWidth); 1/1000 text-space units except for Type 3 (see glyph_to_text).
    virtual double advance(std::uint32_t code) const = 0;
    // Scale from glyph space to text space (0.001, or FontMatrix[0] of a Type 3 font).
    virtual double glyph_to_text() const { return 0.001; }
    // True when word spacing (Tw) applies: single-byte code 32.
    virtual bool word_space(const CharCode& c) const { return c.len == 1 && c.code == 32; }
    virtual bool vertical() const { return false; }
    // Unicode value of a code following veraPDF's PDFont.toUnicode (ToUnicode CMap, then the standard
    // encoding / Adobe Glyph List routes of ISO 32000 9.10.2); nullptr when no mapping exists. The pointer
    // stays valid for the font's lifetime (per-code cache).
    virtual const std::u32string* to_unicode(std::uint32_t code) const = 0;
};

// Loads each font dictionary once per document (cache by object reference or dictionary identity).
class FontRegistry {
public:
    explicit FontRegistry(Document& doc);
    ~FontRegistry();
    // Font for a font dictionary; nullptr when `font_dict` is not a dictionary.
    const Font* get(const Obj& font_dict);
    // Every font loaded so far, in load order.
    const std::vector<std::unique_ptr<Font>>& all() const { return fonts_; }
    Document& document() { return doc_; }

private:
    Document& doc_;
    std::vector<std::unique_ptr<Font>> fonts_;
    std::unordered_map<std::uint64_t, Font*> by_ref_;
    std::vector<std::pair<Obj, Font*>> direct_;
    std::shared_ptr<fontimpl::DocCache> cache_;   // per-document font program / CMap caches
};

// Factory implemented by the font module: builds the Font object for a dictionary.
std::unique_ptr<Font> load_font(Document& doc, const Obj& font_dict);

}  // namespace eqx
