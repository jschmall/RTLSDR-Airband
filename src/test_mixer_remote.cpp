/*
 * test_mixer_remote.cpp
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

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <libconfig.h++>
#include <sstream>
#include <string>

#include "logging.h"  // ConfigApplyError, config_error_is_recoverable
#include "mixer_remote.h"
#include "mixer_remote_wire.h"
#include "rtl_airband.h"

using namespace std;

// mixer_remote_send_init()/mixer_remote_send() are exercised against a real AF_UNIX SOCK_DGRAM
// socket bound at a path inside TestBaseClass's per-test temp_dir - unlike udp_stream, this
// transport is AF_UNIX, so a real bound filesystem path (not just a socketpair()) is what
// production actually uses, and what "receiver isn't listening yet" (item 2's rationale for
// never connect()-ing) needs to be tested against.
class MixerRemoteSendTest : public TestBaseClass {
   protected:
    void SetUp(void) override {
        TestBaseClass::SetUp();
        memset(&rdata, 0, sizeof(rdata));
        listen_socket = -1;
    }

    void TearDown(void) override {
        mixer_remote_send_shutdown(&rdata);
        free(const_cast<char*>(rdata.dest_path));
        if (listen_socket != -1) {
            close(listen_socket);
        }
        TestBaseClass::TearDown();
    }

    // binds a real AF_UNIX SOCK_DGRAM socket at temp_dir/name and returns its path
    string bind_listener(const string& name) {
        listen_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (listen_socket == -1) {
            return "";
        }
        struct timeval timeout = {1, 0};
        if (setsockopt(listen_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            return "";
        }
        string path = temp_dir + "/" + name;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (bind(listen_socket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            return "";
        }
        return path;
    }

    // returns the number of bytes received, or -1 on timeout/error
    ssize_t recv_packet(void* buf, size_t buf_len) { return recvfrom(listen_socket, buf, buf_len, 0, NULL, NULL); }

    int listen_socket;
    mixer_remote_send_data rdata;
};

TEST_F(MixerRemoteSendTest, sends_a_decodable_packet_to_a_real_listener) {
    string path = bind_listener("mix1.sock");
    ASSERT_FALSE(path.empty());

    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 3;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 4));

    float samples[4] = {1.0f, -0.5f, 0.25f, -1.0f};
    mixer_remote_send(&rdata, samples, 4, true);

    uint8_t buf[sizeof(mixer_remote_packet_header) + 4 * sizeof(float) + 100];
    ssize_t received = recv_packet(buf, sizeof(buf));
    ASSERT_EQ(received, (ssize_t)(sizeof(mixer_remote_packet_header) + 4 * sizeof(float)));

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, (size_t)received, &hdr));
    EXPECT_EQ(hdr.stream_id, 3u);
    EXPECT_EQ(hdr.sample_rate, (uint32_t)WAVE_RATE);
    EXPECT_EQ(hdr.seq, 0u);
    EXPECT_EQ(hdr.flags & MIXER_REMOTE_FLAG_HAS_SIGNAL, MIXER_REMOTE_FLAG_HAS_SIGNAL);

    float decoded[4];
    ASSERT_EQ(mixer_remote_decode_payload(buf, (size_t)received, hdr, decoded, 4), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(decoded[i], samples[i]);
    }
    EXPECT_EQ(rdata.dropped_packet_count, 0u);
}

TEST_F(MixerRemoteSendTest, seq_increments_each_send) {
    string path = bind_listener("mix1.sock");
    ASSERT_FALSE(path.empty());
    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 1;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 2));

    float samples[2] = {0.1f, 0.2f};
    mixer_remote_send(&rdata, samples, 2, true);
    mixer_remote_send(&rdata, samples, 2, true);

    uint8_t buf[sizeof(mixer_remote_packet_header) + 2 * sizeof(float)];
    ASSERT_GT(recv_packet(buf, sizeof(buf)), 0);
    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
    EXPECT_EQ(hdr.seq, 0u);

    ASSERT_GT(recv_packet(buf, sizeof(buf)), 0);
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
    EXPECT_EQ(hdr.seq, 1u);
}

TEST_F(MixerRemoteSendTest, has_signal_false_is_reflected_in_the_flag) {
    string path = bind_listener("mix1.sock");
    ASSERT_FALSE(path.empty());
    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 1;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 1));

    float samples[1] = {0.0f};
    mixer_remote_send(&rdata, samples, 1, false);

    uint8_t buf[sizeof(mixer_remote_packet_header) + sizeof(float)];
    ASSERT_GT(recv_packet(buf, sizeof(buf)), 0);
    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
    EXPECT_EQ(hdr.flags & MIXER_REMOTE_FLAG_HAS_SIGNAL, 0u);
}

// The core "nothing listening yet is safe/inert" contract (see mixer_remote_send_init()'s
// comment for why this transport never connect()s): init must still succeed, and send() must
// not crash, when nothing is bound at dest_path at all.
TEST_F(MixerRemoteSendTest, no_listener_does_not_crash_and_counts_a_drop) {
    string path = temp_dir + "/nobody_listening.sock";
    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 1;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 4));

    float samples[4] = {1, 2, 3, 4};
    mixer_remote_send(&rdata, samples, 4, true);

    EXPECT_EQ(rdata.dropped_packet_count, 1u);
}

TEST_F(MixerRemoteSendTest, dest_path_too_long_fails_init) {
    string long_path(200, 'x');  // longer than sizeof(sockaddr_un::sun_path)
    rdata.dest_path = strdup(long_path.c_str());
    rdata.stream_id = 1;
    EXPECT_FALSE(mixer_remote_send_init(&rdata, 4));
    EXPECT_EQ(rdata.send_socket, -1);
}

TEST_F(MixerRemoteSendTest, len_exceeding_init_capacity_is_dropped_not_overflowed) {
    string path = bind_listener("mix1.sock");
    ASSERT_FALSE(path.empty());
    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 1;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 2));  // buffer sized for 2 samples

    float samples[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    mixer_remote_send(&rdata, samples, 8, true);  // more samples than the buffer was sized for

    EXPECT_EQ(rdata.dropped_packet_count, 1u);

    // nothing should have been sent
    uint8_t buf[64];
    EXPECT_EQ(recv_packet(buf, sizeof(buf)), -1);
}

TEST_F(MixerRemoteSendTest, send_before_init_is_a_safe_no_op) {
    rdata.send_socket = -1;
    float samples[1] = {1.0f};
    mixer_remote_send(&rdata, samples, 1, true);  // must not crash
    EXPECT_EQ(rdata.dropped_packet_count, 0u);
}

TEST_F(MixerRemoteSendTest, shutdown_before_init_is_a_safe_no_op) {
    rdata.send_socket = -1;
    rdata.send_buf = NULL;
    mixer_remote_send_shutdown(&rdata);  // must not crash
}

TEST_F(MixerRemoteSendTest, shutdown_is_idempotent) {
    string path = bind_listener("mix1.sock");
    ASSERT_FALSE(path.empty());
    rdata.dest_path = strdup(path.c_str());
    rdata.stream_id = 1;
    ASSERT_TRUE(mixer_remote_send_init(&rdata, 1));

    mixer_remote_send_shutdown(&rdata);
    EXPECT_EQ(rdata.send_socket, -1);
    mixer_remote_send_shutdown(&rdata);  // must not double-close or double-free
}

// ---- receive side ----

// mixer_remote_listeners is a process-wide registry (see mixer_remote.h's comment for why it's
// a plain global, not a file-static) - reset it before/after every test in this fixture so
// registrations from one test can never leak into another, and stop any listener thread a test
// may have started.
class MixerRemoteRecvTest : public TestBaseClass {
   protected:
    void SetUp(void) override {
        TestBaseClass::SetUp();
        mixer_remote_reset_registry();
    }

    void TearDown(void) override {
        mixer_remote_recv_shutdown();
        mixer_remote_reset_registry();
        TestBaseClass::TearDown();
    }
};

TEST_F(MixerRemoteRecvTest, get_or_create_listener_returns_same_pointer_for_same_path) {
    mixer_remote_listener_t* a = mixer_remote_get_or_create_listener("/tmp/a.sock");
    mixer_remote_listener_t* b = mixer_remote_get_or_create_listener("/tmp/a.sock");
    mixer_remote_listener_t* c = mixer_remote_get_or_create_listener("/tmp/b.sock");

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(mixer_remote_listeners.size(), 2u);
}

TEST_F(MixerRemoteRecvTest, register_route_rejects_duplicate_stream_id_on_same_listener) {
    mixer_t mixer = {};
    mixer_remote_listener_t* listener = mixer_remote_get_or_create_listener("/tmp/a.sock");

    EXPECT_TRUE(mixer_remote_register_route(listener, 1, &mixer, 0));
    EXPECT_FALSE(mixer_remote_register_route(listener, 1, &mixer, 1));  // duplicate stream_id
    EXPECT_TRUE(mixer_remote_register_route(listener, 2, &mixer, 1));   // distinct stream_id is fine
    EXPECT_EQ(listener->routes.size(), 2u);
}

class MixerRemoteDispatchTest : public MixerRemoteRecvTest {
   protected:
    void SetUp(void) override {
        MixerRemoteRecvTest::SetUp();
        // mixer's atomic members are given real values by mixer_t's in-class initializer
        // (mixer_t mixer{}; below) when this fixture object is constructed - assigning a whole
        // mixer_t here (`mixer = {};`) would instead need mixer_t's copy-assignment operator,
        // which is implicitly deleted because it contains std::atomic members.
        mixer.input_count = 0;
        mixer.input_capacity = 0;
        mixer.reserve_inputs = 0;
        slot = mixer_connect_input(&mixer, 1.0f, 0.0f);
        listener = mixer_remote_get_or_create_listener("/tmp/dispatch.sock");
        mixer_remote_register_route(listener, /*stream_id=*/7, &mixer, slot);
    }

    // encodes a valid packet for stream_id 7 into buf, returns its length
    size_t make_packet(uint8_t* buf, size_t buf_len, uint64_t seq, float fill = 0.5f, uint32_t sample_rate = WAVE_RATE, uint32_t sample_count = WAVE_BATCH) {
        float samples[WAVE_BATCH];
        for (int i = 0; i < WAVE_BATCH; ++i) {
            samples[i] = fill;
        }
        return mixer_remote_encode_packet(sample_rate, 7, seq, true, samples, sample_count, buf, buf_len);
    }

    mixer_t mixer{};
    int slot;
    mixer_remote_listener_t* listener;
};

TEST_F(MixerRemoteDispatchTest, valid_packet_is_applied_to_the_mixer_slot) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    size_t len = make_packet(buf, sizeof(buf), 0, 0.75f);
    ASSERT_GT(len, 0u);

    mixer_remote_dispatch_packet(listener, buf, len, getuid());

    EXPECT_TRUE(mixer.inputs[slot].ready);
    EXPECT_TRUE(mixer.inputs[slot].has_signal);
    EXPECT_FLOAT_EQ(mixer.inputs[slot].wavein[0], 0.75f);
    EXPECT_EQ(listener->routes[0].last_packet_time, time(NULL));
}

TEST_F(MixerRemoteDispatchTest, wrong_sender_uid_is_rejected_and_mixer_is_untouched) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    size_t len = make_packet(buf, sizeof(buf), 0);
    ASSERT_GT(len, 0u);

    mixer_remote_dispatch_packet(listener, buf, len, getuid() + 1);  // never our own uid

    EXPECT_EQ(listener->rejected_uid_count, 1u);
    EXPECT_FALSE(mixer.inputs[slot].ready);
}

TEST_F(MixerRemoteDispatchTest, malformed_header_is_counted_at_the_listener_not_a_route) {
    uint8_t buf[sizeof(mixer_remote_packet_header)];
    memset(buf, 0xFF, sizeof(buf));  // garbage - fails magic check

    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());

    EXPECT_EQ(listener->malformed_header_count, 1u);
    EXPECT_FALSE(mixer.inputs[slot].ready);
}

TEST_F(MixerRemoteDispatchTest, unknown_stream_id_is_rejected) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    float samples[WAVE_BATCH] = {0};
    size_t len = mixer_remote_encode_packet(WAVE_RATE, /*stream_id=*/999, 0, true, samples, WAVE_BATCH, buf, sizeof(buf));
    ASSERT_GT(len, 0u);

    mixer_remote_dispatch_packet(listener, buf, len, getuid());

    EXPECT_EQ(listener->unknown_stream_count, 1u);
    EXPECT_FALSE(mixer.inputs[slot].ready);
}

TEST_F(MixerRemoteDispatchTest, rate_mismatch_is_rejected_and_counted_on_the_route) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    size_t len = make_packet(buf, sizeof(buf), 0, 0.5f, /*sample_rate=*/WAVE_RATE + 1);
    ASSERT_GT(len, 0u);

    mixer_remote_dispatch_packet(listener, buf, len, getuid());

    EXPECT_EQ(listener->routes[0].rate_mismatch_count, 1u);
    EXPECT_FALSE(mixer.inputs[slot].ready);
}

TEST_F(MixerRemoteDispatchTest, sample_count_mismatch_is_rejected_and_counted_on_the_route) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    size_t len = make_packet(buf, sizeof(buf), 0, 0.5f, WAVE_RATE, /*sample_count=*/WAVE_BATCH - 1);
    ASSERT_GT(len, 0u);

    mixer_remote_dispatch_packet(listener, buf, len, getuid());

    EXPECT_EQ(listener->routes[0].sample_count_mismatch_count, 1u);
    EXPECT_FALSE(mixer.inputs[slot].ready);
}

TEST_F(MixerRemoteDispatchTest, in_order_sequence_advances_last_seq_with_no_gap_counted) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    ASSERT_GT(make_packet(buf, sizeof(buf), 0), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());
    ASSERT_GT(make_packet(buf, sizeof(buf), 1), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());

    EXPECT_EQ(listener->routes[0].seq_gap_count, 0u);
    EXPECT_EQ(listener->routes[0].seq_reorder_or_duplicate_count, 0u);
    EXPECT_EQ(listener->routes[0].last_seq, 1u);
}

TEST_F(MixerRemoteDispatchTest, a_skipped_sequence_number_is_counted_as_a_gap) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    ASSERT_GT(make_packet(buf, sizeof(buf), 0), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());
    ASSERT_GT(make_packet(buf, sizeof(buf), 5), 0u);  // skipped 1-4
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());

    EXPECT_EQ(listener->routes[0].seq_gap_count, 1u);
    EXPECT_EQ(listener->routes[0].last_seq, 5u);
}

TEST_F(MixerRemoteDispatchTest, a_duplicate_sequence_number_is_counted_but_does_not_rewind_last_seq) {
    uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
    ASSERT_GT(make_packet(buf, sizeof(buf), 0), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());
    ASSERT_GT(make_packet(buf, sizeof(buf), 5), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());
    ASSERT_GT(make_packet(buf, sizeof(buf), 3), 0u);  // older than the highest seen (5)
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());

    EXPECT_EQ(listener->routes[0].seq_reorder_or_duplicate_count, 1u);
    EXPECT_EQ(listener->routes[0].last_seq, 5u);  // did not rewind to 3

    // a genuinely new, in-order packet (5 -> 6) after the stale one is still classified
    // correctly - seq_gap_count stays at 1 (from the earlier 0 -> 5 jump), it does not reset.
    ASSERT_GT(make_packet(buf, sizeof(buf), 6), 0u);
    mixer_remote_dispatch_packet(listener, buf, sizeof(buf), getuid());
    EXPECT_EQ(listener->routes[0].seq_gap_count, 1u);
    EXPECT_EQ(listener->routes[0].last_seq, 6u);
}

// Exercises the real listener thread end-to-end: a real mixer_remote_send() over a real bound
// AF_UNIX SOCK_DGRAM socket, received by the real mixer_remote_recv_start() thread (including
// its SCM_CREDENTIALS extraction), landing in a real mixer_t slot via mixer_put_samples().
TEST_F(MixerRemoteRecvTest, end_to_end_through_the_real_listener_thread_and_socket) {
    string path = temp_dir + "/recv.sock";

    mixer_t mixer = {};
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;
    int slot = mixer_connect_input(&mixer, 1.0f, 0.0f);
    ASSERT_GE(slot, 0);

    mixer_remote_listener_t* listener = mixer_remote_get_or_create_listener(path);
    ASSERT_TRUE(mixer_remote_register_route(listener, /*stream_id=*/5, &mixer, slot));

    mixer_remote_recv_start();
    ASSERT_TRUE(listener->thread_started);

    mixer_remote_send_data sdata;
    memset(&sdata, 0, sizeof(sdata));
    sdata.dest_path = strdup(path.c_str());
    sdata.stream_id = 5;
    ASSERT_TRUE(mixer_remote_send_init(&sdata, WAVE_BATCH));

    float samples[WAVE_BATCH];
    for (int i = 0; i < WAVE_BATCH; ++i) {
        samples[i] = 0.5f;
    }
    mixer_remote_send(&sdata, samples, WAVE_BATCH, true);

    for (int attempt = 0; attempt < 50 && !mixer.inputs[slot].ready; ++attempt) {
        usleep(20000);
    }

    EXPECT_TRUE(mixer.inputs[slot].ready);
    EXPECT_TRUE(mixer.inputs[slot].has_signal);
    EXPECT_FLOAT_EQ(mixer.inputs[slot].wavein[0], 0.5f);
    EXPECT_EQ(listener->rejected_uid_count, 0u);
    EXPECT_EQ(listener->malformed_header_count, 0u);
    EXPECT_EQ(listener->unknown_stream_count, 0u);

    mixer_remote_send_shutdown(&sdata);
    free(const_cast<char*>(sdata.dest_path));
}

TEST_F(MixerRemoteRecvTest, listener_with_no_routes_is_skipped_and_does_not_start_a_thread) {
    mixer_remote_get_or_create_listener(temp_dir + "/unused.sock");  // never registered a route

    mixer_remote_recv_start();

    EXPECT_FALSE(mixer_remote_listeners[0]->thread_started);
}

// ---- config parsing: parse_mixers()'s remote_inputs block (config.cpp) ----

// mixers/mixer_count are process-wide globals (defined in test_mixer.cpp, since mixer.cpp
// references them as extern and rtl_airband.cpp's real definitions can't be linked here - see
// that file's comment) - reset them after each test so a later test in this binary never sees a
// dangling pointer into this test's stack-allocated mixers_arr.
class ParseMixersRemoteInputsTest : public MixerRemoteRecvTest {
   protected:
    void TearDown(void) override {
        mixers = nullptr;
        mixer_count = 0;
        MixerRemoteRecvTest::TearDown();
    }
};

TEST_F(ParseMixersRemoteInputsTest, connects_input_registers_route_and_sets_label) {
    libconfig::Config cfg;
    cfg.readString(R"(
mixers: {
  mix1: {
    remote_inputs: (
      { listen_path = "/tmp/mix1.sock"; stream_id = 7; ampfactor = 0.8; balance = -0.2; label = "Remote Unit"; }
    );
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
)");
    libconfig::Setting& mx = cfg.lookup("mixers");

    mixer_t mixers_arr[1]{};
    mixers = mixers_arr;
    mixer_count = 0;

    int connected = parse_mixers(mx);

    ASSERT_EQ(connected, 1);
    EXPECT_EQ(mixers_arr[0].input_count.load(), 1);
    ASSERT_NE(mixers_arr[0].inputs, nullptr);
    EXPECT_FLOAT_EQ(mixers_arr[0].inputs[0].ampfactor, 0.8f);
    ASSERT_NE(mixers_arr[0].inputs[0].remote_label, nullptr);
    EXPECT_STREQ(mixers_arr[0].inputs[0].remote_label, "Remote Unit");
    // No local source to look up - mixer_tx_tag() falls back to remote_label for exactly these.
    EXPECT_EQ(mixers_arr[0].inputs[0].source_device_idx, -1);
    EXPECT_EQ(mixers_arr[0].inputs[0].source_channel_idx, -1);

    ASSERT_EQ(mixer_remote_listeners.size(), 1u);
    EXPECT_EQ(mixer_remote_listeners[0]->listen_path, "/tmp/mix1.sock");
    ASSERT_EQ(mixer_remote_listeners[0]->routes.size(), 1u);
    EXPECT_EQ(mixer_remote_listeners[0]->routes[0].stream_id, 7u);
    EXPECT_EQ(mixer_remote_listeners[0]->routes[0].mixer, &mixers_arr[0]);
    EXPECT_EQ(mixer_remote_listeners[0]->routes[0].slot_idx, 0);
}

TEST_F(ParseMixersRemoteInputsTest, omitted_label_leaves_remote_label_null) {
    libconfig::Config cfg;
    cfg.readString(R"(
mixers: {
  mix1: {
    remote_inputs: ( { listen_path = "/tmp/mix1.sock"; stream_id = 1; } );
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
)");
    libconfig::Setting& mx = cfg.lookup("mixers");

    mixer_t mixers_arr[1]{};
    mixers = mixers_arr;

    ASSERT_EQ(parse_mixers(mx), 1);
    EXPECT_EQ(mixers_arr[0].inputs[0].remote_label, nullptr);
}

TEST_F(ParseMixersRemoteInputsTest, two_routes_can_share_one_listen_path) {
    libconfig::Config cfg;
    cfg.readString(R"(
mixers: {
  mix1: {
    remote_inputs: (
      { listen_path = "/tmp/mix1.sock"; stream_id = 1; },
      { listen_path = "/tmp/mix1.sock"; stream_id = 2; }
    );
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
)");
    libconfig::Setting& mx = cfg.lookup("mixers");

    mixer_t mixers_arr[1]{};
    mixers = mixers_arr;

    ASSERT_EQ(parse_mixers(mx), 1);
    EXPECT_EQ(mixers_arr[0].input_count.load(), 2);
    ASSERT_EQ(mixer_remote_listeners.size(), 1u);  // one listener, multiplexing both routes
    EXPECT_EQ(mixer_remote_listeners[0]->routes.size(), 2u);
}

// Exercises the real error() path via config_error_is_recoverable/ConfigApplyError (same
// mechanism dynamic_reload's live channel-append path uses, logging.h) rather than letting a
// genuinely malformed config _Exit(1) the whole test binary.
TEST_F(ParseMixersRemoteInputsTest, duplicate_stream_id_on_same_listen_path_is_a_config_error) {
    libconfig::Config cfg;
    cfg.readString(R"(
mixers: {
  mix1: {
    remote_inputs: (
      { listen_path = "/tmp/mix1.sock"; stream_id = 7; },
      { listen_path = "/tmp/mix1.sock"; stream_id = 7; }
    );
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
)");
    libconfig::Setting& mx = cfg.lookup("mixers");

    mixer_t mixers_arr[1]{};
    mixers = mixers_arr;

    config_error_is_recoverable = true;
    ostringstream captured;
    streambuf* old_cerr = cerr.rdbuf(captured.rdbuf());
    EXPECT_THROW(parse_mixers(mx), ConfigApplyError);
    cerr.rdbuf(old_cerr);
    config_error_is_recoverable = false;

    EXPECT_NE(captured.str().find("duplicate stream_id"), string::npos);
}

TEST_F(ParseMixersRemoteInputsTest, missing_stream_id_is_a_config_error) {
    libconfig::Config cfg;
    cfg.readString(R"(
mixers: {
  mix1: {
    remote_inputs: ( { listen_path = "/tmp/mix1.sock"; } );
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
)");
    libconfig::Setting& mx = cfg.lookup("mixers");

    mixer_t mixers_arr[1]{};
    mixers = mixers_arr;

    config_error_is_recoverable = true;
    ostringstream captured;
    streambuf* old_cerr = cerr.rdbuf(captured.rdbuf());
    EXPECT_THROW(parse_mixers(mx), ConfigApplyError);
    cerr.rdbuf(old_cerr);
    config_error_is_recoverable = false;

    EXPECT_NE(captured.str().find("missing stream_id"), string::npos);
}
