/*
 * test_mixer.cpp
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

#include "rtl_airband.h"

using namespace std;

// mixer.cpp references these as extern; normally defined in rtl_airband.cpp
// (globals) and output.cpp (disable_channel_outputs()), neither of which can be
// linked here (rtl_airband.cpp's main() would conflict with gtest's, and pulling
// in output.cpp for one function would drag in a much larger dependency chain).
// Matches the xcalloc-stub pattern in test_udp_stream.cpp for the same reason -
// none of these are touched by mix_waveforms(), the only function under test here.
mixer_t* mixers = nullptr;
int mixer_count = 0;
volatile int do_exit = 0;
void disable_channel_outputs(channel_t*) {}

class MixerTest : public TestBaseClass {};

TEST_F(MixerTest, accumulates_scaled_samples_into_sum) {
    float sum[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float in[4] = {10.0f, 10.0f, 10.0f, 10.0f};
    mix_waveforms(sum, in, 0.5f, 4);

    EXPECT_FLOAT_EQ(sum[0], 6.0f);
    EXPECT_FLOAT_EQ(sum[1], 7.0f);
    EXPECT_FLOAT_EQ(sum[2], 8.0f);
    EXPECT_FLOAT_EQ(sum[3], 9.0f);
}

TEST_F(MixerTest, zero_multiplier_is_a_true_no_op) {
    // mix_waveforms() short-circuits entirely on mult == 0.0f (see mixer.cpp) -
    // confirm sum is genuinely untouched, not just added-to-with-zero
    float sum[3] = {1.0f, 2.0f, 3.0f};
    float in[3] = {100.0f, 200.0f, 300.0f};
    mix_waveforms(sum, in, 0.0f, 3);

    EXPECT_FLOAT_EQ(sum[0], 1.0f);
    EXPECT_FLOAT_EQ(sum[1], 2.0f);
    EXPECT_FLOAT_EQ(sum[2], 3.0f);
}

TEST_F(MixerTest, negative_multiplier_subtracts) {
    float sum[2] = {10.0f, 10.0f};
    float in[2] = {3.0f, 4.0f};
    mix_waveforms(sum, in, -1.0f, 2);

    EXPECT_FLOAT_EQ(sum[0], 7.0f);
    EXPECT_FLOAT_EQ(sum[1], 6.0f);
}

TEST_F(MixerTest, successive_calls_accumulate_multiple_inputs) {
    // this is the actual usage pattern in mixer.cpp: one mix_waveforms() call per
    // connected input, all accumulating into the same output buffer
    float sum[2] = {0.0f, 0.0f};
    float in_a[2] = {1.0f, 1.0f};
    float in_b[2] = {2.0f, 2.0f};

    mix_waveforms(sum, in_a, 1.0f, 2);
    mix_waveforms(sum, in_b, 0.5f, 2);

    EXPECT_FLOAT_EQ(sum[0], 2.0f);  // 1*1.0 + 2*0.5
    EXPECT_FLOAT_EQ(sum[1], 2.0f);
}
