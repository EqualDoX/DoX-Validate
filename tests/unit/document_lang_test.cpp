// Unit tests of the document module's language-tag checks.
#include <string>

#include "rules/document/lang.h"
#include "unit.h"

using eqx::doclang::PacLang;

namespace {
std::u32string u(const char* s) {
    std::u32string o;
    for (; *s; ++s) o.push_back(static_cast<unsigned char>(*s));
    return o;
}
PacLang pac(const char* s) { return eqx::doclang::pac_check(u(s)); }
}  // namespace

EQX_TEST(document_lang_verapdf_regex) {
    using eqx::doclang::verapdf_lang_ok;
    EXPECT(verapdf_lang_ok(u("en")));
    EXPECT(verapdf_lang_ok(u("en-US")));
    EXPECT(verapdf_lang_ok(u("x-eqx")));
    EXPECT(verapdf_lang_ok(u("i-klingon")));
    EXPECT(verapdf_lang_ok(u("zz")));
    EXPECT(verapdf_lang_ok(u("de-DE-1996")));
    EXPECT(!verapdf_lang_ok(u("")));
    EXPECT(!verapdf_lang_ok(u("en_US")));
    EXPECT(!verapdf_lang_ok(u("en-")));
    EXPECT(!verapdf_lang_ok(u("-en")));
    EXPECT(!verapdf_lang_ok(u("e1")));
    EXPECT(!verapdf_lang_ok(u("abcdefghi")));
    EXPECT(!verapdf_lang_ok(u("en-abcdefghi")));
    EXPECT(!verapdf_lang_ok(u("en US")));
    EXPECT(!verapdf_lang_ok(std::u32string(U"dé")));
}

EQX_TEST(document_lang_pac) {
    EXPECT(eqx::doclang::pac_check(std::nullopt) == PacLang::Missing);
    EXPECT(pac("") == PacLang::Missing);
    EXPECT(pac("en") == PacLang::Valid);
    EXPECT(pac("de-DE") == PacLang::Valid);
    EXPECT(pac("EN-us") == PacLang::Valid);
    EXPECT(pac("zh-Hant-TW") == PacLang::Valid);
    EXPECT(pac("de-DE-1996") == PacLang::Valid);
    EXPECT(pac("en-US-x-foo") == PacLang::Valid);
    EXPECT(pac("sgn-BE-FR") == PacLang::Valid);
    EXPECT(pac("en_US") == PacLang::Syntax);       // U1-D03
    // Unknown primary subtags of a well-formed tag: PAC 26.1 shows them under its syntax condition (U1-H02, probes
    // p19-p21); they are kept apart from tags that are not well-formed.
    EXPECT(pac("zz") == PacLang::Unregistered);
    EXPECT(pac("zzz") == PacLang::Unregistered);
    EXPECT(pac("qaa") == PacLang::Unregistered);
    EXPECT(pac("abcde") == PacLang::Unregistered);
    EXPECT(pac("zz-ZZ") == PacLang::Unregistered);
    EXPECT(pac("en-a") == PacLang::Syntax);
    EXPECT(pac("x") == PacLang::Syntax);
    EXPECT(pac("en ") == PacLang::Syntax);
    EXPECT(pac("x-eqx") == PacLang::PrivateUse);   // U1-H01
    EXPECT(pac("i-klingon") == PacLang::IanaRegistered);   // U1-S03
    EXPECT(eqx::doclang::is_registered_language("gsw"));
    EXPECT(!eqx::doclang::is_registered_language("zz"));
}

// ISO 14289-1 7.2 -> ISO 32000-1 14.9.2.2: "a Language-Tag as defined in RFC 3066", the grammar
// Primary-subtag 1*8ALPHA *( "-" 1*8(ALPHA / DIGIT) ). PAC's check asks for more (BCP 47 well-formedness, a primary
// subtag of the IANA registry). Every value below is where the two part: the normative test holds, PAC's does not.
EQX_TEST(document_lang_rfc3066_valid_but_not_pac) {
    using eqx::doclang::verapdf_lang_ok;
    struct Case {
        const char* value;
        PacLang pac;
    };
    const Case cases[] = {
        // language names instead of codes (real-world documents; a PDF/UA Reference Suite file has 'English')
        {"Deutsch", PacLang::Unregistered},
        {"English", PacLang::Unregistered},
        {"deutsch-DE", PacLang::Unregistered},
        // the bounds of the grammar (veraPDF's 7.2-t29 pass files): 1 and 8 letters
        {"portugue", PacLang::Unregistered},
        {"portugue-pt", PacLang::Unregistered},
        {"p", PacLang::Syntax},
        {"p-pt", PacLang::Syntax},
        // unassigned codes of the registered lengths
        {"zz", PacLang::Unregistered},
        {"qaa", PacLang::Unregistered},
        // a registered language with subtags BCP 47 does not allow in that place
        {"en-US-US", PacLang::Syntax},
        {"en-a", PacLang::Syntax},
        // private use and the IANA prefix, which RFC 3066 2.2 reserves for exactly that
        {"x-corpus", PacLang::PrivateUse},
        {"i-klingon", PacLang::IanaRegistered},
        {"x", PacLang::Syntax},
    };
    for (const Case& c : cases) {
        EXPECT(verapdf_lang_ok(u(c.value)));
        EXPECT(pac(c.value) == c.pac);
    }
}

// Where both tests fail: the value is not a Language-Tag at all, which is 7.2-29 whoever looks.
EQX_TEST(document_lang_invalid_for_both) {
    using eqx::doclang::verapdf_lang_ok;
    const char* cases[] = {"en_US", "en US", "en-", "-en", "e1", "abcdefghi", "en-abcdefghi", "de--DE", "en_US!"};
    for (const char* c : cases) {
        EXPECT(!verapdf_lang_ok(u(c)));
        EXPECT(pac(c) == PacLang::Syntax);
    }
    EXPECT(!verapdf_lang_ok(std::u32string(U"dé")));
    EXPECT(eqx::doclang::pac_check(std::u32string(U"dé")) == PacLang::Syntax);
    // An empty Lang: no Language-Tag for 7.2-29, no document language for PAC.
    EXPECT(!verapdf_lang_ok(u("")));
    EXPECT(pac("") == PacLang::Missing);
}

// Registered tags pass both, whatever their case (RFC 3066 2.1: tags are case insensitive).
EQX_TEST(document_lang_valid_for_both) {
    using eqx::doclang::verapdf_lang_ok;
    const char* cases[] = {"de", "DE", "de-DE", "en-US", "EN-us", "gsw", "zh-Hant-TW", "de-DE-1996", "sgn-BE-FR", "en-US-x-foo"};
    for (const char* c : cases) {
        EXPECT(verapdf_lang_ok(u(c)));
        EXPECT(pac(c) == PacLang::Valid);
    }
}
