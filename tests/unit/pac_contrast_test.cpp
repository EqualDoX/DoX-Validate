// Unit tests of the background rendering helpers of the pac_quality module.
#include "rules/pac_quality/jpx_reduce.h"
#include "rules/pac_quality/pac_contrast.h"

#include "unit.h"

using namespace eqx;

EQX_TEST(pac_contrast_wcag_ratio) {
    const float black[3] = {0, 0, 0}, white[3] = {1, 1, 1}, grey[3] = {0.5f, 0.5f, 0.5f};
    EXPECT(pacc::wcag_contrast(black, white) > 20.99 && pacc::wcag_contrast(black, white) < 21.01);
    EXPECT(pacc::wcag_contrast(white, black) > 20.99);
    EXPECT(pacc::wcag_contrast(grey, white) > 3.9 && pacc::wcag_contrast(grey, white) < 4.1);
    EXPECT(pacc::wcag_contrast(grey, grey) == 1.0);
}

// An 8x8 lossless RGB image of one colour (JP2 file with one decomposition level, written by OpenJPEG).
static const unsigned char kJp2[] = {
    0,0,0,12,106,80,32,32,13,10,135,10,0,0,0,20,102,116,121,112,106,112,50,32,
    0,0,0,0,106,112,50,32,0,0,0,45,106,112,50,104,0,0,0,22,105,104,100,114,
    0,0,0,8,0,0,0,8,0,3,7,7,0,0,0,0,0,15,99,111,108,114,1,0,
    0,0,0,0,16,0,0,0,180,106,112,50,99,255,79,255,81,0,47,0,0,0,0,0,
    8,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,8,0,0,0,
    0,0,0,0,0,0,3,7,1,1,7,1,1,7,1,1,255,82,0,12,0,0,0,1,
    0,1,4,4,0,1,255,92,0,7,64,64,72,72,80,255,100,0,37,0,1,67,114,101,
    97,116,101,100,32,98,121,32,79,112,101,110,74,80,69,71,32,118,101,114,115,105,111,110,
    32,50,46,53,46,52,255,144,0,10,0,0,0,0,0,57,0,1,255,147,207,180,36,20,
    0,81,92,8,65,129,106,8,207,180,48,17,80,84,136,66,224,192,181,0,10,128,127,207,
    180,40,17,80,83,177,63,32,0,5,248,159,128,128,128,255,217,
};

EQX_TEST(pac_contrast_jpx_decodes) {
    jpx::Image im;
    std::string why;
    EXPECT(jpx::decode_reduced(kJp2, sizeof kJp2, 100, im, &why));
    EXPECT_EQ(im.width, 8);
    EXPECT_EQ(im.height, 8);
    EXPECT_EQ(im.components, 3);
    EXPECT_EQ(int(im.samples[0]), 200);
    EXPECT_EQ(int(im.samples[1]), 10);
    EXPECT_EQ(int(im.samples[2]), 30);
    // reduced by one level
    jpx::Image small;
    EXPECT(jpx::decode_reduced(kJp2, sizeof kJp2, 4, small, &why));
    EXPECT_EQ(small.width, 4);
    EXPECT_EQ(int(small.samples[0]), 200);
}

EQX_TEST(pac_contrast_jpx_refuses_damage) {
    jpx::Image im;
    const unsigned char junk[] = {0xff, 0x4f, 0xff, 0x51, 0x00};
    EXPECT(!jpx::decode_reduced(junk, sizeof junk, 100, im));
    EXPECT(!jpx::decode_reduced(kJp2, 40, 100, im));
    EXPECT(!jpx::decode_reduced(nullptr, 0, 100, im));
}
