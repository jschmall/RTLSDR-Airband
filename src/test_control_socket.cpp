/*
 * test_control_socket.cpp
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
#include <unistd.h>
#include <thread>

#include "control_socket.h"
#include "rtl_airband.h"

using namespace std;

// control_socket.cpp references these as extern; normally defined in rtl_airband.cpp, which
// can't be linked here (its main() would conflict with gtest's) - same pattern as mixers/
// mixer_count in test_mixer.cpp. Left empty/zero throughout: these tests exercise parsing and
// validation paths only, not the parts of dispatch that need a fully wired-up device_t/input_t.
device_t* devices = nullptr;
int device_count = 0;
char* cfgfile = nullptr;

class ControlSocketParseTest : public TestBaseClass {};

TEST_F(ControlSocketParseTest, parses_string_and_int_fields) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line(R"({"cmd":"retune","device":0,"freq":462562500})", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(fields["cmd"], "retune");
    EXPECT_EQ(fields["device"], "0");
    EXPECT_EQ(fields["freq"], "462562500");
}

TEST_F(ControlSocketParseTest, parses_float_fields) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line(R"({"cmd":"set_gain","device":0,"gain":30.5})", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(fields["gain"], "30.5");
}

TEST_F(ControlSocketParseTest, parses_mixer_name_string) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line(R"({"cmd":"mixer_enable","mixer":"mix1"})", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(fields["mixer"], "mix1");
}

TEST_F(ControlSocketParseTest, handles_whitespace_between_tokens) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line(R"( { "cmd" : "channel_enable" , "device" : 1 , "channel" : 2 } )", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(fields["cmd"], "channel_enable");
    EXPECT_EQ(fields["device"], "1");
    EXPECT_EQ(fields["channel"], "2");
}

TEST_F(ControlSocketParseTest, empty_object_parses_to_no_fields) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line("{}", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_TRUE(fields.empty());
}

TEST_F(ControlSocketParseTest, rejects_missing_opening_brace) {
    map<string, string> fields;
    string error;
    EXPECT_FALSE(control_socket_parse_command_line(R"("cmd":"retune"})", &fields, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ControlSocketParseTest, rejects_unterminated_string) {
    map<string, string> fields;
    string error;
    EXPECT_FALSE(control_socket_parse_command_line(R"({"cmd":"retune)", &fields, &error));
}

TEST_F(ControlSocketParseTest, rejects_missing_colon) {
    map<string, string> fields;
    string error;
    EXPECT_FALSE(control_socket_parse_command_line(R"({"cmd" "retune"})", &fields, &error));
}

TEST_F(ControlSocketParseTest, rejects_trailing_garbage_instead_of_comma_or_brace) {
    map<string, string> fields;
    string error;
    EXPECT_FALSE(control_socket_parse_command_line(R"({"cmd":"retune" "device":0})", &fields, &error));
}

TEST_F(ControlSocketParseTest, unescapes_backslash_sequences_in_string_values) {
    map<string, string> fields;
    string error;
    bool ok = control_socket_parse_command_line(R"({"mixer":"a\"b"})", &fields, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(fields["mixer"], "a\"b");
}

class ControlSocketDispatchTest : public TestBaseClass {
   protected:
    void SetUp() override {
        TestBaseClass::SetUp();
        devices = nullptr;
        device_count = 0;
        mixer_count = 0;
    }
};

TEST_F(ControlSocketDispatchTest, missing_cmd_is_rejected) {
    string response = control_socket_dispatch_command({});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("missing 'cmd'"), string::npos);
}

TEST_F(ControlSocketDispatchTest, unknown_cmd_is_rejected) {
    string response = control_socket_dispatch_command({{"cmd", "not_a_real_command"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("unknown cmd"), string::npos);
}

TEST_F(ControlSocketDispatchTest, retune_rejects_out_of_range_device) {
    // device_count is 0 in this fixture, so any device index is out of range - exercises the
    // bounds check without needing a real device_t/input_t.
    string response = control_socket_dispatch_command({{"cmd", "retune"}, {"device", "0"}, {"freq", "100000000"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("device index out of range"), string::npos);
}

TEST_F(ControlSocketDispatchTest, retune_rejects_missing_freq) {
    device_t dev = {};
    dev.mode = R_MULTICHANNEL;
    devices = &dev;
    device_count = 1;

    string response = control_socket_dispatch_command({{"cmd", "retune"}, {"device", "0"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("'freq'"), string::npos);
}

static int fake_retune_set_centerfreq_ok(input_t* const, int const) {
    return 0;
}

static int fake_retune_set_centerfreq_fails(input_t* const, int const) {
    return -1;
}

// Simulates demodulate()'s consumption block (rtl_airband.cpp) closely enough to exercise
// handle_retune()'s poll loop end-to-end, without needing device_apply_retune()'s full
// channels/bins fixture (that's covered directly in test_live_reconfig.cpp). Publishes
// centerfreq_apply_failed BEFORE clearing pending_centerfreq_request, matching the fixed
// ordering - see both fields' comments (rtl_airband.h).
static std::thread start_retune_consumer(device_t* dev, std::atomic<bool>* stop_consumer) {
    return std::thread([dev, stop_consumer]() {
        while (!stop_consumer->load()) {
            int pending = dev->pending_centerfreq_request.load(std::memory_order_acquire);
            if (pending >= 0) {
                int ret = dev->input->set_centerfreq(dev->input, pending);
                dev->centerfreq_apply_failed.store(ret != 0, std::memory_order_release);
                dev->pending_centerfreq_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });
}

TEST_F(ControlSocketDispatchTest, retune_reports_error_on_hardware_failure_without_touching_input_state) {
    // The regression this test guards against: handle_retune() used to check
    // dev->input->state == INPUT_FAILED, which input_set_centerfreq() (input-common.cpp) no
    // longer sets on a transient hardware failure - it must key off centerfreq_apply_failed
    // instead, and must never require/imply input->state changing.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_retune_set_centerfreq_fails;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.pending_centerfreq_request = -1;
    dev.centerfreq_apply_failed = false;
    devices = &dev;
    device_count = 1;

    std::atomic<bool> stop_consumer{false};
    std::thread consumer = start_retune_consumer(&dev, &stop_consumer);

    string response = control_socket_dispatch_command({{"cmd", "retune"}, {"device", "0"}, {"freq", "100000000"}});

    stop_consumer.store(true);
    consumer.join();

    EXPECT_NE(response.find("\"ok\":false"), string::npos) << "response: " << response;
    EXPECT_NE(response.find("hardware retune failed"), string::npos) << "response: " << response;
    EXPECT_EQ(input.state, INPUT_RUNNING);
}

TEST_F(ControlSocketDispatchTest, retune_reports_ok_on_hardware_success) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_retune_set_centerfreq_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.pending_centerfreq_request = -1;
    dev.centerfreq_apply_failed = false;
    devices = &dev;
    device_count = 1;

    std::atomic<bool> stop_consumer{false};
    std::thread consumer = start_retune_consumer(&dev, &stop_consumer);

    string response = control_socket_dispatch_command({{"cmd", "retune"}, {"device", "0"}, {"freq", "100000000"}});

    stop_consumer.store(true);
    consumer.join();

    EXPECT_NE(response.find("\"ok\":true"), string::npos) << "response: " << response;
}

TEST_F(ControlSocketDispatchTest, channel_enable_rejects_missing_device) {
    string response = control_socket_dispatch_command({{"cmd", "channel_enable"}, {"channel", "0"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("'device'"), string::npos);
}

// Regression tests for a real crash: get_device_and_channel() used to bounds-check purely on
// dev->channel_count, which try_remove_channels() (live_reconfig.cpp) can leave stale after a
// timed-out removal - a channel index can be "still counted as live" while its removal is
// actually in flight or has already completed (LAME encoder freed). Letting channel_enable
// through in that window would reconnect outputs without ever reallocating LAME
// (channel_apply_enable() doesn't - only init_output() does, at startup/append time), so the
// next process_outputs() pass calls into LAME with a null encoder and crashes the whole output
// thread. See channel_t::removed's comment (rtl_airband.h) for the full scenario.

TEST_F(ControlSocketDispatchTest, channel_enable_rejects_channel_with_removal_in_flight) {
    channel_t chan = {};
    chan.pending_enable_request = -1;
    chan.pending_remove_request = 1;  // removal requested, not yet consumed by output_thread()
    chan.removed = false;

    device_t dev = {};
    dev.channel_count = 1;
    dev.channels = &chan;
    devices = &dev;
    device_count = 1;

    string response = control_socket_dispatch_command({{"cmd", "channel_enable"}, {"device", "0"}, {"channel", "0"}});

    EXPECT_NE(response.find("\"ok\":false"), string::npos) << "response: " << response;
    EXPECT_NE(response.find("being removed"), string::npos) << "response: " << response;
    EXPECT_EQ(chan.pending_enable_request.load(), -1) << "must never post an enable request against this index";
}

TEST_F(ControlSocketDispatchTest, channel_enable_rejects_already_removed_channel) {
    channel_t chan = {};
    chan.pending_enable_request = -1;
    chan.pending_remove_request = -1;  // removal already completed and consumed
    chan.removed = true;               // ...but dev->channel_count was never decremented for it

    device_t dev = {};
    dev.channel_count = 1;
    dev.channels = &chan;
    devices = &dev;
    device_count = 1;

    string response = control_socket_dispatch_command({{"cmd", "channel_enable"}, {"device", "0"}, {"channel", "0"}});

    EXPECT_NE(response.find("\"ok\":false"), string::npos) << "response: " << response;
    EXPECT_NE(response.find("already been removed"), string::npos) << "response: " << response;
    EXPECT_EQ(chan.pending_enable_request.load(), -1) << "must never post an enable request against this index";
}

TEST_F(ControlSocketDispatchTest, channel_disable_also_rejects_removed_channel) {
    // Same bounds-check helper (get_device_and_channel()) backs channel_disable too - one test to
    // confirm the fix isn't specific to channel_enable's call site.
    channel_t chan = {};
    chan.pending_remove_request = -1;
    chan.removed = true;

    device_t dev = {};
    dev.channel_count = 1;
    dev.channels = &chan;
    devices = &dev;
    device_count = 1;

    string response = control_socket_dispatch_command({{"cmd", "channel_disable"}, {"device", "0"}, {"channel", "0"}});

    EXPECT_NE(response.find("\"ok\":false"), string::npos) << "response: " << response;
    EXPECT_NE(response.find("already been removed"), string::npos) << "response: " << response;
}

TEST_F(ControlSocketDispatchTest, channel_enable_accepts_a_normal_never_removed_channel) {
    // Confirms the new check doesn't over-reject a channel that was never touched by removal -
    // no consumer thread here, so channel_request_enable() will time out waiting for
    // confirmation (nothing resets pending_enable_request back to -1), same as a real "output
    // thread is busy" case. What matters for this test is that get_device_and_channel() let the
    // request through at all (pending_enable_request actually got posted), not full end-to-end
    // application - that path is already covered by the retune consumer-thread tests above.
    channel_t chan = {};
    chan.pending_remove_request = -1;
    chan.removed = false;

    device_t dev = {};
    dev.channel_count = 1;
    dev.channels = &chan;
    devices = &dev;
    device_count = 1;

    string response = control_socket_dispatch_command({{"cmd", "channel_enable"}, {"device", "0"}, {"channel", "0"}});

    EXPECT_EQ(response.find("being removed"), string::npos) << "response: " << response;
    EXPECT_EQ(response.find("already been removed"), string::npos) << "response: " << response;
    EXPECT_EQ(response.find("out of range"), string::npos) << "response: " << response;
    EXPECT_EQ(chan.pending_enable_request.load(), 1) << "request should have been posted (even if not yet confirmed)";
}

TEST_F(ControlSocketDispatchTest, mixer_enable_rejects_unknown_mixer_name) {
    string response = control_socket_dispatch_command({{"cmd", "mixer_enable"}, {"mixer", "does_not_exist"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("unknown mixer"), string::npos);
}

TEST_F(ControlSocketDispatchTest, mixer_disable_rejects_missing_mixer_field) {
    string response = control_socket_dispatch_command({{"cmd", "mixer_disable"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("missing 'mixer'"), string::npos);
}

TEST_F(ControlSocketDispatchTest, dispatch_command_line_reports_parse_errors) {
    string response = control_socket_dispatch_command_line("not json at all");
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("parse error"), string::npos);
}

// The wire protocol is one JSON response per line (control_socket_read_thread(), control_socket.
// cpp) - a raw, unescaped newline embedded in a response string breaks every client's framing,
// truncating the message before its closing brace. Nearly every error_response() message is
// built from a captured cerr message that ends in "\n" (e.g. config.cpp's parse-error printouts
// during a dynamic_reload live channel append - see config_error_is_recoverable, logging.h), so
// this isn't a hypothetical: it's the normal shape of an append-failure error message. Using an
// embedded raw newline in the "cmd" field here (rather than a config-parse error, which would
// need a much heavier live_reconfig fixture) exercises the same json_escape() path with the
// same class of un-sanitized, attacker/config-controlled text.
TEST_F(ControlSocketDispatchTest, dispatch_command_line_response_never_contains_a_raw_newline) {
    string response = control_socket_dispatch_command_line("{\"cmd\":\"bogus\ncmd\"}");
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_EQ(response.find('\n'), string::npos) << "response: " << response;
    EXPECT_NE(response.find("bogus\\ncmd"), string::npos) << "response: " << response;
}

// handle_connection() is the one function in this file that does real socket I/O rather than
// pure parsing/dispatch - these use a real AF_UNIX socketpair() so the timeout/buffer-cap logic
// under test is exercised for real, not mocked away. Short timeout_sec/max_buffered_bytes values
// (rather than control_socket.h's production defaults) keep these fast - see handle_connection()'s
// declaration comment (control_socket.h) for why a real socket is unavoidable here.
class HandleConnectionTest : public TestBaseClass {
   protected:
    int server_fd = -1;
    int client_fd = -1;

    void SetUp() override {
        TestBaseClass::SetUp();
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        server_fd = fds[0];
        client_fd = fds[1];
    }

    void TearDown() override {
        if (client_fd >= 0) {
            close(client_fd);
        }
        TestBaseClass::TearDown();
    }
};

TEST_F(HandleConnectionTest, closes_connection_after_recv_timeout_with_no_data) {
    std::thread server([&]() { handle_connection(server_fd, /*timeout_sec=*/1, /*max_buffered_bytes=*/16384); });

    // Client sends nothing at all - handle_connection()'s recv() should time out (not hang
    // forever) and close its end, which the client observes as EOF (recv() returning 0).
    char buf[16];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0) << "expected EOF (server closed after timeout), got " << n;

    server.join();
}

TEST_F(HandleConnectionTest, closes_connection_when_buffer_exceeds_cap_without_a_newline) {
    std::thread server([&]() { handle_connection(server_fd, /*timeout_sec=*/5, /*max_buffered_bytes=*/16); });

    // Send more than max_buffered_bytes with no newline - handle_connection() should give up on
    // this connection immediately rather than buffering it indefinitely.
    string junk(64, 'x');
    ASSERT_GT(send(client_fd, junk.data(), junk.size(), 0), 0);

    char buf[16];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0) << "expected EOF (server closed after exceeding buffer cap), got " << n;

    server.join();
}

TEST_F(HandleConnectionTest, still_dispatches_and_responds_to_a_real_command_normally) {
    // Confirms the new timeout/cap logic doesn't disturb the normal request/response path -
    // a real end-to-end round trip over the actual socket, not just dispatch_command_line()
    // called directly.
    std::thread server([&]() { handle_connection(server_fd, /*timeout_sec=*/5, /*max_buffered_bytes=*/16384); });

    string request = "not valid json\n";
    ASSERT_GT(send(client_fd, request.data(), request.size(), 0), 0);

    char buf[256] = {0};
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n, 0);
    string response(buf, (size_t)n);
    EXPECT_NE(response.find("\"ok\":false"), string::npos) << "response: " << response;
    EXPECT_NE(response.find('\n'), string::npos) << "response should be newline-terminated";

    close(client_fd);
    client_fd = -1;  // TearDown already closes it; avoid a double-close
    server.join();
}
