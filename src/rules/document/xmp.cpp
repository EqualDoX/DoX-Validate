// SPDX-License-Identifier: BSD-3-Clause
//
// This file contains a C++ translation of parts of the Adobe XMP Toolkit for Java (XMPCore: ParseRDF, the schema
// registry's standard namespaces, Utils) as distributed in veraPDF 1.30.2 (verapdf-xmp-core), and of veraPDF's
// additions to it (VeraPDFMeta, VeraPDFXMPNode).
// Copyright 2006 Adobe Systems Incorporated. All Rights Reserved. veraPDF additions: Copyright (c) 2015-2026,
// veraPDF Consortium. Translation and modifications: Copyright (c) 2026 EqualDoX (https://equaldox.de).
// Licensed under the BSD 3-Clause License (LICENSES/BSD-3-Clause.txt). The EqualDoX Attribution License does not
// apply to this file (THIRD_PARTY_NOTICES.md).
//
#include "rules/document/xmp.h"

#include <utility>

#include "rules/document/xml_dom.h"
#include "util/text.h"

namespace eqx::xmp {

namespace {

struct XmpError {
    std::string msg;
};
[[noreturn]] void bad(const char* msg) { throw XmpError{msg}; }

constexpr const char* kXmlLang = "xml:lang";
constexpr const char* kDefaultPrefix = "_dflt";   // ParseRDF.DEFAULT_PREFIX

// XMPSchemaRegistryImpl.registerStandardNamespaces
const std::pair<const char*, const char*> kStandard[] = {
    {"http://www.w3.org/XML/1998/namespace", "xml"},
    {kNsRdf, "rdf"},
    {kNsDc, "dc"},
    {"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/", "Iptc4xmpCore"},
    {"http://iptc.org/std/Iptc4xmpExt/2008-02-29/", "Iptc4xmpExt"},
    {"http://ns.adobe.com/DICOM/", "DICOM"},
    {"http://ns.useplus.org/ldf/xmp/1.0/", "plus"},
    {kNsX, "x"},
    {"http://ns.adobe.com/iX/1.0/", "iX"},
    {"http://ns.adobe.com/xap/1.0/", "xmp"},
    {"http://ns.adobe.com/xap/1.0/rights/", "xmpRights"},
    {kNsXmpMM, "xmpMM"},
    {"http://ns.adobe.com/xap/1.0/bj/", "xmpBJ"},
    {"http://ns.adobe.com/xmp/note/", "xmpNote"},
    {"http://ns.adobe.com/pdf/1.3/", "pdf"},
    {"http://ns.adobe.com/pdfx/1.3/", "pdfx"},
    {"http://www.npes.org/pdfx/ns/id/", "pdfxid"},
    {"http://www.aiim.org/pdfa/ns/schema#", "pdfaSchema"},
    {"http://www.aiim.org/pdfa/ns/property#", "pdfaProperty"},
    {"http://www.aiim.org/pdfa/ns/type#", "pdfaType"},
    {"http://www.aiim.org/pdfa/ns/field#", "pdfaField"},
    {"http://www.aiim.org/pdfa/ns/id/", "pdfaid"},
    {kNsPdfaExtension, "pdfaExtension"},
    {"http://ns.adobe.com/photoshop/1.0/", "photoshop"},
    {"http://ns.adobe.com/album/1.0/", "album"},
    {"http://ns.adobe.com/exif/1.0/", "exif"},
    {"http://cipa.jp/exif/1.0/", "exifEX"},
    {"http://ns.adobe.com/exif/1.0/aux/", "aux"},
    {"http://ns.adobe.com/tiff/1.0/", "tiff"},
    {"http://ns.adobe.com/png/1.0/", "png"},
    {"http://ns.adobe.com/jpeg/1.0/", "jpeg"},
    {"http://ns.adobe.com/jp2k/1.0/", "jp2k"},
    {"http://ns.adobe.com/camera-raw-settings/1.0/", "crs"},
    {"http://ns.adobe.com/StockPhoto/1.0/", "bmsp"},
    {"http://ns.adobe.com/creatorAtom/1.0/", "creatorAtom"},
    {"http://ns.adobe.com/asf/1.0/", "asf"},
    {"http://ns.adobe.com/xmp/wav/1.0/", "wav"},
    {"http://ns.adobe.com/bwf/bext/1.0/", "bext"},
    {"http://ns.adobe.com/riff/info/", "riffinfo"},
    {"http://ns.adobe.com/xmp/1.0/Script/", "xmpScript"},
    {"http://ns.adobe.com/TransformXMP/", "txmp"},
    {"http://ns.adobe.com/swf/1.0/", "swf"},
    {"http://ns.adobe.com/xmp/1.0/DynamicMedia/", "xmpDM"},
    {"http://ns.adobe.com/xmp/transient/1.0/", "xmpx"},
    {"http://ns.adobe.com/xap/1.0/t/", "xmpT"},
    {"http://ns.adobe.com/xap/1.0/t/pg/", "xmpTPg"},
    {"http://ns.adobe.com/xap/1.0/g/", "xmpG"},
    {"http://ns.adobe.com/xap/1.0/g/img/", "xmpGImg"},
    {"http://ns.adobe.com/xap/1.0/sType/Font#", "stFnt"},
    {"http://ns.adobe.com/xap/1.0/sType/Dimensions#", "stDim"},
    {"http://ns.adobe.com/xap/1.0/sType/ResourceEvent#", "stEvt"},
    {"http://ns.adobe.com/xap/1.0/sType/ResourceRef#", "stRef"},
    {"http://ns.adobe.com/xap/1.0/sType/Version#", "stVer"},
    {"http://ns.adobe.com/xap/1.0/sType/Job#", "stJob"},
    {"http://ns.adobe.com/xap/1.0/sType/ManifestItem#", "stMfs"},
    {"http://ns.adobe.com/xmp/Identifier/qual/1.0/", "xmpidq"},
};

// ParseRDF term kinds
enum Term {
    T_OTHER = 0, T_RDF, T_ID, T_ABOUT, T_PARSE_TYPE, T_RESOURCE, T_NODE_ID, T_DATATYPE, T_DESCRIPTION, T_LI,
    T_ABOUT_EACH, T_ABOUT_EACH_PREFIX, T_BAG_ID
};

// The parts of a DOM element or attribute the RDF parser reads (getNamespaceURI, getPrefix, getLocalName).
struct XName {
    const std::optional<std::string>* ns;
    std::optional<std::string> prefix;   // null when the name has no prefix
    const std::string* local;
    const std::string* qname;
};
XName xname(const xml::Node& n) {
    return {&n.ns, n.prefix.empty() ? std::nullopt : std::optional<std::string>(n.prefix), &n.local, &n.qname};
}
XName xname(const xml::Attr& a) {
    return {&a.ns, a.prefix.empty() ? std::nullopt : std::optional<std::string>(a.prefix), &a.local, &a.qname};
}

bool is_rdf(const std::optional<std::string>& ns) { return ns && *ns == kNsRdf; }

Term term_kind(const std::string& local, std::optional<std::string> ns, bool is_attr,
               const std::optional<std::string>& owner_ns) {
    if (!ns && (local == "about" || local == "ID") && is_attr && is_rdf(owner_ns)) ns = kNsRdf;
    if (!is_rdf(ns)) return T_OTHER;
    if (local == "li") return T_LI;
    if (local == "parseType") return T_PARSE_TYPE;
    if (local == "Description") return T_DESCRIPTION;
    if (local == "about") return T_ABOUT;
    if (local == "resource") return T_RESOURCE;
    if (local == "RDF") return T_RDF;
    if (local == "ID") return T_ID;
    if (local == "nodeID") return T_NODE_ID;
    if (local == "datatype") return T_DATATYPE;
    if (local == "aboutEach") return T_ABOUT_EACH;
    if (local == "aboutEachPrefix") return T_ABOUT_EACH_PREFIX;
    if (local == "bagID") return T_BAG_ID;
    return T_OTHER;
}
Term term_of(const xml::Node& n) {
    if (!n.is_element()) return T_OTHER;
    return term_kind(n.local, n.ns, false, std::nullopt);
}
Term term_of(const xml::Attr& a, const xml::Node& owner) { return term_kind(a.local, a.ns, true, owner.ns); }

bool is_property_element_name(Term t) {
    if (t == T_DESCRIPTION || (t >= T_ABOUT_EACH && t <= T_BAG_ID)) return false;
    return !(t >= T_RDF && t <= T_DATATYPE);
}

bool whitespace_node(const xml::Node& n) {
    if (!n.is_text()) return false;
    for (char32_t c : utf8_to_u32(n.value))
        if (!xml::java_whitespace(c)) return false;
    return true;
}

bool is_xml_lang(const xml::Attr& a) { return a.qname == kXmlLang; }

bool name_start_char(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}
bool name_char(char32_t c) {
    return name_start_char(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7;
}

// ------------------------------------------------------------------------------- ParseRDF port
class RdfParser {
public:
    RdfParser(Node& root, Registry& reg) : root_(root), reg_(reg) {}

    void rdf_RDF(const xml::Node& rdf) { node_element_list(root_, rdf); }

private:
    Node& root_;
    Registry& reg_;
    std::optional<std::string> about_;   // name of the XMP root (top-level rdf:about)

    void node_element_list(Node& parent, const xml::Node& rdf) {
        for (const auto& c : rdf.children)
            if (!whitespace_node(c)) node_element(parent, c, true);
    }

    void node_element(Node& parent, const xml::Node& n, bool top) {
        Term t = term_of(n);
        if (t != T_DESCRIPTION && t != T_OTHER) bad("Node element must be rdf:Description or typed node");
        if (top && t == T_OTHER) bad("Top level typed node not allowed");
        node_element_attrs(parent, n, top);
        property_element_list(parent, n, top);
    }

    void node_element_attrs(Node& parent, const xml::Node& n, bool top) {
        int exclusive = 0;
        for (const auto& a : n.attrs) {
            if (a.is_xmlns()) continue;
            Term t = term_of(a, n);
            switch (t) {
                case T_ID:
                case T_NODE_ID:
                case T_ABOUT:
                    if (exclusive > 0) bad("Mutally exclusive about, ID, nodeID attributes");
                    ++exclusive;
                    if (top && t == T_ABOUT) {
                        if (about_ && !about_->empty()) {
                            if (*about_ != a.value && !a.value.empty()) bad("Mismatched top level rdf:about values");
                        } else {
                            about_ = a.value;
                        }
                    }
                    break;
                case T_OTHER:
                    add_child_node(parent, xname(a), a.value, top);
                    break;
                default:
                    bad("Invalid nodeElement attribute");
            }
        }
    }

    void property_element_list(Node& parent, const xml::Node& xml_parent, bool top) {
        for (const auto& c : xml_parent.children) {
            if (whitespace_node(c)) continue;
            if (!c.is_element()) bad("Expected property element node not found");
            property_element(parent, c, top);
        }
    }

    void property_element(Node& parent, const xml::Node& n, bool top) {
        if (!is_property_element_name(term_of(n))) bad("Invalid property element name");
        std::vector<const xml::Attr*> attrs;
        for (const auto& a : n.attrs)
            if (!a.is_xmlns()) attrs.push_back(&a);
        if (attrs.size() > 3) {
            empty_property_element(parent, n, top);
            return;
        }
        for (const xml::Attr* a : attrs) {
            if (is_xml_lang(*a)) continue;
            if (a->local == "datatype" && is_rdf(a->ns)) {
                literal_property_element(parent, n, top);
            } else if (!(a->local == "parseType" && is_rdf(a->ns))) {
                empty_property_element(parent, n, top);
            } else if (a->value == "Literal") {
                bad("ParseTypeLiteral property element not allowed");
            } else if (a->value == "Resource") {
                parse_type_resource_property_element(parent, n, top);
            } else if (a->value == "Collection") {
                bad("ParseTypeCollection property element not allowed");
            } else {
                bad("ParseTypeOther property element not allowed");
            }
            return;
        }
        if (!n.children.empty()) {
            for (const auto& c : n.children)
                if (!c.is_text()) {
                    resource_property_element(parent, n, top);
                    return;
                }
            literal_property_element(parent, n, top);
        } else {
            empty_property_element(parent, n, top);
        }
    }

    void resource_property_element(Node& parent, const xml::Node& n, bool top) {
        if (top && n.qname == "iX:changes") return;
        Node& compound = add_child_node(parent, xname(n), "", top);
        for (const auto& a : n.attrs) {
            if (a.is_xmlns()) continue;
            if (is_xml_lang(a)) add_qualifier_node(compound, kXmlLang, a.value);
            else if (a.local == "ID" && is_rdf(a.ns)) continue;
            else bad("Invalid attribute for resource property element");
        }
        bool found = false;
        for (const auto& c : n.children) {
            if (whitespace_node(c)) continue;
            if (c.is_element() && !found) {
                bool rdf = is_rdf(c.ns);
                if (rdf && c.local == "Bag") {
                    compound.options |= kOptArray;
                } else if (rdf && c.local == "Seq") {
                    compound.options |= kOptArray | kOptArrayOrdered;
                } else if (rdf && c.local == "Alt") {
                    compound.options |= kOptArray | kOptArrayOrdered | kOptArrayAlternate;
                } else {
                    compound.options |= kOptStruct;
                    if (!rdf && c.local != "Description") {
                        if (!c.ns) bad("All XML elements must be in a namespace");
                        add_qualifier_node(compound, "rdf:type", *c.ns + ":" + c.local);
                    }
                }
                node_element(compound, c, false);
                if (compound.has_value_child) fixup_qualified_node(compound);
                else if (compound.options & kOptArrayAlternate) detect_alt_text(compound);
                found = true;
            } else if (found) {
                bad("Invalid child of resource property element");
            } else {
                bad("Children of resource property element must be XML elements");
            }
        }
        if (!found) bad("Missing child of resource property element");
    }

    void literal_property_element(Node& parent, const xml::Node& n, bool top) {
        Node& child = add_child_node(parent, xname(n), "", top);
        for (const auto& a : n.attrs) {
            if (a.is_xmlns()) continue;
            if (is_xml_lang(a)) add_qualifier_node(child, kXmlLang, a.value);
            else if (is_rdf(a.ns) && (a.local == "ID" || a.local == "datatype")) continue;
            else bad("Invalid attribute for literal property element");
        }
        std::string text;
        for (const auto& c : n.children) {
            if (!c.is_text()) bad("Invalid child of literal property element");
            text += c.value;
        }
        child.value = std::move(text);
    }

    void parse_type_resource_property_element(Node& parent, const xml::Node& n, bool top) {
        Node& st = add_child_node(parent, xname(n), "", top);
        st.options |= kOptStruct;
        for (const auto& a : n.attrs) {
            if (a.is_xmlns()) continue;
            if (is_xml_lang(a)) add_qualifier_node(st, kXmlLang, a.value);
            else if (is_rdf(a.ns) && (a.local == "ID" || a.local == "parseType")) continue;
            else bad("Invalid attribute for ParseTypeResource property element");
        }
        property_element_list(st, n, false);
        if (st.has_value_child) fixup_qualified_node(st);
    }

    void empty_property_element(Node& parent, const xml::Node& n, bool top) {
        bool has_property_attrs = false, has_resource_attr = false, has_node_id_attr = false,
             has_value_attr = false;
        const xml::Attr* value_node = nullptr;
        if (!n.children.empty()) bad("Nested content not allowed with rdf:resource or property attributes");
        for (const auto& a : n.attrs) {
            if (a.is_xmlns()) continue;
            switch (term_of(a, n)) {
                case T_ID:
                    break;
                case T_RESOURCE:
                    if (has_node_id_attr) bad("Empty property element can't have both rdf:resource and rdf:nodeID");
                    if (has_value_attr) bad("Empty property element can't have both rdf:value and rdf:resource");
                    has_resource_attr = true;
                    if (!has_value_attr) value_node = &a;
                    break;
                case T_NODE_ID:
                    if (has_resource_attr) bad("Empty property element can't have both rdf:resource and rdf:nodeID");
                    has_node_id_attr = true;
                    break;
                case T_OTHER:
                    if (a.local == "value" && is_rdf(a.ns)) {
                        if (has_resource_attr) bad("Empty property element can't have both rdf:value and rdf:resource");
                        has_value_attr = true;
                        value_node = &a;
                    } else if (!is_xml_lang(a)) {
                        has_property_attrs = true;
                    }
                    break;
                default:
                    bad("Unrecognized attribute of empty property element");
            }
        }
        Node& child = add_child_node(parent, xname(n), "", top);
        bool child_is_struct = false;
        if (has_value_attr || has_resource_attr) {
            child.value = value_node ? value_node->value : std::string();
            if (!has_value_attr) child.options |= kOptUri;
        } else if (has_property_attrs) {
            child.options |= kOptStruct;
            child_is_struct = true;
        }
        for (const auto& a : n.attrs) {
            if (&a == value_node || a.is_xmlns()) continue;
            switch (term_of(a, n)) {
                case T_ID:
                case T_NODE_ID:
                    break;
                case T_RESOURCE:
                    add_qualifier_node(child, "rdf:resource", a.value);
                    break;
                case T_OTHER:
                    if (!child_is_struct) add_qualifier_node(child, a.qname, a.value);
                    else if (is_xml_lang(a)) add_qualifier_node(child, kXmlLang, a.value);
                    else add_child_node(child, xname(a), a.value, false);
                    break;
                default:
                    bad("Unrecognized attribute of empty property element");
            }
        }
    }

    Node& find_schema_node(const std::string& ns) {
        for (auto& s : root_.children)
            if (s.name == ns) return s;
        Node s;
        s.name = ns;
        s.options = kOptSchemaNode;
        auto p = reg_.prefix_of(ns);
        s.value = p ? *p : reg_.register_namespace(ns, kDefaultPrefix);
        root_.children.push_back(std::move(s));
        return root_.children.back();
    }

    static void assert_child_not_existing(const Node& parent, const std::string& name) {
        if (name == "[]") return;
        for (const auto& c : parent.children)
            if (c.name == name) bad("Duplicate property or field node");
    }

    Node& add_child_node(Node& parent, const XName& nm, const std::string& value, bool top) {
        if (!*nm.ns) bad("XML namespace required for all elements and attributes");
        std::string ns = **nm.ns;
        if (ns == kNsDcDeprecated) ns = kNsDc;
        auto prefix = reg_.prefix_of(ns);
        if (!prefix) prefix = reg_.register_namespace(ns, nm.prefix ? *nm.prefix : kDefaultPrefix);
        std::string child_name = *prefix + *nm.local;
        Node* target = &parent;
        if (top) target = &find_schema_node(ns);
        bool array_item = child_name == "rdf:li";
        bool value_node = child_name == "rdf:value";
        Node c;
        c.name = child_name;
        c.value = value;
        c.original_prefix = nm.prefix;
        assert_child_not_existing(*target, child_name);
        Node* added;
        if (!value_node) {
            target->children.push_back(std::move(c));
            added = &target->children.back();
        } else {
            target->children.insert(target->children.begin(), std::move(c));
            added = &target->children.front();
        }
        if (value_node) {
            if (top || !(target->options & kOptStruct)) bad("Misplaced rdf:value element");
            target->has_value_child = true;
        }
        if (array_item) {
            if (!(target->options & kOptArray)) bad("Misplaced rdf:li element");
            added->name = "[]";
        }
        return *added;
    }

    static void add_qualifier(Node& parent, Node q) {
        if (q.name != "[]")
            for (const auto& e : parent.qualifiers)
                if (e.name == q.name) bad("Duplicate qualifier");
        q.options |= kOptQualifier;
        parent.options |= kOptHasQualifiers;
        if (q.name == kXmlLang) {
            parent.options |= kOptHasLanguage;
            parent.qualifiers.insert(parent.qualifiers.begin(), std::move(q));
        } else if (q.name == "rdf:type") {
            parent.options |= kOptHasType;
            bool lang = (parent.options & kOptHasLanguage) != 0;
            parent.qualifiers.insert(parent.qualifiers.begin() + (lang ? 1 : 0), std::move(q));
        } else {
            parent.qualifiers.push_back(std::move(q));
        }
    }

    static void add_qualifier_node(Node& parent, const std::string& name, const std::string& value) {
        Node q;
        q.name = name;
        q.value = name == kXmlLang ? normalize_lang_value(value) : value;
        add_qualifier(parent, std::move(q));
    }

    static void fixup_qualified_node(Node& p) {
        Node value_node = std::move(p.children.front());
        if (value_node.options & kOptHasLanguage) {
            if (p.options & kOptHasLanguage) bad("Redundant xml:lang for rdf:value element");
            Node lang = std::move(value_node.qualifiers.front());
            value_node.qualifiers.erase(value_node.qualifiers.begin());
            value_node.options &= ~kOptHasLanguage;
            add_qualifier(p, std::move(lang));
        }
        for (auto& q : value_node.qualifiers) add_qualifier(p, q);
        for (std::size_t i = 1; i < p.children.size(); ++i) add_qualifier(p, p.children[i]);
        p.has_value_child = false;
        p.options &= ~kOptStruct;
        p.options |= value_node.options;
        p.value = value_node.value;
        p.children = std::move(value_node.children);
    }

    static void detect_alt_text(Node& arr) {
        if (!(arr.options & kOptArrayAlternate) || arr.children.empty()) return;
        bool alt_text = false;
        for (const auto& c : arr.children)
            if (c.options & kOptHasLanguage) {
                alt_text = true;
                break;
            }
        if (!alt_text) return;
        arr.options |= kOptArrayAltText;
        // XMPNodeUtils.normalizeLangArray: the first x-default item (from the second on) moves to the front;
        // when it was the second item, the (former) first item takes its value (toolkit behaviour kept as is)
        for (std::size_t i = 1; i < arr.children.size(); ++i) {
            const Node& c = arr.children[i];
            if (!c.qualifiers.empty() && c.qualifiers.front().value == "x-default") {
                Node moved = std::move(arr.children[i]);
                arr.children.erase(arr.children.begin() + static_cast<std::ptrdiff_t>(i));
                arr.children.insert(arr.children.begin(), std::move(moved));
                if (i == 1) arr.children[1].value = arr.children[0].value;
                break;
            }
        }
    }
};

// XMPMetaParser.findRootNode
const xml::Node* find_root(const std::vector<xml::Node>& nodes, bool xmpmeta_required, int depth) {
    if (depth > 600) return nullptr;
    for (const auto& n : nodes) {
        if (n.kind == xml::Node::Kind::Text || n.kind == xml::Node::Kind::PI) continue;
        if (n.is_element() && (n.local == "xmpmeta" || n.local == "xapmeta") && n.ns && *n.ns == kNsX)
            return find_root(n.children, false, depth + 1);
        if (n.is_element() && !xmpmeta_required && n.local == "RDF" && is_rdf(n.ns)) return &n;
        if (const xml::Node* r = find_root(n.children, xmpmeta_required, depth + 1)) return r;
    }
    return nullptr;
}


}  // namespace

const Node* Node::lang_qualifier() const {
    for (const auto& q : qualifiers)
        if (q.name == kXmlLang) return &q;
    return nullptr;
}

// ------------------------------------------------------------------------------------------- registry
Registry::Registry() {
    for (const auto& [ns, p] : kStandard) {
        ns_to_prefix_[ns] = std::string(p) + ":";
        prefix_to_ns_[std::string(p) + ":"] = ns;
    }
}

std::optional<std::string> Registry::prefix_of(const std::string& ns) const {
    auto it = ns_to_prefix_.find(ns);
    if (it == ns_to_prefix_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> Registry::ns_of(std::string prefix) const {
    if (prefix.empty() || prefix.back() != ':') prefix += ':';
    auto it = prefix_to_ns_.find(prefix);
    if (it == prefix_to_ns_.end()) return std::nullopt;
    return it->second;
}

std::string Registry::register_namespace(const std::string& ns, std::string suggested) {
    if (ns.empty()) bad("Empty schema namespace URI");
    if (suggested.empty()) bad("Empty prefix");
    if (suggested.back() != ':') suggested += ':';
    {
        std::u32string p = utf8_to_u32(std::string_view(suggested).substr(0, suggested.size() - 1));
        bool ok = !p.empty() && name_start_char(p[0]) && p[0] != ':';
        for (std::size_t i = 1; ok && i < p.size(); ++i) ok = name_char(p[i]) && p[i] != ':';
        if (!ok) bad("The prefix is a bad XML name");
    }
    if (auto p = prefix_of(ns)) return *p;
    if (prefix_to_ns_.count(suggested)) {
        std::string base = suggested.substr(0, suggested.size() - 1);
        std::string gen = suggested;
        for (int i = 1; prefix_to_ns_.count(gen); ++i) gen = base + "_" + std::to_string(i) + "_:";
        suggested = gen;
    }
    ns_to_prefix_[ns] = suggested;
    prefix_to_ns_[suggested] = ns;
    return suggested;
}

// --------------------------------------------------------------------------------------------- packet
Packet Packet::parse(std::string_view bytes) {
    Packet pk;
    xml::Document doc;
    std::string err;
    if (!xml::parse(bytes, xml::Mode::Xmp, doc, &err)) {
        pk.error_ = "XML parsing failure: " + err;
        return pk;
    }
    const xml::Node* rdf = find_root(doc.children, false, 0);
    if (!rdf) {   // XMPMetaParser returns an empty XMPMetaImpl
        pk.valid_ = true;
        return pk;
    }
    try {
        RdfParser p(pk.root_, pk.registry_);
        p.rdf_RDF(*rdf);
    } catch (const XmpError& e) {
        pk.error_ = e.msg;
        pk.root_ = Node();
        return pk;
    }
    pk.valid_ = true;
    // VeraPDFMeta.update / addSchema
    for (const auto& schema : pk.root_.children) {
        for (const auto& child : schema.children) {
            std::size_t colon = child.name.find(':');
            std::string name = colon == std::string::npos ? child.name : child.name.substr(colon + 1);
            std::string prefix = colon == std::string::npos ? std::string() : child.name.substr(0, colon);
            auto ns = pk.registry_.ns_of(prefix);
            if (ns && *ns == kNsPdfaExtension && name == "schemas") continue;
            pk.properties_.push_back({child.original_prefix, ns, name, &child});
        }
    }
    return pk;
}

bool Packet::contains_namespace(std::string_view ns) const {
    for (const auto& p : properties_)
        if (p.ns && *p.ns == ns) return true;
    return false;
}

const Property* Packet::property(std::string_view ns, std::string_view name) const {
    for (const auto& p : properties_)
        if (p.ns && *p.ns == ns && p.name == name) return &p;
    return nullptr;
}

std::optional<std::string> Packet::simple_text(const std::string& ns, const std::string& name) const {
    auto prefix = registry_.prefix_of(ns);
    if (!prefix) return std::nullopt;   // XMPException "Unregistered schema namespace URI" -> null
    for (const auto& schema : root_.children) {
        if (schema.name != ns) continue;
        for (const auto& c : schema.children)
            if (c.name == *prefix + name) {
                if (!c.is_simple()) return std::nullopt;   // "Required property is not simple" -> null
                return c.value;
            }
    }
    return std::nullopt;
}

std::optional<std::string> Packet::dc_title() const {
    for (const auto& p : properties_)
        if (p.ns && *p.ns == kNsDc && p.prefix && *p.prefix == "dc" && p.name == "title")
            return language_alternative(*p.node);
    return std::nullopt;
}

std::set<std::string> Packet::declarations() const {
    std::set<std::string> out;
    const Property* p = property(kNsPdfDeclarations, "declarations");
    if (!p) return out;
    for (const auto& decl : p->node->children)
        for (const auto& n : decl.children) {
            std::size_t colon = n.name.find(':');
            std::string local = colon == std::string::npos ? n.name : n.name.substr(colon + 1);
            if (local == "conformsTo") out.insert(n.value);
        }
    return out;
}

std::optional<std::int64_t> Packet::pdfua_part() const {
    auto v = simple_text(kNsPdfuaId, "part");
    if (!v) return std::nullopt;
    return java_parse_int(*v);
}

std::optional<std::string> Packet::pdfua_rev() const { return simple_text(kNsPdfuaId, "rev"); }

std::string Packet::property_id(const Property& p) {
    return (p.ns ? *p.ns : std::string("null")) + " - " + (p.prefix ? *p.prefix : std::string("null")) + ":" +
           p.name;
}

// ------------------------------------------------------------------------------------------- helpers
bool is_language_alternative(const Node& n) {
    if (n.children.empty()) return false;
    for (const auto& c : n.children)
        if (!c.lang_qualifier()) return false;
    return true;
}

bool lang_alt_x_default(const Node& n) {
    if (n.children.size() != 1) return false;
    for (const auto& q : n.children.front().qualifiers)
        if (q.name == kXmlLang && q.value == "x-default") return true;
    return false;
}

std::optional<std::string> language_alternative(const Node& n) {
    if (n.children.empty()) return std::nullopt;
    std::optional<std::string> first;
    for (const auto& c : n.children)
        for (const auto& q : c.qualifiers)
            if (q.name == kXmlLang) {
                if (q.value == "x-default") return c.value;
                if (!first) first = c.value;
            }
    return first;
}

std::string normalize_lang_value(const std::string& v) {
    if (v == "x-default") return v;
    int sub = 1;
    std::string out;
    for (char c : v) {
        if (c == '-' || c == '_') {
            out.push_back('-');
            ++sub;
        } else if (c == ' ') {
            continue;
        } else if (sub != 2) {
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
        } else {
            out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
        }
    }
    return out;
}

std::optional<std::int64_t> java_parse_int(std::string_view utf8) {
    // java.lang.Integer.parseInt(s, 10): optional sign, then Character.digit(c, 10) >= 0 for every character
    static const char32_t zeros[] = {
        0x30, 0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66, 0xCE6, 0xD66, 0xDE6, 0xE50,
        0xED0, 0xF20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40,
        0x1C50, 0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10, 0x104A0, 0x11066, 0x110F0,
        0x11136, 0x111D0, 0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0, 0x11C50, 0x11D50,
        0x11DA0, 0x16A60, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E950};
    std::u32string s = utf8_to_u32(utf8);
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
        if (s.size() == 1) return std::nullopt;
    }
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        int d = -1;
        for (char32_t z : zeros)
            if (s[i] >= z && s[i] <= z + 9) {
                d = static_cast<int>(s[i] - z);
                break;
            }
        if (d < 0) return std::nullopt;
        v = v * 10 + d;
        if (v > 2147483648LL) return std::nullopt;
    }
    if (neg) v = -v;
    if (v > 2147483647LL || v < -2147483648LL) return std::nullopt;
    return v;
}

}  // namespace eqx::xmp
