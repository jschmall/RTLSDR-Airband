/*
 * test_udp_stream.cpp
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rtl_airband.h"

using namespace std;

// udp_stream.cpp calls XCALLOC (-> xcalloc), which is normally defined in util.cpp;
// stub it here rather than link in util.cpp, which pulls in unrelated globals (fft_size).
void* xcalloc(size_t nmemb, size_t size, const char*, const int, const char*) {
    return calloc(nmemb, size);
}

// len passed to udp_stream_init()/udp_stream_write() is a sample count, not a byte
// count - this test exists because a unit mismatch here once caused every packet
// to carry 4x the intended payload (see src/udp_stream.cpp).
class UdpStreamTest : public TestBaseClass {
   protected:
    void SetUp(void) override {
        TestBaseClass::SetUp();

        listen_socket = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_NE(listen_socket, -1);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        ASSERT_EQ(setsockopt(listen_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;  // let the OS pick a free port
        ASSERT_EQ(bind(listen_socket, (struct sockaddr*)&addr, sizeof(addr)), 0);

        socklen_t addr_len = sizeof(addr);
        ASSERT_EQ(getsockname(listen_socket, (struct sockaddr*)&addr, &addr_len), 0);
        port = to_string(ntohs(addr.sin_port));

        memset(&sdata, 0, sizeof(sdata));
        sdata.dest_address = "127.0.0.1";
        sdata.dest_port = port.c_str();
    }

    void TearDown(void) override {
        udp_stream_shutdown(&sdata);
        free(sdata.stereo_buffer);
        free(sdata.convert_buffer);
        close(listen_socket);
        TestBaseClass::TearDown();
    }

    // returns the number of bytes received, or -1 on timeout/error
    ssize_t recv_packet(void* buf, size_t buf_len) { return recvfrom(listen_socket, buf, buf_len, 0, NULL, NULL); }

    int listen_socket;
    string port;
    udp_stream_data sdata;
};

TEST_F(UdpStreamTest, mono_sends_exact_sample_count_in_bytes) {
    const size_t len = 4;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));

    float data[len] = {1.0f, 2.0f, 3.0f, 4.0f};
    udp_stream_write(&sdata, data, len);

    float received[len + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * sizeof(float)));
    for (size_t i = 0; i < len; ++i) {
        EXPECT_FLOAT_EQ(received[i], data[i]);
    }
}

TEST_F(UdpStreamTest, stereo_sends_exact_interleaved_sample_count_in_bytes) {
    const size_t len = 4;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_STEREO, len));
    ASSERT_EQ(sdata.stereo_buffer_len, len * 2);

    float left[len] = {1.0f, 2.0f, 3.0f, 4.0f};
    float right[len] = {-1.0f, -2.0f, -3.0f, -4.0f};
    udp_stream_write(&sdata, left, right, len);

    float received[len * 2 + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * 2 * sizeof(float)));
    for (size_t i = 0; i < len; ++i) {
        EXPECT_FLOAT_EQ(received[2 * i], left[i]);
        EXPECT_FLOAT_EQ(received[2 * i + 1], right[i]);
    }
}

TEST_F(UdpStreamTest, no_socket_does_not_crash) {
    // never call udp_stream_init(), so send_socket stays at its zero-initialized value
    sdata.send_socket = -1;
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    udp_stream_write(&sdata, data, 4);
}

TEST_F(UdpStreamTest, mono_s16le_sends_exact_byte_count_and_clamped_values) {
    const size_t len = 4;
    sdata.format = STREAM_FORMAT_S16LE;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));

    float data[len] = {0.5f, -0.5f, 1.5f, -2.0f};  // last two exercise clamping to [-1,1]
    udp_stream_write(&sdata, data, len);

    int16_t received[len + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * sizeof(int16_t)));
    EXPECT_NEAR(received[0], 16384, 1);
    EXPECT_NEAR(received[1], -16384, 1);
    EXPECT_EQ(received[2], 32767);   // clamped from 1.5f
    EXPECT_EQ(received[3], -32767);  // clamped from -2.0f
}

TEST_F(UdpStreamTest, stereo_s16le_sends_exact_interleaved_byte_count) {
    const size_t len = 4;
    sdata.format = STREAM_FORMAT_S16LE;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_STEREO, len));
    ASSERT_EQ(sdata.convert_buffer_len, len * 2);

    float left[len] = {0.5f, 0.5f, 0.5f, 0.5f};
    float right[len] = {-0.5f, -0.5f, -0.5f, -0.5f};
    udp_stream_write(&sdata, left, right, len);

    int16_t received[len * 2 + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * 2 * sizeof(int16_t)));
    for (size_t i = 0; i < len; ++i) {
        EXPECT_NEAR(received[2 * i], 16384, 1);
        EXPECT_NEAR(received[2 * i + 1], -16384, 1);
    }
}

TEST_F(UdpStreamTest, mono_s8_sends_exact_byte_count_and_clamped_values) {
    const size_t len = 4;
    sdata.format = STREAM_FORMAT_S8;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));

    float data[len] = {0.4f, -0.4f, 1.5f, -2.0f};  // last two exercise clamping to [-1,1]
    udp_stream_write(&sdata, data, len);

    int8_t received[len + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * sizeof(int8_t)));
    EXPECT_NEAR(received[0], 51, 1);
    EXPECT_NEAR(received[1], -51, 1);
    EXPECT_EQ(received[2], 127);   // clamped from 1.5f
    EXPECT_EQ(received[3], -127);  // clamped from -2.0f
}

TEST_F(UdpStreamTest, stereo_s8_sends_exact_interleaved_byte_count) {
    const size_t len = 4;
    sdata.format = STREAM_FORMAT_S8;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_STEREO, len));
    ASSERT_EQ(sdata.convert_buffer_len, len * 2);

    float left[len] = {0.4f, 0.4f, 0.4f, 0.4f};
    float right[len] = {-0.4f, -0.4f, -0.4f, -0.4f};
    udp_stream_write(&sdata, left, right, len);

    int8_t received[len * 2 + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    ASSERT_EQ(bytes, (ssize_t)(len * 2 * sizeof(int8_t)));
    for (size_t i = 0; i < len; ++i) {
        EXPECT_NEAR(received[2 * i], 51, 1);
        EXPECT_NEAR(received[2 * i + 1], -51, 1);
    }
}
