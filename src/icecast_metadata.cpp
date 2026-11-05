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
#include <shout/shout.h>
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
              shout_set_mount(s, t.mount.c_str()) == SHOUTERR_SUCCESS && shout_set_user(s, t.username.c_str()) == SHOUTERR_SUCCESS &&
              shout_set_password(s, t.password.c_str()) == SHOUTERR_SUCCESS;
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
    if (t->shout == nullptr) {
        t->shout = make_metadata_shout(*t);
        if (t->shout == nullptr) {
            t->failure_count++;
            return;
        }
    }
    shout_t* s = (shout_t*)t->shout;
    shout_metadata_t* meta = shout_metadata_new();
    if (meta == NULL || shout_metadata_add(meta, "song", value.c_str()) != SHOUTERR_SUCCESS) {
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
