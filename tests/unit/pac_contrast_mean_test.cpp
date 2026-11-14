// pacc::rounded_mean: the multiplication by a reciprocal must give the quotient of the division for every sum and count
// the image reduction can produce (and falls back to the division outside that range).
#include "rules/pac_quality/pac_contrast.h"

#include "unit.h"

using namespace eqx;

EQX_TEST(pac_contrast_rounded_mean_equals_the_division) {
    unsigned long long mismatches = 0;
    for (unsigned count = 1; count < 256; ++count)
        for (unsigned sum = 0; sum < (1u << 16); ++sum)
            if (pacc::rounded_mean(sum, count) != (sum + count / 2) / count) ++mismatches;
    EXPECT_EQ(mismatches, 0ull);
    // outside the table: the plain division
    EXPECT_EQ(pacc::rounded_mean(1u << 20, 3), ((1u << 20) + 1) / 3);
    EXPECT_EQ(pacc::rounded_mean(70000, 300), (70000u + 150) / 300);
    EXPECT_EQ(pacc::rounded_mean(12495, 49), (12495u + 24) / 49);   // the largest sum of 49 samples
}
