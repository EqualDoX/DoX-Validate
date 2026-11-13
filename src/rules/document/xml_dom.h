// Strict, namespace-aware XML reader for the XMP and XFA packets the document module inspects.
//
// veraPDF parses these packets with a Xerces DocumentBuilder (SecureXML.newSafeDocumentBuilder: namespace
// aware, comments dropped, DOCTYPE refused) and gives up on the whole packet when Xerces rejects it. Whether a
// packet is accepted therefore decides rules such as UA-1 5-1 / 7.1-9, so this reader reproduces Xerces'
// well-formedness and namespace checks (illegal characters, undefined entities, unbound prefixes, duplicate
// attributes, multiple roots, '<' in attribute values, "]]>" in text, "--" in comments, ...) instead of using a
// lenient parser (pugixml accepts all of these, which would turn veraPDF failures into passes).
//
// The DOM mirrors what veraPDF's code sees: elements, text, CDATA sections and processing instructions (no
// comments; adjacent text merged the way Xerces' DOM builder appends character data), attributes in the order
// of Xerces' NamedNodeMap (sorted by qualified name), line ends normalised, attribute whitespace normalised.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eqx::xml {

inline constexpr const char* kNsXml = "http://www.w3.org/XML/1998/namespace";
inline constexpr const char* kNsXmlns = "http://www.w3.org/2000/xmlns/";

struct Attr {
    std::string qname, prefix, local;  // prefix empty when the name has none
    std::optional<std::string> ns;     // namespace URI (DOM getNamespaceURI); nullopt = no namespace
    std::string value;
    bool is_xmlns() const { return prefix == "xmlns" || (prefix.empty() && qname == "xmlns"); }
};

struct Node {
    enum class Kind : unsigned char { Element, Text, CData, PI };
    Kind kind = Kind::Element;
    std::string qname, prefix, local;  // Element (UTF-8)
    std::optional<std::string> ns;     // Element namespace URI
    std::string value;                 // Text / CData content, PI data
    std::string target;                // PI target
    std::vector<Attr> attrs;           // Element attributes, sorted by qualified name
    std::vector<Node> children;

    bool is_element() const { return kind == Kind::Element; }
    bool is_text() const { return kind == Kind::Text; }
};

struct Document {
    std::vector<Node> children;   // prolog/epilog PIs and the root element
    std::string encoding;         // encoding used to decode the packet
};

enum class Mode {
    Single,   // one attempt on the byte stream (GFPDAcroForm XFA config)
    Xmp,      // XMPMetaParser: on failure, Latin1Converter + FixASCIIControlsReader and a second attempt
};

// Returns false (and an error description) when the packet is rejected.
bool parse(std::string_view bytes, Mode mode, Document& doc, std::string* error = nullptr);

// java.lang.Character.isWhitespace (ParseRDF.isWhitespaceNode).
bool java_whitespace(char32_t c);

}  // namespace eqx::xml
