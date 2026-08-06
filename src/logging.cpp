/*
 * logging.cpp
 *
 * Copyright (C) 2022-2023 charlie-foxtrot
 * Copyright (c) 2015-2022 Tomasz Lemiech <szpajder@gmail.com>
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

#include <stdarg.h>  // va_start() / va_end()
#include <sys/time.h>
#include <unistd.h>  // getpid()
#include <cstdio>    // fopen()
#include <cstring>   // strerror()
#include <ctime>     // gmtime_r()
#include <iostream>  // cerr()

#include "logging.h"

LogDestination log_destination = SYSLOG;
bool log_json_format = false;
FILE* debugf = NULL;
thread_local bool config_error_is_recoverable = false;

void error() {
    if (config_error_is_recoverable) {
        // Process keeps running - do not close_debug()/_Exit() as the normal path below does.
        throw ConfigApplyError("configuration error");
    }
    close_debug();
    _Exit(1);
}

void init_debug(const char* file) {
#ifdef DEBUG
    if (!file)
        return;
    if ((debugf = fopen(file, "a")) == NULL) {
        std::cerr << "Could not open debug file " << file << ": " << strerror(errno) << "\n";
        error();
    }
#else
    UNUSED(file);
#endif /* DEBUG */
}

void close_debug() {
#ifdef DEBUG
    if (!debugf)
        return;
    fclose(debugf);
#endif /* DEBUG */
}

namespace {

const char* priority_name(int priority) {
    switch (priority) {
        case LOG_EMERG:
            return "emerg";
        case LOG_ALERT:
            return "alert";
        case LOG_CRIT:
            return "crit";
        case LOG_ERR:
            return "error";
        case LOG_WARNING:
            return "warning";
        case LOG_NOTICE:
            return "notice";
        case LOG_INFO:
            return "info";
        case LOG_DEBUG:
            return "debug";
        default:
            return "unknown";
    }
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

}  // namespace

std::string build_json_log_line(int priority, const std::string& message) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    char timestamp[96];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (long)(tv.tv_usec / 1000));

    std::string line = "{\"timestamp\":\"";
    line += timestamp;
    line += "\",\"level\":\"";
    line += priority_name(priority);
    line += "\",\"pid\":";
    line += std::to_string(getpid());
    line += ",\"message\":\"";
    line += json_escape(message);
    line += "\"}";
    return line;
}

void log(int priority, const char* format, ...) {
    va_list args;
    va_start(args, format);

    if (log_json_format) {
        char buf[4096];
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);

        std::string message(buf);
        // the JSON line itself is the record separator - a trailing newline in the
        // rendered message (every call site's format string ends in one) would just
        // become a literal "\n" inside the message string, so strip it first
        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        std::string line = build_json_log_line(priority, message);

        switch (log_destination) {
            case SYSLOG:
                syslog(priority, "%s", line.c_str());
                break;
            case STDERR:
                fprintf(stderr, "%s\n", line.c_str());
                break;
            case NONE:
                break;
        }
        return;
    }

    switch (log_destination) {
        case SYSLOG:
            vsyslog(priority, format, args);
            break;
        case STDERR:
            vfprintf(stderr, format, args);
            break;
        case NONE:
            break;
    }
    va_end(args);
}
