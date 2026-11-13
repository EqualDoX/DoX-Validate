// Debug view of the parsed model (dox-validate --dump FILE): pages, structure tree (both flavours),
// content stream instances, marked-content frames, content items and text runs.
#pragma once

#include <string>

namespace eqx {

std::string dump_model(const std::string& path);

}  // namespace eqx
