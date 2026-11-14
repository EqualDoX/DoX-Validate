// Font dictionaries with equal /Widths arrays share one copy of the numbers and one qpdf array (pdfont.cpp,
// shared_widths): every dictionary must read exactly as before, and every font must report the widths of its own array.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/document.h"
#include "fonts/font.h"
#include "fonts/model.h"
#include "fonts/util.h"
#include "unit.h"

using namespace eqx;

namespace {

struct FontCase {
    const char* name;
    const char* widths;      // the /Widths value as PDF syntax, or nullptr for an indirect array
    const char* indirect;    // the array for the indirect case
    double expect[4];        // advance of codes 32..35
};

const FontCase kCases[] = {
    {"F1", "[500 600 700 800]", nullptr, {500, 600, 700, 800}},
    {"F2", "[500 600 700 800]", nullptr, {500, 600, 700, 800}},          // equal to F1: shares its array
    {"F3", "[500 600 700 801]", nullptr, {500, 600, 700, 801}},          // differs in one number
    {"F4", "[500 600.0 700 800]", nullptr, {500, 600, 700, 800}},        // a real: same numbers, another array
    {"F5", nullptr, "[500 600 700 800]", {500, 600, 700, 800}},          // indirect array
    {"F6", "[500 600 700]", nullptr, {500, 600, 700, 0}},                // shorter
    {"F7", "[9007199254740993 1 2 3]", nullptr, {9007199254740992.0, 1, 2, 3}},
    {"F8", "[9007199254740992 1 2 3]", nullptr, {9007199254740992.0, 1, 2, 3}},   // equal as doubles only
    {"F9", "[500 600 700 800]", nullptr, {500, 600, 700, 800}},          // a third one like F1
    {"F10", "[500 /Name 700 800]", nullptr, {500, 0, 700, 800}},         // no number at 33
    {"F11", "[500 /Name 700 800]", nullptr, {500, 0, 700, 800}},
    {"F12", "[-0.0 600 700 800]", nullptr, {-0.0, 600, 700, 800}},
    {"F13", "[0.0 600 700 800]", nullptr, {0.0, 600, 700, 800}},
    {"F14", "[500 null 700 800]", nullptr, {500, 0, 700, 800}},          // a null among the numbers
    {"F15", "[500 null 700 800]", nullptr, {500, 0, 700, 800}},
};

std::string make_document() {
    QPDF q;
    q.emptyPDF();
    Obj fonts = QPDFObjectHandle::newDictionary();
    for (const FontCase& c : kCases) {
        Obj f = QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Test /FirstChar 32 /LastChar 35 >>");
        if (c.widths) f.replaceKey("/Widths", QPDFObjectHandle::parse(c.widths));
        else f.replaceKey("/Widths", q.makeIndirectObject(QPDFObjectHandle::parse(c.indirect)));
        fonts.replaceKey(std::string("/") + c.name, q.makeIndirectObject(f));
    }
    Obj page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    Obj res = QPDFObjectHandle::newDictionary();
    res.replaceKey("/Font", fonts);
    page.replaceKey("/Resources", res);
    q.addPage(page, true);
    auto path = std::filesystem::temp_directory_path() / ("eqx_font_memory_" + std::to_string(::getpid()) + ".pdf");
    QPDFWriter w(q, path.string().c_str());
    w.setStaticID(true);
    w.setObjectStreamMode(qpdf_o_disable);
    w.write();
    return path.string();
}

}  // namespace

EQX_TEST(fonts_with_equal_widths_share_one_array) {
    const std::string path = make_document();
    OpenStatus status;
    std::string error;
    auto doc = Document::open(path, &status, &error);
    EXPECT(doc != nullptr);
    if (!doc) return;
    Obj fonts = cos::get(doc->pages()[0].resources, "Font");
    EXPECT(fonts.isDictionary());
    // what every dictionary says before any font is loaded
    std::vector<std::string> before;
    for (const FontCase& c : kCases) before.push_back(cos::get(fonts, c.name).unparseResolved());
    FontRegistry registry(*doc);
    std::vector<const Font*> loaded;
    for (const FontCase& c : kCases) loaded.push_back(registry.get(cos::get(fonts, c.name)));
    for (std::size_t i = 0; i < std::size(kCases); ++i) {
        const FontCase& c = kCases[i];
        Obj dict = cos::get(fonts, c.name);
        // the dictionary reads as before
        EXPECT_EQ(dict.unparseResolved(), before[i]);
        Obj w = dict.getKey("/Widths");
        EXPECT(w.isArray());
        EXPECT_EQ(w.isIndirect(), c.widths == nullptr);
        // and the font has the widths of its own array
        EXPECT(loaded[i] != nullptr);
        if (!loaded[i]) continue;
        for (int k = 0; k < 4; ++k) {
            const double got = loaded[i]->advance(static_cast<std::uint32_t>(32 + k));
            if (!(got == c.expect[k]) || std::signbit(got) != std::signbit(c.expect[k]))
                eqx_test::fail(__FILE__, __LINE__, std::string(c.name) + ": advance of code " + std::to_string(32 + k) + " is " +
                                                       std::to_string(got));
        }
    }
    const auto widths_of = [&](const char* name) { return cos::get(fonts, name).getKey("/Widths"); };
    // equal direct integer arrays are one object now
    EXPECT(widths_of("F2").isSameObjectAs(widths_of("F1")));
    EXPECT(widths_of("F9").isSameObjectAs(widths_of("F1")));
    // everything else keeps its own array
    for (const char* other : {"F3", "F4", "F5", "F6", "F7", "F8", "F10", "F12", "F13", "F14"})
        EXPECT(!widths_of(other).isSameObjectAs(widths_of("F1")));
    EXPECT(!widths_of("F8").isSameObjectAs(widths_of("F7")));     // different integers behind the same double
    EXPECT(!widths_of("F11").isSameObjectAs(widths_of("F10")));   // not made of integers only
    EXPECT(!widths_of("F13").isSameObjectAs(widths_of("F12")));
    EXPECT(!widths_of("F15").isSameObjectAs(widths_of("F14")));
    // a second registry over the same document finds the shared arrays and the same widths
    FontRegistry again(*doc);
    for (std::size_t i = 0; i < std::size(kCases); ++i) {
        const Font* f = again.get(cos::get(fonts, kCases[i].name));
        EXPECT(f != nullptr);
        for (int k = 0; f && k < 4; ++k) EXPECT(f->advance(static_cast<std::uint32_t>(32 + k)) == kCases[i].expect[k]);
        EXPECT_EQ(cos::get(fonts, kCases[i].name).unparseResolved(), before[i]);
    }
    std::filesystem::remove(path);
}

// ---- per-code records ------------------------------------------------------------------------------------------

namespace {

using eqx::fontimpl::CMap;
using eqx::fontimpl::DocCache;
using eqx::fontimpl::GlyphFacts;
using eqx::fontimpl::VeraFont;
using eqx::fontimpl::VeraKind;
namespace GlyphBits = eqx::fontimpl::GlyphBits;

// The bits as the font rules used to read them off GlyphFacts.
std::uint16_t reference_bits(const GlyphFacts& g) {
    std::uint16_t b = 0;
    if (g.present && !*g.present) b |= GlyphBits::kMissing;
    if (g.width_program && g.width_dictionary && !(std::fabs(*g.width_program - *g.width_dictionary) <= 1)) b |= GlyphBits::kWidthMismatch;
    if (g.has_unicode) b |= GlyphBits::kHasUnicode;
    if (g.unicode_invalid) b |= GlyphBits::kUnicodeInvalid;
    if (g.has_unicode20) b |= GlyphBits::kHasUnicode20;
    if (g.unicode_invalid20) b |= GlyphBits::kUnicodeInvalid20;
    if (g.unicode_pua20) b |= GlyphBits::kUnicodePua20;
    if (g.name == GlyphFacts::Name::Notdef) b |= GlyphBits::kNotdef;
    return b;
}

std::string make_font_document() {
    QPDF q;
    q.emptyPDF();
    Obj fonts = QPDFObjectHandle::newDictionary();
    // a standard font with an encoding, one with differences, a Type 3 font, a Type 0 font without a program
    fonts.replaceKey("/Std", q.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")));
    fonts.replaceKey("/Diff", q.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /FirstChar 30 /LastChar 40 "
        "/Widths [1 2 3 4 5 6 7 8 9 10 11] "
        "/Encoding << /Type /Encoding /Differences [32 /space /.notdef /A /uniE000 /nosuchglyph 200 /Euro] >> >>")));
    fonts.replaceKey("/T3", q.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1 1] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << >> "
        "/Encoding << /Type /Encoding /Differences [65 /a /b] >> /FirstChar 65 /LastChar 66 /Widths [500 600] >>")));
    Obj cid = q.makeIndirectObject(QPDFObjectHandle::parse(
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
        "/Supplement 0 >> /DW 750 /W [0 [100 200 300] 10 20 400 65535 [900]] >>"));
    Obj t0 = QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H >>");
    t0.replaceKey("/DescendantFonts", QPDFObjectHandle::newArray({cid}));
    fonts.replaceKey("/T0", q.makeIndirectObject(t0));
    Obj page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    Obj res = QPDFObjectHandle::newDictionary();
    res.replaceKey("/Font", fonts);
    page.replaceKey("/Resources", res);
    q.addPage(page, true);
    auto path = std::filesystem::temp_directory_path() / ("eqx_font_codes_" + std::to_string(::getpid()) + ".pdf");
    QPDFWriter w(q, path.string().c_str());
    w.setStaticID(true);
    w.write();
    return path.string();
}

std::u32string text_of(const std::u32string* t) { return t ? *t : std::u32string(U"\U0010FFFF none"); }

// A font answers from its records what a freshly loaded font computes, whatever the order of the questions.
void check_font(Document& doc, const Obj& dict, const std::vector<std::uint32_t>& codes, unsigned seed) {
    auto font = eqx::fontimpl::load_vera_font(doc, std::make_shared<DocCache>(), dict);
    auto fresh = eqx::fontimpl::load_vera_font(doc, std::make_shared<DocCache>(), dict);
    EXPECT(font && fresh);
    if (!font || !fresh) return;
    std::mt19937 rng(seed);
    std::vector<std::uint32_t> order = codes;
    for (int round = 0; round < 3; ++round) {   // every code three times, in three orders, the questions in any order
        std::shuffle(order.begin(), order.end(), rng);
        for (std::uint32_t code : order) {
            const unsigned q = rng() % 6;
            double adv = 0;
            std::uint16_t bits = 0;
            const std::u32string* text = nullptr;
            for (int k = 0; k < 3; ++k) {
                const int what = (q + k * (q % 2 ? 1 : 2)) % 3;
                if (what == 0) adv = font->advance(code);
                else if (what == 1) bits = font->glyph_bits(code);
                else text = font->to_unicode(code);
            }
            const GlyphFacts g = fresh->facts(code);
            const double want_adv = fresh->advance(code);
            int bad = 0;
            if (!(adv == want_adv)) bad |= 1;
            if (bits != reference_bits(g)) bad |= 2;
            if (text_of(text) != (g.has_unicode ? eqx::u16_to_u32(g.unicode) : text_of(nullptr))) bad |= 4;
            if (text != font->to_unicode(code)) bad |= 8;   // the same string object every time
            if (bad) {
                eqx_test::fail(__FILE__, __LINE__, "font record of code " + std::to_string(code) + " differs (" + std::to_string(bad) + ")");
                return;
            }
        }
    }
}

}  // namespace

EQX_TEST(font_code_records_match_fresh_computation) {
    const std::string path = make_font_document();
    OpenStatus status;
    std::string error;
    auto doc = Document::open(path, &status, &error);
    EXPECT(doc != nullptr);
    if (!doc) return;
    Obj fonts = cos::get(doc->pages()[0].resources, "Font");
    std::vector<std::uint32_t> bytes;
    for (std::uint32_t c = 0; c < 256; ++c) bytes.push_back(c);
    // single-byte fonts also asked for codes they cannot show
    std::vector<std::uint32_t> odd = bytes;
    for (std::uint32_t c : {256u, 257u, 0x1234u, 0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0xFFFFFFFFu}) odd.push_back(c);
    check_font(*doc, cos::get(fonts, "Std"), odd, 1);
    check_font(*doc, cos::get(fonts, "Diff"), odd, 2);
    check_font(*doc, cos::get(fonts, "T3"), odd, 3);
    // a Type 0 font over every two-byte code and some longer ones: more records than slot numbers (65535)
    std::vector<std::uint32_t> wide;
    for (std::uint32_t c = 0; c < 0x10010; ++c) wide.push_back(c);
    for (std::uint32_t c : {0x123456u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) wide.push_back(c);
    check_font(*doc, cos::get(fonts, "T0"), wide, 4);
    std::filesystem::remove(path);
}

EQX_TEST(font_each_code_matches_vera_codes) {
    using namespace std::string_literals;
    std::mt19937 rng(11);
    const auto compare = [&](const VeraFont& font, bool expect_direct) {
        for (int round = 0; round < 400; ++round) {
            std::string s(rng() % 14, '\0');
            for (char& c : s) c = (rng() % 5 == 0) ? '\xFF' : static_cast<char>(rng());
            std::vector<eqx::CharCode> want;
            font.vera_codes(s, want);
            std::vector<std::uint32_t> got;
            EXPECT_EQ(font.codes_direct(), expect_direct);
            if (!font.codes_direct()) continue;
            font.each_code(s, [&](std::uint32_t code) { got.push_back(code); });
            bool same = got.size() == want.size();
            for (std::size_t i = 0; same && i < got.size(); ++i) same = got[i] == want[i].code;
            if (!same) eqx_test::fail(__FILE__, __LINE__, "each_code differs from vera_codes");
        }
    };
    VeraFont simple;
    simple.vkind = VeraKind::Type1;
    compare(simple, true);
    VeraFont type3;
    type3.vkind = VeraKind::Type3;
    compare(type3, true);
    VeraFont no_cmap;
    no_cmap.vkind = VeraKind::Type0;
    compare(no_cmap, true);   // no codes either way
    VeraFont identity;
    identity.vkind = VeraKind::Type0;
    auto plain = std::make_shared<CMap>();
    plain->add_codespace("\x00\x00"s, "\xFF\xFF"s);
    identity.cmap_file = plain;
    compare(identity, true);
    VeraFont mixed;
    mixed.vkind = VeraKind::Type0;
    auto ranges = std::make_shared<CMap>();
    ranges->add_codespace("\x00"s, "\x80"s);
    ranges->add_codespace("\x81\x40"s, "\x9F\xFC"s);
    mixed.cmap_file = ranges;
    compare(mixed, false);
}
