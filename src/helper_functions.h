/*
 * helper_functions.h
 *
 * Copyright (C) 2023 charlie-foxtrot
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

#ifndef _HELPER_FUNCTIONS_H
#define _HELPER_FUNCTIONS_H

#include <sys/resource.h>  // struct rusage
#include <sys/time.h>      // struct timeval
#include <ctime>           // struct tm
#include <string>

// defined in rtl_airband.h; forward-declared here so this header doesn't need to pull it in
struct icecast_tx_tag_state;

bool dir_exists(const std::string& dir_path);
bool file_exists(const std::string& file_path);
bool make_dir(const std::string& dir_path);
bool make_subdirs(const std::string& basedir, const std::string& subdirs);
std::string make_dated_subdirs(const std::string& basedir, const struct tm* time);
std::string make_icecast_mountpoint(const std::string& mountpoint);
double rusage_cpu_seconds(const struct rusage& ru);

// Builds the Icecast "song" tag content for a currently-active transmission: the configured
// label if set, else a "%.3f MHz" formatted frequency (matching the existing
// send_scan_freq_tags fallback in process_outputs()). Returns "" when has_signal is false,
// so callers can send that value straight through to clear the Now Playing tag.
std::string compute_tx_tag_content(bool has_signal, const char* label, int frequency_hz);

// Advances an icecast_tx_tag_state for one process_outputs() tick given the currently-desired
// tag content. Returns true (and fills out_value) exactly when out_value should be sent to
// Icecast now; false if nothing should be sent this tick. See icecast_tx_tag_state's comment
// in rtl_airband.h for the flap-handling rule this implements.
bool icecast_tx_tag_step(icecast_tx_tag_state* state, const std::string& desired_tag, const struct timeval& now, int delay_sec, std::string* out_value);

#endif /* _HELPER_FUNCTIONS_H */
