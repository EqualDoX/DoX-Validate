// The order of a page's text runs for the WCAG 1.4.1 check: wcag::sort_run_keys must give what std::sort gives with
// the same comparison, for every input: the plain page (lines down the page, each left to right), columns, equal keys,
// reversed and random orders, NaN coordinates.
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "rules/pac_quality/wcag_color.h"
#include "unit.h"

using namespace eqx;

namespace {

struct Rng {
    std::uint32_t s;
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
};

using Keys = std::vector<wcag::RunKey>;

bool same(const Keys& a, const Keys& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id) return false;
    return true;
}

// true when the shortcut and std::sort agree on `keys`
bool agrees(Keys keys) {
    Keys expected = keys;
    std::sort(expected.begin(), expected.end(), wcag::run_key_before);
    wcag::sort_run_keys(keys);
    return same(keys, expected);
}

Keys page(Rng& rng, int lines, int per_line, int columns) {
    Keys k;
    int id = 0;
    for (int c = 0; c < columns; ++c)
        for (int l = 0; l < lines; ++l)
            for (int r = 0; r < per_line; ++r)
                k.push_back({700.0 - 12.0 * l, 72.0 + 250.0 * c + 30.0 * r + rng.below(5), id++});
    return k;
}

}  // namespace

EQX_TEST(wcag_color_sort_plain_page_is_reversed_lines) {
    Rng rng{1};
    Keys k = page(rng, 40, 9, 1);
    EXPECT(agrees(k));
    Keys sorted = k;
    wcag::sort_run_keys(sorted);
    EXPECT_EQ(sorted.front().id, 39 * 9);        // the last line first
    EXPECT_EQ(sorted.back().id, 8);              // the end of the first line last
    EXPECT(std::is_sorted(sorted.begin(), sorted.end(), wcag::run_key_before));
}

EQX_TEST(wcag_color_sort_equals_std_sort) {
    Rng rng{20261114};
    for (int t = 0; t < 400; ++t) {
        Keys k = page(rng, 1 + rng.below(30), 1 + rng.below(8), 1 + rng.below(3));
        const int kind = t % 8;
        if (kind == 1) std::reverse(k.begin(), k.end());
        if (kind == 2)   // random order
            for (std::size_t i = k.size(); i > 1; --i) std::swap(k[i - 1], k[static_cast<std::size_t>(rng.below(int(i)))]);
        if (kind == 3 && k.size() > 3) k[k.size() / 2] = k[k.size() / 3];                    // an equal key (another id)
        if (kind == 3 && k.size() > 3) k[k.size() / 2].id = 100000;
        if (kind == 4 && !k.empty()) k[k.size() / 2].base = std::numeric_limits<double>::quiet_NaN();
        if (kind == 5 && !k.empty()) k[0].x0 = std::numeric_limits<double>::quiet_NaN();
        if (kind == 6 && k.size() > 2) k[1].base = k[0].base + 3;                            // a superscript: inside the stretch
        if (kind == 7 && k.size() > 2) k.back().base = k.front().base + 1;                   // the last run above the first line
        EXPECT(agrees(k));
    }
    EXPECT(agrees({}));
    EXPECT(agrees({{1.0, 2.0, 0}}));
    EXPECT(agrees({{1.0, 2.0, 0}, {1.0, 2.0, 1}}));     // equal keys
    EXPECT(agrees({{-0.0, 2.0, 0}, {0.0, 2.0, 1}}));    // equal for the comparison
    EXPECT(agrees({{2.0, 2.0, 0}, {1.0, 5.0, 1}, {1.5, 0.0, 2}}));   // the third stretch is not below the second
}
