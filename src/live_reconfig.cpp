/*
 * live_reconfig.cpp
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

#include "live_reconfig.h"
#include <unistd.h>  // usleep
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <libconfig.h++>
#include <sstream>
#include "input-common.h"
#include "logging.h"  // ConfigApplyError, config_error_is_recoverable
#include "rtl_airband.h"

using namespace std;

size_t compute_channel_bin(int channel_freq, int centerfreq, int sample_rate, size_t num_fft_bins) {
    return (size_t)ceil((channel_freq + sample_rate - centerfreq) / (double)(sample_rate / num_fft_bins) - 1.0) % num_fft_bins;
}

uint32_t compute_channel_dm_dphi(int channel_freq, int centerfreq, int sample_rate) {
    double dm_dphi = (double)(channel_freq - centerfreq);  // downmix freq in Hz

    // See config.cpp's original derivation of this formula for the full explanation - this must
    // stay bit-for-bit identical to the parse-time computation so a live retune produces exactly
    // the same result a restart with the new centerfreq would have.
    double decimation_factor = (double)sample_rate / (double)WAVE_RATE;
    double dm_dphi_correction = (double)WAVE_RATE / 2.0;
    dm_dphi_correction *= (decimation_factor - round(decimation_factor));
    dm_dphi_correction *= (double)(channel_freq - centerfreq) / ((double)sample_rate / 2.0);

    dm_dphi -= dm_dphi_correction;
    dm_dphi /= (double)WAVE_RATE;
    dm_dphi -= trunc(dm_dphi);
    dm_dphi *= 256.0 * 65536.0;
    return (uint32_t)((int)dm_dphi);
}

bool device_request_retune(device_t* dev, int new_centerfreq) {
    if (dev->mode != R_MULTICHANNEL) {
        // R_SCAN devices retune via their own controller_thread's fixed-offset scheme; accepting
        // a manual retune here would fight it and break the offset invariant it relies on.
        return false;
    }
    dev->pending_centerfreq_request.store(new_centerfreq, std::memory_order_release);
    return true;
}

void reconnect_channel_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        if (output->type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(output->data);
            if (icecast->shout == NULL) {
                shout_setup(icecast, channel->mode);
            }
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)(output->data);
            udp_stream_init(sdata, channel->mode, (size_t)WAVE_BATCH);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_enable_input(mdata->mixer, mdata->input);
            // File/rawfile outputs need no action here - output_file_ready() (output.cpp)
            // lazily reopens on the next signal, same as it already does for a fresh channel.
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(output->data);
            if (pdata->context == NULL) {
                pulse_init();
                pulse_setup(pdata, channel->mode);
            }
#endif /* WITH_PULSEAUDIO */
        }
        output->enabled = true;
    }
}

void channel_apply_enable(channel_t* channel) {
    channel->enabled = true;
    reconnect_channel_outputs(channel);
}

void channel_apply_disable(channel_t* channel) {
    channel->enabled = false;
    disable_channel_outputs(channel);
}

// See channel_t::pending_remove_request's comment (rtl_airband.h) and channel_request_remove()'s
// declaration (live_reconfig.h) for the full rationale of what this does and does not free.
// `enabled = false` is set first (before anything is touched or freed) for the same reason
// channel_apply_disable() sets it first: output_check_thread() (src/output.cpp) gates its own
// per-channel output access on this flag - setting it first is what keeps that thread from
// reaching in while this teardown runs, same protection the pre-existing disable feature already
// relies on.
void channel_teardown_for_removal(channel_t* channel) {
    channel->enabled = false;
    disable_channel_outputs(channel);
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        if (output->lame) {
            lame_close(output->lame);
            output->lame = NULL;
        }
        if (output->lamebuf) {
            free(output->lamebuf);
            output->lamebuf = NULL;
        }
    }
}

namespace {

// Shared by every request-side function below: posts value into *request, then polls until the
// owning thread consumes it (resets to -1) or timeout_us elapses. Returns false on timeout -
// callers treat that as "request accepted but not confirmed applied," not a hard failure, since
// the request is still pending and will be applied on the owning thread's next pass.
bool post_request_and_wait(std::atomic<int>* request, int value, int timeout_us) {
    request->store(value, std::memory_order_release);
    const int poll_interval_us = 2000;
    int waited_us = 0;
    while (request->load(std::memory_order_acquire) != -1 && waited_us < timeout_us) {
        usleep(poll_interval_us);
        waited_us += poll_interval_us;
    }
    return request->load(std::memory_order_acquire) == -1;
}

}  // namespace

bool channel_request_enable(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_enable_request, 1, timeout_us);
}

bool channel_request_disable(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_enable_request, 0, timeout_us);
}

bool channel_request_remove(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_remove_request, 1, timeout_us);
}

bool mixer_request_enable(mixer_t* mixer, int timeout_us) {
    return post_request_and_wait(&mixer->pending_enable_request, 1, timeout_us);
}

bool mixer_request_disable(mixer_t* mixer, int timeout_us) {
    return post_request_and_wait(&mixer->pending_enable_request, 0, timeout_us);
}

void device_apply_retune(device_t* dev, int new_centerfreq) {
    // Runs inside the demod thread that exclusively owns dev - same thread as AFC's own per-bin
    // adjustments (see the AFC class in rtl_airband.cpp), so plain assignments here are safe
    // without any lock: there is only ever one writer to bins/base_bins/dm_dphi at a time.
    for (int j = 0; j < dev->channel_count; j++) {
        channel_t* channel = dev->channels + j;
        int channel_freq = channel->freqlist[0].frequency;
        dev->base_bins[j] = dev->bins[j] = compute_channel_bin(channel_freq, new_centerfreq, dev->input->sample_rate, fft_size);
        if (channel->needs_raw_iq) {
            channel->dm_dphi = compute_channel_dm_dphi(channel_freq, new_centerfreq, dev->input->sample_rate);
            channel->dm_phi = 0;
        }
    }
    input_set_centerfreq(dev->input, new_centerfreq);
}

namespace {

// Mirrors config.cpp's static parse_anynum2int() exactly (int Hz, float treated as MHz*1e6, or a
// suffixed string via atofs()) - duplicated rather than exported because that function is
// deliberately internal to the startup parser; this is the read-only diff path's own copy.
int snapshot_parse_anynum2int(libconfig::Setting& f) {
    if (f.getType() == libconfig::Setting::TypeInt) {
        return (int)f;
    } else if (f.getType() == libconfig::Setting::TypeFloat) {
        return (int)((double)f * 1e6);
    } else if (f.getType() == libconfig::Setting::TypeString) {
        char* s = strdup((char const*)f);
        int ret = (int)atofs(s);
        free(s);
        return ret;
    }
    return 0;
}

// Only numeric "gain" (plain dB, int or float) is diffable against a single live gain value -
// SoapySDR's per-element string form ("key=value,...") has no single number to compare or apply
// via input_set_gain(), so it's left out of the snapshot entirely (never reported as a diff).
bool snapshot_get_numeric_gain(libconfig::Setting& dev_setting, float* gain) {
    if (!dev_setting.exists("gain")) {
        return false;
    }
    libconfig::Setting& g = dev_setting["gain"];
    if (g.getType() == libconfig::Setting::TypeInt) {
        *gain = (float)(int)g;
        return true;
    } else if (g.getType() == libconfig::Setting::TypeFloat) {
        *gain = (float)(double)g;
        return true;
    }
    return false;
}

// Attempts to parse and append snap.channel_count - dev->channel_count new channels (the caller
// has already confirmed this fits within dev->channel_capacity) from their raw config Settings.
// All-or-nothing: on any single new channel's parse failure, nothing is published - dev's
// channel_count is only ever advanced on full success, and until then nothing at index >=
// dev->channel_count is visible to any other thread (demod/output), so there's nothing to unwind
// there. A channel that individually parsed fine earlier in a batch that later fails is simply
// leaked (its heap allocations - strdup'd labels, XCALLOC'd outputs - are never freed) rather
// than unwound: this is a rare, operator-triggered path (editing a config file and retrying), so
// a few hundred bytes of leak on a failed attempt is an acceptable tradeoff against a real
// rollback's complexity. On failure, *error_text carries the best available diagnostic text.
bool try_append_channels(device_t* dev, int dev_idx, const DeviceConfigSnapshot& snap, string* error_text) {
    int old_count = dev->channel_count;
    int new_count = old_count;

    config_error_is_recoverable = true;
    ostringstream captured;
    std::streambuf* old_cerr_buf = std::cerr.rdbuf(captured.rdbuf());
    bool ok = true;
    string exception_text;
    try {
        for (int k = old_count; k < snap.channel_count; k++) {
            libconfig::Setting& chan_setting = (*snap.raw_channels_setting)[snap.raw_channel_indices[k]];
            channel_t* channel = dev->channels + k;
            if (!parse_channel(chan_setting, dev, dev_idx, k, channel)) {
                // parse_channel() returning false here is the same pre-existing legacy-value
                // quirk documented on parse_channel() itself (config.cpp) - this one channel is
                // silently not counted, matching startup behavior for the same input. Nothing to
                // connect for it.
                continue;
            }
            // Mirrors main()'s startup call to init_output() for every output right after
            // parse_devices() returns (rtl_airband.cpp): a freshly parsed channel_t's output_t
            // structs are allocated by parse_outputs() (inside parse_channel() above), but none
            // of them have a LAME encoder or an open connection until this runs - unlike
            // re-enabling an already-live channel (channel_apply_enable() -> reconnect_channel_
            // outputs()), which can assume that part already happened once at startup.
            for (int o = 0; o < channel->output_count; o++) {
                if (!init_output(channel, channel->outputs + o)) {
                    throw std::runtime_error("failed to initialize output " + to_string(o) + " for appended channel " + to_string(k));
                }
            }
            new_count = k + 1;
        }
    } catch (const std::exception& e) {
        // Catches both ConfigApplyError (what recoverable-mode error() throws - config.cpp's
        // call sites print a human-readable message to cerr immediately before calling error(),
        // captured above) and any raw libconfig::ConfigException (e.g. SettingNotFoundException
        // from indexing a required-but-missing key like chan_setting["outputs"] - config.cpp's
        // parser assumes many required keys exist rather than calling error() for every one of
        // them, exactly as a startup config missing the same key would already fail today).
        // Either way the process must survive; e.what() is the fallback when nothing was ever
        // written to cerr on this particular failure path.
        ok = false;
        exception_text = e.what();
    }
    std::cerr.rdbuf(old_cerr_buf);
    config_error_is_recoverable = false;

    if (!ok) {
        string captured_text = captured.str();
        *error_text = captured_text.empty() ? exception_text : captured_text;
        return false;
    }
    dev->channel_count.store(new_count, std::memory_order_release);
    return true;
}

// Attempts to remove dev->channel_count - target_count channels from the tail (the caller has
// already confirmed the surviving prefix [0, target_count) matches - see compute_and_apply_diff()
// 's replace/decrease handling). target_count is an explicit parameter rather than derived from
// a DeviceConfigSnapshot because a tail *replace* (not just a pure decrease) needs to remove down
// to the common-prefix length, which can be less than the snapshot's own final channel_count when
// channels are also being appended afterward in the same call. Unlike try_append_channels(), this
// is NOT all-or-nothing: dev->channel_count is decremented by one immediately after each
// individual channel's teardown is confirmed complete, so a mid-batch failure
// (channel_request_remove() timing out - e.g. the output thread is stuck or unusually slow)
// leaves dev->channel_count at a valid, still-tail-consistent smaller value rather than either an
// inconsistent one or losing an already-confirmed removal. Returns the number of channels
// actually removed; on a partial result (< old dev->channel_count - target_count), *error_text
// explains why the remainder was left in place.
int try_remove_channels(device_t* dev, int target_count, string* error_text) {
    int removed = 0;
    for (int k = dev->channel_count - 1; k >= target_count; k--) {
        channel_t* channel = dev->channels + k;
        if (!channel_request_remove(channel)) {
            *error_text = "channel " + to_string(k) + " removal request timed out waiting for the output thread";
            return removed;
        }
        dev->channel_count.store(k, std::memory_order_release);
        removed++;
    }
    return removed;
}

}  // namespace

bool parse_config_snapshot(const string& config_path, ConfigSnapshot* out, string* error) {
    out->raw_config = std::make_shared<libconfig::Config>();
    libconfig::Config& cfg = *out->raw_config;
    try {
        cfg.readFile(config_path.c_str());
    } catch (const libconfig::FileIOException&) {
        *error = "could not read config file '" + config_path + "'";
        return false;
    } catch (const libconfig::ParseException& pe) {
        *error = string("parse error at ") + pe.getFile() + ":" + to_string(pe.getLine()) + ": " + pe.getError();
        return false;
    }
    libconfig::Setting& root = cfg.getRoot();

    out->mixers.clear();
    if (root.exists("mixers")) {
        libconfig::Setting& mx = root["mixers"];
        for (int i = 0; i < mx.getLength(); i++) {
            if (mx[i].exists("disable") && (bool)mx[i]["disable"] == true) {
                continue;
            }
            MixerConfigSnapshot m;
            const char* name = mx[i].getName();
            m.name = name ? name : "";
            m.enabled = mx[i].exists("enabled") ? (bool)mx[i]["enabled"] : true;
            out->mixers.push_back(m);
        }
    }

    out->devices.clear();
    if (!root.exists("devices")) {
        *error = "config has no 'devices' section";
        return false;
    }
    libconfig::Setting& devs = root["devices"];
    for (int i = 0; i < devs.getLength(); i++) {
        if (devs[i].exists("disable") && (bool)devs[i]["disable"] == true) {
            continue;
        }
        DeviceConfigSnapshot dev;
        dev.type = devs[i].exists("type") ? (const char*)devs[i]["type"] : "rtlsdr";
        if (devs[i].exists("mode") && strncmp((const char*)devs[i]["mode"], "scan", 4) == 0) {
            dev.mode = R_SCAN;
        } else {
            dev.mode = R_MULTICHANNEL;
        }
        dev.sample_rate = devs[i].exists("sample_rate") ? snapshot_parse_anynum2int(devs[i]["sample_rate"]) : 0;
        dev.centerfreq = (dev.mode == R_MULTICHANNEL && devs[i].exists("centerfreq")) ? snapshot_parse_anynum2int(devs[i]["centerfreq"]) : 0;
        dev.has_gain = snapshot_get_numeric_gain(devs[i], &dev.gain);

        dev.channel_enabled.clear();
        dev.channel_signature.clear();
        dev.raw_channel_indices.clear();
        dev.raw_channels_setting = nullptr;
        if (devs[i].exists("channels")) {
            libconfig::Setting& chans = devs[i]["channels"];
            dev.raw_channels_setting = &chans;
            for (int j = 0; j < chans.getLength(); j++) {
                if (chans[j].exists("disable") && (bool)chans[j]["disable"] == true) {
                    continue;
                }
                dev.channel_enabled.push_back(chans[j].exists("enabled") ? (bool)chans[j]["enabled"] : true);
                dev.channel_signature.push_back(build_channel_identity_signature(chans[j]));
                dev.raw_channel_indices.push_back(j);
            }
        }
        dev.channel_count = (int)dev.channel_enabled.size();

        out->devices.push_back(dev);
    }
    return true;
}

DiffResult compute_and_apply_diff(const ConfigSnapshot& snapshot) {
    DiffResult result;

    if ((int)snapshot.devices.size() != device_count) {
        result.skipped_requires_restart.push_back("device count changed (" + to_string(device_count) + " -> " + to_string(snapshot.devices.size()) + ")");
    } else {
        for (int i = 0; i < device_count; i++) {
            device_t* dev = devices + i;
            const DeviceConfigSnapshot& snap = snapshot.devices[i];
            string label = "device[" + to_string(i) + "]";

            if (dev->input->driver_type != nullptr && snap.type != string(dev->input->driver_type)) {
                result.skipped_requires_restart.push_back(label + ": driver type changed");
                continue;  // nothing else about this device is safely comparable
            }
            if (snap.mode != dev->mode) {
                result.skipped_requires_restart.push_back(label + ": mode changed");
                continue;
            }
            if (snap.sample_rate != 0 && snap.sample_rate != dev->input->sample_rate) {
                result.skipped_requires_restart.push_back(label + ": sample_rate changed");
            }

            // Longest common prefix, by raw config signature (build_channel_identity_signature(),
            // config.cpp), between what's live and what the file now says - see DeviceConfig
            // Snapshot::channel_signature's comment. Channels from L onward differ in SOME way
            // (a pure count change, a reordering, or an in-place field edit - freq/modulation/
            // bandwidth/squelch/notch/ctcss/outputs - all look the same at this level: "the
            // channel that used to be at this index isn't there anymore").
            int compare_limit = min((int)dev->channel_count, snap.channel_count);
            int common_prefix = 0;
            while (common_prefix < compare_limit && dev->channels[common_prefix].config_signature != nullptr &&
                   snap.channel_signature[common_prefix] == dev->channels[common_prefix].config_signature) {
                common_prefix++;
            }
            bool tail_diverges = common_prefix < dev->channel_count || common_prefix < snap.channel_count;

            if (tail_diverges && dev->mode != R_MULTICHANNEL) {
                // Structurally shouldn't happen for a real R_SCAN config (validated at parse time
                // to always have exactly one channel) - but kept as an explicit, cheap guard
                // matching the documented non-goal: R_SCAN's single channel holds a "freqs" list
                // and its own controller-thread fixed-offset retune scheme (see item 2's "Device
                // Modes"), neither of which this tail-replace mechanism (or item 27/29's add/
                // remove it's built from) has ever supported.
                result.skipped_requires_restart.push_back(label + ": channel count or definition changed (R_SCAN devices don't support live channel changes)");
            } else if (tail_diverges) {
                bool ok = true;
                if (common_prefix < dev->channel_count) {
                    int to_remove = dev->channel_count - common_prefix;
                    string apply_error;
                    int removed = try_remove_channels(dev, common_prefix, &apply_error);
                    if (removed > 0) {
                        result.applied.push_back(label + ": removed " + to_string(removed) + " channel(s) from the tail" + (common_prefix < snap.channel_count ? " (replacing)" : ""));
                    }
                    if (removed < to_remove) {
                        result.skipped_requires_restart.push_back(label + ": " + to_string(to_remove - removed) + " channel(s) could not be removed: " + apply_error);
                        ok = false;
                    }
                }
                if (ok && dev->channel_count < snap.channel_count) {
                    int to_add = snap.channel_count - dev->channel_count;
                    if (dev->channel_count + to_add > dev->channel_capacity) {
                        result.skipped_requires_restart.push_back(label + ": " + to_string(to_add) + " new/replacement channel(s) declared but only " +
                                                                  to_string(dev->channel_capacity - dev->channel_count) + " reserve_channels slot(s) available");
                    } else {
                        int added_from = dev->channel_count;
                        string apply_error;
                        if (try_append_channels(dev, i, snap, &apply_error)) {
                            result.applied.push_back(label + ": added " + to_string(dev->channel_count - added_from) + " channel(s) (index " + to_string(added_from) + "-" +
                                                     to_string(dev->channel_count - 1) + ")");
                        } else {
                            result.skipped_requires_restart.push_back(label + ": " + to_string(to_add) + " new/replacement channel(s) failed to parse and were not added: " + apply_error +
                                                                      " (fix the config and retry reload_diff - no restart needed)");
                        }
                    }
                }
            }

            // Untouched common-prefix channels can still have had just "enabled" toggled - the
            // one field intentionally excluded from the signature above, so it never forces a
            // tear-down/replace of its own. Runs regardless of whether a tail replace also
            // happened above.
            for (int j = 0; j < common_prefix; j++) {
                channel_t* channel = dev->channels + j;
                bool live_enabled = channel->enabled.load();
                if (snap.channel_enabled[j] != live_enabled) {
                    bool confirmed = snap.channel_enabled[j] ? channel_request_enable(channel) : channel_request_disable(channel);
                    result.applied.push_back(label + " channel[" + to_string(j) + "]: enabled -> " + (snap.channel_enabled[j] ? "true" : "false") +
                                             (confirmed ? "" : " (request posted, not yet confirmed applied)"));
                }
            }

            if (dev->mode == R_MULTICHANNEL && snap.centerfreq != 0 && snap.centerfreq != dev->input->centerfreq) {
                if (device_request_retune(dev, snap.centerfreq)) {
                    result.applied.push_back(label + ": centerfreq -> " + to_string(snap.centerfreq));
                } else {
                    result.skipped_requires_restart.push_back(label + ": centerfreq changed but retune request was rejected");
                }
            }

            if (snap.has_gain) {
                // No live-readable "current gain" exists (each driver's dev_data is private to
                // its own .cpp), so this reapplies unconditionally rather than truly diffing -
                // harmless (idempotent), just not a precise "only report a real change" signal.
                errno = 0;
                if (input_set_gain(dev->input, snap.gain) == 0) {
                    result.applied.push_back(label + ": gain -> " + to_string(snap.gain));
                } else if (errno != ENOTSUP) {
                    result.skipped_requires_restart.push_back(label + ": gain present in config but failed to apply live, see logs");
                }
            }
        }
    }

    if ((int)snapshot.mixers.size() != mixer_count) {
        result.skipped_requires_restart.push_back("mixer count changed");
    } else {
        for (int i = 0; i < mixer_count; i++) {
            mixer_t* mixer = mixers + i;
            const MixerConfigSnapshot& snap = snapshot.mixers[i];
            if (mixer->name != nullptr && snap.name != string(mixer->name)) {
                result.skipped_requires_restart.push_back("mixer[" + to_string(i) + "]: name changed");
                continue;
            }
            if (snap.enabled != mixer->enabled) {
                bool confirmed = snap.enabled ? mixer_request_enable(mixer) : mixer_request_disable(mixer);
                result.applied.push_back(string("mixer '") + (mixer->name ? mixer->name : "") + "': enabled -> " + (snap.enabled ? "true" : "false") +
                                         (confirmed ? "" : " (request posted, not yet confirmed applied)"));
            }
        }
    }

    return result;
}
