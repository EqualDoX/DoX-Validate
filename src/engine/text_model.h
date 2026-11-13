// Machine-readable reading view of one PDF for semantic quality checks outside the validator.
#pragma once

#include <string>

namespace eqx {

// `eqx-text-model/1` JSON: pages, catalog /Lang, Info /Title, structure elements and text runs.
// Sets *ok to false (and returns a JSON object with `error`) when the file cannot be opened.
std::string text_model_json(const std::string& path, bool* ok);

}  // namespace eqx
