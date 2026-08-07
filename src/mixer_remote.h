/*
 * mixer_remote.h
 * Cross-instance mixer input: send-side socket I/O and receive-side listener
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

#ifndef _MIXER_REMOTE_H
#define _MIXER_REMOTE_H

#include <pthread.h>
#include <sys/types.h>  // uid_t
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "rtl_airband.h"  // mixer_remote_send_data, mixer_t

// ---- send side ----

// Allocates the scratch send buffer (sized for one packet's worth of `len` samples) and opens
// the AF_UNIX SOCK_DGRAM socket. Unlike udp_stream_init(), this never connect()s the socket: an
// AF_UNIX SOCK_DGRAM connect() requires the target socket file to already exist, but the
// receiving instance may start (or gain a matching remote_inputs entry via reload_diff) after
// this one does - a hard init failure here would make init_output() fail, which is fatal at
// startup (see rtl_airband.cpp's init_output() and CLAUDE.md item 15). Every packet instead
// uses sendto() with the destination address supplied explicitly - see mixer_remote_send()'s
// comment for how a not-yet-existing receiver is handled per-packet. Only fails on a genuine
// local error: socket() failing, or dest_path too long to fit in a sockaddr_un.
bool mixer_remote_send_init(mixer_remote_send_data* rdata, size_t len);

// Encodes and sends one tick (len samples) of audio. A no-op if mixer_remote_send_init() was
// never called or failed (rdata->send_socket == -1). Never blocks (MSG_DONTWAIT); any failure -
// len exceeding the scratch buffer's capacity, or the sendto() itself (e.g. ENOENT because the
// receiver isn't listening yet, or ECONNREFUSED if it stopped without cleaning up its socket) -
// is counted in rdata->dropped_packet_count, never fatal, and not logged per-packet so a
// sibling instance being down doesn't flood syslog.
void mixer_remote_send(mixer_remote_send_data* rdata, const float* samples, size_t len, bool has_signal);

void mixer_remote_send_shutdown(mixer_remote_send_data* rdata);

// ---- receive side ----

// One (listen_path, stream_id) -> (mixer, slot) mapping, plus its own observability counters.
// Only the listener's single receive thread ever writes these counters (mixer_remote_recv_start()
// starts exactly one thread per listener) - write_stats_file() (output.cpp) reads them from a
// different thread with no lock, the same established convention as every other per-output size_t
// counter in this codebase (dropped_packet_count, output_overrun_count, etc - see CLAUDE.md items
// 22/23): eventually-consistent reads of a plain size_t are accepted here, same as everywhere else.
struct mixer_remote_route_t {
    uint32_t stream_id = 0;
    mixer_t* mixer = nullptr;
    int slot_idx = -1;

    bool have_last_seq = false;
    uint64_t last_seq = 0;
    time_t last_packet_time = 0;  // 0 = no packet ever received for this route - lets an operator
                                  // tell "sender alive, channel quiet" apart from "sender/process
                                  // gone", which mixer_thread()'s existing silence-fill alone can't

    // decode_payload() failed for a packet already resolved to this route (format/length
    // mismatch) - distinct from the listener-level malformed_header_count below, which covers a
    // packet whose header itself couldn't be trusted enough to identify a route at all.
    size_t malformed_payload_count = 0;
    size_t rate_mismatch_count = 0;
    size_t sample_count_mismatch_count = 0;
    size_t seq_gap_count = 0;
    size_t seq_reorder_or_duplicate_count = 0;
};

// One AF_UNIX SOCK_DGRAM listen path, potentially multiplexing several remote_inputs entries
// (routes) via the packet header's stream_id.
struct mixer_remote_listener_t {
    std::string listen_path;
    std::vector<mixer_remote_route_t> routes;

    int fd = -1;
    pthread_t thread{};
    bool thread_started = false;

    size_t rejected_uid_count = 0;
    size_t unknown_stream_count = 0;
    size_t malformed_header_count = 0;
};

// Process-wide registry, built up by parse_mixers() (config.cpp) during startup config parsing -
// the same single-threaded window mixer_finalize_capacity() (mixer.cpp) already relies on - and
// consumed by mixer_remote_recv_start(). A plain global, not a file-static, for the same reason
// mixer_capacity_finalized (rtl_airband.h) is: the unittests binary links every test_*.cpp into
// one process, and tests need to reset this between cases via mixer_remote_reset_registry().
extern std::vector<mixer_remote_listener_t*> mixer_remote_listeners;

// Finds the listener for listen_path, creating and registering it if this is the first
// remote_inputs entry seen for that path. Never fails.
mixer_remote_listener_t* mixer_remote_get_or_create_listener(const std::string& listen_path);

// Adds a route to listener for stream_id -> (mixer, slot_idx). Returns false (leaving the
// listener's routes unchanged) if stream_id is already registered on this listener - two
// remote_inputs entries sharing a listen_path must have distinct stream_id values. config.cpp's
// parse_mixers() checks this itself before consuming a mixer input slot via
// mixer_connect_input(), so in practice this duplicate check is a second line of defense; it's
// exercised directly by unit tests.
bool mixer_remote_register_route(mixer_remote_listener_t* listener, uint32_t stream_id, mixer_t* mixer, int slot_idx);

// Frees every registered listener and clears the registry. Only safe to call before
// mixer_remote_recv_start() (or after mixer_remote_recv_shutdown()) - does not touch any socket
// or thread. Exposed for unit tests to reset state between cases; production startup never calls
// this (the registry is only ever built once, at parse time, and lives for the process lifetime).
void mixer_remote_reset_registry();

// Binds a socket and starts one receive thread for every registered listener that has at least
// one route. A bind()/setsockopt() failure is logged and that listener is skipped (its routes
// stay permanently silence-filled, same as any input whose producer never shows up) rather than
// treated as fatal - unlike init_output()'s per-channel failures, a remote_inputs listener is an
// optional, best-effort service in the same spirit as control_socket_start()/stats_http_start(),
// and a bind failure on one listener shouldn't take down a mixer's other, working inputs/outputs.
void mixer_remote_recv_start();

// Signals every listener thread to stop, joins them, closes their sockets, and unlinks their
// socket files. Idempotent - safe to call even if mixer_remote_recv_start() was never called, or
// if a given listener never actually started (its bind() failed).
void mixer_remote_recv_shutdown();

// Handles one already-received datagram for `listener`: validates magic/version/rate/sample
// count, looks up the target route by stream_id, decodes the payload, classifies the sequence
// number, updates every counter above, and (on a fully successful decode) calls
// mixer_put_samples(). sender_uid is the credential already extracted from SCM_CREDENTIALS by the
// caller (mixer_remote_recv_start()'s listener thread) - passed in rather than re-extracted here
// so this function has no socket/cmsg dependency and can be unit tested directly by constructing
// a listener and calling this with an encoded buffer, mirroring
// control_socket_dispatch_command_line()'s testability shape (control_socket.h).
void mixer_remote_dispatch_packet(mixer_remote_listener_t* listener, const uint8_t* buf, size_t len, uid_t sender_uid);

#endif /* _MIXER_REMOTE_H */
