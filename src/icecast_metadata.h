/*
 * icecast_metadata.h
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

#ifndef _ICECAST_METADATA_H
#define _ICECAST_METADATA_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

// libshout's metadata call opens a new HTTP connection (DNS lookup, connect, request, response)
// and blocks until it finishes, ~350 ms to Broadcastify in production. Made on the output thread,
// that stalled every send_tx_tags mixer for 2-3 audio batches at each transmission start and end.
// Updates now go to one worker thread instead.
//
// A target holds a copy of the output's connection settings, so the worker never reads the
// output's icecast_data, which a live channel removal frees. The output and any queued job each
// hold a shared_ptr, so whichever lets go last frees it.
struct IcecastMetaTarget {
    std::string host;
    int port = 0;
    int tls_mode = 0;
    std::string mount;
    std::string username;
    std::string password;

    // Touched only by the worker thread: a never-opened shout_t used just for metadata requests,
    // created on first use. It is never the output's streaming shout_t, whose connection the
    // output thread owns.
    void* shout = nullptr;

    std::atomic<size_t> update_count{0};
    std::atomic<size_t> failure_count{0};
    // A value replaced by a newer one before the worker sent it.
    std::atomic<size_t> superseded_count{0};

    // Guarded by the owning IcecastMetaQueue's mutex.
    bool queued = false;
    std::string pending_value;

    ~IcecastMetaTarget();
};

// Pending updates, at most one per target: a newer value replaces one still waiting, since only the
// latest tag matters. Targets are served in the order they first became pending.
class IcecastMetaQueue {
   public:
    // Never blocks on the network. Returns false when the value replaced one still waiting.
    bool submit(const std::shared_ptr<IcecastMetaTarget>& target, const std::string& value);
    // Blocks until an update is pending or shutdown() is called; returns false on shutdown.
    bool take(std::shared_ptr<IcecastMetaTarget>* target, std::string* value);
    void shutdown();
    size_t pending() const;

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<IcecastMetaTarget> > ready_;
    bool shutdown_ = false;
};

// Copies the connection settings out of an icecast output. Called at config parse time for an
// output with send_tx_tags or send_scan_freq_tags.
struct icecast_data;
std::shared_ptr<IcecastMetaTarget> icecast_metadata_target_new(const icecast_data* icecast);

// Queues "song" = value for the target's mount. A no-op for a null target.
void icecast_metadata_submit(const std::shared_ptr<IcecastMetaTarget>& target, const std::string& value);

void icecast_metadata_start();
// Waits briefly for an in-flight request, then lets the process exit without it: a request to an
// unreachable server can block for a TCP connect timeout, which shouldn't hold up a restart.
void icecast_metadata_shutdown();

#endif /* _ICECAST_METADATA_H */
