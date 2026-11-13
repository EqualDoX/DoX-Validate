// Language-tag checks of the document module.
//
//   verapdf_lang_ok  veraPDF 1.30.2 CosLang rule (UA-1 7.2-29, UA-2/WTPDF 8.4.4-2):
//                    /^[a-zA-Z]{1,8}(-[a-zA-Z0-9]{1,8})*$/.test(unicodeValue) on the decoded text string.
//   pac_check        PAC 26.1 check ValidLanguage on the document language:
//                    BCP 47 (RFC 5646) well-formedness plus a lookup of the primary language subtag in the IANA
//                    Language Subtag Registry (generated table, see tools/gen_lang_registry.py).
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
    Syntax,          // CheckFailValidLanguageSyntax (also unknown primary subtags: 'zz', 'zzz', 'qaa', 'abcde')
    PrivateUse,      // CheckFailValidLanguagePrimaryTagX ("x-..." private use tag)
    IanaRegistered,  // CheckWarnValidLanguagePrimaryTagI ("i-..." IANA registrations, warning)
};

// `value` = decoded document language, nullopt when the catalog has no /Lang text string.
PacLang pac_check(const std::optional<std::u32string>& value);

// Primary language subtag (any case) registered in the IANA registry as a single subtag of Type "language".
bool is_registered_language(std::string_view subtag);

const char* registry_file_date();

}  // namespace eqx::doclang
