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

#include <chrono>
#include <thread>

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
// Only the O_MIXER branch of the real disable_channel_outputs() (src/output.cpp) is reproduced
// here - icecast/file/pulse teardown needs shout/lame/real file I/O this test binary doesn't
// link in, but the O_MIXER branch is pure mixer.cpp, which is linked. Without this, a test that
// live-removes a mixer-connected channel (test_live_reconfig.cpp) could never exercise the real
// mixer_disable_input()/permanent-reclaim path at all.
void disable_channel_outputs(channel_t* channel, bool permanent) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        output->enabled = false;
        if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_disable_input(mdata->mixer, mdata->input, permanent);
        }
    }
}

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

TEST_F(MixerTest, select_active_tag_input_none_active) {
    mixinput_t inputs[3] = {};
    EXPECT_EQ(mixer_select_active_tag_input(inputs, 3), -1);
}

TEST_F(MixerTest, select_active_tag_input_single_active) {
    mixinput_t inputs[3] = {};
    inputs[1].has_signal = true;
    EXPECT_EQ(mixer_select_active_tag_input(inputs, 3), 1);
}

TEST_F(MixerTest, select_active_tag_input_lowest_index_wins) {
    mixinput_t inputs[4] = {};
    inputs[2].has_signal = true;
    inputs[3].has_signal = true;
    EXPECT_EQ(mixer_select_active_tag_input(inputs, 4), 2);
}

TEST_F(MixerTest, select_active_tag_input_zero_input_count) {
    mixinput_t inputs[1] = {};
    inputs[0].has_signal = true;
    EXPECT_EQ(mixer_select_active_tag_input(inputs, 0), -1);
}

class MixerReadyTest : public MixerTest {
   protected:
    mixer_t mixer = {};
    mixinput_t inputs[3] = {};
    bool todo[3] = {true, true, true};
    bool mask[3] = {true, true, true};

    void SetUp() override {
        MixerTest::SetUp();
        mixer.inputs = inputs;
        mixer.inputs_todo = todo;
        mixer.input_mask = mask;
        mixer.input_count = 3;
    }
};

TEST_F(MixerReadyTest, waits_for_an_input_that_is_merely_late) {
    todo[0] = todo[1] = false;
    inputs[2].missed_batches = MIXER_STALE_BATCHES - 1;
    EXPECT_FALSE(mixer_ready_to_emit(&mixer));
    todo[2] = false;
    EXPECT_TRUE(mixer_ready_to_emit(&mixer));
}

TEST_F(MixerReadyTest, does_not_wait_for_an_input_that_stopped) {
    todo[0] = todo[1] = false;
    inputs[2].missed_batches = MIXER_STALE_BATCHES;
    EXPECT_TRUE(mixer_ready_to_emit(&mixer));
    todo[1] = true;  // another input is still due
    EXPECT_FALSE(mixer_ready_to_emit(&mixer));
}

TEST_F(MixerReadyTest, keeps_its_pace_when_every_input_stopped) {
    for (int i = 0; i < 3; i++) {
        inputs[i].missed_batches = MIXER_STALE_BATCHES;
    }
    EXPECT_FALSE(mixer_ready_to_emit(&mixer)) << "must fall back to the interval, not emit on every tick";
}

TEST_F(MixerReadyTest, ignores_masked_off_inputs) {
    todo[0] = todo[1] = false;
    mask[2] = false;
    EXPECT_TRUE(mixer_ready_to_emit(&mixer));
}

// Runs the real mixer_thread() in real time: a sender that never sends must not cost the input
// that does.
TEST_F(MixerTest, a_silent_input_does_not_slow_the_mixer_down) {
    mixer_t mixer = {};
    mixer.name = "m";
    mixer.interval = MIX_DIVISOR;
    mixer.channel.state = CH_DIRTY;
    mixers = &mixer;
    mixer_count = 1;
    mixer_capacity_finalized = false;
    int const live = mixer_connect_input(&mixer, 1.0f, 0.0f);
    ASSERT_NE(mixer_connect_input(&mixer, 1.0f, 0.0f), -1);

    Signal signal;
    do_exit = 0;
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, NULL, &mixer_thread, &signal), 0);
    vector<float> samples(WAVE_BATCH, 0.1f);
    size_t overruns_before = 0;
    int emitted = 0;
    auto const start = chrono::steady_clock::now();
    // 5 ms ticks and a batch every 125 ms: 2 s for the mixer to notice, then 6 s measured
    for (int tick = 0; tick < 1600; tick++) {
        this_thread::sleep_until(start + chrono::milliseconds(5 * tick));
        if (mixer.channel.state == CH_READY) {
            emitted += tick >= 400;
            mixer.channel.state = CH_DIRTY;
        }
        if (tick % 25 == 0) {
            mixer_put_samples(&mixer, live, samples.data(), true, WAVE_BATCH);
        }
        if (tick == 400) {
            overruns_before = mixer.inputs[live].input_overrun_count;
        }
    }
    do_exit = 1;
    pthread_join(thread, NULL);
    do_exit = 0;
    mixers = nullptr;
    mixer_count = 0;

    EXPECT_LE(mixer.inputs[live].input_overrun_count - overruns_before, 2u);
    EXPECT_GE(emitted, 45);  // of 48
}

// mixer_finalize_capacity() operates on the global mixers/mixer_count (like production code
// does, via main()), so each test points those globals at its own local mixer_t(s) and resets
// mixer_capacity_finalized - otherwise a prior test's finalize would leak into the next one, since
// it's a single process-wide flag (see mixer_capacity_finalized's declaration in rtl_airband.h for
// why it isn't a mixer.cpp file-static: this exact cross-test leak risk).
class MixerCapacityTest : public MixerTest {
   protected:
    void SetUp() override {
        MixerTest::SetUp();
        mixer_capacity_finalized = false;
        mixers = nullptr;
        mixer_count = 0;
    }
};

TEST_F(MixerCapacityTest, connect_input_grows_capacity_before_finalize) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    int idx0 = mixer_connect_input(&mixer, 1.0f, 0.0f);
    int idx1 = mixer_connect_input(&mixer, 1.0f, 0.0f);

    EXPECT_EQ(idx0, 0);
    EXPECT_EQ(idx1, 1);
    EXPECT_EQ(mixer.input_count.load(), 2);
    EXPECT_EQ(mixer.input_capacity, 2);
}

TEST_F(MixerCapacityTest, finalize_reserves_headroom_on_top_of_startup_inputs) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 3;

    mixer_connect_input(&mixer, 1.0f, 0.0f);
    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    EXPECT_EQ(mixer.input_capacity, 5);  // 2 connected at "startup" + 3 reserved
    EXPECT_TRUE(mixer_capacity_finalized);
}

TEST_F(MixerCapacityTest, post_finalize_append_within_headroom_does_not_move_backing_array) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 2;

    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    mixinput_t* inputs_before = mixer.inputs;
    bool* todo_before = mixer.inputs_todo;
    bool* mask_before = mixer.input_mask;

    // Simulates a dynamic_reload live channel-append connecting a new mixer input after startup.
    int idx = mixer_connect_input(&mixer, 1.0f, 0.0f);

    EXPECT_EQ(idx, 1);
    EXPECT_EQ(mixer.input_count.load(), 2);
    // Proves no realloc happened: mixer_thread()/mixer_put_samples() on another thread would
    // still be dereferencing a valid, unmoved pointer.
    EXPECT_EQ(mixer.inputs, inputs_before);
    EXPECT_EQ(mixer.inputs_todo, todo_before);
    EXPECT_EQ(mixer.input_mask, mask_before);
}

TEST_F(MixerCapacityTest, post_finalize_append_beyond_headroom_is_rejected_cleanly) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    int result = mixer_connect_input(&mixer, 1.0f, 0.0f);

    EXPECT_EQ(result, -1);
    EXPECT_EQ(mixer.input_count.load(), 1);  // unchanged - rejected, not partially applied
    ASSERT_NE(mixer_get_error(), nullptr);
    EXPECT_NE(std::string(mixer_get_error()).find("capacity"), std::string::npos);
}

TEST_F(MixerCapacityTest, finalize_on_empty_mixer_with_no_reserve_leaves_inputs_null) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();  // must not crash on the realloc(ptr, 0) edge case

    EXPECT_EQ(mixer.inputs, nullptr);
    EXPECT_EQ(mixer.input_capacity, 0);
    EXPECT_TRUE(mixer_capacity_finalized);
}

TEST_F(MixerCapacityTest, finalize_on_empty_mixer_with_reserve_allocates_fresh_zeroed_array) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 2;

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    ASSERT_NE(mixer.inputs, nullptr);
    EXPECT_EQ(mixer.input_capacity, 2);
    EXPECT_FALSE(mixer.input_mask[0]);
    EXPECT_FALSE(mixer.input_mask[1]);

    // A live-append-only mixer (no inputs connected at startup) still accepts input up to its
    // reserved headroom.
    int idx = mixer_connect_input(&mixer, 1.0f, 0.0f);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(mixer.input_count.load(), 1);
}

// item 39: mixer_disable_input(..., permanent=true) tombstones the slot so mixer_connect_input()
// can reclaim it, instead of every live channel edit/removal permanently burning a fresh
// reserve_inputs slot (item 30's documented caveat).
TEST_F(MixerCapacityTest, disable_input_permanent_marks_removed_and_masks_without_disturbing_others) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    mixer_connect_input(&mixer, 1.0f, 0.0f);
    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixer_disable_input(&mixer, 0, /*permanent=*/true);

    EXPECT_FALSE(mixer.input_mask[0]);
    EXPECT_TRUE(mixer.input_removed[0]);
    // The other input is untouched - still active and not tombstoned.
    EXPECT_TRUE(mixer.input_mask[1]);
    EXPECT_FALSE(mixer.input_removed[1]);
    EXPECT_TRUE(mixer.enabled);  // one input still alive
}

TEST_F(MixerCapacityTest, disable_input_non_permanent_does_not_tombstone_the_slot) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;
    mixer.name = "mix1";

    mixer_connect_input(&mixer, 1.0f, 0.0f);
    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixer_disable_input(&mixer, 0);  // ordinary temporary disable - permanent defaults to false

    EXPECT_FALSE(mixer.input_mask[0]);
    EXPECT_FALSE(mixer.input_removed[0]);
}

TEST_F(MixerCapacityTest, connect_input_reuses_a_permanently_removed_slot_pre_finalize) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    int idx0 = mixer_connect_input(&mixer, 1.0f, 0.0f);
    mixer_connect_input(&mixer, 1.0f, 0.0f);
    EXPECT_EQ(mixer.input_capacity, 2);

    mixer_disable_input(&mixer, idx0, /*permanent=*/true);

    int idx_reused = mixer_connect_input(&mixer, 2.5f, 0.3f);

    EXPECT_EQ(idx_reused, idx0);  // slot 0 reclaimed, not a new slot
    EXPECT_EQ(mixer.input_count.load(), 2);
    EXPECT_EQ(mixer.input_capacity, 2);  // no growth needed
    EXPECT_TRUE(mixer.input_mask[0]);
    EXPECT_FALSE(mixer.input_removed[0]);
    EXPECT_FLOAT_EQ(mixer.inputs[0].ampfactor, 2.5f);
    EXPECT_FLOAT_EQ(mixer.inputs[0].ampl, fminf(1.0f, 1.0f - 0.3f));
    EXPECT_FALSE(mixer.inputs[0].ready);
    EXPECT_FALSE(mixer.inputs[0].has_signal);
}

TEST_F(MixerCapacityTest, connect_input_reuses_a_removed_slot_post_finalize_without_exceeding_capacity) {
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    mixer_connect_input(&mixer, 1.0f, 0.0f);

    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();
    ASSERT_EQ(mixer.input_capacity, 1);  // no reserve headroom

    mixer_disable_input(&mixer, 0, /*permanent=*/true);

    // Post-finalize, growth is rejected - but reuse doesn't grow, so this must still succeed.
    int idx = mixer_connect_input(&mixer, 1.0f, 0.0f);

    EXPECT_EQ(idx, 0);
    EXPECT_EQ(mixer.input_count.load(), 1);
    EXPECT_EQ(mixer.input_capacity, 1);
    EXPECT_TRUE(mixer.input_mask[0]);
    EXPECT_FALSE(mixer.input_removed[0]);
}

TEST_F(MixerCapacityTest, connect_input_reused_slot_still_works_through_mixer_put_samples) {
    // Confirms the reused slot's mutex (never re-initialized on reuse - see mixer_connect_input()'s
    // comment) is still a valid, usable mutex, not just that the bookkeeping fields look right.
    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;

    int idx0 = mixer_connect_input(&mixer, 1.0f, 0.0f);
    mixer_disable_input(&mixer, idx0, /*permanent=*/true);
    int idx_reused = mixer_connect_input(&mixer, 1.0f, 0.0f);
    ASSERT_EQ(idx_reused, idx0);

    float samples[WAVE_LEN] = {0};
    samples[0] = 0.42f;
    mixer_put_samples(&mixer, idx_reused, samples, /*has_signal=*/true, WAVE_LEN);

    EXPECT_TRUE(mixer.inputs[idx_reused].ready);
    EXPECT_FLOAT_EQ(mixer.inputs[idx_reused].wavein[0], 0.42f);
}
