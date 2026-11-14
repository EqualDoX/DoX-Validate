// Unit tests of what the pac_quality module keeps of the content when it gathers it page by page: the text of a
// marked-content sequence in its narrowest width (PackedText), and the text items that can decide a sequence's
// ascent top (AscentTops), which must give the top the items give one after the other.
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "rules/pac_quality/pac_quality_text.h"
#include "unit.h"

using namespace eqx::pacq;

namespace {

struct Lcg {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
};

std::uint64_t bits(double v) {
    std::uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

struct Item {
    std::uint32_t font;
    double size, ury;
};

// The ascent top as the items give it one after the other (the loop over the content items).
bool reference_top(const std::vector<Item>& items, const std::vector<double>& ascent, double& out) {
    bool valid = false;
    double best = 0;
    for (const Item& it : items) {
        const double top = it.ury - 0.8 * it.size + ascent[it.font] * it.size;
        if (!valid || top > best) best = top;
        valid = true;
    }
    out = best;
    return valid;
}

}  // namespace

EQX_TEST(pac_quality_packed_text) {
    const std::vector<std::u32string> texts = {
        U"",
        U"plain",
        U"a text that is longer than what a string keeps without allocating, still narrow",
        std::u32string{0x41, 0xE4, 0xFF},
        std::u32string{0x41, 0x100, 0x20AC, 0xD800, 0xFFFF},
        std::u32string{0x41, 0x1F600, 0x10FFFF, 0x110000, 0xFFFFFFFFu, 0},
        std::u32string{0x20, 0x09, 0x3000},
    };
    for (const std::u32string& t : texts) {
        PackedText p;
        for (char32_t c : t) p.push_back(c);   // the narrow code points first, then what widens the text
        EXPECT_EQ(p.size(), t.size());
        EXPECT_EQ(p.empty(), t.empty());
        EXPECT(p.str() == t);
        if (!t.empty()) EXPECT(p.front() == t.front() && p.back() == t.back());
        std::u32string out = U"x";
        p.append_to(out);
        EXPECT(out == U"x" + t);
        PackedText q;
        q.append(t);
        q.append(std::u32string_view(t));
        EXPECT(q.str() == t + t);
        EXPECT_EQ(p.all_of([](char32_t c) { return is_whitespace(c); }), whitespace_only(t));
    }
    // widening keeps what was stored narrow, at every step
    PackedText p;
    std::u32string expect;
    Lcg rng;
    for (int i = 0; i < 3000; ++i) {
        const char32_t c = i < 1000 ? char32_t(rng.next() % 0x100) : i < 2000 ? char32_t(rng.next() % 0x10000) : char32_t(rng.next());
        p.push_back(c);
        expect.push_back(c);
        if (i % 97 == 0) EXPECT(p.str() == expect);
    }
    EXPECT(p.str() == expect);
}

EQX_TEST(pac_quality_ascent_tops_give_the_top_of_the_items_in_order) {
    Lcg rng;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double sizes[] = {12, 12, 9.5, 24, 1e-3, 7.25, 1e99, 1e100, 1e200, 1.7e308, inf};
    const double tops[] = {700, 700, 688.5, 12.25, -40, 0, -0.0, 1e99, -1e99, 1e100, -1e100, 1.7e308, -1.7e308, inf, -inf, nan};
    // ascents as ascent_em gives them: a/1000 with 0 < a < 2500, or 0.8
    const std::vector<std::vector<double>> ascents = {
        {0.8, 0.8, 0.8, 0.8}, {0.718, 0.905, 0.001, 2.499}, {2.499, 0.001, 0.8, 1.0}, {0.75, 0.75, 1.9, 0.683}};
    int checked = 0;
    for (int round = 0; round < 20000; ++round) {
        std::vector<Item> items;
        const int n = 1 + int(rng.next() % 12);
        // most sequences are ordinary text; some have a value that is not moderate somewhere
        const bool ordinary = rng.next() % 4 != 0;
        for (int i = 0; i < n; ++i) {
            Item it;
            it.font = rng.next() % 4;
            it.size = sizes[rng.next() % (ordinary ? 6 : 11)];
            it.ury = tops[rng.next() % (ordinary ? 7 : 16)];
            if (rng.next() % 3 == 0 && !items.empty()) it = items.back();   // the same item again
            items.push_back(it);
        }
        AscentTops t;
        for (const Item& it : items) t.add(it.font, it.size, it.ury);
        for (const std::vector<double>& a : ascents) {
            double want = 0, got = 0;
            const bool want_valid = reference_top(items, a, want);
            const bool got_valid = t.resolve(a, got);
            EXPECT(want_valid == got_valid);
            // the same value, bit by bit (a NaN top is a NaN top; which NaN does not show anywhere)
            EXPECT((std::isnan(want) && std::isnan(got)) || bits(want) == bits(got));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 80000);
    AscentTops none;
    double top = 0;
    EXPECT(!none.resolve(ascents[0], top));
}
