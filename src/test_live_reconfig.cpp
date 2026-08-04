/*
 * test_live_reconfig.cpp
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

#include <thread>

#include "live_reconfig.h"
#include "rtl_airband.h"

using namespace std;

// live_reconfig.cpp's reconnect_channel_outputs() references these as extern; normally defined
// in output.cpp/pulse.cpp, neither of which can be linked here for the same reasons documented
// in test_mixer.cpp. Not exercised by the tests below - all test fixtures use output_count=0,
// so these are only needed to satisfy the linker.
void shout_setup(icecast_data*, mix_modes) {}
#ifdef WITH_PULSEAUDIO
void pulse_init() {}
int pulse_setup(pulse_data*, mix_modes) {
    return 0;
}
#endif /* WITH_PULSEAUDIO */
#ifdef NFM
// config.cpp's parse_devices() references this as extern under NFM; normally defined in
// rtl_airband.cpp, which can't be linked here for the same reasons documented above. None of the
// tests below call parse_devices() itself (only parse_channel()/compute_and_apply_diff(), which
// don't need it) - only needed to satisfy the linker.
float alpha = 0.0f;
#endif /* NFM */
// try_append_channels() (via this file) calls this as the "make a freshly appended channel's
// outputs actually ready to encode/send" step; normally defined in rtl_airband.cpp (calls
// airlame_init() from output.cpp), neither of which can be linked here for the same reasons
// documented above. The ChannelAppendTest cases below test the append/publish logic itself, not
// real LAME/icecast/udp connection setup - that's exercised by system_tests/tests/
// test_channel_add.py against a real binary instead. Always "succeeds" here so the append logic
// under test isn't gated on something this stub can't meaningfully do.
bool init_output(channel_t*, output_t*) {
    return true;
}

class LiveReconfigTest : public TestBaseClass {};

TEST_F(LiveReconfigTest, compute_channel_bin_matches_hand_computed_value) {
    // sample_rate=2000000, fft_size=2048 -> bin width = 976.5625 Hz (truncated to 976 by the
    // integer division baked into the original formula, see live_reconfig.cpp's comment)
    int const sample_rate = 2000000;
    size_t const num_bins = 2048;
    int const centerfreq = 100000000;
    int const channel_freq = 100050000;  // +50kHz from center

    size_t bin = compute_channel_bin(channel_freq, centerfreq, sample_rate, num_bins);

    // bin_width = sample_rate / fft_size = 976 (integer division)
    // bin = ceil((channel_freq + sample_rate - centerfreq) / (double)bin_width - 1.0) % fft_size
    //     = ceil((100050000 + 2000000 - 100000000) / 976.0 - 1.0) % 2048
    //     = ceil(2050000 / 976.0 - 1.0) % 2048
    double const bin_width = (double)(sample_rate / (int)num_bins);
    size_t const expected = (size_t)ceil((channel_freq + sample_rate - centerfreq) / bin_width - 1.0) % num_bins;
    EXPECT_EQ(bin, expected);
}

TEST_F(LiveReconfigTest, compute_channel_bin_at_center_frequency) {
    // A channel exactly at the tuned center should land near bin fft_size-1 (the formula's -1.0
    // term), not bin 0 - this pins the "off by one" convention rather than deriving it.
    int const sample_rate = 2000000;
    size_t const num_bins = 2048;
    int const centerfreq = 100000000;

    size_t bin = compute_channel_bin(centerfreq, centerfreq, sample_rate, num_bins);
    double const bin_width = (double)(sample_rate / (int)num_bins);
    size_t const expected = (size_t)ceil(sample_rate / bin_width - 1.0) % num_bins;
    EXPECT_EQ(bin, expected);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_is_zero_when_channel_equals_center) {
    // No downmix needed when the channel sits exactly on the tuned center frequency.
    uint32_t dphi = compute_channel_dm_dphi(100000000, 100000000, 2000000);
    EXPECT_EQ(dphi, 0u);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_is_deterministic) {
    // Same inputs must always produce the same output - this is the property the live-retune
    // path depends on to stay in sync with config.cpp's parse-time computation.
    uint32_t a = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    uint32_t b = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    EXPECT_EQ(a, b);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_sign_flips_with_offset_direction) {
    // A channel above center and the mirror-image channel below center should downmix in
    // opposite directions.
    uint32_t above = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    uint32_t below = compute_channel_dm_dphi(99950000, 100000000, 2000000);
    EXPECT_NE(above, below);
}

TEST_F(LiveReconfigTest, device_request_retune_accepts_r_multichannel) {
    device_t dev = {};
    dev.mode = R_MULTICHANNEL;
    dev.pending_centerfreq_request = -1;

    EXPECT_TRUE(device_request_retune(&dev, 123456789));
    EXPECT_EQ(dev.pending_centerfreq_request.load(), 123456789);
}

TEST_F(LiveReconfigTest, device_request_retune_rejects_r_scan) {
    // R_SCAN devices retune via their own controller_thread's fixed-offset scheme; this path
    // must never fight it.
    device_t dev = {};
    dev.mode = R_SCAN;
    dev.pending_centerfreq_request = -1;

    EXPECT_FALSE(device_request_retune(&dev, 123456789));
    EXPECT_EQ(dev.pending_centerfreq_request.load(), -1);
}

TEST_F(LiveReconfigTest, channel_apply_disable_sets_enabled_false) {
    // channel_apply_disable()/channel_apply_enable() are the apply-side halves, exercised
    // directly here as pure functions (see channel_t::pending_enable_request's comment,
    // rtl_airband.h, for why these are only safe to call for real from the output thread).
    channel_t channel = {};
    channel.enabled = true;
    channel.output_count = 0;

    channel_apply_disable(&channel);
    EXPECT_FALSE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_apply_enable_sets_enabled_true) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;

    channel_apply_enable(&channel);
    EXPECT_TRUE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_request_enable_posts_pending_request) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    // A short timeout since nothing consumes the request in this unit test - just confirming
    // the request/apply split's request side does its one job (post the value) without
    // touching `enabled` itself.
    channel_request_enable(&channel, /*timeout_us=*/1000);
    EXPECT_EQ(channel.pending_enable_request.load(), 1);
    EXPECT_FALSE(channel.enabled.load());  // not applied - no output thread consumed it here
}

TEST_F(LiveReconfigTest, channel_request_disable_posts_pending_request) {
    channel_t channel = {};
    channel.enabled = true;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    channel_request_disable(&channel, /*timeout_us=*/1000);
    EXPECT_EQ(channel.pending_enable_request.load(), 0);
    EXPECT_TRUE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_request_confirms_when_consumed_promptly) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    // Simulate the output thread consuming the request concurrently.
    std::thread consumer([&channel]() {
        while (channel.pending_enable_request.load(std::memory_order_acquire) == -1) {
            std::this_thread::yield();
        }
        channel_apply_enable(&channel);
        channel.pending_enable_request.store(-1, std::memory_order_release);
    });

    bool confirmed = channel_request_enable(&channel, /*timeout_us=*/500000);
    consumer.join();

    EXPECT_TRUE(confirmed);
    EXPECT_TRUE(channel.enabled.load());
}

class ConfigSnapshotTest : public TestBaseClass {
   protected:
    std::string write_config(const std::string& contents) {
        std::string path = temp_dir + "/test.conf";
        FILE* f = fopen(path.c_str(), "w");
        fwrite(contents.data(), 1, contents.size(), f);
        fclose(f);
        return path;
    }
};

TEST_F(ConfigSnapshotTest, parses_basic_multichannel_device) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  sample_rate = 2048000;
  channels: ( { freq = 120.000000; enabled = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].type, "rtlsdr");
    EXPECT_EQ(snapshot.devices[0].mode, R_MULTICHANNEL);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 120000000);
    EXPECT_EQ(snapshot.devices[0].sample_rate, 2048000);
    ASSERT_TRUE(snapshot.devices[0].has_gain);
    EXPECT_FLOAT_EQ(snapshot.devices[0].gain, 30.0f);
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
    EXPECT_TRUE(snapshot.devices[0].channel_enabled[0]);
}

TEST_F(ConfigSnapshotTest, channel_enabled_defaults_true_when_absent) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  channels: ( { freq = 120.000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
    EXPECT_TRUE(snapshot.devices[0].channel_enabled[0]);
}

TEST_F(ConfigSnapshotTest, skips_disabled_devices_and_channels) {
    std::string path = write_config(R"(
devices:
(
{
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  disable = true;
  channels: ( { freq = 120.000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
},
{
  type = "rtlsdr";
  index = 1;
  gain = 30;
  centerfreq = 121000000;
  channels: (
    { freq = 121.000000; disable = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 121.100000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
}
);
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    // Only the non-disabled device shows up, matching parse_devices()'s own indexing convention.
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 121000000);
    // Only the non-disabled channel shows up within it.
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
}

TEST_F(ConfigSnapshotTest, r_scan_mode_has_no_centerfreq_and_string_gain_is_not_numeric) {
    std::string path = write_config(R"(
devices:
({
  type = "soapysdr";
  device_string = "driver=fake";
  mode = "scan";
  gain = "LNA=10,VGA=5";
  channels: ( { freqs = ( 120.000000, 121.000000 ); outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].mode, R_SCAN);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 0);  // not captured for R_SCAN
    EXPECT_FALSE(snapshot.devices[0].has_gain);    // string form isn't a single diffable number
}

TEST_F(ConfigSnapshotTest, mixer_enabled_keyword_and_disable_keyword) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  channels: ( { freq = 120.000000; outputs: ( { type = "mixer"; name = "mix1"; } ); } );
});
mixers: {
  mix1: { enabled = false; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); };
  mix2: { disable = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); };
};
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    // mix2 is "disable"d at parse time - never shows up, same as a disabled device/channel.
    ASSERT_EQ(snapshot.mixers.size(), 1u);
    EXPECT_EQ(snapshot.mixers[0].name, "mix1");
    EXPECT_FALSE(snapshot.mixers[0].enabled);
}

TEST_F(ConfigSnapshotTest, reports_error_on_missing_file) {
    ConfigSnapshot snapshot;
    std::string error;
    EXPECT_FALSE(parse_config_snapshot(temp_dir + "/does_not_exist.conf", &snapshot, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigSnapshotTest, reports_error_on_malformed_config) {
    std::string path = write_config("devices: ( { this is not valid libconfig");
    ConfigSnapshot snapshot;
    std::string error;
    EXPECT_FALSE(parse_config_snapshot(path, &snapshot, &error));
    EXPECT_FALSE(error.empty());
}

class DiffApplyTest : public LiveReconfigTest {
   protected:
    void SetUp() override {
        LiveReconfigTest::SetUp();
        devices = nullptr;
        device_count = 0;
        mixers = nullptr;
        mixer_count = 0;
    }
};

TEST_F(DiffApplyTest, device_count_mismatch_is_requires_restart_and_applies_nothing_else) {
    device_t dev = {};
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;  // zero devices - mismatches live device_count of 1
    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("device count changed"), std::string::npos);
    EXPECT_TRUE(result.applied.empty());
}

TEST_F(DiffApplyTest, channel_enabled_diff_calls_channel_enable_disable) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    // channel_t has non-copyable std::atomic members, so construct the array in place rather
    // than copy-initializing from locals.
    channel_t chans[2] = {};
    chans[0].enabled = true;
    chans[1].enabled = false;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 2;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 2;
    dsnap.centerfreq = 120000000;  // unchanged - no retune expected
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    dsnap.channel_enabled = {false, true};  // flip both
    snapshot.devices.push_back(dsnap);

    // compute_and_apply_diff() only posts requests (see live_reconfig.h's comment on why - the
    // apply side must run on the owning output thread, not here) and then polls for
    // consumption; simulate that thread so this test stays fast and still exercises the full
    // request -> apply round trip rather than just the posting half.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            for (int k = 0; k < 2; k++) {
                int pending = chans[k].pending_enable_request.exchange(-1, std::memory_order_acq_rel);
                if (pending == 1) {
                    channel_apply_enable(&chans[k]);
                } else if (pending == 0) {
                    channel_apply_disable(&chans[k]);
                }
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_FALSE(dev.channels[0].enabled.load());
    EXPECT_TRUE(dev.channels[1].enabled.load());
    EXPECT_EQ(result.applied.size(), 2u);
    EXPECT_EQ(dev.pending_centerfreq_request.load(), -1);  // centerfreq unchanged, no request posted
}

TEST_F(DiffApplyTest, centerfreq_diff_posts_a_retune_request) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120050000;  // changed
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.pending_centerfreq_request.load(), 120050000);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("centerfreq"), std::string::npos);
}

TEST_F(DiffApplyTest, driver_type_change_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "soapysdr";  // different from live "rtlsdr"
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("driver type changed"), std::string::npos);
    EXPECT_TRUE(result.applied.empty());
}

TEST_F(DiffApplyTest, mode_change_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_SCAN;  // different from live R_MULTICHANNEL
    dsnap.channel_count = 0;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("mode changed"), std::string::npos);
}

TEST_F(DiffApplyTest, sample_rate_change_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2400000;  // changed
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("sample_rate changed"), std::string::npos);
}

TEST_F(DiffApplyTest, gain_not_supported_by_driver_is_silently_skipped_not_reported) {
    // set_gain == NULL (the rtlsdr driver leaves it unset in this fixture on purpose) -
    // input_set_gain() returns ENOTSUP, which reload_diff treats as "nothing to report", not a
    // failure worth flagging on every single reload.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_gain = nullptr;
    input.dev_data = &fake_dev_data;  // input_set_gain() asserts this non-null before the set_gain-null check

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = true;
    dsnap.gain = 30.0f;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

int fake_set_gain_ok(input_t* const, float const) {
    return 0;
}

TEST_F(DiffApplyTest, gain_applied_via_input_set_gain_is_reported) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_gain = &fake_set_gain_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = true;
    dsnap.gain = 30.0f;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("gain"), std::string::npos);
}

TEST_F(DiffApplyTest, mixer_count_mismatch_is_requires_restart) {
    ConfigSnapshot snapshot;
    snapshot.mixers.push_back({"mix1", true});
    mixer_count = 0;  // live has none

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("mixer count changed"), std::string::npos);
}

TEST_F(DiffApplyTest, mixer_enabled_diff_calls_mixer_enable_disable) {
    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixers = &mixer;
    mixer_count = 1;

    ConfigSnapshot snapshot;
    snapshot.mixers.push_back({"mix1", false});  // flip to disabled

    // Same rationale as the channel_enabled_diff test above - simulate the owning output thread
    // consuming the request so this stays fast and exercises the full round trip.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = mixer.pending_enable_request.exchange(-1, std::memory_order_acq_rel);
            if (pending == 1) {
                mixer_enable(&mixer);
            } else if (pending == 0) {
                mixer_disable(&mixer);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_FALSE(mixer.enabled);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("mix1"), std::string::npos);
}

// Dynamic channel add: compute_and_apply_diff() detecting/applying a config file's channel count
// growing for a device (a pure tail append, within dev->channel_capacity's pre-reserved
// headroom - see rtl_airband.h's comment on channel_capacity). Unlike the other DiffApplyTest
// cases above, these need a real raw_channels_setting/raw_channel_indices, which only
// parse_config_snapshot() populates from an actual file - hence write_config() here too.
class ChannelAppendTest : public DiffApplyTest {
   protected:
    std::string write_config(const std::string& contents) {
        std::string path = temp_dir + "/test.conf";
        FILE* f = fopen(path.c_str(), "w");
        fwrite(contents.data(), 1, contents.size(), f);
        fclose(f);
        return path;
    }
};

TEST_F(ChannelAppendTest, appends_one_new_channel_within_capacity) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    // index 0 is the already-live channel; index 1 is pre-reserved (zeroed) capacity, exactly as
    // parse_devices() would leave it after sizing for reserve_channels = 1.
    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; label = "new-chan"; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("added 1 channel"), std::string::npos);
    ASSERT_EQ(chans[1].freq_count, 1);
    EXPECT_EQ(chans[1].freqlist[0].frequency, 120050000);
    EXPECT_STREQ(chans[1].freqlist[0].label, "new-chan");
    EXPECT_EQ(chans[1].output_count, 1);
    // The existing (index 0) channel must be untouched.
    EXPECT_EQ(chans[0].output_count, 0);
}

TEST_F(ChannelAppendTest, appends_multiple_new_channels_within_capacity) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[3] = {};
    size_t bins[3] = {0, 0, 0};
    size_t base_bins[3] = {0, 0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.channel_capacity = 3;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "a"; } ); },
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "b"; } ); },
    { freq = 120100000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "c"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 3);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("added 3 channel"), std::string::npos);
    EXPECT_EQ(chans[0].freqlist[0].frequency, 120000000);
    EXPECT_EQ(chans[1].freqlist[0].frequency, 120050000);
    EXPECT_EQ(chans[2].freqlist[0].frequency, 120100000);
}

TEST_F(ChannelAppendTest, exceeding_reserved_capacity_is_requires_restart_and_applies_nothing) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[1] = {};
    chans[0].enabled = true;
    size_t bins[1] = {0};
    size_t base_bins[1] = {0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 1;  // no reserve_channels headroom
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);  // unchanged
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("reserve_channels"), std::string::npos);
}

TEST_F(ChannelAppendTest, malformed_new_channel_is_reported_and_does_not_crash_or_partially_apply) {
    // The critical safety-net test for the recoverable-error mechanism (logging.h/.cpp): a
    // startup-time config error here would call error() -> _Exit(1), taking the whole test
    // binary (and, in production, the whole running instance) down. This new channel has no
    // "outputs" block at all, which parse_channel() rejects the same way it always has.
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    DiffResult result = compute_and_apply_diff(snapshot);

    // Still running, count untouched, failure surfaced instead of a crash.
    EXPECT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("failed to parse"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
    // config_error_is_recoverable and cerr must both be restored, not left in the "diverted"
    // state, so a later real error() call (e.g. from a subsequent test) behaves normally.
    EXPECT_FALSE(config_error_is_recoverable);
}

TEST_F(ChannelAppendTest, r_scan_channel_count_change_is_requires_restart_not_append) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;

    channel_t chans[1] = {};
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_SCAN;
    dev.channel_count = 1;
    dev.channel_capacity = 1;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_SCAN;
    dsnap.channel_count = 2;  // shouldn't be reachable for a real R_SCAN config, but guarded anyway
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("R_SCAN"), std::string::npos);
}

TEST_F(ChannelAppendTest, channel_count_decrease_is_still_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;

    channel_t chans[2] = {};
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 2;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 1;  // decreased
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);  // unchanged - decrease is never attempted live
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("channel count changed"), std::string::npos);
}
