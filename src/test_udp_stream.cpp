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

TEST_F(UdpStreamTest, mono_s16le_oversized_len_does_not_overflow_buffer) {
    // These bounds checks used to be assert(), which is compiled out by -DNDEBUG in the
    // default Release build - so a len/buffer-size mismatch (the same bug class behind
    // this fork's historical 4x-oversend incident) would silently overrun convert_buffer
    // in a shipped binary. Confirm the check is a real runtime guard, not just a debug-mode
    // assertion: no crash, and nothing sent for the oversized call.
    const size_t len = 4;
    sdata.format = STREAM_FORMAT_S16LE;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));

    float oversized_data[len * 10];
    for (size_t i = 0; i < len * 10; ++i)
        oversized_data[i] = 0.5f;
    udp_stream_write(&sdata, oversized_data, len * 10);

    int16_t received[len * 10 + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));
    EXPECT_EQ(bytes, -1);  // nothing sent - the write was dropped, not truncated or overflowed
}

TEST_F(UdpStreamTest, stereo_oversized_len_does_not_overflow_buffer) {
    const size_t len = 4;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_STEREO, len));

    float left[len * 10];
    float right[len * 10];
    for (size_t i = 0; i < len * 10; ++i) {
        left[i] = 0.5f;
        right[i] = -0.5f;
    }
    udp_stream_write(&sdata, left, right, len * 10);

    float received[len * 20 + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));
    EXPECT_EQ(bytes, -1);  // nothing sent - the write was dropped, not truncated or overflowed
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

// --- resample_linear() ---------------------------------------------------

TEST(ResampleLinearTest, identity_when_rates_match) {
    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float out[4];
    size_t n = resample_linear(in, 4, 8000, 8000, out, 4);
    ASSERT_EQ(n, 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(out[i], in[i]);
    }
}

TEST(ResampleLinearTest, downsamples_2to1_by_picking_every_other_sample) {
    // 16000 -> 8000 is exactly the NFM-build-to-trunkrecorder case this feature
    // targets. With a clean 2:1 ratio every output sample lands exactly on an
    // input sample (no fractional blending), so this is exact, not approximate.
    float in[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    float out[3];
    size_t n = resample_linear(in, 6, 16000, 8000, out, 3);
    ASSERT_EQ(n, 3u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 3.0f);
    EXPECT_FLOAT_EQ(out[2], 5.0f);
}

TEST(ResampleLinearTest, upsamples_1to2_by_interpolating_between_samples) {
    float in[3] = {0.0f, 10.0f, 20.0f};
    float out[6];
    size_t n = resample_linear(in, 3, 8000, 16000, out, 6);
    ASSERT_EQ(n, 6u);
    EXPECT_FLOAT_EQ(out[0], 0.0f);   // in[0]
    EXPECT_FLOAT_EQ(out[1], 5.0f);   // midpoint of in[0], in[1]
    EXPECT_FLOAT_EQ(out[2], 10.0f);  // in[1]
    EXPECT_FLOAT_EQ(out[3], 15.0f);  // midpoint of in[1], in[2]
    EXPECT_FLOAT_EQ(out[4], 20.0f);  // in[2]
    EXPECT_FLOAT_EQ(out[5], 20.0f);  // past the end - clamped to the last sample
}

TEST(ResampleLinearTest, truncates_to_out_capacity) {
    float in[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    float out[2];
    size_t n = resample_linear(in, 6, 16000, 8000, out, 2);
    ASSERT_EQ(n, 2u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 3.0f);
}

TEST(ResampleLinearTest, handles_zero_length_input) {
    float out[4];
    EXPECT_EQ(resample_linear(nullptr, 0, 16000, 8000, out, 4), 0u);
}

TEST(ResampleLinearTest, handles_invalid_rates) {
    float in[2] = {1.0f, 2.0f};
    float out[4];
    EXPECT_EQ(resample_linear(in, 2, 0, 8000, out, 4), 0u);
    EXPECT_EQ(resample_linear(in, 2, 16000, 0, out, 4), 0u);
}

// --- udp_stream_write() with a configured sample_rate ---------------------

TEST_F(UdpStreamTest, mono_downsamples_before_sending_when_sample_rate_configured) {
    const size_t len = 4;
    sdata.sample_rate = WAVE_RATE / 2;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));
    ASSERT_EQ(sdata.resample_buffer_len, len / 2);

    float data[len] = {1.0f, 2.0f, 3.0f, 4.0f};
    udp_stream_write(&sdata, data, len);

    float received[len + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    // half the samples, at half the "rate" - the resampler picks every other
    // input sample for a clean 2:1 ratio (see ResampleLinearTest above)
    ASSERT_EQ(bytes, (ssize_t)((len / 2) * sizeof(float)));
    EXPECT_FLOAT_EQ(received[0], 1.0f);
    EXPECT_FLOAT_EQ(received[1], 3.0f);
}

TEST_F(UdpStreamTest, stereo_downsamples_each_channel_independently) {
    const size_t len = 4;
    sdata.sample_rate = WAVE_RATE / 2;
    ASSERT_TRUE(udp_stream_init(&sdata, MM_STEREO, len));
    ASSERT_EQ(sdata.resample_stereo_buffer_len, len / 2);

    float left[len] = {1.0f, 2.0f, 3.0f, 4.0f};
    float right[len] = {-1.0f, -2.0f, -3.0f, -4.0f};
    udp_stream_write(&sdata, left, right, len);

    float received[len + 100];
    ssize_t bytes = recv_packet(received, sizeof(received));

    // 2 output samples per channel, interleaved - confirms resampling each
    // channel separately (before interleaving) didn't blend left into right
    ASSERT_EQ(bytes, (ssize_t)(len * sizeof(float)));  // (len/2 samples) * 2 channels
    EXPECT_FLOAT_EQ(received[0], 1.0f);                // left[0]
    EXPECT_FLOAT_EQ(received[1], -1.0f);               // right[0]
    EXPECT_FLOAT_EQ(received[2], 3.0f);                // left[2]
    EXPECT_FLOAT_EQ(received[3], -3.0f);               // right[2]
}

TEST_F(UdpStreamTest, unset_sample_rate_defaults_to_wave_rate_no_resampling) {
    const size_t len = 4;
    ASSERT_EQ(sdata.sample_rate, 0);  // zero-initialized in SetUp, like a real config default
    ASSERT_TRUE(udp_stream_init(&sdata, MM_MONO, len));
    EXPECT_EQ(sdata.sample_rate, WAVE_RATE);
    EXPECT_EQ(sdata.resample_buffer, nullptr);
}
