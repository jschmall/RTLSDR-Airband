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

bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len) {
    // pre-allocate the stereo buffer
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    // pre-allocate the format conversion buffer, if the configured format is not native float
    if (sdata->format != STREAM_FORMAT_FLOAT32) {
        sdata->convert_buffer_len = (mode == MM_STEREO) ? len * 2 : len;
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

    log(LOG_INFO, "udp_stream: sending %s %s at %d Hz to %s:%s\n", mode == MM_MONO ? "Mono" : "Stereo", format_name(sdata->format), WAVE_RATE, sdata->dest_address, sdata->dest_port);
    return true;
}

// len is a sample count (not a byte count) in all udp_stream_write()/udp_stream_init() signatures.
void udp_stream_write(udp_stream_data* sdata, const float* data, size_t len) {
    if (sdata->send_socket == -1) {
        return;
    }

    // Send without blocking or checking for success
    switch (sdata->format) {
        case STREAM_FORMAT_S16LE: {
            if (len > sdata->convert_buffer_len) {
                log(LOG_ERR, "udp_stream: len %zu exceeds S16LE convert buffer size %zu, dropping packet\n", len, sdata->convert_buffer_len);
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
        if (len * 2 > sdata->stereo_buffer_len) {
            log(LOG_ERR, "udp_stream: len %zu exceeds stereo buffer size %zu, dropping packet\n", len * 2, sdata->stereo_buffer_len);
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
