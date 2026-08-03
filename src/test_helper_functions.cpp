/*
 * test_output.cpp
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

#include <cstring>
#include "helper_functions.h"
#include "rtl_airband.h"

using namespace std;

class HelperFunctionsTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void create_file(const string& filepath) {
        fclose(fopen(filepath.c_str(), "wb"));
        EXPECT_TRUE(file_exists(filepath));
    }
};

TEST_F(HelperFunctionsTest, dir_exists_true) {
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, dir_exists_false) {
    EXPECT_FALSE(dir_exists("/not/a/real/dir"));
}

TEST_F(HelperFunctionsTest, dir_exists_not_dir) {
    string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_FALSE(dir_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, file_exists_true) {
    string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_TRUE(file_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, file_exists_false) {
    EXPECT_FALSE(file_exists(temp_dir + "/nothing"));
}

TEST_F(HelperFunctionsTest, file_exists_not_file) {
    EXPECT_FALSE(file_exists(temp_dir));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_dir_normal) {
    const string dir_path = temp_dir + "/a";
    EXPECT_FALSE(dir_exists(dir_path));
    EXPECT_TRUE(make_dir(dir_path));
    EXPECT_TRUE(dir_exists(dir_path));
}

TEST_F(HelperFunctionsTest, make_dir_exists) {
    EXPECT_TRUE(dir_exists(temp_dir));
    EXPECT_TRUE(make_dir(temp_dir));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_dir_empty) {
    EXPECT_FALSE(make_dir(""));
}

TEST_F(HelperFunctionsTest, make_dir_fail) {
    EXPECT_FALSE(make_dir("/this/path/does/not/exist"));
}

TEST_F(HelperFunctionsTest, make_dir_file_in_the_way) {
    const string file_path = temp_dir + "/some_file";
    create_file(file_path);
    EXPECT_FALSE(make_dir(file_path));
}

TEST_F(HelperFunctionsTest, make_subdirs_exists) {
    EXPECT_TRUE(dir_exists(temp_dir));
    EXPECT_TRUE(make_subdirs(temp_dir, ""));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_subdirs_one_subdir) {
    const string path = "bob";
    EXPECT_FALSE(dir_exists(temp_dir + "/" + path));
    EXPECT_TRUE(make_subdirs(temp_dir, path));
    EXPECT_TRUE(dir_exists(temp_dir + "/" + path));
}

TEST_F(HelperFunctionsTest, make_subdirs_multiple_subdir) {
    const string path = "bob/joe/sam";
    EXPECT_FALSE(dir_exists(temp_dir + "/" + path));
    EXPECT_TRUE(make_subdirs(temp_dir, path));
    EXPECT_TRUE(dir_exists(temp_dir + "/" + path));
}

TEST_F(HelperFunctionsTest, make_subdirs_file_in_the_way) {
    const string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_TRUE(file_exists(file_in_dir));
    EXPECT_FALSE(make_subdirs(temp_dir, "some_file/some_dir"));
    EXPECT_FALSE(dir_exists(file_in_dir));
    EXPECT_TRUE(file_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_normal) {
    EXPECT_EQ(make_icecast_mountpoint("stream.mp3"), "/stream.mp3");
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_empty) {
    EXPECT_EQ(make_icecast_mountpoint(""), "/");
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_longer_than_fixed_buffer_used_to_allow) {
    // output.cpp used to build this string into a fixed char[100] stack buffer via
    // sprintf(), which would overflow for a mountpoint this long. Confirm a long value
    // is preserved in full rather than truncated or overflowing.
    const string long_mountpoint(500, 'a');
    EXPECT_EQ(make_icecast_mountpoint(long_mountpoint), "/" + long_mountpoint);
}

TEST_F(HelperFunctionsTest, make_subdirs_create_base) {
    EXPECT_FALSE(dir_exists(temp_dir + "/base_dir/a"));
    EXPECT_TRUE(make_subdirs(temp_dir + "/base_dir", "a"));
    EXPECT_TRUE(dir_exists(temp_dir + "/base_dir/a"));
}

TEST_F(HelperFunctionsTest, make_subdirs_extra_slashes) {
    EXPECT_FALSE(dir_exists(temp_dir + "/a/b/c/d"));
    EXPECT_TRUE(make_subdirs(temp_dir, "///a/b////c///d"));
    EXPECT_TRUE(dir_exists(temp_dir + "/a/b/c/d"));
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_normal) {
    struct tm time_struct;

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);

    const string dir_path = temp_dir + "/2010/03/07";

    EXPECT_FALSE(dir_exists(dir_path));
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_path);
    EXPECT_TRUE(dir_exists(dir_path));
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_fail) {
    struct tm time_struct;

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);

    EXPECT_EQ(make_dated_subdirs("/invalid/base/dir", &time_struct), "");
}

TEST_F(HelperFunctionsTest, rusage_cpu_seconds_combines_user_and_system_time) {
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    ru.ru_utime.tv_sec = 2;
    ru.ru_utime.tv_usec = 500000;
    ru.ru_stime.tv_sec = 1;
    ru.ru_stime.tv_usec = 250000;

    EXPECT_DOUBLE_EQ(rusage_cpu_seconds(ru), 3.75);
}

TEST_F(HelperFunctionsTest, rusage_cpu_seconds_zero) {
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));

    EXPECT_DOUBLE_EQ(rusage_cpu_seconds(ru), 0.0);
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_no_signal_is_empty) {
    EXPECT_EQ(compute_tx_tag_content(false, "Fire Dispatch", 154265000), "");
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_uses_label) {
    EXPECT_EQ(compute_tx_tag_content(true, "Fire Dispatch", 154265000), "Fire Dispatch");
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_falls_back_to_frequency_without_label) {
    EXPECT_EQ(compute_tx_tag_content(true, NULL, 154265000), "154.265 MHz");
}

namespace {
struct timeval make_tv(long sec) {
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return tv;
}
}  // namespace

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_noop_when_already_matches_applied) {
    icecast_tx_tag_state state;
    string out;
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(100), 3, &out));
    EXPECT_FALSE(state.pending);
    EXPECT_EQ(state.applied, "");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_defers_first_change_until_deadline) {
    icecast_tx_tag_state state;
    string out;

    // transmission starts - change is queued but not applied yet
    EXPECT_FALSE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(100), 3, &out));
    EXPECT_TRUE(state.pending);
    EXPECT_EQ(state.applied, "");

    // deadline not reached yet
    EXPECT_FALSE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(102), 3, &out));
    EXPECT_EQ(state.applied, "");

    // deadline reached - applies now
    EXPECT_TRUE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(103), 3, &out));
    EXPECT_EQ(out, "Fire Dispatch");
    EXPECT_EQ(state.applied, "Fire Dispatch");
    EXPECT_FALSE(state.pending);
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_zero_delay_applies_immediately) {
    icecast_tx_tag_state state;
    string out;
    EXPECT_TRUE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(100), 0, &out));
    EXPECT_EQ(out, "Fire Dispatch");
    EXPECT_EQ(state.applied, "Fire Dispatch");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_change_while_pending_updates_value_not_deadline) {
    icecast_tx_tag_state state;
    string out;

    // first change starts a 3s deferred window
    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(100), 3, &out));

    // a second, different desired value arrives before the deadline - updates the pending
    // value but must not push the deadline back
    EXPECT_FALSE(icecast_tx_tag_step(&state, "B", make_tv(101), 3, &out));
    EXPECT_EQ(state.pending_value, "B");

    // original deadline (100+3=103) still governs, not a new one relative to t=101
    EXPECT_FALSE(icecast_tx_tag_step(&state, "B", make_tv(102), 3, &out));
    EXPECT_TRUE(icecast_tx_tag_step(&state, "B", make_tv(103), 3, &out));
    EXPECT_EQ(out, "B");
    EXPECT_EQ(state.applied, "B");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_revert_to_applied_while_pending_cancels) {
    icecast_tx_tag_state state;
    string out;

    // squelch opens...
    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(100), 3, &out));
    EXPECT_TRUE(state.pending);

    // ...and closes again before the deferred window elapses - reverting to the
    // already-applied value ("") must cancel the pending change outright
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(101), 3, &out));
    EXPECT_FALSE(state.pending);

    // confirm no delayed firing past the original deadline
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(104), 3, &out));
    EXPECT_EQ(state.applied, "");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_steady_state_repeats_are_noops) {
    icecast_tx_tag_state state;
    string out;

    EXPECT_TRUE(icecast_tx_tag_step(&state, "A", make_tv(100), 0, &out));
    EXPECT_EQ(state.applied, "A");

    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(105), 0, &out));
    EXPECT_EQ(state.applied, "A");
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_some_exist) {
    struct tm time_struct;

    const string dir_through_month = temp_dir + "/2010/03/";

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_through_month + "07");

    EXPECT_TRUE(dir_exists(dir_through_month));
    EXPECT_FALSE(dir_exists(dir_through_month + "08"));

    strptime("2010-3-8", "%Y-%m-%d", &time_struct);
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_through_month + "08");
    EXPECT_TRUE(dir_exists(dir_through_month + "08"));
}
