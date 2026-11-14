// Font dictionaries with equal /Widths arrays share one copy of the numbers and one qpdf array (pdfont.cpp,
// shared_widths): every dictionary must read exactly as before, and every font must report the widths of its own array.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/document.h"
#include "fonts/font.h"
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
    for (const char* other : {"F3", "F4", "F5", "F6", "F7", "F8", "F10", "F12", "F13"})
        EXPECT(!widths_of(other).isSameObjectAs(widths_of("F1")));
    EXPECT(!widths_of("F8").isSameObjectAs(widths_of("F7")));     // different integers behind the same double
    EXPECT(!widths_of("F11").isSameObjectAs(widths_of("F10")));   // not made of integers only
    EXPECT(!widths_of("F13").isSameObjectAs(widths_of("F12")));
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
