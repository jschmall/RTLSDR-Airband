/*
 * input-common.h
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
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

#ifndef _INPUT_COMMON_H
#define _INPUT_COMMON_H 1
#include <pthread.h>
#include <libconfig.h++>

#if __GNUC__ >= 4
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MODULE_EXPORT extern "C"
#endif /* __GNUC__ */

typedef enum { SFMT_UNDEF = 0, SFMT_U8, SFMT_S8, SFMT_S16, SFMT_F32 } sample_format_t;
#define SAMPLE_FORMAT_CNT 5

typedef enum { INPUT_UNKNOWN = 0, INPUT_INITIALIZED, INPUT_RUNNING, INPUT_FAILED, INPUT_STOPPED, INPUT_DISABLED } input_state_t;
#define INPUT_STATE_CNT 6

typedef struct input_t input_t;

struct input_t {
    // Set once by each driver's own <type>_input_new(), e.g. "rtlsdr"/"soapysdr"/"mirisdr"/
    // "file" - matches the config file's "type" string. Only consumed by the dynamic_reload
    // reload_diff path (live_reconfig.cpp) to detect a device's driver type changing across a
    // config edit, which is out of v1 scope and needs a restart rather than a live apply.
    const char* driver_type;
    unsigned char* buffer;
    void* dev_data;
    size_t buf_size, bufs, bufe;
    size_t overflow_count;
    size_t underrun_count;
    // Failed input_set_centerfreq() calls (transient hardware errors on an already-running
    // device) - does not affect input->state, unlike overflow/underrun which are informational
    // only. See input_set_centerfreq()'s comment (input-common.cpp).
    size_t centerfreq_retune_failure_count;
    input_state_t state;
    sample_format_t sfmt;
    float fullscale;
    int bytes_per_sample;
    int sample_rate;
    int centerfreq;
    int (*parse_config)(input_t* const input, libconfig::Setting& cfg);
    int (*init)(input_t* const input);
    void* (*run_rx_thread)(void* input_ptr);  // to be launched via pthread_create()
    int (*set_centerfreq)(input_t* const input, int const centerfreq);
    // set_gain/set_bandwidth are nullable, unlike set_centerfreq - not every driver has a live
    // hook for these (e.g. rtlsdr has no tuner bandwidth API at all). input_set_gain()/
    // input_set_bandwidth() return -1/ENOTSUP when the driver's pointer is NULL, so callers get
    // a clear "not supported by this driver" result instead of a silent no-op.
    int (*set_gain)(input_t* const input, float const gain);
    int (*set_bandwidth)(input_t* const input, int const bandwidth);
    int (*stop)(input_t* const input);
    pthread_t rx_thread;
    pthread_mutex_t buffer_lock;
};

input_t* input_new(char const* const type);
int input_init(input_t* const input);
int input_parse_config(input_t* const input, libconfig::Setting& cfg);
int input_start(input_t* const input);
int input_set_centerfreq(input_t* const input, int const centerfreq);
int input_set_gain(input_t* const input, float const gain);
int input_set_bandwidth(input_t* const input, int const bandwidth);
int input_stop(input_t* const input);

#endif /* _INPUT_COMMON_H */
