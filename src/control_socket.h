/*
 * control_socket.h
 * dynamic_reload Unix domain control socket - listener thread + JSON command protocol
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

#ifndef _CONTROL_SOCKET_H
#define _CONTROL_SOCKET_H 1

#include <map>
#include <string>

// Starts the listener thread on the given Unix domain socket path. Returns false on failure
// (path already in use by something else, permission error, etc).
bool control_socket_start(const std::string& path);

// Signals the listener thread to stop and joins it. Idempotent - safe to call even if
// control_socket_start() was never called or failed.
void control_socket_shutdown();

// Below is exposed purely so it can be unit tested without any socket I/O - not part of the
// public start/shutdown lifecycle callers outside this module should use.

// Parses one line of the wire protocol: a single flat JSON object, {"key": <string|number|bool>,
// ...}, no nesting - that's all this command set needs, so this is a small purpose-built parser
// rather than a general JSON parser. On success, fields holds each key mapped to its value's raw
// text (quotes stripped for strings, otherwise the literal token) for the caller to interpret
// per-command. On failure, returns false and sets *error.
bool control_socket_parse_command_line(const std::string& line, std::map<std::string, std::string>* fields, std::string* error);

// Dispatches one already-parsed command to the matching live_reconfig primitive and returns the
// JSON response line (without a trailing newline).
std::string control_socket_dispatch_command(const std::map<std::string, std::string>& fields);

// Parses and dispatches one wire-protocol line in a single call - what handle_connection() uses
// per line it reads off the socket. Exposed for the same testability reason as the two above.
std::string control_socket_dispatch_command_line(const std::string& line);

#endif /* _CONTROL_SOCKET_H */
