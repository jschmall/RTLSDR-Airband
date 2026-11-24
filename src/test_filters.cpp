/*
 * test_filters.cpp
 *
 * Copyright (C) 2023 charlie-foxtrot
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

#include "filters.h"

using namespace std;

class FiltersTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void TearDown(void) { TestBaseClass::TearDown(); }
};

TEST_F(FiltersTest, default_notch) {
    NotchFilter notch;
    EXPECT_FALSE(notch.enabled());
}

TEST_F(FiltersTest, lowpass_below_nyquist_settles_to_unity_gain) {
    LowpassFilter lowpass(7999.5f, 16000.0f);
    ASSERT_TRUE(lowpass.enabled());
    float r = 0.0f, j = 0.0f;
    for (int i = 0; i < 200000; i++) {
        r = 1.0f;
        j = 0.0f;
        lowpass.apply(r, j);
    }
    EXPECT_NEAR(r, 1.0f, 0.02f);  // the pole is so close to the unit circle that float rounding shows
}

TEST_F(FiltersTest, lowpass_at_or_above_nyquist_is_disabled) {
    for (float freq : {8000.0f, 8000.5f, 8500.0f, 20000.0f}) {
        LowpassFilter lowpass(freq, 16000.0f);
        EXPECT_FALSE(lowpass.enabled()) << freq;
    }
    EXPECT_FALSE(LowpassFilter(4000.0f, 8000.0f).enabled());
    EXPECT_TRUE(LowpassFilter(3999.5f, 8000.0f).enabled());
}

TEST_F(FiltersTest, default_lowpass) {
    LowpassFilter lowpass;
    EXPECT_FALSE(lowpass.enabled());
}
