# Fork Changelog

Tracks changes specific to this fork ([`jschmall/RTLSDR-Airband`](https://github.com/jschmall/RTLSDR-Airband)),
on top of what's inherited from [`rtl-airband/RTLSDR-Airband`](https://github.com/rtl-airband/RTLSDR-Airband).
Upstream changes are not duplicated here — see `git log upstream/main` or upstream's PR history for those.

This fork has no release tags; the binary's `-v` output is a git commit hash, generated at build
time from the working tree (see `CMakeLists.txt` / `src/CMakeModules/version.cmake`). Entries below
are dated by when the change was made, not by a version number.

## 2026-07-23 (rdio_api branch)

- **Add native rdio-scanner call-upload support** (`src/rdio_scanner.cpp`, new) — replaces the
  `post_write_script` + external CSV mapping (`rdio_mappings.csv`) previously used to push
  completed transmissions to a [rdio-scanner](https://github.com/chuot/rdio-scanner) instance.
  New `rdio_scanner: { server; port; use_tls; api_key; system_id; system_label; talkgroup_id;
  talkgroup_label; talkgroup_tag; talkgroup_group; source_id; delete_after_upload; timeout_ms;
  max_retries; }` config group on `file` outputs (requires `split_on_transmission = true`).
  System/talkgroup metadata now lives directly in the channel's config instead of being looked
  up from a directory-name-keyed CSV, and the upload timestamp/frequency come straight from the
  transmission's actual `timeval`/`channel_t` state instead of being re-parsed out of the output
  filename. Uploads run on a single background worker thread via libcurl (bounded queue,
  drop-oldest-and-log on overflow, bounded retries), so an unreachable rdio-scanner instance
  can't block the output thread or the audio pipeline; a failed upload never deletes the local
  MP3 even with `delete_after_upload = true`. New build dependency: `libcurl4-openssl-dev`,
  gated behind `-DRDIO_SCANNER=ON` (default ON). Field-mapping logic covered by
  `test_rdio_scanner.cpp`; the worker/queue/libcurl path was validated end-to-end by hand against
  a mock `/api/call-upload` server (success, `delete_after_upload`, and unreachable-server retry
  cases) rather than an automated system test — that's a documented follow-up.
  `R_SCAN` (frequency-scanning) channels are not yet supported.

## 2026-07-23

- **Fix: `udp_stream` output sent 4x too much data per buffer, reading past `channel->waveout`.**
  The `sendto` byte-length fix from 2026-06-13 (`374c52c`) changed `udp_stream_write()`'s internal
  `len` convention from "already bytes" to "sample count, converted to bytes internally," but the
  three call sites (`src/rtl_airband.cpp` `init_output()`, `src/output.cpp` `process_outputs()`
  mono/stereo branches) still passed `WAVE_BATCH * sizeof(float)` — a byte count under the old
  convention. The double conversion meant every packet requested 4x the correct payload, reading
  past the end of `channel->waveout`/`waveout_r` into adjacent `channel_t` memory and transmitting
  it. This is what had been showing up downstream as "~4x realtime throughput" and choppy audio,
  previously misdiagnosed as a missing send-side pacing loop — see the (now removed) "Open Issue:
  UDP Stream Pacing" section that used to be in `CLAUDE.md`. Fixed by making `len` a sample count
  consistently across `src/output.cpp`, `src/rtl_airband.cpp`, and `src/udp_stream.cpp`.
- **Add unit tests for `udp_stream.cpp`** (`src/test_udp_stream.cpp`) — asserts mono and stereo
  `udp_stream_write()` calls produce the exact expected byte count and content on a real loopback
  UDP socket, covering the contract the bug above violated. Wired `udp_stream.cpp` into the
  unit test build (`src/CMakeLists.txt`).

## 2026-06-13

- **Fix: `sendto()` byte length in `udp_stream.cpp`** (`374c52c`) — was passed a float element
  count where it needed a byte count.

## 2025-06-29

- **Add `post_write_script` + `min_rx_seconds` output options** (`ad2540b`), cherry-picked from
  `yegors/RTLSDR-Airband` commit `bb36bb0`. Both require `split_on_transmission = true`;
  `post_write_script` is used to upload completed transmission files to RDIO via API.
