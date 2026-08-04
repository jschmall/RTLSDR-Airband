/*
 * logging.h
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

#ifndef _LOGGING_H
#define _LOGGING_H 1

#include <syslog.h>   // LOG_ERR
#include <cstdio>     // FILE
#include <stdexcept>  // std::runtime_error
#include <string>

#define nop() \
    do {      \
    } while (0)
#define UNUSED(x) (void)(x)

#ifdef DEBUG
#define DEBUG_PATH "rtl_airband_debug.log"
#define debug_print(fmt, ...)                                 \
    do {                                                      \
        fprintf(debugf, "%s(): " fmt, __func__, __VA_ARGS__); \
        fflush(debugf);                                       \
    } while (0)
#define debug_bulk_print(fmt, ...)                            \
    do {                                                      \
        fprintf(debugf, "%s(): " fmt, __func__, __VA_ARGS__); \
    } while (0)
#else
#define debug_print(fmt, ...) nop()
#define debug_bulk_print(fmt, ...) nop()
#endif /* DEBUG */

enum LogDestination { SYSLOG, STDERR, NONE };
extern LogDestination log_destination;
extern bool log_json_format;
extern FILE* debugf;

// Set (only) by the dynamic_reload live channel-append path in live_reconfig.cpp around a
// synchronous call into config.cpp's per-channel parsing code, which calls error() on ~50 sites
// for a malformed config value. Startup parsing must still exit hard on a bad config, but a bad
// value in a channel appended to an already-running process must not be allowed to take the
// whole process down - see error()'s definition (logging.cpp). thread_local because it's set and
// cleared entirely within the control-socket thread's handling of a single command; every other
// thread (startup's main thread included) must always see it false.
extern thread_local bool config_error_is_recoverable;

// Thrown by error() instead of calling _Exit(1) when config_error_is_recoverable is set. The
// caller that set the flag is expected to catch this - the human-readable error text is not
// carried on the exception itself (call sites print it to cerr immediately before calling
// error()); the catching code is expected to have redirected cerr to capture that text. See
// live_reconfig.cpp's live channel-append path.
struct ConfigApplyError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void error();
void init_debug(const char* file);
void close_debug();
void log(int priority, const char* format, ...);

// pure - no I/O or wall-clock dependency in the escaping logic itself - so it can be
// unit tested directly. message should not include a trailing newline.
std::string build_json_log_line(int priority, const std::string& message);

#endif /* _LOGGING_H */
