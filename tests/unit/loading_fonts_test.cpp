// Font code reading: the shortcut of CMap::read_code for the code space <0000> <FFFF> against the general procedure
// (veraPDF CMap.getCodeFromStream), which is kept here as the reference.
#include <climits>
#include <random>
#include <string>

#include "fonts/cmap.h"
#include "unit.h"

using namespace eqx::fontimpl;

namespace {

// The general procedure of CMap::read_code, as it was before the shortcut.
bool reference_read_code(const CMap& m, std::string_view bytes, std::size_t& pos, std::int32_t& code) {
    unsigned char cc[5] = {0, 0, 0, 0, 0};
    long long prev_shortest = m.shortest_cs;
    for (int i = 0; i <= 4; ++i) {
        int b = pos < bytes.size() ? static_cast<unsigned char>(bytes[pos++]) : 0xFF;
        cc[i] = static_cast<unsigned char>(b);
        std::size_t len = static_cast<std::size_t>(i) + 1;
        for (const CodeSpace& cs : m.codespaces) {
            if (cs.contains(cc, len)) {
                std::int64_t v = 0;
                for (std::size_t k = 0; k < len; ++k)
                    v += static_cast<std::int64_t>(static_cast<std::uint64_t>(cc[k]) << ((len - k - 1) * 8));
                std::int32_t res = static_cast<std::int32_t>(v);
                if (res != -1) {
                    code = res;
                    return true;
                }
            }
        }
        long long shortest = INT_MAX;
        for (const CodeSpace& cs : m.codespaces) {
            bool partial = true, thrown = false;
            for (int j = 0; j <= i; ++j) {
                if (static_cast<std::size_t>(j) >= cs.length()) {
                    thrown = true;
                    break;
                }
                unsigned char lo = static_cast<unsigned char>(cs.begin[static_cast<std::size_t>(j)]);
                unsigned char hi = static_cast<unsigned char>(cs.end[static_cast<std::size_t>(j)]);
                if (cc[j] < lo || cc[j] > hi) {
                    partial = false;
                    break;
                }
            }
            if (thrown) break;
            if (partial && shortest > static_cast<long long>(cs.length())) shortest = static_cast<long long>(cs.length());
        }
        if (shortest == INT_MAX && !m.codespaces.empty()) {
            long long n = prev_shortest - i - 1;
            if (n < 0) return false;
            std::size_t k = static_cast<std::size_t>(std::min<long long>(n, static_cast<long long>(bytes.size() - std::min(pos, bytes.size()))));
            pos += k;
            code = 0;
            return true;
        }
        prev_shortest = shortest;
    }
    code = 0;
    return true;
}

// Reads a whole string the way VeraFont::vera_codes does, with either function.
template <class Read>
std::string trace(const CMap& m, std::string_view bytes, Read read) {
    std::string out;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::size_t start = pos;
        std::int32_t code = -7;
        if (!read(m, bytes, pos, code)) {
            out += " stop";
            break;
        }
        out += " " + std::to_string(code) + "@" + std::to_string(pos);
        if (pos == start) break;
    }
    return out;
}

void compare(const CMap& m, unsigned seed) {
    std::mt19937 rng(seed);
    for (int round = 0; round < 400; ++round) {
        std::string s(rng() % 12, '\0');
        for (char& c : s) {
            const unsigned r = rng() % 8;
            c = r == 0 ? '\0' : r == 1 ? '\xFF' : r == 2 ? '\xFE' : static_cast<char>(rng());
        }
        const std::string want = trace(m, s, reference_read_code);
        const std::string got = trace(m, s, [](const CMap& c, std::string_view b, std::size_t& p, std::int32_t& code) {
            return c.read_code(b, p, code);
        });
        if (got != want) eqx_test::fail(__FILE__, __LINE__, "read_code differs: got" + got + " want" + want);
    }
}

CMap with_codespaces(std::initializer_list<std::pair<std::string, std::string>> ranges) {
    CMap m;
    for (const auto& [b, e] : ranges) m.add_codespace(b, e);
    return m;
}

}  // namespace

EQX_TEST(cmap_read_code_matches_general_procedure) {
    using namespace std::string_literals;
    compare(with_codespaces({{"\x00\x00"s, "\xFF\xFF"s}}), 1);                                  // the shortcut
    compare(with_codespaces({{"\x00\x00"s, "\xFF\xFE"s}}), 2);                                  // almost
    compare(with_codespaces({{"\x00\x01"s, "\xFF\xFF"s}}), 3);
    compare(with_codespaces({{"\x00"s, "\x80"s}, {"\x81\x40"s, "\x9F\xFC"s}}), 4);              // mixed lengths
    compare(with_codespaces({{"\x00"s, "\xFF"s}}), 5);
    compare(with_codespaces({{"\x00\x00\x00"s, "\xFF\xFF\xFF"s}}), 6);
    compare(with_codespaces({{"\x00\x00\x00\x00"s, "\xFF\xFF\xFF\xFF"s}}), 7);                  // code -1
    compare(with_codespaces({}), 8);
    compare(with_codespaces({{"\x00\x00"s, "\xFF\xFF"s}, {"\x00\x00\x00"s, "\x00\x00\x00"s}}), 9);   // second one overlaps: ignored
    CMap used = with_codespaces({{"\x00\x00"s, "\xFF\xFF"s}});
    CMap user;
    user.use(std::make_shared<const CMap>(used));   // code space taken over by /UseCMap; shortest_cs stays INT_MAX
    compare(user, 10);
}
