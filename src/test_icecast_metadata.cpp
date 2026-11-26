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

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include "icecast_metadata.h"
#include "rtl_airband.h"

using namespace std;

namespace {

shared_ptr<IcecastMetaTarget> make_target(const char* mount) {
    auto t = make_shared<IcecastMetaTarget>();
    t->mount = mount;
    return t;
}

shared_ptr<IcecastMetaTarget> http_target(int port) {
    auto t = make_shared<IcecastMetaTarget>();
    t->host = "127.0.0.1";
    t->port = port;
    t->mount = "/live.mp3";
    t->username = "source";
    t->password = "hackme";
    return t;
}

// One-connection server on 127.0.0.1: reads a request's head, answers, and keeps the connection
// open until the client closes it (as Icecast 2.5 does after a 401).
class OneShotServer {
   public:
    explicit OneShotServer(string answer) : answer_(std::move(answer)) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(listen_fd_, (struct sockaddr*)&addr, &len);
        port_ = ntohs(addr.sin_port);
        listen(listen_fd_, 1);
        thread_ = thread([this] { serve(); });
    }
    ~OneShotServer() {
        thread_.join();
        close(listen_fd_);
    }
    int port() const { return port_; }
    // Valid once the client's call has returned.
    string request() {
        thread_.join();
        thread_ = thread([] {});
        return request_;
    }

   private:
    void serve() {
        int c = accept(listen_fd_, NULL, NULL);
        if (c < 0) {
            return;
        }
        char buf[1024];
        while (request_.find("\r\n\r\n") == string::npos) {
            ssize_t n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            request_.append(buf, n);
        }
        send(c, answer_.data(), answer_.size(), 0);
        while (recv(c, buf, sizeof(buf), 0) > 0) {
        }
        close(c);
    }
    string answer_;
    string request_;
    int listen_fd_;
    int port_;
    thread thread_;
};

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

TEST(IcecastMetadataHttpTest, request_sends_basic_auth_up_front) {
    EXPECT_EQ(icecast_metadata_request(*http_target(8000), "Fire Dispatch & more"),
              "GET /admin/metadata?mode=updinfo&charset=UTF-8&mount=%2Flive.mp3&song=Fire%20Dispatch%20%26%20more HTTP/1.0\r\n"
              "Host: 127.0.0.1:8000\r\n"
              "Authorization: Basic c291cmNlOmhhY2ttZQ==\r\n"
              "User-Agent: RTLSDR-Airband\r\n"
              "\r\n");
}

TEST(IcecastMetadataHttpTest, request_encodes_a_cleared_title_and_utf8) {
    string r = icecast_metadata_request(*http_target(8000), " ");
    EXPECT_NE(r.find("&song=%20 HTTP/1.0"), string::npos) << r;
    r = icecast_metadata_request(*http_target(8000), "Caf\xc3\xa9");
    EXPECT_NE(r.find("&song=Caf%C3%A9 HTTP/1.0"), string::npos) << r;
}

TEST(IcecastMetadataHttpTest, request_pads_base64_for_every_length) {
    auto t = http_target(8000);
    t->username = "s";
    t->password = "p1";  // "s:p1", 4 bytes
    EXPECT_NE(icecast_metadata_request(*t, "x").find("Basic czpwMQ==\r\n"), string::npos);
    t->password = "p12";  // 5 bytes
    EXPECT_NE(icecast_metadata_request(*t, "x").find("Basic czpwMTI=\r\n"), string::npos);
    t->password = "p";  // 3 bytes
    EXPECT_NE(icecast_metadata_request(*t, "x").find("Basic czpw\r\n"), string::npos);
}

TEST(IcecastMetadataHttpTest, an_accepted_update) {
    OneShotServer server("HTTP/1.0 200 OK\r\nContent-Type: text/xml\r\n\r\n<iceresponse><return>1</return></iceresponse>");
    auto t = http_target(server.port());
    string error;
    EXPECT_TRUE(icecast_metadata_send_http(*t, "Unit 12", &error)) << error;
    EXPECT_EQ(server.request(), icecast_metadata_request(*t, "Unit 12"));
}

// Icecast 2.5 answers a refused login with 401 and keeps the connection open: the answer is
// read without waiting for the server to close.
TEST(IcecastMetadataHttpTest, a_refused_login_says_so_without_the_password) {
    OneShotServer server("HTTP/1.1 401 Authentication Required\r\nConnection: Keep-Alive\r\nContent-Length: 0\r\n\r\n");
    auto t = http_target(server.port());
    t->password = "s3cret-pw";
    string error;
    auto start = chrono::steady_clock::now();
    EXPECT_FALSE(icecast_metadata_send_http(*t, "Unit 12", &error));
    EXPECT_LT(chrono::steady_clock::now() - start, chrono::seconds(2));
    EXPECT_EQ(error, "the server answered HTTP/1.1 401 Authentication Required");
    EXPECT_EQ(error.find("s3cret-pw"), string::npos);
}

TEST(IcecastMetadataHttpTest, no_server_listening) {
    int port;
    {
        OneShotServer server("");
        port = server.port();
        // Connect once so the server thread finishes, then the port is closed.
        auto t = http_target(port);
        string ignored;
        icecast_metadata_send_http(*t, "x", &ignored);
    }
    string error;
    EXPECT_FALSE(icecast_metadata_send_http(*http_target(port), "x", &error));
    EXPECT_EQ(error.rfind("can't connect: ", 0), 0u) << error;
}
