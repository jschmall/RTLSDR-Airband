/*
 * icecast_metadata.cpp
 * Background sender for Icecast metadata updates (send_tx_tags, send_scan_freq_tags).
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

#include "icecast_metadata.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <shout/shout.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include "config.h"
#include "helper_functions.h"
#include "logging.h"
#include "rtl_airband.h"

using namespace std;

IcecastMetaTarget::~IcecastMetaTarget() {
    if (shout != nullptr) {
        shout_free((shout_t*)shout);
    }
}

bool IcecastMetaQueue::submit(const shared_ptr<IcecastMetaTarget>& target, const string& value) {
    bool fresh;
    {
        lock_guard<mutex> lock(mutex_);
        target->pending_value = value;
        fresh = !target->queued;
        if (fresh) {
            target->queued = true;
            ready_.push_back(target);
        }
    }
    cv_.notify_one();
    return fresh;
}

bool IcecastMetaQueue::take(shared_ptr<IcecastMetaTarget>* target, string* value) {
    unique_lock<mutex> lock(mutex_);
    cv_.wait(lock, [this] { return shutdown_ || !ready_.empty(); });
    if (shutdown_) {
        return false;
    }
    *target = ready_.front();
    ready_.pop_front();
    (*target)->queued = false;
    *value = (*target)->pending_value;
    return true;
}

void IcecastMetaQueue::shutdown() {
    {
        lock_guard<mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

size_t IcecastMetaQueue::pending() const {
    lock_guard<mutex> lock(mutex_);
    return ready_.size();
}

shared_ptr<IcecastMetaTarget> icecast_metadata_target_new(const icecast_data* icecast) {
    auto target = make_shared<IcecastMetaTarget>();
    target->host = icecast->hostname;
    target->port = icecast->port;
#ifdef LIBSHOUT_HAS_TLS
    target->tls_mode = icecast->tls_mode;
#endif /* LIBSHOUT_HAS_TLS */
    target->mount = make_icecast_mountpoint(icecast->mountpoint);
    target->username = icecast->username;
    target->password = icecast->password;
    return target;
}

namespace {

const int HTTP_TIMEOUT_MS = 10000;

string url_encode(const string& s) {
    static const char hex[] = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

string base64(const string& s) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
    size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        unsigned n = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8) | (unsigned char)s[i + 2];
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i + 1 == s.size()) {
        unsigned n = (unsigned char)s[i] << 16;
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == s.size()) {
        unsigned n = ((unsigned char)s[i] << 16) | ((unsigned char)s[i + 1] << 8);
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// Waits for fd to become ready for events, up to the time left until deadline.
bool wait_fd(int fd, short events, chrono::steady_clock::time_point deadline) {
    while (true) {
        auto left = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now()).count();
        if (left <= 0) {
            return false;
        }
        struct pollfd p = {fd, events, 0};
        int r = poll(&p, 1, (int)left);
        if (r > 0) {
            return true;
        }
        if (r == 0 || errno != EINTR) {
            return false;
        }
    }
}

// A non-blocking connection to host:port, trying each address in turn, or -1 with error set.
int connect_to(const string& host, int port, chrono::steady_clock::time_point deadline, string* error) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        *error = string("can't look up the host: ") + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* a = res; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            *error = string("socket: ") + strerror(errno);
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            break;
        }
        if (errno == EINPROGRESS && wait_fd(fd, POLLOUT, deadline)) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                break;
            }
            *error = string("can't connect: ") + strerror(err);
        } else {
            *error = errno == EINPROGRESS ? string("can't connect: timed out") : string("can't connect: ") + strerror(errno);
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

}  // namespace

string icecast_metadata_request(const IcecastMetaTarget& t, const string& song) {
    return "GET /admin/metadata?mode=updinfo&charset=UTF-8&mount=" + url_encode(t.mount) + "&song=" + url_encode(song) +
           " HTTP/1.0\r\n"
           "Host: " +
           t.host + ":" + to_string(t.port) +
           "\r\n"
           "Authorization: Basic " +
           base64(t.username + ":" + t.password) +
           "\r\n"
           "User-Agent: RTLSDR-Airband\r\n"
           "\r\n";
}

bool icecast_metadata_send_http(const IcecastMetaTarget& t, const string& song, string* error) {
    auto deadline = chrono::steady_clock::now() + chrono::milliseconds(HTTP_TIMEOUT_MS);
    int fd = connect_to(t.host, t.port, deadline, error);
    if (fd < 0) {
        return false;
    }
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    string request = icecast_metadata_request(t, song);
    size_t sent = 0;
    while (sent < request.size()) {
        ssize_t n = send(fd, request.data() + sent, request.size() - sent, send_flags);
        if (n > 0) {
            sent += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && wait_fd(fd, POLLOUT, deadline)) {
            continue;
        } else {
            *error = n < 0 ? string("send: ") + strerror(errno) : string("send: timed out");
            close(fd);
            return false;
        }
    }
    // Only the status line matters; the server closes the connection after its answer.
    string response;
    char buf[1024];
    while (response.find("\r\n") == string::npos && response.size() < 4096) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            response.append(buf, n);
        } else if (n == 0) {
            break;
        } else if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && wait_fd(fd, POLLIN, deadline)) {
            continue;
        } else {
            *error = errno == EAGAIN || errno == EWOULDBLOCK ? string("no answer: timed out") : string("no answer: ") + strerror(errno);
            close(fd);
            return false;
        }
    }
    close(fd);
    string status = response.substr(0, response.find("\r\n"));
    if (status.compare(0, 5, "HTTP/") == 0 && status.find(' ') != string::npos && status.compare(status.find(' ') + 1, 3, "200") == 0) {
        return true;
    }
    *error = status.empty() ? string("the server closed the connection without an answer") : "the server answered " + status;
    return false;
}

namespace {

// Never freed: a worker still stuck in a request when shutdown gives up on it keeps using these
// while the process exits, so they must outlive static destruction.
IcecastMetaQueue* queue = new IcecastMetaQueue;
mutex* exit_mutex = new mutex;
condition_variable* exit_cv = new condition_variable;
bool worker_exited = false;
thread worker;

const auto SHUTDOWN_WAIT = chrono::seconds(2);

shout_t* make_metadata_shout(const IcecastMetaTarget& t) {
    shout_t* s = shout_new();
    if (s == NULL) {
        return NULL;
    }
    bool ok = shout_set_host(s, t.host.c_str()) == SHOUTERR_SUCCESS && shout_set_protocol(s, SHOUT_PROTOCOL_HTTP) == SHOUTERR_SUCCESS && shout_set_port(s, t.port) == SHOUTERR_SUCCESS &&
              shout_set_mount(s, t.mount.c_str()) == SHOUTERR_SUCCESS && shout_set_user(s, t.username.c_str()) == SHOUTERR_SUCCESS && shout_set_password(s, t.password.c_str()) == SHOUTERR_SUCCESS;
#ifdef LIBSHOUT_HAS_TLS
    ok = ok && shout_set_tls(s, t.tls_mode) == SHOUTERR_SUCCESS;
#endif /* LIBSHOUT_HAS_TLS */
    if (!ok) {
        log(LOG_WARNING, "Icecast metadata for %s:%d%s: %s\n", t.host.c_str(), t.port, t.mount.c_str(), shout_get_error(s));
        shout_free(s);
        return NULL;
    }
    return s;
}

void send_update(IcecastMetaTarget* t, const string& value) {
#ifdef LIBSHOUT_HAS_TLS
    const bool use_libshout = t->tls_mode != SHOUT_TLS_DISABLED;
#else
    const bool use_libshout = false;
#endif /* LIBSHOUT_HAS_TLS */
    if (!use_libshout) {
        string error;
        if (icecast_metadata_send_http(*t, icecast_song_value(value), &error)) {
            t->update_count++;
        } else {
            log(LOG_WARNING, "Failed to set Icecast metadata for %s:%d%s: %s\n", t->host.c_str(), t->port, t->mount.c_str(), error.c_str());
            t->failure_count++;
        }
        return;
    }
    if (t->shout == nullptr) {
        t->shout = make_metadata_shout(*t);
        if (t->shout == nullptr) {
            t->failure_count++;
            return;
        }
    }
    shout_t* s = (shout_t*)t->shout;
    shout_metadata_t* meta = shout_metadata_new();
    if (meta == NULL || shout_metadata_add(meta, "song", icecast_song_value(value).c_str()) != SHOUTERR_SUCCESS) {
        log(LOG_WARNING, "Failed to add shout metadata\n");
        t->failure_count++;
        if (meta != NULL) {
            shout_metadata_free(meta);
        }
        return;
    }
    if (SHOUT_SET_METADATA(s, meta) == SHOUTERR_SUCCESS) {
        t->update_count++;
    } else {
        log(LOG_WARNING, "Failed to set shout metadata for %s:%d%s: %s\n", t->host.c_str(), t->port, t->mount.c_str(), shout_get_error(s));
        t->failure_count++;
    }
    shout_metadata_free(meta);
}

void worker_main() {
    shared_ptr<IcecastMetaTarget> target;
    string value;
    while (queue->take(&target, &value)) {
        send_update(target.get(), value);
        target.reset();
    }
    lock_guard<mutex> lock(*exit_mutex);
    worker_exited = true;
    exit_cv->notify_all();
}

}  // namespace

void icecast_metadata_submit(const shared_ptr<IcecastMetaTarget>& target, const string& value) {
    if (!target) {
        return;
    }
    if (!queue->submit(target, value)) {
        target->superseded_count++;
    }
}

void icecast_metadata_start() {
    worker = thread(worker_main);
}

void icecast_metadata_shutdown() {
    if (!worker.joinable()) {
        return;
    }
    queue->shutdown();
    unique_lock<mutex> lock(*exit_mutex);
    if (exit_cv->wait_for(lock, SHUTDOWN_WAIT, [] { return worker_exited; })) {
        lock.unlock();
        worker.join();
    } else {
        log(LOG_WARNING, "Icecast metadata request still in progress at shutdown, not waiting for it\n");
        worker.detach();
    }
}
