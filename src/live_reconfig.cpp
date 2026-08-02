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
#include <libconfig.h++>
#include "input-common.h"
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

}  // namespace

bool parse_config_snapshot(const string& config_path, ConfigSnapshot* out, string* error) {
    libconfig::Config cfg;
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
        if (devs[i].exists("channels")) {
            libconfig::Setting& chans = devs[i]["channels"];
            for (int j = 0; j < chans.getLength(); j++) {
                if (chans[j].exists("disable") && (bool)chans[j]["disable"] == true) {
                    continue;
                }
                dev.channel_enabled.push_back(chans[j].exists("enabled") ? (bool)chans[j]["enabled"] : true);
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

            if (snap.channel_count != dev->channel_count) {
                result.skipped_requires_restart.push_back(label + ": channel count changed");
            } else {
                for (int j = 0; j < dev->channel_count; j++) {
                    channel_t* channel = dev->channels + j;
                    bool live_enabled = channel->enabled.load();
                    if (snap.channel_enabled[j] != live_enabled) {
                        bool confirmed = snap.channel_enabled[j] ? channel_request_enable(channel) : channel_request_disable(channel);
                        result.applied.push_back(label + " channel[" + to_string(j) + "]: enabled -> " + (snap.channel_enabled[j] ? "true" : "false") +
                                                 (confirmed ? "" : " (request posted, not yet confirmed applied)"));
                    }
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
