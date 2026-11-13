// Unit tests of the pure pac_quality helpers (URL / e-mail detection, white space, path and file-name
// detection, TOC entry matching).
#include "rules/pac_quality/pac_quality_text.h"

#include "unit.h"

using namespace eqx::pacq;

EQX_TEST(pac_quality_whitespace) {
    EXPECT(whitespace_only(U""));
    EXPECT(whitespace_only(U" "));
    EXPECT(whitespace_only(U" \t\r\n\u00A0\u2003\u3000"));
    EXPECT(!whitespace_only(U" a "));
    EXPECT(!whitespace_only(U"\u200B"));   // zero width space is not white space (.NET Char.IsWhiteSpace)
    EXPECT(is_whitespace(U'\u0085'));
    EXPECT(is_whitespace(U'\u2029'));
    EXPECT(!is_whitespace(U'_'));
}

EQX_TEST(pac_quality_find_linkable_urls) {
    EXPECT_EQ(find_linkable(U"Visit https://example.org/ for more.").value_or(""), std::string("https://example.org/"));
    EXPECT_EQ(find_linkable(U"HTTP://EXAMPLE.ORG").value_or(""), std::string("HTTP://EXAMPLE.ORG"));
    EXPECT_EQ(find_linkable(U"see www.example.de.").value_or(""), std::string("www.example.de."));
    EXPECT(find_linkable(U"(www.bund.de/abc)").has_value());
    EXPECT(!find_linkable(U"http://").has_value());
    EXPECT(!find_linkable(U"http:// example").has_value());
    EXPECT(!find_linkable(U"www.").has_value());
    EXPECT(!find_linkable(U"www.example").has_value());      // no top-level domain
    EXPECT(!find_linkable(U"awww.example.org").has_value()); // not at a word boundary
    EXPECT(!find_linkable(U"shttp://x.org").has_value());
    EXPECT(!find_linkable(U"example.org").has_value());      // bare domains are ambiguous
    EXPECT(!find_linkable(U"Plain text without links.").has_value());
}

EQX_TEST(pac_quality_find_linkable_email) {
    EXPECT_EQ(find_linkable(U"Mail: kontakt@equaldox.de.").value_or(""), std::string("kontakt@equaldox.de"));
    EXPECT_EQ(find_linkable(U"a.b+c@sub.example.co.uk").value_or(""), std::string("a.b+c@sub.example.co.uk"));
    EXPECT(!find_linkable(U"user@localhost").has_value());
    EXPECT(!find_linkable(U"@example.org").has_value());
    EXPECT(!find_linkable(U"name@ example.org").has_value());
    EXPECT(!find_linkable(U"price 5 @ 3.50").has_value());
}

EQX_TEST(pac_quality_generated_alt) {
    EXPECT(looks_like_path_or_file_name("image1.png"));
    EXPECT(looks_like_path_or_file_name("  image1.PNG "));
    EXPECT(!looks_like_path_or_file_name("Picture 3.JPG"));        // PAC: logo neu.jpg, report v2.docx pass
    EXPECT(looks_like_path_or_file_name("C:\\My Docs\\a.png"));
    EXPECT(!looks_like_path_or_file_name("Beschreibung: G:\\Folien\\logo.jpg"));
    EXPECT(looks_like_path_or_file_name("C:\\image.jpg"));
    EXPECT(!looks_like_path_or_file_name("C:/Users/x/Desktop/foto"));   // PAC: G:/a/b passes
    EXPECT(!looks_like_path_or_file_name("/home/user/img"));       // PAC: /home/a/b passes
    EXPECT(looks_like_path_or_file_name("example.org"));           // PAC: flagged
    EXPECT(!looks_like_path_or_file_name("a.ai"));                 // PAC: a two-letter ending passes
    EXPECT(looks_like_path_or_file_name("images/chart.svg"));
    EXPECT(looks_like_path_or_file_name("report.docx"));
    EXPECT(!looks_like_path_or_file_name(""));
    EXPECT(!looks_like_path_or_file_name("Grey square"));
    EXPECT(!looks_like_path_or_file_name("Alternative text on a paragraph"));
    EXPECT(!looks_like_path_or_file_name("Logo of example.com"));
    EXPECT(!looks_like_path_or_file_name("Bar chart: sales 2024 vs. 2025."));
    EXPECT(!looks_like_path_or_file_name("Format .png"));
    EXPECT(!looks_like_path_or_file_name("/"));
    EXPECT(!looks_like_path_or_file_name("and/or"));
}

EQX_TEST(pac_quality_toc_matching) {
    EXPECT(normalize_toc_text(U"  1 Introduction ........ 3 ") == std::u32string(U"1 introduction"));
    EXPECT(toc_text_matches(U"1 Introduction ........ 3", U"1 Introduction"));
    EXPECT(toc_text_matches(U"Section heading", U"Section\nheading"));
    EXPECT(toc_text_matches(U"ÜBERSICHT 5", U"Übersicht"));
    EXPECT(!toc_text_matches(U"Entry linking to a page", U"Section heading"));
    EXPECT(!toc_text_matches(U"", U"Heading"));
    EXPECT(!toc_text_matches(U"12", U"Heading"));
}
