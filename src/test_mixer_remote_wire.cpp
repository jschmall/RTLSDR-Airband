/*
 * test_mixer_remote_wire.cpp
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

#include <cstdint>
#include <cstring>
#include <vector>

#include "mixer_remote_wire.h"

using namespace std;

class MixerRemoteWireTest : public TestBaseClass {};

TEST_F(MixerRemoteWireTest, encode_decode_round_trip_preserves_everything) {
    const uint32_t sample_count = 4;
    float samples[sample_count] = {1.0f, -0.5f, 0.25f, -1.0f};
    uint8_t buf[sizeof(mixer_remote_packet_header) + sample_count * sizeof(float)];

    size_t written = mixer_remote_encode_packet(16000, 7, 42, true, samples, sample_count, buf, sizeof(buf));
    ASSERT_EQ(written, sizeof(buf));

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, written, &hdr));
    EXPECT_EQ(hdr.sample_rate, 16000u);
    EXPECT_EQ(hdr.stream_id, 7u);
    EXPECT_EQ(hdr.seq, 42u);
    EXPECT_EQ(hdr.sample_count, sample_count);
    EXPECT_EQ(hdr.flags & MIXER_REMOTE_FLAG_HAS_SIGNAL, MIXER_REMOTE_FLAG_HAS_SIGNAL);
    EXPECT_EQ(hdr.format, MIXER_REMOTE_FORMAT_FLOAT32);

    float decoded[sample_count];
    ASSERT_EQ(mixer_remote_decode_payload(buf, written, hdr, decoded, sample_count), sample_count);
    for (uint32_t i = 0; i < sample_count; ++i) {
        EXPECT_FLOAT_EQ(decoded[i], samples[i]);
    }
}

TEST_F(MixerRemoteWireTest, has_signal_false_clears_flag) {
    float samples[1] = {0.0f};
    uint8_t buf[sizeof(mixer_remote_packet_header) + sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, false, samples, 1, buf, sizeof(buf)), 0u);

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
    EXPECT_EQ(hdr.flags & MIXER_REMOTE_FLAG_HAS_SIGNAL, 0u);
}

TEST_F(MixerRemoteWireTest, zero_sample_count_encodes_header_only) {
    uint8_t buf[sizeof(mixer_remote_packet_header)];
    size_t written = mixer_remote_encode_packet(8000, 1, 0, false, nullptr, 0, buf, sizeof(buf));
    EXPECT_EQ(written, sizeof(mixer_remote_packet_header));
}

TEST_F(MixerRemoteWireTest, encode_fails_when_buffer_too_small) {
    float samples[4] = {0, 0, 0, 0};
    uint8_t buf[sizeof(mixer_remote_packet_header) + 4 * sizeof(float) - 1];  // one byte short
    EXPECT_EQ(mixer_remote_encode_packet(8000, 1, 1, true, samples, 4, buf, sizeof(buf)), 0u);
}

TEST_F(MixerRemoteWireTest, decode_header_rejects_short_buffer) {
    uint8_t buf[sizeof(mixer_remote_packet_header) - 1];
    memset(buf, 0, sizeof(buf));
    mixer_remote_packet_header hdr;
    EXPECT_FALSE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
}

TEST_F(MixerRemoteWireTest, decode_header_rejects_bad_magic) {
    float samples[1] = {1.0f};
    uint8_t buf[sizeof(mixer_remote_packet_header) + sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, true, samples, 1, buf, sizeof(buf)), 0u);

    // corrupt the magic field (first 4 bytes)
    buf[0] ^= 0xFF;

    mixer_remote_packet_header hdr;
    EXPECT_FALSE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
}

TEST_F(MixerRemoteWireTest, decode_header_rejects_wrong_version) {
    float samples[1] = {1.0f};
    uint8_t buf[sizeof(mixer_remote_packet_header) + sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, true, samples, 1, buf, sizeof(buf)), 0u);

    mixer_remote_packet_header* hdr_in_place = reinterpret_cast<mixer_remote_packet_header*>(buf);
    hdr_in_place->version = MIXER_REMOTE_VERSION + 1;

    mixer_remote_packet_header hdr;
    EXPECT_FALSE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
}

TEST_F(MixerRemoteWireTest, decode_payload_rejects_unsupported_format) {
    float samples[1] = {1.0f};
    uint8_t buf[sizeof(mixer_remote_packet_header) + sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, true, samples, 1, buf, sizeof(buf)), 0u);

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));
    hdr.format = 0xBEEF;  // not MIXER_REMOTE_FORMAT_FLOAT32

    float decoded[1];
    EXPECT_EQ(mixer_remote_decode_payload(buf, sizeof(buf), hdr, decoded, 1), 0u);
}

TEST_F(MixerRemoteWireTest, decode_payload_rejects_sample_count_over_capacity) {
    float samples[4] = {1, 2, 3, 4};
    uint8_t buf[sizeof(mixer_remote_packet_header) + 4 * sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, true, samples, 4, buf, sizeof(buf)), 0u);

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));

    float decoded[2];  // smaller than hdr.sample_count (4)
    EXPECT_EQ(mixer_remote_decode_payload(buf, sizeof(buf), hdr, decoded, 2), 0u);
}

TEST_F(MixerRemoteWireTest, decode_payload_rejects_truncated_buffer_never_reads_oob) {
    float samples[4] = {1, 2, 3, 4};
    uint8_t buf[sizeof(mixer_remote_packet_header) + 4 * sizeof(float)];
    ASSERT_GT(mixer_remote_encode_packet(8000, 1, 1, true, samples, 4, buf, sizeof(buf)), 0u);

    mixer_remote_packet_header hdr;
    ASSERT_TRUE(mixer_remote_decode_header(buf, sizeof(buf), &hdr));

    // Simulate a datagram that claims sample_count=4 but the actual received buffer
    // (as recvmsg() would report via its return value) is shorter than the header
    // says the payload should be - must reject, not read past `len`.
    float decoded[4];
    EXPECT_EQ(mixer_remote_decode_payload(buf, sizeof(buf) - 1, hdr, decoded, 4), 0u);
}

TEST_F(MixerRemoteWireTest, decode_payload_huge_claimed_sample_count_does_not_overflow) {
    // A malicious/corrupt header claiming an enormous sample_count must be rejected by
    // the out_capacity check before any multiplication that could overflow - this is
    // the same bounds-check discipline udp_stream_write() uses (item 11 in CLAUDE.md).
    uint8_t buf[sizeof(mixer_remote_packet_header)];
    mixer_remote_packet_header hdr{};
    hdr.magic = MIXER_REMOTE_MAGIC;
    hdr.version = MIXER_REMOTE_VERSION;
    hdr.format = MIXER_REMOTE_FORMAT_FLOAT32;
    hdr.sample_count = 0xFFFFFFFFu;
    memcpy(buf, &hdr, sizeof(hdr));

    float decoded[4];
    EXPECT_EQ(mixer_remote_decode_payload(buf, sizeof(buf), hdr, decoded, 4), 0u);
}

TEST_F(MixerRemoteWireTest, classify_seq_first_packet) {
    EXPECT_EQ(mixer_remote_classify_seq(false, 0, 5), mixer_remote_seq_class::kFirst);
}

TEST_F(MixerRemoteWireTest, classify_seq_in_order) {
    EXPECT_EQ(mixer_remote_classify_seq(true, 10, 11), mixer_remote_seq_class::kInOrder);
}

TEST_F(MixerRemoteWireTest, classify_seq_gap) {
    EXPECT_EQ(mixer_remote_classify_seq(true, 10, 15), mixer_remote_seq_class::kGap);
}

TEST_F(MixerRemoteWireTest, classify_seq_duplicate) {
    EXPECT_EQ(mixer_remote_classify_seq(true, 10, 10), mixer_remote_seq_class::kDuplicateOrReorder);
}

TEST_F(MixerRemoteWireTest, classify_seq_reorder_older) {
    EXPECT_EQ(mixer_remote_classify_seq(true, 10, 3), mixer_remote_seq_class::kDuplicateOrReorder);
}
