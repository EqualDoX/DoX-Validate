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
// XMP reader with the semantics of veraPDF 1.30.2 (verapdf-xmp-core, a port of the Adobe XMP Toolkit):
//
//   XMPMetaParser   packet -> DOM (xml_dom.h: Xerces-strict, Latin-1 / control-character second attempt), root
//                   node search (x:xmpmeta / x:xapmeta / rdf:RDF), no normalisation (veraPDF parses with
//                   setOmitNormalization(true))
//   ParseRDF        the RDF grammar: rdf:Description, attribute and element properties, rdf:Bag/Seq/Alt,
//                   parseType="Resource", rdf:value, qualifiers (xml:lang normalised), duplicate property and
//                   qualifier rejection, mismatched rdf:about rejection; every violation rejects the packet
//   schema registry standard prefixes; a namespace keeps the prefix it was first registered with, a taken
//                   prefix is renamed "p_1_"
//   VeraPDFMeta     the top-level property list (pdfaExtension:schemas excluded) with each property's
//                   namespace, local name and *original* prefix (the prefix written in the packet)
//
// A rejected packet is what veraPDF models as AXLMainXMPPackage(null, false): no properties, no identification.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eqx::xmp {

inline constexpr const char* kNsRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
inline constexpr const char* kNsX = "adobe:ns:meta/";
inline constexpr const char* kNsDc = "http://purl.org/dc/elements/1.1/";
inline constexpr const char* kNsDcDeprecated = "http://purl.org/dc/1.1/";
inline constexpr const char* kNsPdfuaId = "http://www.aiim.org/pdfua/ns/id/";
inline constexpr const char* kNsPdfaExtension = "http://www.aiim.org/pdfa/ns/extension/";
inline constexpr const char* kNsXmpMM = "http://ns.adobe.com/xap/1.0/mm/";
inline constexpr const char* kNsPdfDeclarations = "http://pdfa.org/declarations/";

// org.verapdf.xmp.options.PropertyOptions bits used by the parser
enum : std::uint32_t {
    kOptUri = 0x2,
    kOptHasQualifiers = 0x10,
    kOptQualifier = 0x20,
    kOptHasLanguage = 0x40,
    kOptHasType = 0x80,
    kOptStruct = 0x100,
    kOptArray = 0x200,
    kOptArrayOrdered = 0x400,
    kOptArrayAlternate = 0x800,
    kOptArrayAltText = 0x1000,
    kOptSchemaNode = 0x80000000u,
};

struct Node {
    std::string name;                            // registry prefix + local name, "[]" for array items; schema: URI
    std::string value;
    std::optional<std::string> original_prefix;  // prefix written in the packet (XMPNode.getOriginalPrefix)
    std::uint32_t options = 0;
    bool has_value_child = false;
    std::vector<Node> children;
    std::vector<Node> qualifiers;

    bool is_simple() const { return (options & (kOptStruct | kOptArray)) == 0; }
    const Node* lang_qualifier() const;          // the xml:lang qualifier, nullptr when none
};

class Registry {
public:
    Registry();
    std::optional<std::string> prefix_of(const std::string& ns) const;      // with trailing ':'
    std::optional<std::string> ns_of(std::string prefix) const;             // prefix with or without ':'
    std::string register_namespace(const std::string& ns, std::string suggested);   // throws on bad input

private:
    std::unordered_map<std::string, std::string> ns_to_prefix_, prefix_to_ns_;
};

// VeraPDFXMPNode view of a top-level property.
struct Property {
    std::optional<std::string> prefix;   // original prefix (null when the packet used none)
    std::optional<std::string> ns;       // namespace resolved through the registry prefix
    std::string name;                    // local name
    const Node* node = nullptr;
};

class Packet {
public:
    // Parses a decoded metadata stream (VeraPDFMeta.parse). Never throws.
    static Packet parse(std::string_view bytes);

    bool valid() const { return valid_; }                  // false: veraPDF's XMPException path
    const std::string& error() const { return error_; }
    const std::vector<Property>& properties() const { return properties_; }

    // VeraPDFMeta.containsPropertiesFromNamespace
    bool contains_namespace(std::string_view ns) const;
    // VeraPDFMeta.getProperty(namespace, name): first property with that namespace and local name
    const Property* property(std::string_view ns, std::string_view name) const;
    // XMPMetaImpl.getProperty + getSimpleTextProperty: value of a simple top-level property; nullopt when it is
    // absent, not simple (veraPDF catches the XMPException and yields null) or the namespace is unregistered
    std::optional<std::string> simple_text(const std::string& ns, const std::string& name) const;

    // AXLMainXMPPackage.getdc_title: language alternative of the first dc:title written with the prefix "dc"
    std::optional<std::string> dc_title() const;
    // VeraPDFMeta.getDeclarations: conformsTo values of pdfd:declarations
    std::set<std::string> declarations() const;
    // AXLPDFUAIdentification.getpart: Integer.parseInt of pdfuaid:part, nullopt when absent or not an integer
    std::optional<std::int64_t> pdfua_part() const;
    std::optional<std::string> pdfua_rev() const;

    // XMP property ID used by veraPDF's object de-duplication: "<ns> - <prefix>:<name>"
    static std::string property_id(const Property& p);

private:
    bool valid_ = false;
    std::string error_;
    Node root_;
    Registry registry_;
    std::vector<Property> properties_;
};

// VeraPDFXMPNode.isLanguageAlternative: children present and every child has an xml:lang qualifier
bool is_language_alternative(const Node& n);
// AXLXMPLangAlt.getxDefault: exactly one child whose xml:lang is "x-default"
bool lang_alt_x_default(const Node& n);
// VeraPDFXMPNode.getLanguageAlternative: x-default value, else the first value with a language
std::optional<std::string> language_alternative(const Node& n);
// org.verapdf.xmp.impl.Utils.normalizeLangValue
std::string normalize_lang_value(const std::string& v);
// java.lang.Integer.parseInt (decimal, Unicode digits, optional sign); nullopt on NumberFormatException
std::optional<std::int64_t> java_parse_int(std::string_view utf8);

}  // namespace eqx::xmp
