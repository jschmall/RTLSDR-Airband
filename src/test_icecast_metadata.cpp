/*
 * test_icecast_metadata.cpp
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

#include <gtest/gtest.h>
#include <string.h>
#include "icecast_metadata.h"
#include "rtl_airband.h"

using namespace std;

namespace {

shared_ptr<IcecastMetaTarget> make_target(const char* mount) {
    auto t = make_shared<IcecastMetaTarget>();
    t->mount = mount;
    return t;
}

}  // namespace

TEST(IcecastMetaQueueTest, submitted_value_is_taken) {
    IcecastMetaQueue q;
    auto t = make_target("/a");
    EXPECT_TRUE(q.submit(t, "Fire Dispatch"));
    shared_ptr<IcecastMetaTarget> got;
    string value;
    ASSERT_TRUE(q.take(&got, &value));
    EXPECT_EQ(got, t);
    EXPECT_EQ(value, "Fire Dispatch");
    EXPECT_EQ(q.pending(), 0u);
}

TEST(IcecastMetaQueueTest, newer_value_replaces_one_still_waiting) {
    IcecastMetaQueue q;
    auto t = make_target("/a");
    EXPECT_TRUE(q.submit(t, "Fire Dispatch"));
    EXPECT_FALSE(q.submit(t, ""));
    EXPECT_EQ(q.pending(), 1u);
    shared_ptr<IcecastMetaTarget> got;
    string value;
    ASSERT_TRUE(q.take(&got, &value));
    EXPECT_EQ(value, "");
    EXPECT_EQ(q.pending(), 0u);
}

TEST(IcecastMetaQueueTest, targets_are_served_in_the_order_they_became_pending) {
    IcecastMetaQueue q;
    auto a = make_target("/a");
    auto b = make_target("/b");
    q.submit(a, "1");
    q.submit(b, "2");
    q.submit(a, "3");
    shared_ptr<IcecastMetaTarget> got;
    string value;
    ASSERT_TRUE(q.take(&got, &value));
    EXPECT_EQ(got, a);
    EXPECT_EQ(value, "3");
    ASSERT_TRUE(q.take(&got, &value));
    EXPECT_EQ(got, b);
    EXPECT_EQ(value, "2");
}

TEST(IcecastMetaQueueTest, value_submitted_while_one_is_being_sent_is_queued_again) {
    IcecastMetaQueue q;
    auto t = make_target("/a");
    q.submit(t, "Fire Dispatch");
    shared_ptr<IcecastMetaTarget> got;
    string value;
    ASSERT_TRUE(q.take(&got, &value));
    // The worker is now sending "Fire Dispatch"; the transmission ends meanwhile.
    EXPECT_TRUE(q.submit(t, ""));
    ASSERT_TRUE(q.take(&got, &value));
    EXPECT_EQ(value, "");
}

TEST(IcecastMetaQueueTest, take_returns_false_after_shutdown) {
    IcecastMetaQueue q;
    q.submit(make_target("/a"), "x");
    q.shutdown();
    shared_ptr<IcecastMetaTarget> got;
    string value;
    EXPECT_FALSE(q.take(&got, &value));
}

TEST(IcecastMetaQueueTest, queued_job_keeps_target_alive_after_output_lets_go) {
    IcecastMetaQueue q;
    auto t = make_target("/a");
    weak_ptr<IcecastMetaTarget> watch = t;
    q.submit(t, "x");
    t.reset();
    EXPECT_FALSE(watch.expired());
    shared_ptr<IcecastMetaTarget> got;
    string value;
    ASSERT_TRUE(q.take(&got, &value));
    got.reset();
    EXPECT_TRUE(watch.expired());
}

TEST(IcecastMetadataTest, target_copies_settings_out_of_the_output) {
    icecast_data* idata = new icecast_data();
    idata->hostname = strdup("audio9.example.com");
    idata->port = 8000;
    idata->mountpoint = strdup("abc123");
    idata->username = strdup("source");
    idata->password = strdup("hunter2");
    auto t = icecast_metadata_target_new(idata);
    free(const_cast<char*>(idata->hostname));
    free(const_cast<char*>(idata->mountpoint));
    free(const_cast<char*>(idata->username));
    free(const_cast<char*>(idata->password));
    delete idata;

    EXPECT_EQ(t->host, "audio9.example.com");
    EXPECT_EQ(t->port, 8000);
    EXPECT_EQ(t->mount, "/abc123");
    EXPECT_EQ(t->username, "source");
    EXPECT_EQ(t->password, "hunter2");
    EXPECT_EQ(t->shout, nullptr);
}

TEST(IcecastMetadataTest, submit_counts_superseded_values_and_ignores_null_target) {
    auto t = make_target("/counted");
    icecast_metadata_submit(t, "a");
    icecast_metadata_submit(t, "b");
    icecast_metadata_submit(t, "c");
    EXPECT_EQ(t->superseded_count.load(), 2u);
    icecast_metadata_submit(nullptr, "ignored");
}
