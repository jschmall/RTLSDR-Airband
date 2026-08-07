/*
 * mixer_remote_wire.h
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

#ifndef _MIXER_REMOTE_WIRE_H
#define _MIXER_REMOTE_WIRE_H

#include <cstddef>
#include <cstdint>

// Wire protocol for streaming one mixer input's audio from a channel in one
// rtl_airband process instance to a mixer in a different instance, over an AF_UNIX
// SOCK_DGRAM socket on the same host (see mixer_remote.cpp for the socket I/O - this
// header/its .cpp are pure encode/decode logic, no I/O, no globals, so they can be
// unit tested directly without any socket).
//
// One datagram == one WAVE_BATCH-sized tick of mono float32 PCM: a fixed 32-byte
// header immediately followed by the raw sample payload. Mono only, since a mixer
// input (mixinput_t::wavein) has no right-channel counterpart to send.

#pragma pack(push, 1)
struct mixer_remote_packet_header {
    uint32_t magic;         // MIXER_REMOTE_MAGIC - wire-format sanity check
    uint16_t version;       // MIXER_REMOTE_VERSION
    uint16_t format;        // a mixer_remote_format value
    uint32_t sample_rate;   // sender's WAVE_RATE; receiver rejects a mismatch (no resampling on this path)
    uint32_t stream_id;     // selects which of the receiver's reserved mixer input slots this targets
    uint64_t seq;           // monotonically increasing per (listen_path, stream_id); detects drop/reorder/dup
    uint32_t sample_count;  // number of samples in the payload; receiver rejects a mismatch against its own WAVE_BATCH
    uint32_t flags;         // bit 0 (MIXER_REMOTE_FLAG_HAS_SIGNAL) mirrors the source channel's squelch state
};
#pragma pack(pop)
static_assert(sizeof(mixer_remote_packet_header) == 32, "wire format size is part of the protocol contract");

constexpr uint32_t MIXER_REMOTE_MAGIC = 0x52414D58;  // "RAMX"
constexpr uint16_t MIXER_REMOTE_VERSION = 1;
constexpr uint32_t MIXER_REMOTE_FLAG_HAS_SIGNAL = 0x1;

// format field values. Only FLOAT32 is implemented by mixer_remote_encode_packet()/
// mixer_remote_decode_payload() for now - same-host loopback has no real bandwidth
// constraint, so narrower formats (mirroring udp_stream_format in rtl_airband.h) are
// deferred until a concrete reason to want them shows up. The field exists so they
// can be added later without a wire format version bump.
enum mixer_remote_format : uint16_t {
    MIXER_REMOTE_FORMAT_FLOAT32 = 0,
};

// Encodes a header + float32 payload into out_buf. Returns the total number of bytes
// written (header + sample_count * sizeof(float)), or 0 if out_buf/samples is NULL or
// out_capacity is too small to hold the whole packet - never writes past out_capacity.
size_t mixer_remote_encode_packet(uint32_t sample_rate, uint32_t stream_id, uint64_t seq, bool has_signal, const float* samples, uint32_t sample_count, uint8_t* out_buf, size_t out_capacity);

// Validates magic/version and parses the header out of buf (len bytes available).
// Returns false (leaving *out untouched) if len is shorter than a header, or the
// magic/version don't match - never reads past len.
bool mixer_remote_decode_header(const uint8_t* buf, size_t len, mixer_remote_packet_header* out);

// Decodes the payload following an already-validated header (hdr) into out_samples
// (out_capacity floats). Returns the number of samples decoded, or 0 if: hdr.format
// isn't MIXER_REMOTE_FORMAT_FLOAT32, hdr.sample_count exceeds out_capacity, or buf/len
// doesn't actually contain hdr.sample_count floats after the header - never reads past
// len or writes past out_capacity.
size_t mixer_remote_decode_payload(const uint8_t* buf, size_t len, const mixer_remote_packet_header& hdr, float* out_samples, size_t out_capacity);

// Classification of a newly-received sequence number against the last one seen for a
// given (listen_path, stream_id) mapping. Pure - the caller owns the "have I seen a
// packet yet" / "what was the last seq" state; this only compares.
enum class mixer_remote_seq_class {
    kFirst,               // no prior sequence observed yet for this mapping
    kInOrder,             // new_seq == last_seq + 1
    kGap,                 // new_seq > last_seq + 1 - one or more packets presumed lost
    kDuplicateOrReorder,  // new_seq <= last_seq
};
mixer_remote_seq_class mixer_remote_classify_seq(bool have_last_seq, uint64_t last_seq, uint64_t new_seq);

#endif /* _MIXER_REMOTE_WIRE_H */
