/*
 * stats_http.cpp
 *
 * Serves the same Prometheus-format content write_stats_file() (src/output.cpp)
 * already writes to stats_filepath every 15s, over plain HTTP, so a Prometheus
 * server can scrape it directly instead of needing a textfile collector per host.
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>  // sig_atomic_t
#include <cstdio>
#include <cstring>
#include <string>

#include "rtl_airband.h"

namespace {

pthread_t http_thread;
int listen_fd = -1;
volatile sig_atomic_t http_shutdown_requested = 0;
bool http_started = false;

std::string read_stats_file() {
    std::string content;
    FILE* f = fopen(stats_filepath, "r");
    if (!f) {
        return content;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    fclose(f);
    return content;
}

// Doesn't parse the request at all - this is a single-purpose server with one
// thing to serve, so any request (regardless of method/path) gets the current
// stats file content back. Handles one connection at a time; scraping is
// low-frequency and low-concurrency, so serial handling keeps this simple.
void handle_connection(int conn_fd) {
    char discard[4096];
    recv(conn_fd, discard, sizeof(discard), 0);  // best-effort; response doesn't depend on it

    std::string body = read_stats_file();
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/plain; version=0.0.4\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;

    send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
    close(conn_fd);
}

void* http_main(void*) {
    while (!http_shutdown_requested) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        // short timeout so the shutdown flag gets rechecked promptly rather than
        // blocking in accept() indefinitely (mirrors the interruptible pattern
        // used for the rdio_scanner worker thread's shutdown)
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) {
            continue;
        }
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            continue;
        }
        handle_connection(conn_fd);
    }
    return NULL;
}

}  // namespace

void stats_http_start() {
    if (!stats_http_port || http_started) {
        return;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log(LOG_ERR, "stats_http: socket() failed: %s\n", strerror(errno));
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)stats_http_port);
    if (inet_pton(AF_INET, stats_http_address, &addr.sin_addr) != 1) {
        log(LOG_ERR, "stats_http: invalid bind address '%s'\n", stats_http_address);
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log(LOG_ERR, "stats_http: bind(%s:%d) failed: %s\n", stats_http_address, stats_http_port, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    if (listen(listen_fd, 8) < 0) {
        log(LOG_ERR, "stats_http: listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    log(LOG_INFO, "stats_http: serving %s on %s:%d\n", stats_filepath, stats_http_address, stats_http_port);
    http_shutdown_requested = 0;
    pthread_create(&http_thread, NULL, &http_main, NULL);
    http_started = true;
}

void stats_http_shutdown() {
    if (!http_started) {
        return;
    }
    http_shutdown_requested = 1;
    pthread_join(http_thread, NULL);
    close(listen_fd);
    listen_fd = -1;
    http_started = false;
}
