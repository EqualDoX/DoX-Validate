// Language-tag checks of the document module.
//
//   verapdf_lang_ok  veraPDF 1.30.2 CosLang rule (UA-1 7.2-29, UA-2/WTPDF 8.4.4-2):
//                    /^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$/.test(unicodeValue) on the decoded text string.
//   pac_check        PAC 26.1 check ValidLanguage on the document language:
//                    BCP 47 (RFC 5646) well-formedness plus a lookup of the primary language subtag in the IANA
//                    Language Subtag Registry (generated table, see tools/gen_lang_registry.py).
//
// The two are different requirements. ISO 14289-1 7.2 refers to ISO 32000-1 14.9.2, whose 14.9.2.2 asks for "a
// Language-Tag as defined in RFC 3066": the grammar verapdf_lang_ok tests. It asks neither for a catalog Lang nor for
// the BCP 47 structure of the tag, and the IANA subtag registry that pac_check consults did not exist in RFC 3066.
// A value such as 'Deutsch', 'English' or 'p' therefore meets 7.2-29 and is a finding of PAC's check only.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace eqx::doclang {

bool verapdf_lang_ok(std::u32string_view value);

enum class PacLang : unsigned char {
    Valid,
    Missing,         // no document language: PAC reports CheckFailValidLanguageSyntax
    Syntax,          // not a well-formed BCP 47 tag: CheckFailValidLanguageSyntax
    Unregistered,    // well-formed, but the primary subtag is not a registered language ('zz', 'qaa', 'Deutsch').
                     // PAC 26.1 shows these under CheckFailValidLanguageSyntax as well; the defect is the one
                     // CheckFailValidLanguagePrimaryTagOther names, and it is reported as that.
    PrivateUse,      // CheckFailValidLanguagePrimaryTagX ("x-..." private use tag)
    IanaRegistered,  // CheckWarnValidLanguagePrimaryTagI ("i-..." IANA registrations, warning)
};

// `value` = decoded document language, nullopt when the catalog has no /Lang text string.
PacLang pac_check(const std::optional<std::u32string>& value);

// Primary language subtag (any case) registered in the IANA registry as a single subtag of Type "language".
bool is_registered_language(std::string_view subtag);

const char* registry_file_date();

}  // namespace eqx::doclang
