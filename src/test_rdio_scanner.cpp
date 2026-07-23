/*
 * test_rdio_scanner.cpp
 *
 * Copyright (C) 2026 Jared Schmall
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

#include "rtl_airband.h"

#ifdef WITH_RDIO_SCANNER

#include <map>

using namespace std;

namespace {

map<string, string> as_map(const vector<pair<string, string>>& fields) {
    map<string, string> m;
    for (const auto& field : fields) {
        m[field.first] = field.second;
    }
    return m;
}

rdio_scanner_job make_job() {
    rdio_scanner_job job;
    job.config.server = "10.0.50.36";
    job.config.port = 3000;
    job.config.use_tls = false;
    job.config.api_key = "a9823e02-fe4e-4c91-b513-c809a032827e";
    job.config.system_id = -1;
    job.config.system_label = "";
    job.config.talkgroup_id = 10612;
    job.config.talkgroup_label = "";
    job.config.talkgroup_tag = "";
    job.config.talkgroup_group = "";
    job.config.source_id = 0;
    job.config.delete_after_upload = false;
    job.config.timeout_ms = 5000;
    job.config.max_retries = 2;
    job.file_path = "/mnt/audio/cdf/tac_1/cdf_tac_1_20260723_120000.mp3";
    job.timestamp_ms = 1784894400123LL;
    job.frequency = 154265000;
    return job;
}

}  // namespace

class RdioScannerTest : public TestBaseClass {};

TEST_F(RdioScannerTest, required_fields_always_present) {
    rdio_scanner_job job = make_job();
    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.at("audioType"), "audio/mpeg");
    EXPECT_EQ(fields.at("timestamp"), "1784894400123");
    EXPECT_EQ(fields.at("frequency"), "154265000");
    EXPECT_EQ(fields.at("key"), "a9823e02-fe4e-4c91-b513-c809a032827e");
    EXPECT_EQ(fields.at("source"), "0");
    EXPECT_EQ(fields.at("talkgroup"), "10612");
}

TEST_F(RdioScannerTest, optional_fields_omitted_when_unset) {
    rdio_scanner_job job = make_job();
    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.count("system"), 0u);
    EXPECT_EQ(fields.count("systemLabel"), 0u);
    EXPECT_EQ(fields.count("talkgroupLabel"), 0u);
    EXPECT_EQ(fields.count("talkgroupTag"), 0u);
    EXPECT_EQ(fields.count("talkgroupGroup"), 0u);
}

TEST_F(RdioScannerTest, optional_fields_included_when_set) {
    rdio_scanner_job job = make_job();
    job.config.system_id = 106;
    job.config.system_label = "CDF";
    job.config.talkgroup_label = "A/G 1";
    job.config.talkgroup_tag = "Fire Tac";
    job.config.talkgroup_group = "Fire";

    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.at("system"), "106");
    EXPECT_EQ(fields.at("systemLabel"), "CDF");
    EXPECT_EQ(fields.at("talkgroupLabel"), "A/G 1");
    EXPECT_EQ(fields.at("talkgroupTag"), "Fire Tac");
    EXPECT_EQ(fields.at("talkgroupGroup"), "Fire");
}

TEST_F(RdioScannerTest, system_id_zero_is_a_valid_value_not_unset) {
    rdio_scanner_job job = make_job();
    job.config.system_id = 0;

    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    ASSERT_EQ(fields.count("system"), 1u);
    EXPECT_EQ(fields.at("system"), "0");
}

#endif /* WITH_RDIO_SCANNER */
