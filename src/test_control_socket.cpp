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

TEST_F(ControlSocketDispatchTest, channel_enable_rejects_missing_device) {
    string response = control_socket_dispatch_command({{"cmd", "channel_enable"}, {"channel", "0"}});
    EXPECT_NE(response.find("\"ok\":false"), string::npos);
    EXPECT_NE(response.find("'device'"), string::npos);
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
