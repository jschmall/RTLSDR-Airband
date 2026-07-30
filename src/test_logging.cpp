/*
 * test_logging.cpp
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

#include <syslog.h>

#include "logging.h"

using namespace std;

class LoggingTest : public TestBaseClass {};

TEST_F(LoggingTest, contains_expected_fields) {
    string line = build_json_log_line(LOG_ERR, "something went wrong");

    EXPECT_NE(line.find("\"level\":\"error\""), string::npos);
    EXPECT_NE(line.find("\"message\":\"something went wrong\""), string::npos);
    EXPECT_NE(line.find("\"pid\":"), string::npos);
    EXPECT_NE(line.find("\"timestamp\":\""), string::npos);
}

TEST_F(LoggingTest, is_well_formed_json_object) {
    string line = build_json_log_line(LOG_INFO, "hello");

    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.front(), '{');
    EXPECT_EQ(line.back(), '}');
}

TEST_F(LoggingTest, maps_priority_to_level_name) {
    EXPECT_NE(build_json_log_line(LOG_EMERG, "x").find("\"level\":\"emerg\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_ALERT, "x").find("\"level\":\"alert\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_CRIT, "x").find("\"level\":\"crit\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_ERR, "x").find("\"level\":\"error\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_WARNING, "x").find("\"level\":\"warning\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_NOTICE, "x").find("\"level\":\"notice\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_INFO, "x").find("\"level\":\"info\""), string::npos);
    EXPECT_NE(build_json_log_line(LOG_DEBUG, "x").find("\"level\":\"debug\""), string::npos);
}

TEST_F(LoggingTest, escapes_double_quotes) {
    string line = build_json_log_line(LOG_INFO, "he said \"hello\"");
    EXPECT_NE(line.find("\"message\":\"he said \\\"hello\\\"\""), string::npos);
}

TEST_F(LoggingTest, escapes_backslashes) {
    string line = build_json_log_line(LOG_INFO, "C:\\path\\to\\file");
    EXPECT_NE(line.find("\"message\":\"C:\\\\path\\\\to\\\\file\""), string::npos);
}

TEST_F(LoggingTest, escapes_embedded_newlines_and_tabs) {
    string line = build_json_log_line(LOG_INFO, "line1\nline2\ttabbed");
    EXPECT_NE(line.find("line1\\nline2\\ttabbed"), string::npos);
    // no raw control characters should have leaked through unescaped
    EXPECT_EQ(line.find('\n'), string::npos);
    EXPECT_EQ(line.find('\t'), string::npos);
}

TEST_F(LoggingTest, escapes_other_control_characters) {
    string message = "bell\x07here";
    string line = build_json_log_line(LOG_INFO, message);
    EXPECT_NE(line.find("\\u0007"), string::npos);
}

TEST_F(LoggingTest, handles_empty_message) {
    string line = build_json_log_line(LOG_INFO, "");
    EXPECT_NE(line.find("\"message\":\"\""), string::npos);
}
