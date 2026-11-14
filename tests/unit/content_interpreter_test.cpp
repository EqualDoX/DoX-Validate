// Unit tests of the content interpreter: which observers get which callbacks, in which order, and what a small page
// leaves in the content model.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/content.h"
#include "core/document.h"
#include "core/structure.h"
#include "unit.h"

using namespace eqx;

namespace {

std::string write_page_pdf(const std::string& content) {
    static int counter = 0;
    QPDF q;
    q.emptyPDF();
    Obj font = q.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    Obj page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    Obj fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/F1", font);
    Obj resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", q.newStream(content));
    q.addPage(page, true);
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_content_interpreter_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    QPDFWriter w(q, path.string().c_str());
    w.setStaticID(true);
    w.write();
    return path.string();
}

std::vector<std::string>* event_log = nullptr;

struct GlyphsOnly : ContentObserver {
    int glyphs = 0;
    void on_glyph(const ContentModel&, const GlyphEvent&) override {
        ++glyphs;
        event_log->push_back("g:first");
    }
};
struct OperatorsOnly : ContentObserver {
    std::vector<std::string> ops;
    void on_operator(const ContentModel&, int, std::uint32_t, const RawOp& op, const GState&) override {
        ops.emplace_back(op.op);
        event_log->push_back("o:second");
    }
};
struct EndsOnly : ContentObserver {
    int ends = 0;
    void on_stream_end(const ContentModel&, int) override { ++ends; }
};
struct Nothing : ContentObserver {};
struct Everything : ContentObserver {
    int glyphs = 0, ops = 0, ends = 0;
    std::size_t items_at_first_glyph = 0, texts_at_first_glyph = 0;
    void on_glyph(const ContentModel& m, const GlyphEvent&) override {
        if (glyphs++ == 0) items_at_first_glyph = m.items.size(), texts_at_first_glyph = m.texts.size();
        event_log->push_back("g:last");
    }
    void on_operator(const ContentModel&, int, std::uint32_t, const RawOp&, const GState&) override {
        ++ops;
        event_log->push_back("o:last");
    }
    void on_stream_end(const ContentModel&, int) override { ++ends; }
};
// Overrides through a base class of its own.
struct InheritsGlyphs : GlyphsOnly {};

}  // namespace

EQX_TEST(content_interpreter_observers_and_model) {
    const std::string path = write_page_pdf(
        "BT /F1 12 Tf 72 700 Td (Hello) Tj [(Wor) -20 (ld)] TJ ET\n0.5 g 10 10 100 50 re f\nq 1 0 0 1 5 5 cm 0 0 m 5 5 l S Q\n");
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path, &status, &err);
    std::filesystem::remove(path);
    EXPECT(doc != nullptr);
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    FontRegistry fonts(*doc);
    ContentModel cm;
    std::vector<std::string> log;
    event_log = &log;
    GlyphsOnly first;
    OperatorsOnly second;
    EndsOnly ends;
    Nothing nothing;
    InheritsGlyphs inherited;
    Everything last;
    interpret_document(*doc, st, fonts, cm, {&first, &second, &ends, &nothing, &inherited, &last});
    event_log = nullptr;

    const std::vector<std::string> ops = {"BT", "Tf", "Td", "Tj", "TJ", "ET", "g", "re", "f", "q", "cm", "m", "l", "S", "Q"};
    EXPECT_EQ(first.glyphs, 10);
    EXPECT_EQ(inherited.glyphs, 10);
    EXPECT_EQ(last.glyphs, 10);
    EXPECT(second.ops == ops);
    EXPECT_EQ(last.ops, static_cast<int>(ops.size()));
    EXPECT_EQ(ends.ends, 1);
    EXPECT_EQ(last.ends, 1);
    // The item and the run of a text-showing operator exist when its first glyph is reported.
    EXPECT_EQ(last.items_at_first_glyph, std::size_t(1));
    EXPECT_EQ(last.texts_at_first_glyph, std::size_t(1));
    // Per event, the observers are called in the order they were given.
    bool ordered = true;
    std::size_t glyph_events = 0, op_events = 0;
    for (std::size_t i = 0; i < log.size();) {
        if (log[i] == "g:first") {
            ordered = ordered && i + 2 < log.size() && log[i + 1] == "g:first" && log[i + 2] == "g:last";   // first, inherited, last
            i += 3;
            ++glyph_events;
        } else if (log[i] == "o:second") {
            ordered = ordered && i + 1 < log.size() && log[i + 1] == "o:last";
            i += 2;
            ++op_events;
        } else {
            ordered = false;
            break;
        }
    }
    EXPECT(ordered);
    EXPECT_EQ(glyph_events, std::size_t(10));
    EXPECT_EQ(op_events, ops.size());

    EXPECT_EQ(cm.streams.size(), std::size_t(1));
    EXPECT_EQ(cm.streams[0].op_count, static_cast<std::uint32_t>(ops.size()));
    EXPECT_EQ(cm.items.size(), std::size_t(4));
    EXPECT_EQ(cm.texts.size(), std::size_t(2));
    const ContentItem& hello = cm.items[0];
    EXPECT(hello.kind == ContentItem::Kind::Text && hello.text == 0 && hello.op_index == 3 && hello.bbox_valid && hello.frame == -1);
    const TextRun& run0 = cm.texts[0];
    EXPECT(run0.text == std::u32string_view(U"Hello"));
    EXPECT_EQ(run0.glyphs, 5u);
    EXPECT_EQ(run0.unmapped, 0u);
    EXPECT(run0.size == 12 && run0.size_user == 12 && run0.tr == 0 && run0.font != nullptr && run0.item == 0);
    EXPECT(run0.fill.family == ColorFamily::Gray && run0.fill.rgb[0] == 0 && run0.fill_alpha == 1);
    EXPECT(&run0.bbox == &hello.bbox);                         // the box of a run is the box of its item
    EXPECT(hello.bbox.llx == 72 && hello.bbox.lly == 700 - 0.2 * 12 && hello.bbox.ury == 700 + 0.8 * 12 && hello.bbox.urx > 72);
    const TextRun& run1 = cm.texts[1];
    EXPECT(run1.text == std::u32string_view(U"World"));
    EXPECT_EQ(run1.glyphs, 5u);
    EXPECT(run1.font == run0.font && run1.item == 1);
    EXPECT(cm.items[1].bbox.llx == hello.bbox.urx);            // the second operator starts where the first ended
    const ContentItem& rect = cm.items[2];
    EXPECT(rect.kind == ContentItem::Kind::LineArt && rect.paint == 1 && rect.exact_rect && rect.text == -1);
    EXPECT(rect.fill.family == ColorFamily::Gray && rect.fill.rgb[0] == 0.5f && rect.fill_alpha == 1.0f);
    EXPECT(rect.bbox_valid && rect.bbox.llx == 10 && rect.bbox.lly == 10 && rect.bbox.urx == 110 && rect.bbox.ury == 60);
    const ContentItem& line = cm.items[3];
    EXPECT(line.kind == ContentItem::Kind::LineArt && line.paint == 2 && !line.exact_rect);
    EXPECT(line.bbox.llx == 5 && line.bbox.lly == 5 && line.bbox.urx == 10 && line.bbox.ury == 10);
}

EQX_TEST(content_interpreter_long_text_operator) {
    // More glyphs in one operator than an item record counts itself, then an ordinary operator.
    const std::size_t n = 70000;
    std::string content = "BT /F1 10 Tf 0 0 Td (" + std::string(n, 'a') + ") Tj (b\\200c) Tj ET\n";
    const std::string path = write_page_pdf(content);
    OpenStatus status;
    std::string err;
    auto doc = Document::open(path, &status, &err);
    std::filesystem::remove(path);
    EXPECT(doc != nullptr);
    if (!doc) return;
    StructTree st;
    st.build(*doc);
    FontRegistry fonts(*doc);
    ContentModel cm;
    interpret_document(*doc, st, fonts, cm, {});
    EXPECT_EQ(cm.texts.size(), std::size_t(2));
    const TextRun& big = cm.texts[0];
    EXPECT_EQ(big.glyphs, static_cast<std::uint32_t>(n));
    EXPECT_EQ(big.unmapped, 0u);
    EXPECT_EQ(big.text.size(), n);
    EXPECT(big.text.front() == U'a' && big.text.back() == U'a');
    const TextRun& small = cm.texts[1];
    EXPECT_EQ(small.glyphs, 3u);
    EXPECT_EQ(small.glyphs - small.unmapped, static_cast<std::uint32_t>(small.text.size()));
    EXPECT(small.text.front() == U'b');
}
