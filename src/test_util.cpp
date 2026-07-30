/*
 * test_util.cpp
 *
 * Copyright (C) 2026 charlie-foxtrot
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "test_base_class.h"

#include <cstring>

#include "rtl_airband.h"

using namespace std;

// util.cpp's dBFS_to_level()/level_to_dBFS() reference fft_size, normally defined
// in rtl_airband.cpp - which can't be linked here (its main() would conflict with
// gtest's). Matches the xcalloc-stub pattern in test_udp_stream.cpp, which avoids
// linking util.cpp itself for exactly this reason; here it's util.cpp we want, so
// fft_size is what gets stubbed instead.
size_t fft_size = 2048;

class UtilTest : public TestBaseClass {};

TEST_F(UtilTest, delta_sec_whole_seconds) {
    timeval start = {100, 0};
    timeval stop = {105, 0};
    EXPECT_DOUBLE_EQ(delta_sec(&start, &stop), 5.0);
}

TEST_F(UtilTest, delta_sec_fractional_seconds) {
    timeval start = {100, 250000};
    timeval stop = {101, 750000};
    EXPECT_NEAR(delta_sec(&start, &stop), 1.5, 1e-9);
}

TEST_F(UtilTest, delta_sec_zero_when_equal) {
    timeval t = {42, 123456};
    EXPECT_DOUBLE_EQ(delta_sec(&t, &t), 0.0);
}

TEST_F(UtilTest, atofs_plain_number) {
    char s[] = "1234.5";
    EXPECT_DOUBLE_EQ(atofs(s), 1234.5);
}

TEST_F(UtilTest, atofs_kilo_suffix) {
    char s[] = "100k";
    EXPECT_DOUBLE_EQ(atofs(s), 100.0 * 1e3);
    char s_upper[] = "100K";
    EXPECT_DOUBLE_EQ(atofs(s_upper), 100.0 * 1e3);
}

TEST_F(UtilTest, atofs_mega_suffix) {
    char s[] = "100M";
    EXPECT_DOUBLE_EQ(atofs(s), 100.0 * 1e6);
}

TEST_F(UtilTest, atofs_giga_suffix) {
    char s[] = "1G";
    EXPECT_DOUBLE_EQ(atofs(s), 1.0 * 1e9);
}

TEST_F(UtilTest, atofs_preserves_input_string) {
    // atofs() temporarily mutates the buffer to strip the suffix - confirm it's
    // restored afterward, since callers (e.g. config.cpp) may reuse the string
    char s[] = "144.39M";
    atofs(s);
    EXPECT_STREQ(s, "144.39M");
}

TEST_F(UtilTest, sincosf_lut_matches_std_sin_cos) {
    sincosf_lut_init();

    for (uint32_t i = 0; i < 8; ++i) {
        // phi is a fixed-point fraction of a full turn (0..1 mapped to 0x0..0xFFFFFF)
        double turn = (double)i / 8.0;
        uint32_t phi = (uint32_t)(turn * (double)(1u << 24));
        float sine, cosine;
        sincosf_lut(phi, &sine, &cosine);

        double angle = turn * 2.0 * M_PI;
        EXPECT_NEAR(sine, std::sin(angle), 0.01) << "at turn " << turn;
        EXPECT_NEAR(cosine, std::cos(angle), 0.01) << "at turn " << turn;
    }
}

TEST_F(UtilTest, dBFS_level_roundtrip) {
    for (float level : {0.01f, 0.1f, 0.5f, 1.0f}) {
        float dbfs = level_to_dBFS(level);
        float roundtrip_level = dBFS_to_level(dbfs);
        EXPECT_NEAR(roundtrip_level, level, level * 0.01f) << "at level " << level;
    }
}

TEST_F(UtilTest, level_to_dBFS_is_never_positive) {
    // level_to_dBFS() is explicitly clamped to a max of 0.0f (see util.cpp)
    for (float level : {0.001f, 1.0f, 100.0f, 1e6f}) {
        EXPECT_LE(level_to_dBFS(level), 0.0f) << "at level " << level;
    }
}
