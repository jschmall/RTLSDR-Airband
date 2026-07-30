/*
 * test_stats_http.cpp
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "rtl_airband.h"

using namespace std;

// stats_http.cpp references these as extern; normally defined in rtl_airband.cpp,
// which can't be linked here (its main() would conflict with gtest's) - matches
// the xcalloc-stub pattern in test_udp_stream.cpp for the same reason.
char* stats_filepath = nullptr;
char* stats_http_address = nullptr;
int stats_http_port = 0;

namespace {

// finds a free ephemeral port by briefly binding to port 0, then releasing it -
// stats_http_start() needs a specific port number up front (it doesn't expose
// the bound port back to the caller), unlike the raw-socket test fixtures
// elsewhere in this codebase that bind(0) directly and keep the socket open
int find_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

string http_get(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((uint16_t)port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return "";
    }
    const char* req = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, req, strlen(req), 0);

    string response;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, n);
    }
    close(fd);
    return response;
}

}  // namespace

class StatsHttpTest : public TestBaseClass {
   protected:
    void SetUp(void) override {
        TestBaseClass::SetUp();
        stats_filepath_str = temp_dir + "/stats.txt";
        FILE* f = fopen(stats_filepath_str.c_str(), "w");
        ASSERT_NE(f, nullptr);
        fputs("channel_activity_counter{freq=\"120.025\"}\t5\n", f);
        fclose(f);

        stats_filepath = strdup(stats_filepath_str.c_str());
        address_str = "127.0.0.1";
        stats_http_address = strdup(address_str.c_str());
        stats_http_port = find_free_port();
    }

    void TearDown(void) override {
        stats_http_shutdown();
        free(stats_filepath);
        free(stats_http_address);
        stats_filepath = nullptr;
        stats_http_address = nullptr;
        stats_http_port = 0;
        TestBaseClass::TearDown();
    }

    string stats_filepath_str;
    string address_str;
};

TEST_F(StatsHttpTest, serves_current_stats_file_content) {
    stats_http_start();

    string response = http_get(stats_http_port);

    ASSERT_NE(response.find("HTTP/1.1 200 OK"), string::npos);
    ASSERT_NE(response.find("Content-Type: text/plain"), string::npos);
    ASSERT_NE(response.find("channel_activity_counter{freq=\"120.025\"}\t5"), string::npos);
}

TEST_F(StatsHttpTest, reflects_updated_file_content_on_next_scrape) {
    stats_http_start();

    string first = http_get(stats_http_port);
    ASSERT_NE(first.find("\t5"), string::npos);

    FILE* f = fopen(stats_filepath_str.c_str(), "w");
    ASSERT_NE(f, nullptr);
    fputs("channel_activity_counter{freq=\"120.025\"}\t9\n", f);
    fclose(f);

    string second = http_get(stats_http_port);
    ASSERT_NE(second.find("\t9"), string::npos);
}

TEST_F(StatsHttpTest, handles_multiple_sequential_requests) {
    stats_http_start();

    for (int i = 0; i < 3; ++i) {
        string response = http_get(stats_http_port);
        ASSERT_NE(response.find("HTTP/1.1 200 OK"), string::npos) << "request " << i;
    }
}

TEST_F(StatsHttpTest, shutdown_is_prompt_and_idempotent) {
    stats_http_start();
    ASSERT_NE(http_get(stats_http_port).find("200 OK"), string::npos);

    stats_http_shutdown();
    stats_http_shutdown();  // should be a no-op, not a crash/hang
}
