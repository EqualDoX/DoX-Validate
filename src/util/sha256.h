// SHA-256 (FIPS 180-4) of a file, streamed in 1 MiB blocks.
#pragma once

#include <string>

namespace eqx {

// Lower-case hex digest; empty string when the file cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace eqx
