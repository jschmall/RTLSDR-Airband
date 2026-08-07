/*
 * mixer_remote.cpp
 *
 * Same-host-only trust model, mirroring control_socket.cpp: the listener socket file is created
 * with restrictive permissions (0600). SO_PASSCRED/SCM_CREDENTIALS is checked per-datagram
 * (the connectionless analog of control_socket.cpp's SO_PEERCRED, which only works on a
 * connected/SOCK_STREAM socket) so a sender must share this process's own UID.
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

#include "mixer_remote.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <csignal>  // sig_atomic_t

#include "mixer_remote_wire.h"
#include "rtl_airband.h"

using namespace std;

// ---- send side ----

bool mixer_remote_send_init(mixer_remote_send_data* rdata, size_t len) {
    rdata->send_socket = -1;
    rdata->seq = 0;
    rdata->send_buf = NULL;
    rdata->send_buf_len = 0;

    if (strlen(rdata->dest_path) >= sizeof(rdata->dest_sockaddr.sun_path)) {
        log(LOG_ERR, "mixer_remote: dest_path too long for AF_UNIX (max %zu bytes): %s\n", sizeof(rdata->dest_sockaddr.sun_path) - 1, rdata->dest_path);
        return false;
    }

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock == -1) {
        log(LOG_ERR, "mixer_remote: socket() failed: %s\n", strerror(errno));
        return false;
    }

    memset(&rdata->dest_sockaddr, 0, sizeof(rdata->dest_sockaddr));
    rdata->dest_sockaddr.sun_family = AF_UNIX;
    strncpy(rdata->dest_sockaddr.sun_path, rdata->dest_path, sizeof(rdata->dest_sockaddr.sun_path) - 1);

    rdata->send_buf_len = sizeof(mixer_remote_packet_header) + len * sizeof(float);
    rdata->send_buf = (uint8_t*)XCALLOC(1, rdata->send_buf_len);

    rdata->send_socket = sock;
    log(LOG_INFO, "mixer_remote: streaming stream_id=%u to %s (fire-and-forget - receiver need not be listening yet)\n", rdata->stream_id, rdata->dest_path);
    return true;
}

void mixer_remote_send(mixer_remote_send_data* rdata, const float* samples, size_t len, bool has_signal) {
    if (rdata->send_socket == -1) {
        return;
    }

    size_t packet_len = mixer_remote_encode_packet(WAVE_RATE, rdata->stream_id, rdata->seq, has_signal, samples, (uint32_t)len, rdata->send_buf, rdata->send_buf_len);
    if (packet_len == 0) {
        log(LOG_WARNING, "mixer_remote: len %zu exceeds send buffer size %zu, dropping packet\n", len, rdata->send_buf_len);
        rdata->dropped_packet_count++;
        return;
    }
    rdata->seq++;

    ssize_t sent = sendto(rdata->send_socket, rdata->send_buf, packet_len, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr*)&rdata->dest_sockaddr, sizeof(rdata->dest_sockaddr));
    if (sent < 0 || (size_t)sent != packet_len) {
        rdata->dropped_packet_count++;
    }
}

void mixer_remote_send_shutdown(mixer_remote_send_data* rdata) {
    if (rdata->send_socket != -1) {
        close(rdata->send_socket);
        rdata->send_socket = -1;
    }
    free(rdata->send_buf);
    rdata->send_buf = NULL;
}

// ---- receive side ----

vector<mixer_remote_listener_t*> mixer_remote_listeners;

namespace {
volatile sig_atomic_t mixer_remote_shutdown_requested = 0;
}  // namespace

mixer_remote_listener_t* mixer_remote_get_or_create_listener(const string& listen_path) {
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        if (listener->listen_path == listen_path) {
            return listener;
        }
    }
    mixer_remote_listener_t* listener = new mixer_remote_listener_t();
    listener->listen_path = listen_path;
    mixer_remote_listeners.push_back(listener);
    return listener;
}

bool mixer_remote_register_route(mixer_remote_listener_t* listener, uint32_t stream_id, mixer_t* mixer, int slot_idx) {
    for (const mixer_remote_route_t& route : listener->routes) {
        if (route.stream_id == stream_id) {
            return false;
        }
    }
    mixer_remote_route_t route;
    route.stream_id = stream_id;
    route.mixer = mixer;
    route.slot_idx = slot_idx;
    listener->routes.push_back(route);
    return true;
}

void mixer_remote_reset_registry() {
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        delete listener;
    }
    mixer_remote_listeners.clear();
}

void mixer_remote_dispatch_packet(mixer_remote_listener_t* listener, const uint8_t* buf, size_t len, uid_t sender_uid) {
    if (sender_uid != getuid()) {
        listener->rejected_uid_count++;
        return;
    }

    mixer_remote_packet_header hdr;
    if (!mixer_remote_decode_header(buf, len, &hdr)) {
        listener->malformed_header_count++;
        return;
    }

    mixer_remote_route_t* route = NULL;
    for (mixer_remote_route_t& candidate : listener->routes) {
        if (candidate.stream_id == hdr.stream_id) {
            route = &candidate;
            break;
        }
    }
    if (route == NULL) {
        listener->unknown_stream_count++;
        return;
    }

    if (hdr.sample_rate != (uint32_t)WAVE_RATE) {
        route->rate_mismatch_count++;
        return;
    }
    if (hdr.sample_count != (uint32_t)WAVE_BATCH) {
        route->sample_count_mismatch_count++;
        return;
    }

    float scratch[WAVE_BATCH];
    if (mixer_remote_decode_payload(buf, len, hdr, scratch, WAVE_BATCH) != (size_t)WAVE_BATCH) {
        route->malformed_payload_count++;
        return;
    }

    // Observability only, not filtering: a duplicate/reordered/gapped packet is still applied to
    // the mixer slot below (mixer_put_samples() always writes "this tick's" audio - there is no
    // reordering buffer here, matching the wider "audio timing derives from the SDR sample rate,
    // there is no wall-clock pacing" design already documented for this codebase). last_seq only
    // ever advances (never rewinds on a duplicate/reorder), so a later genuinely-new packet is
    // still classified correctly against the highest sequence actually seen so far.
    mixer_remote_seq_class seq_class = mixer_remote_classify_seq(route->have_last_seq, route->last_seq, hdr.seq);
    if (seq_class == mixer_remote_seq_class::kGap) {
        route->seq_gap_count++;
        route->last_seq = hdr.seq;
    } else if (seq_class == mixer_remote_seq_class::kDuplicateOrReorder) {
        route->seq_reorder_or_duplicate_count++;
    } else {
        route->last_seq = hdr.seq;
    }
    route->have_last_seq = true;
    route->last_packet_time = time(NULL);

    mixer_put_samples(route->mixer, route->slot_idx, scratch, (hdr.flags & MIXER_REMOTE_FLAG_HAS_SIGNAL) != 0, WAVE_BATCH);
}

namespace {

void* mixer_remote_listener_thread(void* param) {
    mixer_remote_listener_t* listener = (mixer_remote_listener_t*)param;

    while (!mixer_remote_shutdown_requested) {
        struct pollfd pfd;
        pfd.fd = listener->fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) {
            continue;
        }

        uint8_t buf[sizeof(mixer_remote_packet_header) + WAVE_BATCH * sizeof(float)];
        uint8_t cmsg_buf[CMSG_SPACE(sizeof(struct ucred))];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(listener->fd, &msg, 0);
        if (n <= 0) {
            continue;
        }

        uid_t sender_uid = (uid_t)-1;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
                struct ucred cred;
                memcpy(&cred, CMSG_DATA(cmsg), sizeof(cred));
                sender_uid = cred.uid;
                break;
            }
        }
        if (sender_uid == (uid_t)-1) {
            // No SCM_CREDENTIALS ancillary data at all - can't happen for a genuine AF_UNIX peer
            // once SO_PASSCRED is set, but reject rather than trust an unidentifiable sender.
            listener->rejected_uid_count++;
            continue;
        }

        mixer_remote_dispatch_packet(listener, buf, (size_t)n, sender_uid);
    }
    return NULL;
}

}  // namespace

void mixer_remote_recv_start() {
    mixer_remote_shutdown_requested = 0;

    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        if (listener->routes.empty()) {
            continue;
        }

        int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (sock < 0) {
            log(LOG_ERR, "mixer_remote: socket() failed for listener %s: %s\n", listener->listen_path.c_str(), strerror(errno));
            continue;
        }

        int enable = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &enable, sizeof(enable)) != 0) {
            log(LOG_ERR, "mixer_remote: setsockopt(SO_PASSCRED) failed for listener %s: %s\n", listener->listen_path.c_str(), strerror(errno));
            close(sock);
            continue;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (listener->listen_path.size() >= sizeof(addr.sun_path)) {
            log(LOG_ERR, "mixer_remote: listen_path too long: %s\n", listener->listen_path.c_str());
            close(sock);
            continue;
        }
        strncpy(addr.sun_path, listener->listen_path.c_str(), sizeof(addr.sun_path) - 1);

        // Remove a stale socket file left behind by a previous run (e.g. unclean shutdown) -
        // same rationale as control_socket_start()'s identical unlink().
        unlink(listener->listen_path.c_str());

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            log(LOG_ERR, "mixer_remote: bind(%s) failed: %s\n", listener->listen_path.c_str(), strerror(errno));
            close(sock);
            continue;
        }
        if (chmod(listener->listen_path.c_str(), S_IRUSR | S_IWUSR) < 0) {
            log(LOG_WARNING, "mixer_remote: chmod(%s) failed: %s\n", listener->listen_path.c_str(), strerror(errno));
        }

        listener->fd = sock;
        log(LOG_INFO, "mixer_remote: listening on %s (%zu route(s))\n", listener->listen_path.c_str(), listener->routes.size());
        pthread_create(&listener->thread, NULL, &mixer_remote_listener_thread, listener);
        listener->thread_started = true;
    }
}

void mixer_remote_recv_shutdown() {
    mixer_remote_shutdown_requested = 1;
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        if (!listener->thread_started) {
            continue;
        }
        pthread_join(listener->thread, NULL);
        listener->thread_started = false;
        if (listener->fd != -1) {
            close(listener->fd);
            listener->fd = -1;
        }
        unlink(listener->listen_path.c_str());
    }
}
