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
    EXPECT(pac("zz") == PacLang::Syntax);          // U1-H02: unknown primary subtag
    EXPECT(pac("zzz") == PacLang::Syntax);         // probes p19-p21
    EXPECT(pac("qaa") == PacLang::Syntax);
    EXPECT(pac("abcde") == PacLang::Syntax);
    EXPECT(pac("en-a") == PacLang::Syntax);
    EXPECT(pac("x") == PacLang::Syntax);
    EXPECT(pac("en ") == PacLang::Syntax);
    EXPECT(pac("x-eqx") == PacLang::PrivateUse);   // U1-H01
    EXPECT(pac("i-klingon") == PacLang::IanaRegistered);   // U1-S03
    EXPECT(eqx::doclang::is_registered_language("gsw"));
    EXPECT(!eqx::doclang::is_registered_language("zz"));
}
