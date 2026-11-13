// Unit tests of the pure annotations helpers (veraPDF COS conversions, page boxes, number/name trees,
// rich text flattening, media clip Alt).
#include "rules/annotations/annot_helpers.h"

#include <qpdf/QPDF.hh>

#include "unit.h"

using namespace eqx;
using namespace eqx::annot;

namespace {
Obj P(const char* s) { return QPDFObjectHandle::parse(s); }
}  // namespace

EQX_TEST(annotations_vera_integer_and_real) {
    EXPECT_EQ(vera_integer(P("34")).value_or(-1), 34);
    EXPECT_EQ(vera_integer(P("2.9")).value_or(-1), 2);        // (long) truncation
    EXPECT_EQ(vera_integer(P("-2.9")).value_or(0), -2);
    EXPECT_EQ(vera_integer(P("(abc)")).value_or(-1), 3);      // COSString.getInteger = byte length
    EXPECT(!vera_integer(P("/Name")).has_value());
    EXPECT(!vera_integer(P("null")).has_value());
    EXPECT_EQ(vera_real(P("(ab)")).value_or(-1), 2.0);
    EXPECT(!vera_real(P("true")).has_value());
}

EQX_TEST(annotations_vera_strings) {
    Obj d = P("<< /Contents (Hello) /N /Name /U <FEFF00E4> /E () >>");
    EXPECT_EQ(vera_string_key(d, "Contents").value_or(""), std::string("Hello"));
    EXPECT(!vera_string_key(d, "N").has_value());                 // getStringKey: names are null
    EXPECT_EQ(vera_string_key(d, "U").value_or(""), std::string("\xC3\xA4"));
    EXPECT(vera_string_key(d, "E").has_value());
    EXPECT(vera_string_key(d, "E")->empty());
    EXPECT(!vera_string_key(d, "Missing").has_value());
    EXPECT_EQ(vera_string(P("/Abc")).value_or(""), std::string("Abc"));   // COSName.getString
}

EQX_TEST(annotations_real_array) {
    EXPECT_EQ(vera_real_array(P("[1 2.5 3 4]"))->size(), 4u);
    EXPECT_EQ(vera_real_array(P("[1 (xy) 3]"))->at(1), 2.0);
    EXPECT(!vera_real_array(P("[1 /N 3 4]")).has_value());
    EXPECT(!vera_real_array(P("(1 2 3 4)")).has_value());
}

EQX_TEST(annotations_crop_box) {
    QPDF q;
    q.emptyPDF();
    Obj parent = q.makeIndirectObject(P("<< /Type /Pages /MediaBox [0 0 600 800] /CropBox [10 10 700 700] >>"));
    Obj page = q.makeIndirectObject(P("<< /Type /Page >>"));
    page.replaceKey("/Parent", parent);
    auto cb = vera_crop_box(page);
    EXPECT(cb.has_value());
    EXPECT_EQ((*cb)[2], 600.0);   // clipped to the MediaBox
    EXPECT_EQ((*cb)[3], 700.0);
    page.replaceKey("/CropBox", P("/NotAnArray"));   // inherited search stops, MediaBox used
    cb = vera_crop_box(page);
    EXPECT_EQ((*cb)[0], 0.0);
    EXPECT_EQ((*cb)[3], 800.0);
    parent.replaceKey("/Parent", page);   // loop in the page tree
    page.removeKey("/CropBox");
    parent.removeKey("/MediaBox");
    parent.removeKey("/CropBox");
    EXPECT(!vera_crop_box(page).has_value());

    Box c{0, 0, 595, 842};
    EXPECT(outside_crop_box(c, std::vector<double>{600, 600, 620, 620}).value_or(false));
    EXPECT(!outside_crop_box(c, std::vector<double>{576, 600, 596, 620}).value_or(true));
    EXPECT(outside_crop_box(c, std::vector<double>{100, 842, 120, 900}).value_or(false));   // touching counts
    EXPECT(!outside_crop_box(c, std::vector<double>{1, 2, 3}).has_value());
    EXPECT(!outside_crop_box(std::nullopt, std::vector<double>{1, 2, 3, 4}).has_value());
}

EQX_TEST(annotations_number_tree) {
    QPDF q;
    q.emptyPDF();
    Obj leaf1 = q.makeIndirectObject(P("<< /Limits [0 5] /Nums [0 (a) 3 (b) 3 (c)] >>"));
    Obj leaf2 = q.makeIndirectObject(P("<< /Limits [6 9] /Nums [7 (d)] >>"));
    Obj root = q.makeIndirectObject(P("<< /Kids [] >>"));
    root.replaceKey("/Kids", QPDFObjectHandle::newArray({leaf1, leaf2}));
    NumberTree t(root);
    EXPECT_EQ(t.get(0).getStringValue(), std::string("a"));
    EXPECT_EQ(t.get(3).getStringValue(), std::string("c"));   // later duplicate wins (HashMap.put)
    EXPECT_EQ(t.get(7).getStringValue(), std::string("d"));
    EXPECT(t.get(4).isNull());
    EXPECT(t.get(42).isNull());
    // A key listed in a leaf but outside its Limits is not found (PDNumberTreeNode.getObject).
    Obj bad = q.makeIndirectObject(P("<< /Limits [10 12] /Nums [20 (x)] >>"));
    Obj root2 = P("<< >>");
    root2.replaceKey("/Kids", QPDFObjectHandle::newArray({bad}));
    EXPECT(NumberTree(root2).get(20).isNull());
    // Nums wins over Kids, and a loop ends the branch.
    Obj loop = q.makeIndirectObject(P("<< >>"));
    loop.replaceKey("/Kids", QPDFObjectHandle::newArray({loop}));
    EXPECT(NumberTree(loop).get(1).isNull());
    Obj both = P("<< /Nums [1 (n)] >>");
    both.replaceKey("/Kids", QPDFObjectHandle::newArray({leaf1}));
    EXPECT(NumberTree(both).get(0).isNull());
    EXPECT_EQ(NumberTree(both).get(1).getStringValue(), std::string("n"));
    // Direct root (cached separately), odd Nums array, non-integer keys.
    Obj direct = P("<< /Nums [1 (one) 2.0 (two) 3] >>");
    NumberTree t3(direct);
    EXPECT_EQ(t3.get(1).getStringValue(), std::string("one"));
    EXPECT(t3.get(2).isNull());
    EXPECT(t3.get(3).isNull());
}

EQX_TEST(annotations_name_tree) {
    QPDF q;
    q.emptyPDF();
    Obj leaf = q.makeIndirectObject(P("<< /Limits [(a) (m)] /Names [(alpha) 1 /beta 2 (zeta) 3] >>"));
    Obj root = P("<< >>");
    root.replaceKey("/Kids", QPDFObjectHandle::newArray({leaf}));
    NameTree t(root);
    EXPECT_EQ(t.get("alpha").getIntValue(), 1);
    EXPECT_EQ(t.get("beta").getIntValue(), 2);   // name keys count (COSName.getString)
    EXPECT(t.get("zeta").isNull());              // outside the Limits
    EXPECT(t.get("gamma").isNull());
}

EQX_TEST(annotations_rich_text) {
    EXPECT_EQ(rich_text_plain("<p>a</p>").value_or("?"), std::string("a"));
    EXPECT_EQ(rich_text_plain("<?xml version=\"1.0\"?>\n<body xmlns=\"http://www.w3.org/1999/xhtml\"><p>Hello "
                              "<span style=\"x\">big</span> world</p></body>")
                  .value_or("?"),
              std::string("Hello big world"));
    EXPECT_EQ(rich_text_plain("<p>a<!-- c -->b<![CDATA[<c>]]>&amp;&#65;&#x42;</p>").value_or("?"),
              std::string("ab<c>&AB"));
    EXPECT_EQ(rich_text_plain("<p>\n  x\r\n</p>").value_or("?"), std::string("\n  x\n"));
    EXPECT_EQ(rich_text_plain("<p><?pi data?>t</p>").value_or("?"), std::string("datat"));
    EXPECT(!rich_text_plain("plain text").has_value());        // content is not allowed in the prolog
    EXPECT(!rich_text_plain("<p>a</p><p>b</p>").has_value());  // two root elements
    EXPECT(!rich_text_plain("<p>a").has_value());
    EXPECT(!rich_text_plain("<p>&nbsp;</p>").has_value());     // undeclared entity
    EXPECT(!rich_text_plain("<p>a & b</p>").has_value());
    EXPECT(!rich_text_plain("<x:p>a</x:p>").has_value());      // unbound prefix (namespace aware)
    EXPECT(rich_text_plain("<x:p xmlns:x=\"u\">a</x:p>").has_value());
    EXPECT(!rich_text_plain("").has_value());
}

EQX_TEST(annotations_rich_text_entries) {
    Obj d = P("<< /RC (<p>rich</p>) /V (rich) /N /rich /S (no xml) >>");
    EXPECT_EQ(rich_text_entry(d, "RC").value_or("?"), std::string("rich"));
    EXPECT_EQ(text_or_stream_entry(d, "V").value_or("?"), std::string("rich"));
    EXPECT(!text_or_stream_entry(d, "N").has_value());
    EXPECT(!rich_text_entry(d, "N").has_value());
    EXPECT(!rich_text_entry(d, "S").has_value());
    EXPECT(!rich_text_entry(d, "Missing").has_value());
}

EQX_TEST(annotations_media_clip_alt) {
    EXPECT(!media_clip_has_correct_alt(P("<< >>")));
    EXPECT(media_clip_has_correct_alt(P("<< /Alt [] >>")));
    EXPECT(media_clip_has_correct_alt(P("<< /Alt [(en) (A movie) () (Ein Film)] >>")));
    EXPECT(!media_clip_has_correct_alt(P("<< /Alt [(en) ()] >>")));
    EXPECT(!media_clip_has_correct_alt(P("<< /Alt [(en)] >>")));
    EXPECT(!media_clip_has_correct_alt(P("<< /Alt [/en (x)] >>")));
    EXPECT(!media_clip_has_correct_alt(P("<< /Alt (x) >>")));
}
