/*
 * udp_stream.cpp
 *
 * Copyright (C) 2024 charlie-foxtrot
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
#include <string.h>  // strerror()
#include <syslog.h>  // LOG_INFO / LOG_ERR
#include <unistd.h>  // close()
#include <cmath>     // lrintf()
#include <cstdint>   // int16_t, int8_t

#include <arpa/inet.h>  // inet_aton()
#include <netdb.h>      // getaddrinfo()

#include "rtl_airband.h"

static size_t bytes_per_sample(udp_stream_format format) {
    switch (format) {
        case STREAM_FORMAT_S16LE:
            return sizeof(int16_t);
        case STREAM_FORMAT_S8:
            return sizeof(int8_t);
        case STREAM_FORMAT_FLOAT32:
        default:
            return sizeof(float);
    }
}

static const char* format_name(udp_stream_format format) {
    switch (format) {
        case STREAM_FORMAT_S16LE:
            return "16-bit signed int";
        case STREAM_FORMAT_S8:
            return "8-bit signed int";
        case STREAM_FORMAT_FLOAT32:
        default:
            return "32-bit float";
    }
}

static int16_t float_to_s16(float sample) {
    if (sample > 1.0f) {
        sample = 1.0f;
    } else if (sample < -1.0f) {
        sample = -1.0f;
    }
    return (int16_t)lrintf(sample * 32767.0f);
}

static int8_t float_to_s8(float sample) {
    if (sample > 1.0f) {
        sample = 1.0f;
    } else if (sample < -1.0f) {
        sample = -1.0f;
    }
    return (int8_t)lrintf(sample * 127.0f);
}

// simple linear-interpolation resampler - not broadcast-quality, but correct and
// adequate for narrowband voice audio, and enough to decouple the udp_stream wire
// rate from the build's internal WAVE_RATE without pulling in a real DSP resampling
// library for what's typically a single fixed 16000->8000 (NFM build) conversion.
size_t resample_linear(const float* in, size_t len_in, int rate_in, int rate_out, float* out, size_t out_capacity) {
    if (len_in == 0 || rate_in <= 0 || rate_out <= 0 || out_capacity == 0) {
        return 0;
    }

    const double ratio = (double)rate_in / (double)rate_out;
    size_t len_out = (size_t)((double)len_in * rate_out / rate_in);
    if (len_out > out_capacity) {
        len_out = out_capacity;
    }

    for (size_t i = 0; i < len_out; ++i) {
        const double src_pos = (double)i * ratio;
        size_t idx0 = (size_t)src_pos;
        if (idx0 >= len_in) {
            idx0 = len_in - 1;
        }
        size_t idx1 = idx0 + 1;
        if (idx1 >= len_in) {
            idx1 = len_in - 1;
        }
        const double frac = src_pos - (double)idx0;
        out[i] = (float)(in[idx0] * (1.0 - frac) + in[idx1] * frac);
    }
    return len_out;
}

bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len) {
    // if a target sample_rate was configured and differs from WAVE_RATE, every
    // downstream buffer needs to be sized for the resampled length instead of len.
    // resample_buffer (mono) and resample_left_buffer/resample_right_buffer (stereo,
    // resampled independently per channel before interleaving) are mutually exclusive
    // by construction - only one pair is ever allocated, based on mode - so the mono
    // udp_stream_write() overload (also called internally by the stereo overload with
    // already-interleaved, already-resampled data) only resamples again when it's
    // actually the standalone mono path.
    // <= 0 means "unset" (e.g. a test constructing udp_stream_data directly rather
    // than through config.cpp, which always sets this to WAVE_RATE by default)
    if (sdata->sample_rate <= 0) {
        sdata->sample_rate = WAVE_RATE;
    }

    size_t resampled_len = len;
    sdata->resample_buffer = NULL;
    sdata->resample_buffer_len = 0;
    sdata->resample_left_buffer = NULL;
    sdata->resample_right_buffer = NULL;
    sdata->resample_stereo_buffer_len = 0;
    if (sdata->sample_rate != WAVE_RATE) {
        resampled_len = (size_t)((double)len * sdata->sample_rate / WAVE_RATE);
        if (mode == MM_STEREO) {
            sdata->resample_stereo_buffer_len = resampled_len;
            sdata->resample_left_buffer = (float*)XCALLOC(resampled_len, sizeof(float));
            sdata->resample_right_buffer = (float*)XCALLOC(resampled_len, sizeof(float));
        } else {
            sdata->resample_buffer_len = resampled_len;
            sdata->resample_buffer = (float*)XCALLOC(resampled_len, sizeof(float));
        }
    }

    // pre-allocate the stereo buffer
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = resampled_len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    // pre-allocate the format conversion buffer, if the configured format is not native float
    if (sdata->format != STREAM_FORMAT_FLOAT32) {
        sdata->convert_buffer_len = (mode == MM_STEREO) ? resampled_len * 2 : resampled_len;
        sdata->convert_buffer = XCALLOC(sdata->convert_buffer_len, bytes_per_sample(sdata->format));
    } else {
        sdata->convert_buffer_len = 0;
        sdata->convert_buffer = NULL;
    }

    sdata->send_socket = -1;
    sdata->dest_sockaddr_len = 0;

    // lookup address / port
    struct addrinfo hints, *result, *rptr;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    int error = getaddrinfo(sdata->dest_address, sdata->dest_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "udp_stream: could not resolve %s:%s - %s\n", sdata->dest_address, sdata->dest_port, gai_strerror(error));
        return false;
    }

    // check each result and try to create a connection
    for (rptr = result; rptr != NULL; rptr = rptr->ai_next) {
        sdata->send_socket = socket(rptr->ai_family, rptr->ai_socktype, rptr->ai_protocol);
        if (sdata->send_socket == -1) {
            log(LOG_ERR, "udp_stream: socket failed: %s\n", strerror(errno));
            continue;
        }

        if (connect(sdata->send_socket, rptr->ai_addr, rptr->ai_addrlen) == -1) {
            log(LOG_INFO, "udp_stream: connect to %s:%s failed: %s\n", sdata->dest_address, sdata->dest_port, strerror(errno));
            close(sdata->send_socket);
            sdata->send_socket = -1;
            continue;
        }

        sdata->dest_sockaddr = *rptr->ai_addr;
        sdata->dest_sockaddr_len = rptr->ai_addrlen;
        break;
    }
    freeaddrinfo(result);

    // error if no valid socket
    if (sdata->send_socket == -1) {
        log(LOG_ERR, "udp_stream: could not set up UDP socket to %s:%s - all addresses failed\n", sdata->dest_address, sdata->dest_port);
        return false;
    }

    log(LOG_INFO, "udp_stream: sending %s %s at %d Hz to %s:%s\n", mode == MM_MONO ? "Mono" : "Stereo", format_name(sdata->format), sdata->sample_rate, sdata->dest_address, sdata->dest_port);
    return true;
}

// len is a sample count (not a byte count) in all udp_stream_write()/udp_stream_init() signatures.
void udp_stream_write(udp_stream_data* sdata, const float* data, size_t len) {
    if (sdata->send_socket == -1) {
        return;
    }

    // resample_buffer is only ever allocated for the standalone mono path (see
    // udp_stream_init()) - the stereo overload below already resamples before
    // interleaving, so this is a no-op on its internal recursive call
    if (sdata->resample_buffer) {
        len = resample_linear(data, len, WAVE_RATE, sdata->sample_rate, sdata->resample_buffer, sdata->resample_buffer_len);
        data = sdata->resample_buffer;
    }

    // Send without blocking or checking for success
    switch (sdata->format) {
        case STREAM_FORMAT_S16LE: {
            if (len > sdata->convert_buffer_len) {
                log(LOG_ERR, "udp_stream: len %zu exceeds S16LE convert buffer size %zu, dropping packet\n", len, sdata->convert_buffer_len);
                sdata->dropped_packet_count++;
                return;
            }
            int16_t* buf = (int16_t*)sdata->convert_buffer;
            for (size_t i = 0; i < len; ++i) {
                buf[i] = float_to_s16(data[i]);
            }
            sendto(sdata->send_socket, buf, len * sizeof(int16_t), MSG_DONTWAIT | MSG_NOSIGNAL, &sdata->dest_sockaddr, sdata->dest_sockaddr_len);
            break;
        }
        case STREAM_FORMAT_S8: {
            if (len > sdata->convert_buffer_len) {
                log(LOG_ERR, "udp_stream: len %zu exceeds S8 convert buffer size %zu, dropping packet\n", len, sdata->convert_buffer_len);
                sdata->dropped_packet_count++;
                return;
            }
            int8_t* buf = (int8_t*)sdata->convert_buffer;
            for (size_t i = 0; i < len; ++i) {
                buf[i] = float_to_s8(data[i]);
            }
            sendto(sdata->send_socket, buf, len * sizeof(int8_t), MSG_DONTWAIT | MSG_NOSIGNAL, &sdata->dest_sockaddr, sdata->dest_sockaddr_len);
            break;
        }
        case STREAM_FORMAT_FLOAT32:
        default:
            sendto(sdata->send_socket, data, len * sizeof(float), MSG_DONTWAIT | MSG_NOSIGNAL, &sdata->dest_sockaddr, sdata->dest_sockaddr_len);
            break;
    }
}

void udp_stream_write(udp_stream_data* sdata, const float* data_left, const float* data_right, size_t len) {
    if (sdata->send_socket != -1) {
        // each channel is resampled independently before interleaving - resampling
        // the already-interleaved buffer would blend left/right samples together
        if (sdata->resample_left_buffer) {
            resample_linear(data_left, len, WAVE_RATE, sdata->sample_rate, sdata->resample_left_buffer, sdata->resample_stereo_buffer_len);
            len = resample_linear(data_right, len, WAVE_RATE, sdata->sample_rate, sdata->resample_right_buffer, sdata->resample_stereo_buffer_len);
            data_left = sdata->resample_left_buffer;
            data_right = sdata->resample_right_buffer;
        }
        if (len * 2 > sdata->stereo_buffer_len) {
            log(LOG_ERR, "udp_stream: len %zu exceeds stereo buffer size %zu, dropping packet\n", len * 2, sdata->stereo_buffer_len);
            sdata->dropped_packet_count++;
            return;
        }
        for (size_t i = 0; i < len; ++i) {
            sdata->stereo_buffer[2 * i] = data_left[i];
            sdata->stereo_buffer[2 * i + 1] = data_right[i];
        }
        udp_stream_write(sdata, sdata->stereo_buffer, len * 2);
    }
}

void udp_stream_shutdown(udp_stream_data* sdata) {
    if (sdata->send_socket != -1) {
        close(sdata->send_socket);
    }
}
