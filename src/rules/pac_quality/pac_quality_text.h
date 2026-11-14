// Pure text helpers of the pac_quality module (PAC 26.1 Quality-tab semantics). No PDF access here, so
// every helper is unit-tested in tests/unit/pac_quality_text_test.cpp.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace eqx::pacq {

// .NET Char.IsWhiteSpace, which PAC documents for TextTagContainsOnlyWhitespace: Unicode
// categories Zs/Zl/Zp plus U+0009-U+000D, U+0085 (identical to the Unicode White_Space property).
bool is_whitespace(char32_t c);
// True when every code point is white space; an empty string counts as whitespace-only.
bool whitespace_only(std::u32string_view s);

// First "linkable string" PAC's MissingLinkTags check could mean, restricted to unambiguous
// forms (PAC: "URLs, email addresses"): an http:// or https:// URL with a host character after the
// scheme, a www.<label>.<tld> host, or an e-mail address local@domain.tld. Returns the match (UTF-8) or
// nullopt.
std::optional<std::string> find_linkable(std::u32string_view s);

// PAC AltTextsAreNotGenerated: alternative text that is a local path or a file name (PAC's
// example "C:\image.jpg"). Recognised (after trimming white space): drive paths (C:\..., C:/...), UNC
// paths (\\server\...), file: URLs, absolute or relative POSIX paths with a directory separator, and
// file names ending in a known image / document extension.
bool looks_like_path_or_file_name(std::string_view utf8);

// Normalisation used to match a TOC entry's text with a heading's text: lower case (ASCII and Latin-1),
// every run of white space collapsed to one space, leading/trailing white space, leader dots, page
// numbers and punctuation removed.
std::u32string normalize_toc_text(std::u32string_view s);
// True when the normalised texts are both non-empty and one contains the other.
bool toc_text_matches(std::u32string_view toc_entry, std::u32string_view heading);
// The forms of a text that toc_text_matches compares, for a text matched against many others (every heading of a
// document against every TOC entry): made once, then toc_text_matches(TocText, TocText) gives the same answer as the
// function above on the texts they were made from.
struct TocText {
    std::u32string normalized;     // normalize_toc_text
    std::u32string unhyphenated;   // ... without hyphens and soft hyphens (and the space after one)
};
TocText toc_text(std::u32string_view s);
bool toc_text_matches(const TocText& toc_entry, const TocText& heading);

}  // namespace eqx::pacq
