/*
 * test_fm_demod.cpp
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

#include <cmath>
#include <vector>

#include "fm_demod.h"

using namespace std;

class FmDemodTest : public TestBaseClass {};

TEST_F(FmDemodTest, parse_known_names) {
    fm_demod_algo algo = FM_QUADRI_DEMOD;
    EXPECT_TRUE(parse_fm_demod("fast_atan2", &algo));
    EXPECT_EQ(algo, FM_FAST_ATAN2);
    EXPECT_TRUE(parse_fm_demod("atan2", &algo));
    EXPECT_EQ(algo, FM_ATAN2);
    EXPECT_TRUE(parse_fm_demod("quadri", &algo));
    EXPECT_EQ(algo, FM_QUADRI_DEMOD);
}

TEST_F(FmDemodTest, parse_unknown_name_leaves_algo_unchanged) {
    fm_demod_algo algo = FM_ATAN2;
    for (const char* name : {"", "atan", "ATAN2", "atan2 ", "fast"}) {
        EXPECT_FALSE(parse_fm_demod(name, &algo)) << name;
        EXPECT_EQ(algo, FM_ATAN2) << name;
    }
}

TEST_F(FmDemodTest, atan2_is_exact_over_the_full_circle) {
    float const br = cosf(0.4f), bj = sinf(0.4f);
    for (int deg = -179; deg <= 179; deg++) {
        float const step = deg * M_PI / 180.0f;
        EXPECT_NEAR(polar_disc_atan2(cosf(0.4f + step), sinf(0.4f + step), br, bj), deg / 180.0f, 1e-6) << deg;
    }
}

TEST_F(FmDemodTest, atan2_does_not_depend_on_amplitude) {
    for (float const gain : {1e-3f, 1.0f, 1e3f}) {
        EXPECT_NEAR(polar_disc_atan2(gain * cosf(0.2f), gain * sinf(0.2f), gain, 0.0f), 0.2f / M_PI, 1e-6) << gain;
    }
}

TEST_F(FmDemodTest, atan2_of_silence_is_zero) {
    EXPECT_EQ(polar_disc_atan2(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
    EXPECT_EQ(polar_disc_atan2(1.0f, 1.0f, 0.0f, 0.0f), 0.0f);
}

// A CTCSS tone must not come out with harmonics: 136.5 Hz would put one on the 410.8 Hz paging tone.
TEST_F(FmDemodTest, atan2_adds_no_harmonics_to_a_ctcss_tone) {
    int const rate = 16000, n = rate * 2;  // whole number of cycles
    double const tone = 136.5, deviation = 350.0;
    for (double const carrier_offset : {0.0, 300.0}) {
        vector<float> audio;
        double phase = 0.0;
        float pr = 1.0f, pj = 0.0f;
        for (int i = 0; i < n; i++) {
            phase += 2.0 * M_PI * (carrier_offset + deviation * sin(2.0 * M_PI * tone * i / rate)) / rate;
            float const re = cos(phase), im = sin(phase);
            audio.push_back(polar_disc_atan2(re, im, pr, pj));
            pr = re;
            pj = im;
        }
        auto level = [&](double freq) {
            double c = 0.0, s = 0.0;
            for (int i = 0; i < n; i++) {
                c += audio[i] * cos(2.0 * M_PI * freq * i / rate);
                s += audio[i] * sin(2.0 * M_PI * freq * i / rate);
            }
            return hypot(c, s) * 2.0 / n;
        };
        double const fundamental = level(tone);
        EXPECT_NEAR(fundamental, 2.0 * deviation / rate, 1e-4);
        for (int harmonic = 2; harmonic <= 5; harmonic++) {
            EXPECT_LT(20.0 * log10(level(tone * harmonic) / fundamental), -80.0) << harmonic << " at offset " << carrier_offset;
        }
    }
}
