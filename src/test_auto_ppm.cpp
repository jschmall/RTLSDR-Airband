/*
 * test_auto_ppm.cpp
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
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "auto_ppm.h"
#include "input-common.h"
#include "rtl_airband.h"

using namespace std;

namespace {

// Offset a carrier at freq_hz shows when the correction is residual_ppm short of right.
AutoPpmEstimate est(int freq_hz, double residual_ppm, uint32_t updates = 50) {
    return {freq_hz, (float)(-residual_ppm * freq_hz / 1e6), updates};
}

}  // namespace

class AutoPpmDecideTest : public TestBaseClass {
   protected:
    // One evaluation, then (unless the first already acted) a second from newer measurements:
    // persistence means an out-of-deadband residual only steps on its second reading.
    AutoPpmDecision decide2(int correction, vector<AutoPpmEstimate> e) {
        AutoPpmDecision d = auto_ppm_decide(correction, e, params, &state);
        if (d.action != AUTO_PPM_HOLD) {
            return d;
        }
        for (AutoPpmEstimate& x : e) {
            x.updates += 10;
        }
        return auto_ppm_decide(correction, e, params, &state);
    }

    AutoPpmParams params;
    AutoPpmState state;
};

TEST_F(AutoPpmDecideTest, no_estimates_holds) {
    AutoPpmDecision d = auto_ppm_decide(0, {}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_FALSE(d.residual_valid);
    EXPECT_EQ(d.eligible, 0);
}

TEST_F(AutoPpmDecideTest, estimates_with_too_few_updates_are_ignored) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 3.0, params.min_updates - 1)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.eligible, 0);
}

TEST_F(AutoPpmDecideTest, carrier_above_frequency_means_correction_must_drop) {
    // +360 Hz at 120 MHz: the LO runs 3 ppm slow, so the correction has to come down 3 ppm.
    AutoPpmDecision d = decide2(0, {{120000000, 360.0f, 50}});
    ASSERT_TRUE(d.residual_valid);
    EXPECT_NEAR(d.residual_ppm, -3.0, 1e-3);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, -3);
}

TEST_F(AutoPpmDecideTest, residual_scales_with_channel_frequency_not_centerfreq) {
    AutoPpmDecision d = decide2(10, {est(118000000, 2.0), est(136000000, 2.0)});
    ASSERT_TRUE(d.residual_valid);
    EXPECT_NEAR(d.residual_ppm, 2.0, 1e-3);
    EXPECT_EQ(d.new_correction, 12);
}

TEST_F(AutoPpmDecideTest, small_residual_inside_deadband_holds) {
    AutoPpmDecision d = auto_ppm_decide(4, {est(120000000, 0.6), est(125000000, 0.7)}, params, &state);
    EXPECT_TRUE(d.residual_valid);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.new_correction, 4);
}

TEST_F(AutoPpmDecideTest, offset_carrier_channel_is_rejected_as_an_outlier) {
    // Three channels agree on +2 ppm; a climax site sits 5 kHz off (~40 ppm at 125 MHz).
    vector<AutoPpmEstimate> e = {est(120000000, 2.0), est(121500000, 2.2), est(123000000, 1.9), {125000000, -5000.0f, 50}};
    AutoPpmDecision d = decide2(0, e);
    EXPECT_EQ(d.eligible, 4);
    EXPECT_EQ(d.contributing, 3);
    EXPECT_EQ(d.rejected, 1);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 2);
}

TEST_F(AutoPpmDecideTest, two_disagreeing_channels_hold) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 1.0), est(125000000, 6.0)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_FALSE(d.residual_valid);
}

TEST_F(AutoPpmDecideTest, no_majority_among_many_channels_holds) {
    vector<AutoPpmEstimate> e = {est(120000000, -10.0), est(121000000, 0.0), est(122000000, 10.0)};
    AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.contributing, 1);
}

TEST_F(AutoPpmDecideTest, wide_spread_inside_the_outlier_window_holds) {
    // Every value is within 1.5 ppm of the median (1.25) but the extremes are 2.5 apart.
    vector<AutoPpmEstimate> e = {est(120000000, 0.0), est(121000000, 1.0), est(122000000, 1.5), est(123000000, 2.5)};
    AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
}

TEST_F(AutoPpmDecideTest, step_is_limited_to_max_step) {
    AutoPpmDecision d = decide2(0, {est(120000000, 23.0)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 5);
    EXPECT_FALSE(d.clamped);

    state = AutoPpmState();
    d = decide2(0, {est(120000000, -23.0)});
    EXPECT_EQ(d.new_correction, -5);
}

TEST_F(AutoPpmDecideTest, never_goes_past_plus_minus_100_ppm) {
    // Plausible estimates (|implied| <= 100) that would still push past the limit.
    AutoPpmDecision d = decide2(98, {est(120000000, 1.9)});
    EXPECT_EQ(d.new_correction, 100);
    EXPECT_FALSE(d.clamped);

    state = AutoPpmState();
    params.max_step = 100;
    // At the hard limit, anything asking for more implies >100 ppm and is rejected outright.
    d = decide2(100, {est(120000000, 0.99)});
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.new_correction, 100);
    EXPECT_EQ(d.rejected, 1);

    state = AutoPpmState();
    d = decide2(-98, {est(120000000, -1.9)});
    EXPECT_EQ(d.new_correction, -100);
}

TEST_F(AutoPpmDecideTest, estimates_implying_more_than_100_ppm_are_rejected) {
    params.max_step = 100;
    for (double residual : {150.0, -300.0}) {
        state = AutoPpmState();
        AutoPpmDecision d = decide2(0, {est(120000000, residual), est(122000000, residual)});
        EXPECT_EQ(d.action, AUTO_PPM_HOLD) << residual;
        EXPECT_EQ(d.new_correction, 0) << residual;
        EXPECT_EQ(d.rejected, 2) << residual;
        EXPECT_FALSE(d.residual_valid) << residual;
    }
}

TEST_F(AutoPpmDecideTest, one_implausible_channel_does_not_block_the_rest) {
    AutoPpmDecision d = decide2(0, {est(120000000, 2.0), est(121000000, 2.1), est(122000000, 400.0)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 2);
    EXPECT_EQ(d.rejected, 1);
}

TEST_F(AutoPpmDecideTest, smaller_max_ppm_clamps_tighter) {
    params.max_ppm = 20;
    params.max_step = 100;
    AutoPpmDecision d = decide2(0, {est(120000000, 40.0)});
    EXPECT_EQ(d.new_correction, 20);
    EXPECT_TRUE(d.clamped);
}

TEST_F(AutoPpmDecideTest, converging_steps_never_trip_the_divergence_guard) {
    int correction = 0;
    double const truth = 23.0;
    for (int i = 0; i < 10; i++) {
        AutoPpmDecision d = decide2(correction, {est(120000000, truth - correction)});
        ASSERT_NE(d.action, AUTO_PPM_REVERT) << i;
        correction = d.new_correction;
    }
    EXPECT_EQ(correction, 23);
}

TEST_F(AutoPpmDecideTest, diverging_steps_revert_to_the_base_correction) {
    // A sign error in the loop: each step makes the measured error worse.
    state.base_correction = 7;
    int correction = 7;
    double residual = 10.0;
    AutoPpmDecision d = decide2(correction, {est(120000000, residual)});
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    for (int i = 0; i < 2; i++) {
        correction = d.new_correction;
        residual += 5.0;
        d = decide2(correction, {est(120000000, residual)});
    }
    // The second growth is caught on the first reading after the step, before persistence.
    EXPECT_EQ(d.action, AUTO_PPM_REVERT);
    EXPECT_EQ(d.new_correction, 7);
}

TEST_F(AutoPpmDecideTest, one_noisy_step_does_not_trip_the_guard) {
    AutoPpmDecision d = decide2(0, {est(120000000, 10.0)});
    d = decide2(d.new_correction, {est(120000000, 10.5)});  // grew once
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    d = decide2(d.new_correction, {est(120000000, 4.0)});  // then shrank
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    d = decide2(d.new_correction, {est(120000000, 5.0)});  // grew once more
    EXPECT_NE(d.action, AUTO_PPM_REVERT);
}

TEST_F(AutoPpmDecideTest, first_out_of_deadband_reading_waits_for_a_second) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 3.0, 50)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_TRUE(d.residual_valid);
    EXPECT_EQ(state.pending_count, 1);
    d = auto_ppm_decide(0, {est(120000000, 3.0, 60)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 3);
    EXPECT_EQ(state.pending_count, 0);
}

TEST_F(AutoPpmDecideTest, rereading_an_unchanged_estimate_is_not_a_second_measurement) {
    for (int i = 0; i < 5; i++) {
        AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 3.0, 50)}, params, &state);
        EXPECT_EQ(d.action, AUTO_PPM_HOLD) << i;
    }
    EXPECT_EQ(auto_ppm_decide(0, {est(120000000, 3.0, 51)}, params, &state).action, AUTO_PPM_ADJUST);
}

TEST_F(AutoPpmDecideTest, sign_flip_restarts_persistence) {
    auto_ppm_decide(0, {est(120000000, 3.0, 50)}, params, &state);
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, -3.0, 60)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(state.pending_sign, -1);
    EXPECT_EQ(state.pending_count, 1);
    d = auto_ppm_decide(0, {est(120000000, -3.0, 70)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, -3);
}

TEST_F(AutoPpmDecideTest, reading_inside_deadband_resets_persistence) {
    auto_ppm_decide(0, {est(120000000, 3.0, 50)}, params, &state);
    auto_ppm_decide(0, {est(120000000, 0.2, 60)}, params, &state);
    EXPECT_EQ(state.pending_count, 0);
    EXPECT_EQ(auto_ppm_decide(0, {est(120000000, 3.0, 70)}, params, &state).action, AUTO_PPM_HOLD);
}

TEST_F(AutoPpmDecideTest, evaluation_without_a_residual_leaves_persistence_alone) {
    auto_ppm_decide(0, {est(120000000, 3.0, 50)}, params, &state);
    EXPECT_FALSE(auto_ppm_decide(0, {}, params, &state).residual_valid);
    EXPECT_FALSE(auto_ppm_decide(0, {est(120000000, 1.0, 55), est(125000000, 6.0, 55)}, params, &state).residual_valid);
    EXPECT_EQ(auto_ppm_decide(0, {est(120000000, 3.0, 60)}, params, &state).action, AUTO_PPM_ADJUST);
}

TEST_F(AutoPpmDecideTest, too_few_voters_hold) {
    params.min_voters = 2;
    AutoPpmDecision d = decide2(0, {est(120000000, 3.0)});
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_FALSE(d.residual_valid);
    EXPECT_EQ(d.reason, "1 voter, need 2");
    EXPECT_EQ(decide2(0, {est(120000000, 3.0), est(121000000, 3.2)}).action, AUTO_PPM_ADJUST);
}

TEST_F(AutoPpmDecideTest, rejected_outliers_do_not_count_as_voters) {
    params.min_voters = 3;
    vector<AutoPpmEstimate> e = {est(120000000, 2.0), est(121500000, 2.2), {125000000, -5000.0f, 50}};
    AutoPpmDecision d = decide2(0, e);
    EXPECT_EQ(d.eligible, 3);
    EXPECT_EQ(d.contributing, 2);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.reason, "2 voters, need 3");
}

TEST_F(AutoPpmDecideTest, deadband_widens_at_low_band) {
    // 100 Hz at 42.44 MHz is 2.36 ppm; at 160 MHz the 1 ppm floor applies.
    AutoPpmDecision d = decide2(0, {est(42440000, 2.0)});
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.reason, "within deadband");
    EXPECT_NEAR(d.deadband_ppm, 100.0 / 42.44, 1e-3);

    state = AutoPpmState();
    d = decide2(0, {est(42440000, 3.0)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 3);

    state = AutoPpmState();
    d = decide2(0, {est(160000000, 2.0)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_NEAR(d.deadband_ppm, 1.0, 1e-9);
}

TEST_F(AutoPpmDecideTest, zero_deadband_hz_keeps_the_ppm_floor) {
    params.deadband_hz = 0;
    AutoPpmDecision d = decide2(0, {est(42440000, 1.2)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_NEAR(d.deadband_ppm, 1.0, 1e-9);
    state = AutoPpmState();
    EXPECT_EQ(decide2(0, {est(42440000, 0.9)}).action, AUTO_PPM_HOLD);
}

TEST_F(AutoPpmDecideTest, growth_tolerance_scales_with_the_deadband) {
    // At 42.44 MHz the tolerance is 2.36 / 3 = 0.79 ppm: two steps each followed by 0.5 ppm of
    // apparent growth are noise, not divergence.
    int correction = 0;
    double residual = 4.0;
    for (int i = 0; i < 3; i++) {
        AutoPpmDecision d = decide2(correction, {est(42440000, residual)});
        ASSERT_NE(d.action, AUTO_PPM_REVERT) << i;
        correction = d.new_correction;
        residual += 0.5;
    }
}

// Residual sequences auto_ppm acted on in production on 2026-11-03, before this rule set. Each
// value was then measured again from fresh samples, so feed every one twice with growing updates.
TEST_F(AutoPpmDecideTest, replay_of_low_band_hunting_on_042) {
    const double seq[] = {2.80, 0.92, -4.03, 2.28, 1.12, -3.04, 0.81, 1.49, 0.91, -2.10, 1.43, 1.98, -3.32, 1.06, 0.96, -1.24, -1.76};
    const int voters[] = {1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2};
    for (int min_voters : {1, 2}) {
        params.min_voters = min_voters;
        state = AutoPpmState();
        int adjustments = 0;
        uint32_t updates = 50;
        for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
            vector<AutoPpmEstimate> e = {est(42440000, seq[i], updates)};
            if (voters[i] == 2) {
                e.push_back(est(42280000, seq[i], updates));
            }
            for (int rep = 0; rep < 2; rep++) {
                for (AutoPpmEstimate& x : e) {
                    x.updates = updates;
                }
                updates += 10;
                AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
                ASSERT_NE(d.action, AUTO_PPM_REVERT) << min_voters << " " << i;
                adjustments += d.action == AUTO_PPM_ADJUST;
                if (d.action == AUTO_PPM_ADJUST) {
                    updates = 50;  // a step restarts the estimates
                }
            }
        }
        EXPECT_LE(adjustments, min_voters == 2 ? 1 : 4) << min_voters;
    }
}

TEST_F(AutoPpmDecideTest, replay_of_mobile_disagreement_on_172) {
    const double seq[] = {-0.76, 1.65, -1.17, -0.78, 2.27};
    const int voters[] = {1, 2, 2, 1, 1};
    params.min_voters = 2;
    int adjustments = 0;
    uint32_t updates = 50;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        vector<AutoPpmEstimate> e = {est(172225000, seq[i], updates)};
        if (voters[i] == 2) {
            e.push_back(est(172500000, seq[i], updates));
        }
        AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
        ASSERT_NE(d.action, AUTO_PPM_REVERT) << i;
        adjustments += d.action == AUTO_PPM_ADJUST;
        updates += 10;
    }
    EXPECT_EQ(adjustments, 0);
}

TEST_F(AutoPpmDecideTest, single_reference_still_converges) {
    // The dev box's NOAA 162.550 validation: one voter, starting 20 ppm off.
    int correction = -20;
    double const truth = 0.5;
    int evaluations = 0;
    uint32_t updates = 50;
    for (; evaluations < 20 && fabs(truth - correction) >= 1.0; evaluations++) {
        AutoPpmDecision d = auto_ppm_decide(correction, {est(162550000, truth - correction, updates)}, params, &state);
        ASSERT_NE(d.action, AUTO_PPM_REVERT);
        updates = d.action == AUTO_PPM_ADJUST ? 50 : updates + 10;
        correction = d.new_correction;
    }
    EXPECT_LE(fabs(truth - correction), 1.0);
    EXPECT_LE(evaluations, 10);  // 5 steps of up to 5 ppm, two readings each
}

TEST_F(AutoPpmDecideTest, whole_ppm_floor_stops_toggling_between_neighbours) {
    // 169 on 2026-11-04: the dongle sat near 16.5 ppm and the correction flipped 16 <-> 17 on
    // residuals of 0.77..0.94 ppm. A whole-ppm driver can't do better than that.
    const double seq[] = {0.82, -0.80, 0.77, -0.94};
    uint32_t updates = 50;
    for (double r : seq) {
        for (int rep = 0; rep < 2; rep++, updates += 10) {
            EXPECT_EQ(auto_ppm_decide(16, {est(169000000, r, updates)}, params, &state).action, AUTO_PPM_HOLD) << r;
        }
    }
}

TEST_F(AutoPpmDecideTest, opposite_direction_steps_never_trip_the_guard) {
    // Hunting is not divergence: a step up and a later step down, each followed by a larger
    // reading, must not count as two consecutive growing steps.
    AutoPpmDecision d = decide2(0, {est(120000000, 3.0), est(121000000, 3.0)});
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    ASSERT_EQ(d.new_correction, 3);
    d = decide2(3, {est(120000000, -3.6), est(121000000, -3.6)});  // grew, opposite sign
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    ASSERT_EQ(d.new_correction, -1);
    d = decide2(-1, {est(120000000, 4.2), est(121000000, 4.2)});  // grew again, after a step the other way
    EXPECT_NE(d.action, AUTO_PPM_REVERT);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
}

TEST_F(AutoPpmDecideTest, lone_voter_far_off_is_distrusted_once_converged) {
    // 154 on 2026-11-04: SC Fire Tac 2 (153.950) reads a steady -5 kHz, +32.8 ppm. Alone, it
    // stepped the dongle 5 ppm and later tripped the guard.
    vector<AutoPpmEstimate> agree = {est(154740000, 0.2), est(155580000, 0.3)};
    AutoPpmDecision d = auto_ppm_decide(0, agree, params, &state);
    ASSERT_EQ(d.reason, "within deadband");
    EXPECT_TRUE(state.converged);

    for (uint32_t updates = 50; updates < 150; updates += 10) {
        d = auto_ppm_decide(0, {est(153950000, 32.8, updates)}, params, &state);
        ASSERT_EQ(d.action, AUTO_PPM_HOLD) << updates;
        EXPECT_FALSE(d.residual_valid);
        EXPECT_EQ(d.reason, "one channel +32.8 ppm off; waiting for another channel to agree");
    }
    EXPECT_EQ(state.pending_count, 0);

    // A lone voter close to the converged value is still used.
    EXPECT_EQ(decide2(0, {est(154740000, 3.0)}).action, AUTO_PPM_ADJUST);
}

TEST_F(AutoPpmDecideTest, lone_far_off_voter_is_trusted_until_converged) {
    // A single reference has to be able to pull a badly set correction in on its own.
    EXPECT_FALSE(state.converged);
    AutoPpmDecision d = decide2(0, {est(162550000, 20.0)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 5);

    // One voter inside the deadband isn't evidence of convergence.
    state = AutoPpmState();
    auto_ppm_decide(0, {est(162550000, 0.2)}, params, &state);
    EXPECT_FALSE(state.converged);
}

TEST_F(AutoPpmDecideTest, two_channels_agreeing_on_a_large_error_still_step) {
    auto_ppm_decide(0, {est(154740000, 0.2), est(155580000, 0.3)}, params, &state);
    ASSERT_TRUE(state.converged);
    AutoPpmDecision d = decide2(0, {est(154740000, 12.0), est(155580000, 12.3)});
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 5);
}

TEST_F(AutoPpmDecideTest, replay_of_lone_voter_trip_on_154) {
    // Converged on several channels, steps up and later down by 1 ppm, then the first reading
    // after the second step comes from Tac 2 alone. That tripped the guard in production.
    vector<AutoPpmEstimate> agree = {est(154740000, 0.1), est(155580000, 0.2), est(156075000, 0.1)};
    ASSERT_EQ(auto_ppm_decide(-3, agree, params, &state).reason, "within deadband");
    AutoPpmDecision d = decide2(-3, {est(154740000, 1.4), est(155580000, 1.5)});
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    d = decide2(d.new_correction, {est(154740000, -1.6), est(155580000, -1.5)});
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    int const correction = d.new_correction;
    for (uint32_t updates = 50; updates < 100; updates += 10) {
        d = auto_ppm_decide(correction, {est(153950000, 32.8, updates)}, params, &state);
        ASSERT_EQ(d.action, AUTO_PPM_HOLD) << updates;
    }
    EXPECT_TRUE(state.awaiting_check) << "the lone reading must not be used as the post-step check";
}

class AutoPpmParamsTest : public TestBaseClass {
   protected:
    bool parse(const char* text, AutoPpmParams* out, string* error) {
        libconfig::Config cfg;
        cfg.readString(text);
        return auto_ppm_parse_params(cfg.lookup("dev"), out, error);
    }
};

TEST_F(AutoPpmParamsTest, defaults) {
    AutoPpmParams p;
    string error;
    ASSERT_TRUE(parse("dev: { auto_ppm = true; };", &p, &error)) << error;
    EXPECT_EQ(p.interval_sec, 30);
    EXPECT_EQ(p.max_ppm, 100);
    EXPECT_EQ(p.max_step, 5);
    EXPECT_EQ(p.min_voters, 1);
    EXPECT_EQ(p.deadband_hz, 100);
}

TEST_F(AutoPpmParamsTest, explicit_values) {
    AutoPpmParams p;
    string error;
    ASSERT_TRUE(parse("dev: { auto_ppm_interval = 10; auto_ppm_max_ppm = 40; auto_ppm_max_step = 2; };", &p, &error)) << error;
    EXPECT_EQ(p.interval_sec, 10);
    EXPECT_EQ(p.max_ppm, 40);
    EXPECT_EQ(p.max_step, 2);

    ASSERT_TRUE(parse("dev: { auto_ppm_min_voters = 2; auto_ppm_deadband_hz = 0; };", &p, &error)) << error;
    EXPECT_EQ(p.min_voters, 2);
    EXPECT_EQ(p.deadband_hz, 0);
}

TEST_F(AutoPpmParamsTest, out_of_range_values_are_rejected) {
    for (const char* text : {"dev: { auto_ppm_max_ppm = 101; };", "dev: { auto_ppm_max_ppm = 0; };", "dev: { auto_ppm_max_step = 0; };", "dev: { auto_ppm_interval = 4; };",
                             "dev: { auto_ppm_interval = -30; };", "dev: { auto_ppm_max_ppm = 50.5; };", "dev: { auto_ppm_min_voters = 0; };", "dev: { auto_ppm_min_voters = 33; };",
                             "dev: { auto_ppm_deadband_hz = -1; };", "dev: { auto_ppm_deadband_hz = 1001; };"}) {
        AutoPpmParams p;
        string error;
        EXPECT_FALSE(parse(text, &p, &error)) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

// Driver fakes for auto_ppm_tick(): correction lives in a global the fake get_settings reports.
namespace {
int fake_correction = 0;
int fake_set_count = 0;
bool fake_set_fails = false;

int fake_set_correction(input_t* const, int const correction) {
    if (fake_set_fails) {
        return -1;
    }
    fake_correction = correction;
    fake_set_count++;
    return 0;
}

int fake_get_settings(input_t* const, input_settings_t* out) {
    out->gain = 0.0f;
    out->gain_known = false;
    out->correction = (float)fake_correction;
    out->bandwidth = 0;
    return 0;
}
}  // namespace

class AutoPpmTickTest : public TestBaseClass {
   protected:
    void SetUp() override {
        TestBaseClass::SetUp();
        fake_correction = 0;
        fake_set_count = 0;
        fake_set_fails = false;
        input.driver_type = "rtlsdr";
        input.state = INPUT_RUNNING;
        input.dev_data = &fake_dev_data;
        input.set_correction = &fake_set_correction;
        input.get_settings = &fake_get_settings;

        freqs = (freq_t*)calloc(kChannels, sizeof(freq_t));
        channels.reset(new channel_t[kChannels]());
        for (int j = 0; j < kChannels; j++) {
            freqs[j].frequency = 120000000 + j * 1000000;
            channels[j].freqlist = &freqs[j];
            channels[j].freq_count = 1;
            channels[j].enabled = true;
            channels[j].pending_remove_request = -1;
        }
        dev.input = &input;
        dev.channels = channels.get();
        dev.channel_count = kChannels;
        dev.channel_capacity = kChannels;
        ap = new auto_ppm_device();
        ap->enabled = true;
        dev.auto_ppm = ap;
    }
    void TearDown() override {
        delete ap;
        free(freqs);
        TestBaseClass::TearDown();
    }
    // What the demod thread would publish for every channel, under the current generation, if
    // the tuner's true error were truth_ppm.
    // Each call is a newer measurement (more updates) unless the generation changed.
    void measure(double truth_ppm) {
        uint32_t const gen = input.tuning_generation.load();
        if (gen != measured_gen) {
            measured_gen = gen;
            next_updates = 50;
        }
        uint32_t const updates = next_updates;
        next_updates += 10;
        for (int j = 0; j < kChannels; j++) {
            freqs[j].ppm_offset_hz = (float)(-(truth_ppm - fake_correction) * freqs[j].frequency / 1e6);
            freqs[j].ppm_offset_updates = updates;
            freqs[j].ppm_offset_generation = gen;
        }
    }
    // Runs one tick past the interval; set_correction is faked, so bump the generation the way
    // input_set_correction() does.
    void tick() {
        uint32_t const before = (uint32_t)fake_set_count;
        now += 1000.0;
        auto_ppm_tick(&dev, 0, now);
        if ((uint32_t)fake_set_count != before) {
            EXPECT_GT(input.tuning_generation.load(), 0u);
        }
    }

    static const int kChannels = 3;
    int fake_dev_data = 0;
    input_t input{};
    freq_t* freqs = nullptr;
    unique_ptr<channel_t[]> channels;
    device_t dev{};
    auto_ppm_device* ap = nullptr;
    double now = 0.0;
    uint32_t measured_gen = 0;
    uint32_t next_updates = 50;
};

TEST_F(AutoPpmTickTest, converges_in_bounded_steps) {
    for (int i = 0; i < 10; i++) {
        measure(17.0);
        tick();
        EXPECT_LE(abs(fake_correction), 17);
    }
    EXPECT_EQ(fake_correction, 17);
    EXPECT_EQ(ap->adjustments, 4u);  // 5 + 5 + 5 + 2
}

TEST_F(AutoPpmTickTest, stale_generation_estimates_are_not_used) {
    measure(10.0);
    tick();
    measure(10.0);
    tick();
    ASSERT_EQ(fake_correction, 5);
    // No fresh measurements since the change: the old ones are from the previous generation.
    tick();
    EXPECT_EQ(fake_correction, 5);
    EXPECT_EQ(ap->last.eligible, 0);
}

TEST_F(AutoPpmTickTest, waits_for_the_interval) {
    ap->params.interval_sec = 30;
    measure(10.0);
    auto_ppm_tick(&dev, 0, 100.0);
    EXPECT_EQ(ap->state.pending_count, 1);
    measure(10.0);
    auto_ppm_tick(&dev, 0, 110.0);
    EXPECT_EQ(fake_correction, 0) << "inside the interval";
    auto_ppm_tick(&dev, 0, 130.0);
    EXPECT_EQ(fake_correction, 5);
}

TEST_F(AutoPpmTickTest, never_exceeds_the_limit_even_when_the_dongle_is_out_of_spec) {
    ap->params.max_ppm = 100;
    ap->params.max_step = 100;
    fake_correction = 95;
    for (int i = 0; i < 5; i++) {
        measure(140.0);  // implies 140 ppm: rejected as implausible
        tick();
        EXPECT_LE(fake_correction, 100);
    }
    EXPECT_EQ(fake_correction, 95);

    for (int i = 0; i < 5; i++) {
        measure(99.6);  // plausible, but rounds past the limit
        tick();
        EXPECT_LE(fake_correction, 100);
    }
    EXPECT_EQ(fake_correction, 100);
}

TEST_F(AutoPpmTickTest, divergence_reverts_and_disables) {
    ap->state.base_correction = 3;
    fake_correction = 3;
    double truth = 20.0;
    for (int i = 0; i < 10 && !ap->tripped; i++) {
        int const before = fake_set_count;
        measure(truth);
        tick();
        if (fake_set_count != before) {
            truth += 10.0;  // the error runs away faster than each step closes it
        }
    }
    EXPECT_TRUE(ap->tripped);
    EXPECT_EQ(fake_correction, 3);
    EXPECT_FALSE(auto_ppm_active(&dev));

    int const count = fake_set_count;
    measure(5.0);
    tick();
    EXPECT_EQ(fake_set_count, count) << "a tripped device is left alone";
}

TEST_F(AutoPpmTickTest, disabled_device_is_left_alone) {
    ap->enabled = false;
    measure(10.0);
    tick();
    EXPECT_EQ(fake_set_count, 0);
}

TEST_F(AutoPpmTickTest, removed_and_disabled_channels_do_not_vote) {
    channels[0].removed = true;
    channels[1].enabled = false;
    measure(10.0);
    vector<AutoPpmEstimate> e = auto_ppm_collect_estimates(&dev);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].frequency, freqs[2].frequency);
}

TEST_F(AutoPpmTickTest, failed_set_correction_is_not_counted) {
    fake_set_fails = true;
    measure(10.0);
    tick();
    measure(10.0);
    tick();
    EXPECT_EQ(ap->last.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(ap->adjustments, 0u);
    EXPECT_FALSE(ap->state.awaiting_check);
}

TEST_F(AutoPpmTickTest, reload_rearms_and_reports_whether_to_apply_config_correction) {
    ap->tripped = true;
    ap->state.base_correction = 3;
    vector<string> changes;

    EXPECT_FALSE(auto_ppm_reload(&dev, true, 3, &changes)) << "unchanged correction stays with auto_ppm";
    EXPECT_FALSE(ap->tripped);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], "auto_ppm re-armed");

    changes.clear();
    EXPECT_TRUE(auto_ppm_reload(&dev, true, 8, &changes)) << "a changed correction is the new base";
    EXPECT_EQ(ap->state.base_correction, 8);
    EXPECT_TRUE(changes.empty());

    EXPECT_TRUE(auto_ppm_reload(&dev, false, 8, &changes)) << "with auto_ppm off the config value applies";
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], "auto_ppm -> false");
}

TEST_F(AutoPpmTickTest, manual_correction_outside_the_limit_is_refused) {
    ap->params.max_ppm = 50;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, 50));
    EXPECT_FALSE(auto_ppm_correction_allowed(&dev, -51));
    ap->enabled = false;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, -51));
    dev.auto_ppm = nullptr;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, 500));
    dev.auto_ppm = ap;
}

TEST_F(AutoPpmTickTest, status_json) {
    EXPECT_NE(auto_ppm_status_json(&dev, now).find("\"residual_ppm\":null"), string::npos);
    measure(2.0);
    tick();
    EXPECT_NE(auto_ppm_status_json(&dev, now).find("\"pending_count\":1"), string::npos);
    measure(2.0);
    tick();
    string const json = auto_ppm_status_json(&dev, now + 12.0);
    EXPECT_NE(json.find("\"enabled\":true"), string::npos) << json;
    EXPECT_NE(json.find("\"residual_ppm\":2.00"), string::npos) << json;
    EXPECT_NE(json.find("\"contributing\":3"), string::npos) << json;
    EXPECT_NE(json.find("\"adjustments\":1"), string::npos) << json;
    EXPECT_NE(json.find("\"last_adjust_age_s\":12"), string::npos) << json;
    EXPECT_NE(json.find("\"max_ppm\":100"), string::npos) << json;
    EXPECT_NE(json.find("\"disabled_reason\":null"), string::npos) << json;
    EXPECT_NE(json.find("\"min_voters\":1"), string::npos) << json;
    EXPECT_NE(json.find("\"deadband_hz\":100"), string::npos) << json;
    EXPECT_NE(json.find("\"deadband_ppm\":1.00"), string::npos) << json;
    EXPECT_NE(json.find("\"pending_count\":0"), string::npos) << json;
    EXPECT_NE(json.find("\"converged\":false"), string::npos) << json;

    dev.auto_ppm = nullptr;
    EXPECT_EQ(auto_ppm_status_json(&dev, now), "null");
    dev.auto_ppm = ap;
}
