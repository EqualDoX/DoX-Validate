// SHA-256 (FIPS 180-4). The block function uses the SHA extensions of the CPU when it has them (x86-64 SHA-NI,
// detected at run time) and portable code otherwise; both compute the same digest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace eqx {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t n);
    // Lower-case hex digest of everything passed to update(); the object is finished afterwards.
    std::string hex();

    // The portable block function only (unit tests compare both).
    static Sha256 portable();

private:
    using Blocks = void (*)(std::uint32_t state[8], const unsigned char* data, std::size_t blocks);
    Blocks blocks_;
    std::uint32_t h_[8];
    unsigned char buf_[64];
    std::size_t blen_ = 0;
    std::uint64_t total_ = 0;
};

// Lower-case hex digest of a file, streamed; empty string when the file cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace eqx
