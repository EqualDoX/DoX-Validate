// Unit tests of the font engine's veraPDF ports: CMaps (embedded and predefined), the PostScript interpreter,
// Type 1 / TrueType program parsing and Java numeric helpers.
#include <fstream>
#include <iterator>
#include <sstream>

#include "fonts/charstrings.h"
#include "fonts/cmap.h"
#include "fonts/data.h"
#include "fonts/programs.h"
#include "fonts/ps.h"
#include "fonts/util.h"
#include "unit.h"

using namespace eqx;
using namespace eqx::fontimpl;

namespace {

std::vector<std::int32_t> read_all(const CMap& cm, std::string_view bytes, std::vector<std::size_t>* lens = nullptr) {
    std::vector<std::int32_t> out;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        std::size_t start = pos;
        std::int32_t c;
        if (!cm.read_code(bytes, pos, c)) break;
        out.push_back(c);
        if (lens) lens->push_back(pos - start);
        if (pos == start) break;
    }
    return out;
}

std::string u8(const std::u16string& s) { return u16_to_utf8(s); }

}  // namespace

EQX_TEST(fonts_cmap_mixed_codespace) {
    auto cm = parse_cmap_program(
        "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        "/CMapName /Test-H def /WMode 1 def\n"
        "2 begincodespacerange <00> <80> <8140> <9FFC> endcodespacerange\n"
        "2 begincidrange <20> <7e> 1 <8140> <817e> 633 endcidrange\n"
        "1 begincidchar <8150> 9999 endcidchar\n"
        "endcmap CMapName currentdict /CMap defineresource pop end end\n");
    EXPECT_EQ(cm->wmode, 1);
    std::vector<std::size_t> lens;
    auto codes = read_all(*cm, std::string("A\x81\x40" "B", 4), &lens);
    EXPECT_EQ(codes.size(), std::size_t(3));
    EXPECT_EQ(codes[1], 0x8140);
    EXPECT_EQ(lens[1], std::size_t(2));
    EXPECT_EQ(cm->to_cid('A'), 1 + 'A' - 0x20);
    EXPECT_EQ(cm->to_cid(0x8141), 634);
    EXPECT_EQ(cm->to_cid(0x8150), 9999);   // later cidchar wins over the earlier range
    EXPECT_EQ(cm->to_cid(0x9000), 0);
    EXPECT(!cm->contains_code(0x9000));
}

EQX_TEST(fonts_cmap_no_partial_match_skips_shortest_length) {
    auto cm = parse_cmap_program("1 begincodespacerange <8140> <9FFC> endcodespacerange\n");
    // 0x41 matches no range prefix: veraPDF reads (shortest length - 1) more bytes and returns code 0
    auto codes = read_all(*cm, std::string("\x41\x42\x81\x40", 4));
    EXPECT_EQ(codes.size(), std::size_t(2));
    EXPECT_EQ(codes[0], 0);
    EXPECT_EQ(codes[1], 0x8140);
}

EQX_TEST(fonts_cmap_odd_trailing_byte_reads_ff) {
    auto cm = predefined_cmap("Identity-H");
    EXPECT(cm != nullptr);
    auto codes = read_all(*cm, std::string("\x00\x41\x12", 3));
    EXPECT_EQ(codes.size(), std::size_t(2));
    EXPECT_EQ(codes[1], 0x12FF);   // ByteArrayInputStream.read() == -1 -> (byte) 0xFF
    EXPECT_EQ(cm->to_cid(0x1234), 0x1234);
}

EQX_TEST(fonts_cmap_broken_entry_gives_empty_cmap) {
    auto cm = parse_cmap_program("1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                 "2 begincidrange <0000> <00FF> 0 endcidrange\n");   // second entry missing
    EXPECT(cm->codespaces.empty());
    std::vector<std::size_t> lens;
    auto codes = read_all(*cm, "abcdefghij", &lens);
    EXPECT_EQ(codes.size(), std::size_t(2));   // 5 bytes per code, code 0
    EXPECT_EQ(lens[0], std::size_t(5));
}

EQX_TEST(fonts_tounicode_ranges) {
    auto cm = parse_cmap_program(
        "1 begincodespacerange <00> <FF> endcodespacerange\n"
        "2 beginbfchar <01> <FFFE> <02> /space endbfchar\n"
        "3 beginbfrange <10> <12> <0041> <20> <21> [<0062> <00630064>] <30> <31> <FEFE> endbfrange\n");
    std::u16string scratch;
    const std::u16string* s = cm->unicode(0x01, scratch);
    EXPECT(s && *s == std::u16string(1, char16_t(0xFFFE)));
    s = cm->unicode(0x02, scratch);
    EXPECT(s && u8(*s) == "space");   // a name is taken as its text
    s = cm->unicode(0x12, scratch);
    EXPECT(s && u8(*s) == "C");
    s = cm->unicode(0x21, scratch);
    EXPECT(s && u8(*s) == "cd");
    s = cm->unicode(0x31, scratch);   // FEFE + 1 = FEFF -> veraPDF returns U+FFFE
    EXPECT(s && *s == std::u16string(1, char16_t(0xFFFE)));
    EXPECT(cm->unicode(0x40, scratch) == nullptr);
}

EQX_TEST(fonts_tounicode_interval_leading_zero_byte) {
    // ToUnicodeInterval: a value whose first byte is 0 yields only its last byte
    UnicodeInterval iv{0x10, 0x11, 4, 0x00410042};
    EXPECT_EQ(u8(iv.to_unicode(0x10)), std::string("B"));
}

EQX_TEST(fonts_predefined_ucs2) {
    auto u = predefined_cmap("Adobe-Japan1-UCS2");
    EXPECT(u != nullptr);
    std::u16string scratch;
    const std::u16string* s = u->unicode(0x21, scratch);
    EXPECT(s && u8(*s) == "@");
    s = u->unicode(0x3d, scratch);
    EXPECT(s && *s == std::u16string(1, char16_t(0xA5)));
    auto v = predefined_cmap("Identity-V");   // usecmap Identity-H
    EXPECT(v && v->wmode == 1 && v->to_cid(0x0102) == 0x0102);
    auto rksj = predefined_cmap("90ms-RKSJ-H");
    std::vector<std::size_t> lens;
    read_all(*rksj, std::string("A\x82\xa0", 3), &lens);
    EXPECT_EQ(lens.size(), std::size_t(2));
    EXPECT_EQ(lens[1], std::size_t(2));
    EXPECT(predefined_cmap("EQX-Unknown-H") == nullptr);
    EXPECT(is_table118_cmap("UniJIS-UCS2-HW-H") && !is_table118_cmap("Adobe-Japan1-6"));
}

EQX_TEST(fonts_ps_encoding_program) {
    ps::Source src("/Encoding 256 array 0 1 255 {1 index exch /.notdef put} for dup 65 /A put readonly def "
                   "/FontMatrix [0.001 0 0 0.001 0 0] def",
                   ps::Source::Kind::kBuffered);
    ps::Lexer lx(src, true, ps::Lexer::Comments::kNotSeekable);
    ps::ObjectParser op(lx);
    ps::Interpreter in;
    lx.skip_spaces(true);
    ps::Obj o = op.next_object();
    while (lx.token().type != ps::TT::Eof) {
        in.execute(o);
        o = op.next_object();
    }
    const ps::Obj* e = in.user("Encoding");
    EXPECT(e && e->t == ps::Obj::T::Array && e->a->size() == 256);
    EXPECT_EQ((*e->a)[65].s, std::string("A"));
    EXPECT_EQ((*e->a)[66].s, std::string(".notdef"));
    const ps::Obj* m = in.user("FontMatrix");
    EXPECT(m && m->a->size() == 6 && (*m->a)[0].real() == 0.001);
}

namespace {

std::string t1_num(int v) {
    if (v >= -107 && v <= 107) return std::string(1, static_cast<char>(v + 139));
    if (v >= 108 && v <= 1131) {
        v -= 108;
        return {static_cast<char>((v >> 8) + 247), static_cast<char>(v & 0xFF)};
    }
    std::string s(1, '\xff');
    for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((static_cast<unsigned>(v) >> (8 * k)) & 0xFF));
    return s;
}

std::string encrypt(const std::string& data, int r) {
    std::string out;
    for (char ch : std::string(4, '\0') + data) {
        int c = static_cast<unsigned char>(ch) ^ (r >> 8);
        out.push_back(static_cast<char>(c));
        r = ((c + r) * 52845 + 22719) & 0xFFFF;
    }
    return out;
}

std::string type1_font(const std::string& matrix) {
    auto cs = [](int adv) { return t1_num(0) + t1_num(adv) + std::string(1, '\x0d') + std::string(1, '\x0e'); };
    std::string clear = "%!PS-AdobeFont-1.0: T 001\n11 dict begin\n/FontName /T def\n/Encoding 256 array\n"
                        "0 1 255 {1 index exch /.notdef put} for\ndup 97 /a put\nreadonly def\n/FontMatrix " +
                        matrix + " readonly def\ncurrentdict end\ncurrentfile eexec\n";
    std::string priv = "dup /Private 8 dict dup begin\n/RD{string currentfile exch readstring pop}executeonly def\n"
                       "/lenIV 4 def\n/Subrs 0 array\n2 index /CharStrings 3 dict dup begin\n";
    for (auto [n, w] : {std::pair<std::string, int>{".notdef", 500}, {"a", 556}, {"b", 1200}}) {
        std::string e = encrypt(cs(w), 4330);
        priv += "/" + n + " " + std::to_string(e.size()) + " RD " + e + " ND\n";
    }
    priv += "end\nend\nmark currentfile closefile\n";
    std::string zeros;
    for (int i = 0; i < 8; ++i) zeros += std::string(64, '0') + "\n";
    return clear + encrypt(priv, 55665) + zeros + "cleartomark\n";
}

}  // namespace

EQX_TEST(fonts_type1_program) {
    auto d = parse_type1(type1_font("[0.001 0 0 0.001 0 0]"));
    EXPECT(d->parsed);
    EXPECT_EQ(d->widths.size(), std::size_t(3));
    EXPECT_EQ(d->widths.at("a"), 556);
    EXPECT_EQ(d->widths.at("b"), 1200);
    EXPECT(d->encoding_set[97] && d->encoding[97] == "a");
    auto p = make_type1_program(d);
    EXPECT(p->contains_code(97) && !p->contains_code(98) && !p->contains_glyph(".notdef"));
    EXPECT_EQ(p->width(97), 556.0f);
    // non-default FontMatrix: (int)(width * FontMatrix[0] * 1000)
    auto d2 = parse_type1(type1_font("[0.0005 0 0 0.0005 0 0]"));
    EXPECT(d2->parsed && d2->widths.at("a") == 278);
    EXPECT(!parse_type1("%!PS no eexec section\n")->parsed);
}

EQX_TEST(fonts_truetype_fixture_font) {
    std::ifstream in(eqx_test::fixture("../fonts/EQXFixtureSans-Regular.ttf"), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) return;   // fixture font not available
    auto t = parse_truetype(data);
    EXPECT(t->parsed);
    EXPECT_EQ(t->num_glyphs, 110);
    EXPECT_EQ(t->cmaps.size(), std::size_t(2));
    EXPECT(t->cmap(3, 1) && t->cmap(0, 3) && !t->cmap(1, 0));
    SimpleEncodingInfo enc;
    enc.present = true;
    enc.type = SimpleEncodingInfo::Type::Name;
    enc.name = "WinAnsiEncoding";
    auto p = make_truetype_program(t, false, enc);
    EXPECT(p->gf_parsed() && p->contains_code('A') && !p->contains_code(0x01));   // WinAnsi 0x01 = .notdef
    SimpleEncodingInfo std_enc = enc;
    std_enc.name = "StandardEncoding";   // createCIDToNameTable throws: parsed but not "GF parsed"
    auto q = make_truetype_program(t, false, std_enc);
    EXPECT(q->successful() && !q->gf_parsed());
    EXPECT(!parse_truetype("garbage")->parsed);
}

EQX_TEST(fonts_charstring_widths) {
    // Type 2: hstem with an odd operand count carries the width; callsubr enters a local subroutine
    std::string cs = t1_num(300) + t1_num(10) + t1_num(20) + std::string(1, '\x01');
    auto w = type2_charstring_width(cs, nullptr, 0, nullptr, 0);
    EXPECT(w && w->integer == 300);
    std::string cs2 = t1_num(5) + t1_num(6) + std::string(1, '\x15');   // rmoveto without width
    EXPECT(!type2_charstring_width(cs2, nullptr, 0, nullptr, 0));
    // Type 1: sbw
    std::string t1 = t1_num(0) + t1_num(0) + t1_num(640) + t1_num(0) + std::string("\x0c\x07", 2);
    auto w1 = type1_charstring_width(t1, nullptr);
    EXPECT(w1 && w1->integer == 640);
}

EQX_TEST(fonts_java_numerics) {
    EXPECT_EQ(java_round(-2.5), std::int64_t(-2));
    EXPECT_EQ(java_round(2.5), std::int64_t(3));
    EXPECT_EQ(java_d2i(1e20), std::int32_t(2147483647));
    EXPECT(java_parse_float("-1.5E-3").has_value() && !java_parse_float("E5") && !java_parse_float("-"));
    EXPECT(!java_parse_long("", 10) && java_parse_long("17", 8) == std::optional<std::int64_t>(15));
    std::u16string bad = utf16be_decode(std::string("\xD8\x00\x00\x41", 4));
    EXPECT(bad.size() == 2 && bad[0] == 0xFFFD && bad[1] == 'A');
}

EQX_TEST(fonts_data_tables) {
    const fontdata::AglEntry* e = fontdata::agl_find(".notdef");
    EXPECT(e && fontdata::kAglPool[e->pool] == 0);   // veraPDF's AGL maps .notdef to U+0000
    EXPECT(fontdata::agl_find("uni0041") == nullptr);   // no uniXXXX rule in veraPDF's AdobeGlyphList
    const fontdata::StdMetrics* m = fontdata::std_metrics("Helvetica");
    EXPECT(m && fontdata::std_width(*m, "A") == 667);
    EXPECT(fontdata::symbol_set_has("Alpha") && fontdata::zapf_dingbats_has("a100"));
    EXPECT_EQ(std::string(fontdata::kWinAnsi[65]), std::string("A"));
    EXPECT_EQ(fontdata::mac_os_roman_code("Adieresis"), 128);
}
