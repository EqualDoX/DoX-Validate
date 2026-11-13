// Unit tests of the document module's XMP reader (veraPDF / Adobe XMP toolkit semantics).
#include <string>

#include "rules/document/xml_dom.h"
#include "rules/document/xmp.h"
#include "unit.h"

using eqx::xmp::Packet;

namespace {

std::string packet(const std::string& desc_attrs, const std::string& body) {
    return "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
           "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
           "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
           "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
           "xmlns:pdfuaid=\"http://www.aiim.org/pdfua/ns/id/\" xmlns:pdfd=\"http://pdfa.org/declarations/\" " +
           desc_attrs + ">\n" + body + "\n</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
}

const char* kTitle = "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Title</rdf:li></rdf:Alt></dc:title>";

}  // namespace

EQX_TEST(document_xmp_identification_and_title) {
    Packet p = Packet::parse(packet("", std::string("<pdfuaid:part>1</pdfuaid:part>") + kTitle +
                                            "<pdfd:declarations><rdf:Bag><rdf:li rdf:parseType=\"Resource\">"
                                            "<pdfd:conformsTo>http://pdfa.org/declarations/wtpdf#reuse1.0"
                                            "</pdfd:conformsTo></rdf:li></rdf:Bag></pdfd:declarations>"));
    EXPECT(p.valid());
    EXPECT(p.contains_namespace(eqx::xmp::kNsPdfuaId));
    EXPECT(p.pdfua_part() == std::optional<std::int64_t>(1));
    EXPECT(!p.pdfua_rev());
    EXPECT(p.dc_title() == std::optional<std::string>("Title"));
    EXPECT(p.declarations().count("http://pdfa.org/declarations/wtpdf#reuse1.0") == 1);
    const auto* part = p.property(eqx::xmp::kNsPdfuaId, "part");
    EXPECT(part && part->prefix && *part->prefix == "pdfuaid");
}

EQX_TEST(document_xmp_original_prefix_and_attribute_form) {
    std::string x = "<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description rdf:about=\"\" "
                    "xmlns:ua=\"http://www.aiim.org/pdfua/ns/id/\" ua:part=\"2\" ua:rev=\"2024\"/></rdf:RDF>"
                    "</x:xmpmeta>";
    Packet p = Packet::parse(x);
    EXPECT(p.valid());
    EXPECT(p.pdfua_part() == std::optional<std::int64_t>(2));
    EXPECT(p.pdfua_rev() == std::optional<std::string>("2024"));
    const auto* part = p.property(eqx::xmp::kNsPdfuaId, "part");
    EXPECT(part && part->prefix && *part->prefix == "ua");
}

EQX_TEST(document_xmp_rejections) {
    // duplicate property in two rdf:Description elements (XMPNode.assertChildNotExisting)
    std::string dup = packet("", "<pdfuaid:part>1</pdfuaid:part></rdf:Description><rdf:Description rdf:about=\"\" "
                                 "xmlns:pdfuaid=\"http://www.aiim.org/pdfua/ns/id/\"><pdfuaid:part>1</pdfuaid:part>");
    EXPECT(!Packet::parse(dup).valid());
    // unbound prefix, raw '&', DOCTYPE: Xerces rejects the packet in both attempts
    EXPECT(!Packet::parse(packet("", "<pdfx:Foo>1</pdfx:Foo>")).valid());
    EXPECT(!Packet::parse(packet("", "<dc:format>a & b</dc:format>")).valid());
    EXPECT(!Packet::parse("<!DOCTYPE x><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>").valid());
    // mismatched top-level rdf:about
    std::string about = packet("", "</rdf:Description><rdf:Description rdf:about=\"uuid:1\">");
    EXPECT(Packet::parse(packet("", "")).valid());
    EXPECT(!Packet::parse(about.replace(about.find("rdf:about=\"\""), 12, "rdf:about=\"uuid:2\"")).valid());
    // parseType in a non-RDF namespace makes an empty property element with nested content
    EXPECT(!Packet::parse(packet("xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\"",
                                 "<xmpMM:DerivedFrom xmpMM:parseType=\"Resource\"><dc:x>1</dc:x></xmpMM:DerivedFrom>"))
                .valid());
    // CDATA content: resource property element whose child is not an element
    EXPECT(!Packet::parse(packet("", "<dc:format><![CDATA[x]]></dc:format>")).valid());
    // rdf:li outside an array
    EXPECT(!Packet::parse(packet("", "<dc:format><rdf:Description><rdf:li>a</rdf:li></rdf:Description></dc:format>"))
                .valid());
    // trailing garbage after the root element
    EXPECT(!Packet::parse("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>junk").valid());
}

EQX_TEST(document_xmp_second_attempt) {
    // control character: FixASCIIControlsReader turns it into a space
    Packet c = Packet::parse(packet("", std::string("<dc:format>a\x01") + "b</dc:format>" + kTitle));
    EXPECT(c.valid());
    // Latin-1 byte in a UTF-8 packet: Latin1Converter
    Packet l = Packet::parse(packet("", "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Gr\xE4n</rdf:li></rdf:Alt>"
                                        "</dc:title>"));
    EXPECT(l.valid());
    EXPECT(l.dc_title() == std::optional<std::string>("Gr\xC3\xA4n"));
}

EQX_TEST(document_xmp_language_alternatives) {
    Packet p = Packet::parse(packet("", kTitle));
    const auto* t = p.property(eqx::xmp::kNsDc, "title");
    EXPECT(t && eqx::xmp::is_language_alternative(*t->node) && eqx::xmp::lang_alt_x_default(*t->node));
    Packet two = Packet::parse(packet("", "<dc:title><rdf:Alt><rdf:li xml:lang=\"de\">A</rdf:li>"
                                          "<rdf:li xml:lang=\"x-default\">B</rdf:li></rdf:Alt></dc:title>"));
    const auto* t2 = two.property(eqx::xmp::kNsDc, "title");
    EXPECT(t2 && eqx::xmp::is_language_alternative(*t2->node) && !eqx::xmp::lang_alt_x_default(*t2->node));
    EXPECT(two.dc_title() == std::optional<std::string>("B"));
    // simple dc:title: no language alternative (veraPDF dc_title == null)
    Packet simple = Packet::parse(packet("", "<dc:title>Plain</dc:title>"));
    EXPECT(simple.valid() && !simple.dc_title());
    // dc:title under another prefix is not veraPDF's dc_title
    Packet other = Packet::parse(packet("xmlns:d=\"http://purl.org/dc/elements/1.1/\"",
                                        "<d:title><rdf:Alt><rdf:li xml:lang=\"x-default\">T</rdf:li></rdf:Alt></d:title>"));
    EXPECT(other.valid() && !other.dc_title());
    // no rdf:RDF: an empty, valid package
    Packet empty = Packet::parse("<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/><?xpacket end=\"w\"?>");
    EXPECT(empty.valid() && empty.properties().empty());
}

EQX_TEST(document_xmp_helpers) {
    using eqx::xmp::java_parse_int;
    EXPECT(java_parse_int("1") == std::optional<std::int64_t>(1));
    EXPECT(java_parse_int("+1") == std::optional<std::int64_t>(1));
    EXPECT(java_parse_int("01") == std::optional<std::int64_t>(1));
    EXPECT(java_parse_int("\xD9\xA1") == std::optional<std::int64_t>(1));   // ARABIC-INDIC DIGIT ONE
    EXPECT(!java_parse_int(" 1"));
    EXPECT(!java_parse_int(""));
    EXPECT(!java_parse_int("2147483648"));
    EXPECT_EQ(eqx::xmp::normalize_lang_value("EN_us"), std::string("en-US"));
    EXPECT_EQ(eqx::xmp::normalize_lang_value("x-default"), std::string("x-default"));
    EXPECT_EQ(eqx::xmp::normalize_lang_value("X-Default"), std::string("x-DEFAULT"));
}
