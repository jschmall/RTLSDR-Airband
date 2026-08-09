/*
 * mixer_remote_wire.cpp
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

#include "mixer_remote_wire.h"

#include <cstring>

size_t mixer_remote_encode_packet(uint32_t sample_rate, uint32_t stream_id, uint64_t seq, bool has_signal, const float* samples, uint32_t sample_count, uint8_t* out_buf, size_t out_capacity) {
    if (out_buf == nullptr || (samples == nullptr && sample_count > 0)) {
        return 0;
    }

    const size_t payload_len = (size_t)sample_count * sizeof(float);
    const size_t total_len = sizeof(mixer_remote_packet_header) + payload_len;
    if (out_capacity < total_len) {
        return 0;
    }

    mixer_remote_packet_header hdr{};
    hdr.magic = MIXER_REMOTE_MAGIC;
    hdr.version = MIXER_REMOTE_VERSION;
    hdr.format = MIXER_REMOTE_FORMAT_FLOAT32;
    hdr.sample_rate = sample_rate;
    hdr.stream_id = stream_id;
    hdr.seq = seq;
    hdr.sample_count = sample_count;
    hdr.flags = has_signal ? MIXER_REMOTE_FLAG_HAS_SIGNAL : 0;

    memcpy(out_buf, &hdr, sizeof(hdr));
    if (payload_len > 0) {
        memcpy(out_buf + sizeof(hdr), samples, payload_len);
    }
    return total_len;
}

bool mixer_remote_decode_header(const uint8_t* buf, size_t len, mixer_remote_packet_header* out) {
    if (buf == nullptr || out == nullptr || len < sizeof(mixer_remote_packet_header)) {
        return false;
    }
    mixer_remote_packet_header hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != MIXER_REMOTE_MAGIC || hdr.version != MIXER_REMOTE_VERSION) {
        return false;
    }
    *out = hdr;
    return true;
}

size_t mixer_remote_decode_payload(const uint8_t* buf, size_t len, const mixer_remote_packet_header& hdr, float* out_samples, size_t out_capacity) {
    if (buf == nullptr || out_samples == nullptr) {
        return 0;
    }
    if (hdr.format != MIXER_REMOTE_FORMAT_FLOAT32) {
        return 0;
    }
    // Checked before the multiplication below so an attacker-controlled sample_count
    // can never make payload_len overflow past what out_capacity already bounds it to.
    if (hdr.sample_count > out_capacity) {
        return 0;
    }
    const size_t payload_len = (size_t)hdr.sample_count * sizeof(float);
    if (len < sizeof(mixer_remote_packet_header) + payload_len) {
        return 0;
    }
    memcpy(out_samples, buf + sizeof(mixer_remote_packet_header), payload_len);
    return hdr.sample_count;
}

mixer_remote_seq_class mixer_remote_classify_seq(bool have_last_seq, uint64_t last_seq, uint64_t new_seq) {
    if (!have_last_seq) {
        return mixer_remote_seq_class::kFirst;
    }
    if (new_seq == last_seq + 1) {
        return mixer_remote_seq_class::kInOrder;
    }
    if (new_seq > last_seq + 1) {
        return mixer_remote_seq_class::kGap;
    }
    return mixer_remote_seq_class::kDuplicateOrReorder;
}
