/*
 * rdio_scanner.cpp
 *
 * Uploads completed transmission files to a rdio-scanner instance via its
 * /api/call-upload endpoint (https://github.com/chuot/rdio-scanner).
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

#include <curl/curl.h>
#include <string.h>  // strerror()
#include <syslog.h>  // LOG_WARNING / LOG_ERR
#include <unistd.h>  // usleep(), unlink()
#include <cerrno>
#include <ctime>  // clock_gettime(), struct timespec
#include <deque>

#include "rtl_airband.h"

bool rdio_scanner_enabled = false;
std::atomic<size_t> rdio_scanner_queue_drop_count{0};
std::atomic<size_t> rdio_scanner_upload_failure_count{0};

namespace {

pthread_t worker_thread;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
std::deque<rdio_scanner_job> job_queue;
bool shutdown_requested = false;
bool worker_started = false;

size_t curl_append_response(char* ptr, size_t size, size_t nmemb, void* userdata) {
    ((std::string*)userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

bool is_shutdown_requested() {
    pthread_mutex_lock(&queue_mutex);
    bool requested = shutdown_requested;
    pthread_mutex_unlock(&queue_mutex);
    return requested;
}

// libcurl calls this roughly once a second during a transfer (and while connecting); a
// non-zero return aborts curl_easy_perform() early, so a shutdown doesn't have to wait
// out a full connect/transfer timeout on a stalled upload.
int curl_abort_on_shutdown(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return is_shutdown_requested() ? 1 : 0;
}

// builds the multipart body (minus the audio file itself) for one upload attempt
bool upload_once(const rdio_scanner_job& job, std::string* response, long* http_code) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        log(LOG_ERR, "rdio_scanner: curl_easy_init failed\n");
        return false;
    }

    std::string scheme = job.config.use_tls ? "https://" : "http://";
    std::string url = scheme + job.config.server + ":" + std::to_string(job.config.port) + "/api/call-upload";

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "audio");
    curl_mime_filedata(part, job.file_path.c_str());

    std::vector<std::pair<std::string, std::string>> fields = rdio_scanner_build_fields(job);
    for (const auto& field : fields) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, field.first.c_str());
        curl_mime_data(part, field.second.c_str(), CURL_ZERO_TERMINATED);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, job.config.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, job.config.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_on_shutdown);

    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res != CURLE_OK) {
        *response = curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
        ok = (*http_code >= 200 && *http_code < 300);
    }

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return ok;
}

// interruptible version of usleep(): returns early if shutdown is requested while waiting
void backoff_sleep(int attempt) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long backoff_ms = (long)attempt * 1000;  // 1s, 2s, ...
    deadline.tv_sec += backoff_ms / 1000;
    deadline.tv_nsec += (backoff_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&queue_mutex);
    while (!shutdown_requested) {
        if (pthread_cond_timedwait(&queue_cond, &queue_mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&queue_mutex);
}

void process_job(const rdio_scanner_job& job) {
    bool ok = false;
    std::string response;
    long http_code = 0;

    for (int attempt = 0; attempt <= job.config.max_retries && !ok; ++attempt) {
        // always make the first attempt regardless of shutdown state, but don't retry
        // (including the backoff wait) once a shutdown has been requested - a stalled
        // upload can still be cut short mid-attempt via curl_abort_on_shutdown()
        if (attempt > 0) {
            if (is_shutdown_requested()) {
                break;
            }
            backoff_sleep(attempt);
            if (is_shutdown_requested()) {
                break;
            }
        }
        response.clear();
        http_code = 0;
        ok = upload_once(job, &response, &http_code);
    }

    if (ok) {
        debug_print("rdio_scanner: uploaded %s (system=%d talkgroup=%d)\n", job.file_path.c_str(), job.config.system_id, job.config.talkgroup_id);
        if (job.config.delete_after_upload && unlink(job.file_path.c_str()) != 0) {
            log(LOG_WARNING, "rdio_scanner: failed to delete %s after upload: %s\n", job.file_path.c_str(), strerror(errno));
        }
    } else {
        log(LOG_WARNING, "rdio_scanner: failed to upload %s after %d attempt(s), http=%ld: %s\n", job.file_path.c_str(), job.config.max_retries + 1, http_code, response.c_str());
        rdio_scanner_upload_failure_count++;
    }
}

void* worker_main(void*) {
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (job_queue.empty() && !shutdown_requested) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        if (job_queue.empty() && shutdown_requested) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        rdio_scanner_job job = job_queue.front();
        job_queue.pop_front();
        pthread_mutex_unlock(&queue_mutex);

        process_job(job);
    }
    return NULL;
}

}  // namespace

// pure - no network I/O - so it can be unit tested directly
std::vector<std::pair<std::string, std::string>> rdio_scanner_build_fields(const rdio_scanner_job& job) {
    std::vector<std::pair<std::string, std::string>> fields;

    fields.push_back(std::make_pair("audioType", "audio/mpeg"));
    fields.push_back(std::make_pair("dateTime", std::to_string(job.timestamp_sec)));
    fields.push_back(std::make_pair("frequency", std::to_string(job.frequency)));
    fields.push_back(std::make_pair("key", job.config.api_key));
    fields.push_back(std::make_pair("source", std::to_string(job.config.source_id)));
    fields.push_back(std::make_pair("talkgroup", std::to_string(job.config.talkgroup_id)));

    if (job.config.system_id >= 0) {
        fields.push_back(std::make_pair("system", std::to_string(job.config.system_id)));
    }
    if (!job.config.system_label.empty()) {
        fields.push_back(std::make_pair("systemLabel", job.config.system_label));
    }
    if (!job.config.talkgroup_label.empty()) {
        fields.push_back(std::make_pair("talkgroupLabel", job.config.talkgroup_label));
    }
    if (!job.config.talkgroup_tag.empty()) {
        fields.push_back(std::make_pair("talkgroupTag", job.config.talkgroup_tag));
    }
    if (!job.config.talkgroup_group.empty()) {
        fields.push_back(std::make_pair("talkgroupGroup", job.config.talkgroup_group));
    }

    return fields;
}

void rdio_scanner_start() {
    if (!rdio_scanner_enabled || worker_started) {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    shutdown_requested = false;
    pthread_create(&worker_thread, NULL, &worker_main, NULL);
    worker_started = true;
}

void rdio_scanner_shutdown() {
    if (!worker_started) {
        return;
    }
    pthread_mutex_lock(&queue_mutex);
    shutdown_requested = true;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    pthread_join(worker_thread, NULL);
    curl_global_cleanup();
    worker_started = false;
}

void rdio_scanner_enqueue(rdio_scanner_data* config, const std::string& file_path, const timeval& open_time, int frequency) {
    if (!config) {
        return;
    }

    rdio_scanner_job job;
    job.config = *config;
    job.file_path = file_path;
    job.frequency = frequency;
    job.timestamp_sec = (long long)open_time.tv_sec;

    pthread_mutex_lock(&queue_mutex);
    if (job_queue.size() >= (size_t)rdio_scanner_queue_depth) {
        log(LOG_WARNING, "rdio_scanner: upload queue full (%zu), dropping oldest job for %s\n", job_queue.size(), job_queue.front().file_path.c_str());
        job_queue.pop_front();
        rdio_scanner_queue_drop_count++;
    }
    job_queue.push_back(job);
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}
