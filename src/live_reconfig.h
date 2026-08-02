/*
 * live_reconfig.h
 * Live retune/reconfiguration primitives for the dynamic_reload control socket
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

#ifndef _LIVE_RECONFIG_H
#define _LIVE_RECONFIG_H 1

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "rtl_airband.h"

// Pure math, shared with config.cpp's parse-time setup so both paths agree by construction.
size_t compute_channel_bin(int channel_freq, int centerfreq, int sample_rate, size_t num_fft_bins);
uint32_t compute_channel_dm_dphi(int channel_freq, int centerfreq, int sample_rate);

// Called by the control socket thread. Validates dev->mode (only R_MULTICHANNEL devices are
// retunable this way - R_SCAN devices already have their own controller-thread retune loop) and,
// if valid, posts the request; returns false without touching any state otherwise. The actual
// hardware retune and bins/base_bins/dm_dphi recompute happen later, in the demod thread that
// owns this device - see device_apply_retune().
bool device_request_retune(device_t* dev, int new_centerfreq);

// Called from demodulate() (rtl_airband.cpp), in the demod thread that exclusively owns dev, once
// it observes a pending request. Recomputes bins/base_bins/dm_dphi for every channel on this
// device in place (plain assignments - no locking needed, same single-writer-thread invariant
// AFC's own per-bin adjustments already rely on) and retunes the hardware via
// input_set_centerfreq().
void device_apply_retune(device_t* dev, int new_centerfreq);

// Channel enable/disable, split into request (control socket thread) / apply (output thread)
// halves - see channel_t::pending_enable_request's comment (rtl_airband.h) for why: a channel's
// `enabled` flag and `outputs` are owned by whichever output thread processes that channel, so
// applying the change from any other thread (the control socket's) would race that thread's own
// concurrent reads/writes to the same output_t structs. channel_request_enable()/
// channel_request_disable() just post the request (O(1), touch nothing else) and are the only
// half safe to call from the control socket thread; channel_apply_enable()/
// channel_apply_disable() do the real work and must only be called from within output_thread()
// (src/output.cpp), which polls and consumes pending_enable_request once per pass.
bool channel_request_enable(channel_t* channel, int timeout_us = 500000);
bool channel_request_disable(channel_t* channel, int timeout_us = 500000);
void channel_apply_enable(channel_t* channel);
void channel_apply_disable(channel_t* channel);

// Connection-establishing half of init_output() (rtl_airband.cpp), redone for every output on an
// already-initialized channel (lame/lamebuf already allocated once at startup, not touched again
// here). Shared by channel_apply_enable() and mixer_enable() (mixer.cpp) - a mixer's own outputs
// are just outputs on mixer->channel, so the same per-output-type logic applies. Only safe to
// call from the output thread that owns the channel/mixer being reconnected, same as the
// apply-side functions above.
void reconnect_channel_outputs(channel_t* channel);

// Mixer enable/disable, same request/apply split and rationale as the channel functions above -
// see mixer_t::pending_enable_request's comment (rtl_airband.h). mixer_request_enable()/
// mixer_request_disable() are safe to call from the control socket thread; mixer_enable()/
// mixer_disable() (mixer.cpp) must only be called from within output_thread().
bool mixer_request_enable(mixer_t* mixer, int timeout_us = 500000);
bool mixer_request_disable(mixer_t* mixer, int timeout_us = 500000);

// A read-only snapshot of the fields reload_diff can act on - deliberately not a full mirror of
// parse_devices()/parse_channels()/parse_mixers(); it only captures what has a live-apply
// primitive (centerfreq, numeric gain, channel/mixer enabled) plus what's needed to detect
// out-of-v1-scope changes (counts, type, mode, sample_rate). Parsing this never touches
// devices[]/mixers[] or any other live state - re-invoking the real parse_* functions against
// already-running state would corrupt it (they assume a pristine, XCALLOC'd array).
struct DeviceConfigSnapshot {
    std::string type;
    rec_modes mode;
    int channel_count;
    int centerfreq;
    int sample_rate;
    bool has_gain;  // false if "gain" is absent, or present as a non-numeric (per-element) form
    float gain;
    std::vector<bool> channel_enabled;  // size == channel_count
};

struct MixerConfigSnapshot {
    std::string name;
    bool enabled;
};

struct ConfigSnapshot {
    std::vector<DeviceConfigSnapshot> devices;
    std::vector<MixerConfigSnapshot> mixers;
};

// Re-reads cfgfile from disk into a snapshot. Returns false and sets *error on a file/parse
// error (mirrors the read-only nature of this path - never calls error()/exit() the way the
// startup parser does).
bool parse_config_snapshot(const std::string& config_path, ConfigSnapshot* out, std::string* error);

// Diffs snapshot against the live devices[]/mixers[] and applies whatever v1 supports through the
// same primitives a single control-socket command would use (device_request_retune/
// input_set_gain/channel_request_enable/channel_request_disable/mixer_request_enable/
// mixer_request_disable). Anything outside v1 scope (device/channel/mixer count changes,
// sample_rate, type, mode) is reported in skipped_requires_restart and never attempted. Runs on
// the control socket thread, so - like the single-item commands - only ever posts requests, never
// calls the apply-side functions directly.
struct DiffResult {
    std::vector<std::string> applied;
    std::vector<std::string> skipped_requires_restart;
};
DiffResult compute_and_apply_diff(const ConfigSnapshot& snapshot);

#endif /* _LIVE_RECONFIG_H */
