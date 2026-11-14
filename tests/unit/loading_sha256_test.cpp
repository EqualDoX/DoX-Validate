// SHA-256: FIPS 180-4 test vectors, and the block function of the CPU against the portable one.
#include <random>
#include <string>

#include "unit.h"
#include "util/sha256.h"

using namespace eqx;

namespace {

std::string digest(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return h.hex();
}

std::string digest_portable(const std::string& s) {
    Sha256 h = Sha256::portable();
    h.update(s.data(), s.size());
    return h.hex();
}

}  // namespace

EQX_TEST(sha256_test_vectors) {
    for (auto fn : {digest, digest_portable}) {
        EXPECT_EQ(fn(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        EXPECT_EQ(fn("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        EXPECT_EQ(fn("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                  std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
        EXPECT_EQ(fn("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
                  std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
        EXPECT_EQ(fn(std::string(1000000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }
}

EQX_TEST(sha256_cpu_and_portable_agree) {
    std::mt19937 rng(42);
    // every length around the block and padding boundaries, and data fed in uneven pieces
    for (std::size_t n = 0; n < 300; ++n) {
        std::string s(n, '\0');
        for (char& c : s) c = static_cast<char>(rng());
        EXPECT_EQ(digest(s), digest_portable(s));
    }
    std::string big(200000, '\0');
    for (char& c : big) c = static_cast<char>(rng());
    const std::string want = digest_portable(big);
    for (int round = 0; round < 20; ++round) {
        Sha256 h;
        std::size_t at = 0;
        while (at < big.size()) {
            const std::size_t n = std::min<std::size_t>(big.size() - at, rng() % 700);
            h.update(big.data() + at, n);
            at += n;
        }
        EXPECT_EQ(h.hex(), want);
    }
}
