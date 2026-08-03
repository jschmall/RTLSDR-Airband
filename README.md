# RTLSDR-Airband

![version tag](https://img.shields.io/github/v/tag/rtl-airband/RTLSDR-Airband?label=)

## Fork-Specific Features

This is a personal fork of [`rtl-airband/RTLSDR-Airband`](https://github.com/rtl-airband/RTLSDR-Airband)
that tracks upstream `main` and carries a small delta on top of it:

- **`post_write_script` and `min_rx_seconds` file-output options** — run a script after
  each completed recording and skip saving very short transmissions. Cherry-picked from
  [`yegors/RTLSDR-Airband`](https://github.com/yegors/RTLSDR-Airband). Both require
  `split_on_transmission = true`.
- **Native rdio-scanner call uploads** — send completed transmissions straight to a
  [rdio-scanner](https://github.com/chuot/rdio-scanner) server for playback, no external
  script or CSV lookup required.
- **`send_tx_tags` Icecast output option** — push the channel's (or, for a mixer, whichever
  source channel is currently talking) configured `label` as the Icecast "song" tag when a
  transmission starts, and clear it when squelch closes — an on-air indicator for plain
  (non-scanning) channels and mixers. Complements the existing `send_scan_freq_tags`, which
  only applies in scan mode.
- **Configurable `udp_stream` output** — choose the bit depth (32/16/8-bit PCM) and
  sample rate sent over UDP, to match what a downstream consumer like trunk-recorder
  expects.
- **Structured JSON logging** — optional `-j` flag for single-line JSON logs, easier to
  parse and aggregate than the default plain-text format.
- **HTTP metrics endpoint** — serve the existing Prometheus stats file over plain HTTP
  so it can be scraped directly, instead of relying on a textfile collector.
- **SIGHUP config reload** — SIGHUP now triggers a clean restart to pick up config
  changes, instead of just exiting.
- **Assorted stability fixes** — buffer overflow, thread-safety, and error-handling
  fixes not yet merged upstream.

### Metrics Exposed via the Stats Endpoint

`stats_filepath` (and the HTTP metrics endpoint serving it) is written in Prometheus
text-exposition format every 15 seconds. On top of upstream's per-channel signal/squelch
metrics, this fork adds counters for diagnosing *why* a device or output is falling behind,
rather than just knowing that it is.

**Per-device input buffer** (label: `device`):
| Metric | Meaning |
|---|---|
| `buffer_overflow_count` | RX thread overwrote I/Q samples the demod thread hadn't read yet — the demod thread is falling behind. |
| `buffer_underrun_count` | Demod thread found insufficient samples to process a batch and had to wait. Expected to increment frequently under healthy load; a value that goes flat while `buffer_overflow_count` climbs for the same device is the signature of a CPU-saturated demod thread rather than USB/host starvation. |

**Output handoff** (labels: `device` or `mixer`, plus `input` for mixer inputs):
| Metric | Meaning |
|---|---|
| `output_overrun_count` | Demod (or mixer) thread produced a new batch before the output thread drained the previous one. |
| `input_overrun_count` | A device fed a mixer input before the mixer thread consumed the prior contents. |

**Per-output** (labels: `device`+`channel`+`output`, or `mixer`+`output`):
| Metric | Meaning |
|---|---|
| `icecast_disconnect_count` | Icecast output's connection was lost (network error, exceeded send backlog, or the owning device failing) and had to be reconnected. |
| `icecast_backlog_exceeded_count` | Subset of the above — specifically caused by the local encode rate outpacing what Icecast could drain. |
| `icecast_tx_tag_update_count` | Number of times a `send_tx_tags` metadata update (on-air label set/cleared) was successfully applied. |
| `lame_encode_failure_count` | `lame_encode_buffer_ieee_float()` returned an error, for any output that encodes mp3 (icecast, file). |
| `file_write_failure_count` | Short/failed `fwrite()` on a file or rawfile output; the output is disabled immediately after, so this should stay at 0 on a healthy instance. |
| `udp_stream_dropped_packet_count` | A `udp_stream` packet was dropped by a bounds check (length/buffer-size mismatch) instead of overrunning a buffer. |
| `pulse_underflow_count` / `pulse_overflow_count` / `pulse_disconnect_count` | PulseAudio stream underflow/overflow, and disconnects forced by the write path (latency check or a failed write). Requires the `PULSEAUDIO` build option. |

**Process-wide**:
| Metric | Meaning |
|---|---|
| `process_cpu_seconds_total` | Cumulative user+system CPU time for this process (standard Prometheus convention — `rate()` this in Grafana to get CPU utilization, correlatable against the buffer/output counters above). |
| `rdio_scanner_queue_drop_count` / `rdio_scanner_upload_failure_count` | Completed transmissions dropped because the shared upload queue was full, and uploads that failed after exhausting retries. Shared by every `rdio_scanner`-configured output (one upload queue/worker thread process-wide). Requires the `RDIO_SCANNER` build option. |

### Major / Minor Version Changes:

Changes as of v5.1.0:
 - License is now GPLv2 [#503](https://github.com/rtl-airband/RTLSDR-Airband/discussions/503)

NOTE: Repo URL has moved to https://github.com/rtl-airband/RTLSDR-Airband see [#502](https://github.com/rtl-airband/RTLSDR-Airband/discussions/502) for info

Changes as of v5.0.0:
 - PRs will be opened directly against `main` and the `unstable` branch will no longer be used
 - Version tags will be automatically created on each merge to `main`
 - A release will be created on each `major` or `minor` version tag but not `minor` tags
 - Checking out `main` is recommended over using a release artifact to stay on the latest version
 - This repo has significantly diverged from the original project [microtony/RTLSDR-Airband](https://github.com/microtony/RTLSDR-Airband) so it has been been detached (ie no longer a fork).
 - Specific build support for `rpiv1`, `armv7-generic`, and `armv8-generic` have been deprecated for the new default `native`, see [#447](https://github.com/rtl-airband/RTLSDR-Airband/discussions/447)


## Overview

RTLSDR-Airband receives analog radio voice channels and produces
audio streams which can be routed to various outputs, such as online
streaming services like LiveATC.net. Originally the only SDR type
supported by the program was Realtek DVB-T dongle (hence the project's
name). However, thanks to SoapySDR vendor-neutral SDR library, other
radios are now supported as well.

## Documentation

User's manual is now on the [wiki](https://github.com/rtl-airband/RTLSDR-Airband/wiki).

## Credits and thanks

I hereby express my gratitude to everybody who helped with the development and testing
of RTLSDR-Airband. Special thanks go to:

* Dave Pascoe
* SDR Guru
* Marcus Ströbel
* strix-technica
* Tomasz Lemiech
* charlie-foxtrot

## License

Copyright (C) 2022-2025 charlie-foxtrot

Copyright (C) 2015-2022 Tomasz Lemiech <szpajder@gmail.com>

Based on original work by Wong Man Hang <microtony@gmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <https://www.gnu.org/licenses/>.

## Open Source Licenses of bundled code

### gpu_fft

BCM2835 "GPU_FFT" release 2.0
Copyright (c) 2014, Andrew Holme.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of the copyright holder nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### rtl-sdr

* Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
* Copyright (C) 2015 by Kyle Keen <keenerd@gmail.com>
* GNU General Public License Version 2
