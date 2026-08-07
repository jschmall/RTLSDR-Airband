# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## This Is a Fork

This repo is a personal fork of [`rtl-airband/RTLSDR-Airband`](https://github.com/rtl-airband/RTLSDR-Airband).
Upstream is actively maintained; this fork tracks `main` and carries a small, deliberate delta.

Remotes to expect:

| Remote | Repo | Role |
|---|---|---|
| `origin` | `jschmall/RTLSDR-Airband` | This fork |
| `upstream` | `rtl-airband/RTLSDR-Airband` | Source of truth; rebase target |
| `yegors` | `yegors/RTLSDR-Airband` | Reference only — source of the cherry-picked file-output features |

**The value of this fork is that it stays close to upstream and remains easy to rebase.**
Keep the local delta small and well understood.

### Local changes carried on top of upstream

1. **`post_write_script` + `min_rx_seconds`** — cherry-picked from `yegors` commit `bb36bb0`
   ("Rework file output options"). Touches `src/config.cpp`, `src/output.cpp`, `src/rtl_airband.h`.
   Both options require `split_on_transmission = true`. `post_write_script` is used here to
   upload completed transmission files to RDIO via API.
2. **`sendto` byte-length fix** in `src/udp_stream.cpp` — `sendto()` was being passed a float
   *element* count where it needs a *byte* count. Corrected to `len * sizeof(float)`.
3. **UDP unit-mismatch follow-up fix** — the fix above changed `udp_stream_write()`'s internal
   `len` convention from "already bytes" to "sample count, converted to bytes internally", but the
   three call sites (`src/rtl_airband.cpp` `init_output()`, `src/output.cpp` `process_outputs()`
   mono/stereo branches) were left passing `WAVE_BATCH * sizeof(float)` — a byte count under the
   old convention. Net effect: every UDP packet carried 4x the intended payload, reading past the
   end of `channel->waveout`/`waveout_r` into adjacent `channel_t` memory. This explains the
   "~4x realtime throughput" and choppy-audio symptoms below — it was never a pacing problem.
   Fixed by making `len` a sample count at all three call sites; see `test_udp_stream.cpp` for the
   regression test on `udp_stream.cpp`'s byte-length contract.
4. **Native rdio-scanner call-upload support** (`src/rdio_scanner.cpp`, new) — replaces the
   `post_write_script` + external CSV lookup this fork previously used to push completed
   transmissions to a [rdio-scanner](https://github.com/chuot/rdio-scanner) instance's
   `/api/call-upload` endpoint. Adds a `rdio_scanner: { ... }` nested config group on `file`
   outputs (requires `split_on_transmission = true`, same precedent as `min_rx_seconds` /
   `post_write_script`): `server`, `port`, `use_tls`, `api_key`, `system_id`, `system_label`,
   `talkgroup_id`, `talkgroup_label`, `talkgroup_tag`, `talkgroup_group`, `source_id`,
   `delete_after_upload`, `timeout_ms`, `max_retries`. System/talkgroup metadata is declared
   directly per-channel in the config (no path parsing, no CSV file to keep in sync — the CSV
   this replaced mapped `(agency, channel)` directory names to those same fields).
   Uploads are queued (bounded, drop-oldest-and-log on overflow) and sent by a single background
   worker thread via libcurl, so a slow/unreachable rdio-scanner instance never blocks the output
   thread; a completed local MP3 is never deleted on a failed upload, even with
   `delete_after_upload = true`. New build dependency: `libcurl4-openssl-dev`
   (`libcurl`/`CURL::libcurl` via CMake's `FindCURL`), gated behind `-DRDIO_SCANNER=ON` (default
   ON) / `#ifdef WITH_RDIO_SCANNER`, mirroring the existing `PULSEAUDIO` option pattern — builds
   with `-DRDIO_SCANNER=OFF` compile and run identically to before this feature existed.
   Pure field-mapping logic is unit tested in `test_rdio_scanner.cpp`; the worker
   thread/queue/libcurl path was validated manually end-to-end against a mock HTTP server (see
   git history for the session that added this) rather than via an automated system test — a
   system test with a fake `/api/call-upload` fixture is a documented follow-up, not yet done.
   `R_SCAN` (frequency-scanning) channels are not yet supported — the per-channel `talkgroup_id`
   model only maps cleanly to `R_MULTICHANNEL`, which is how this fork is deployed.
5. **`dateTime` instead of `timestamp` in the rdio-scanner upload fields** (`src/rdio_scanner.cpp`,
   `rdio_scanner_build_fields()`) — the initial implementation sent rdio-scanner's `timestamp`
   field (ms-epoch; per rdio-scanner's docs this is the *more* precise of its two accepted
   time fields). In production against the `10.0.50.36:3000` instance this caused nearly every
   real, distinct transmission on a busy talkgroup to be rejected as `duplicate call rejected`,
   with roughly one upload succeeding per hour (diagnosed from the rdio-scanner server log:
   every rejected call had a distinct filename/timestamp seconds-to-minutes apart, ruling out
   client-side retries — already confirmed separately by testing with `max_retries=0` — and
   ruling out a blanket server-side dedup time window, since the pre-existing external
   `post_write_script` upload script uploaded the same files successfully with calls only
   seconds apart). The one substantive difference between the two upload paths was this field:
   the script sends `dateTime` (RFC3339); the native code sent `timestamp` (ms epoch). Switched
   to `dateTime`, sent as a plain Unix epoch in **seconds** (`tv_sec`, UTC, no string formatting
   or hardcoded timezone — unlike the script this replaces, which parses the filename and
   applies a hardcoded `America/Los_Angeles` offset). This fix is a hypothesis validated by the
   script/native contrast, not a confirmed root cause inside rdio-scanner's own code — if
   duplicate rejections persist after this change, the next thing to check is whether this
   specific rdio-scanner deployment's `/api/call-upload` implementation has drifted from its
   documented field semantics for `timestamp` (version-specific parsing bug) rather than
   anything on this fork's side.
6. **Configurable `bit_depth` for `udp_stream` output** (`src/udp_stream.cpp`, `src/config.cpp`,
   `src/rtl_airband.h`) — the `udp_stream` output previously always sent raw 32-bit IEEE-754
   float PCM (`channel->waveout`/`waveout_r`, normalized to roughly ±1.0). Added an optional
   `bit_depth` integer field on a `udp_stream` output block: `32` (default, unchanged float32
   behavior), `16` (signed 16-bit little-endian PCM), or `8` (signed 8-bit PCM), to cut bandwidth
   for downstream consumers that don't need float precision. Conversion clamps each sample to
   [-1.0, 1.0] before scaling (by `INT16_MAX`/`INT8_MAX`) and rounding, so out-of-range AGC
   overshoot can't wrap/UB rather than erroring. Omitting `bit_depth` is fully backward
   compatible — existing configs and the pre-existing float32 byte-length regression test in
   `test_udp_stream.cpp` are unaffected. New `udp_stream_format` enum in `rtl_airband.h`;
   conversion buffer is separate from the existing float interleave `stereo_buffer`, so stereo
   interleaving logic is untouched. Unit tested (byte counts, converted values, clamping) in
   `test_udp_stream.cpp`. The 16-bit *format* (signed int16 PCM) matches what a real downstream
   consumer expects on the wire — trunkrecorder's own UDP audio plugin documents the same
   contract (16-bit int, 8 or 16 kHz, mono) — but the *sample rate* half of that contract is
   still open. A 2026-07-28 TTD (TwoToneDetect) log showing a clean `8000 Hz, mono, s16` WAV
   conversion was initially read as production validation of `bit_depth = 16`, but that's not
   solid: the WAV header's rate comes from whatever TTD itself is configured to write, not from
   anything rtl_airband sends, and ffmpeg will convert a mismatched-rate PCM stream cleanly
   without error (just wrong pitch/speed) — so a clean conversion doesn't prove the rate TTD
   assumed matches the rate rtl_airband actually sent. This instance is confirmed built with
   NFM enabled, so `WAVE_RATE` is 16000 (`rtl_airband.h:70`), not 8000 — TTD needs to be set to
   16000 Hz to match. `bit_depth = 32` (float) remains the default; `bit_depth = 16` is the
   right setting for trunkrecorder/TTD-style consumers, but end-to-end validation against a live
   consumer at the *correct* matching rate (16000, not 8000) is still outstanding.
   `bit_depth = 8` remains unit-tested only, not yet validated against a live consumer.
7. **SIGHUP reload via re-exec** (`src/rtl_airband.cpp`) — SIGHUP previously mapped to the same
   `do_exit` path as SIGINT/TERM/QUIT (a plain exit), meaning any config change across the ~12
   concurrent instances required a manual, one-at-a-time restart. SIGHUP now still runs the exact
   same clean-shutdown sequence (all devices/channels/threads torn down identically to a normal
   exit), but instead of returning from `main()`, calls `execvp(argv[0], argv)` once shutdown has
   fully completed, replacing the process image with a fresh instance under the same PID that
   re-reads the same `-c` config path from disk. This is a re-exec, not a true in-process hot
   reload — audio outputs still momentarily drop during the swap, same as a real restart, and a
   non-foreground (self-daemonizing, no `-F`) invocation would double-fork again on every reload,
   breaking PID-file-based tracking; this fork's actual deployment always uses `-F` (systemd
   manages the process directly), so that path isn't hit in practice. Manually validated end-to-end
   (not covered by an automated test): started the binary, sent `SIGHUP`, confirmed the same PID
   persisted through a full shutdown+restart log sequence; separately confirmed `SIGTERM` still
   exits normally without reloading.
8. **HTTP metrics endpoint** (`src/stats_http.cpp`, new) — `write_stats_file()` (`src/output.cpp`)
   already writes Prometheus-format stats to `stats_filepath` every 15s, but only to a file,
   requiring a textfile collector per host to scrape across the ~12 concurrent instances. Adds
   two top-level config options, `stats_http_address` and `stats_http_port` (must be set
   together, and require `stats_filepath` to also be set) — when present, a background thread
   serves the *current* content of `stats_filepath` (read fresh off disk on every request) over
   plain HTTP to any request on that address:port, regardless of method/path. Single-purpose and
   deliberately minimal: no request parsing beyond discarding the bytes, one connection handled
   at a time (a low-frequency Prometheus scrape target doesn't need concurrency), shutdown polls
   a flag every 500ms rather than force-closing the listening socket from another thread (which
   is racy on Linux). Unit tested end-to-end in `test_stats_http.cpp` (real sockets, real HTTP
   GET, asserts on response content, confirms a second scrape reflects an updated file, confirms
   `stats_http_shutdown()` is idempotent) and manually verified with `curl` against a live
   instance. No new build dependency — plain POSIX sockets, unconditionally compiled in (unlike
   `PULSEAUDIO`/`RDIO_SCANNER`, no `-D...=ON` flag needed).
9. **Configurable `sample_rate` for `udp_stream` output** (`src/udp_stream.cpp`, `src/config.cpp`,
   `src/rtl_airband.h`) — addresses the root cause behind item 6's still-open rate-matching
   follow-up: the wire sample rate was hard-coupled to the build's internal `WAVE_RATE` (8000
   without `NFM`, 16000 with), so a downstream consumer expecting a fixed rate (e.g.
   trunkrecorder/TTD's 8000 Hz) had no way to get it from an `NFM` build without the operator
   separately knowing to reconfigure the consumer side. Adds an optional `sample_rate` integer
   field on a `udp_stream` output block; when set and different from `WAVE_RATE`, each channel
   (mono, or left/right independently before interleaving for stereo — resampling the
   already-interleaved buffer would blend channels together) is linear-interpolation-resampled
   to the configured rate before the existing `bit_depth` conversion/send. Not broadcast-quality
   resampling, but correct and adequate for narrowband voice audio, and avoids pulling in a real
   DSP resampling library for what's typically a single fixed 16000→8000 conversion. Omitting
   `sample_rate` (or setting it equal to `WAVE_RATE`) is fully backward compatible — no
   resampling buffers are allocated and existing configs/tests are unaffected; confirmed via the
   existing `assert`-turned-real-check byte-length tests in `test_udp_stream.cpp` (item 2) plus
   new ones (`ResampleLinearTest`, plus mono/stereo `udp_stream_write()` cases with a configured
   rate). Also manually validated end-to-end: real binary, real UDP listener, `sample_rate` set
   to half `WAVE_RATE` — every received packet was exactly the expected halved byte count.
10. **Icecast mountpoint buffer-overflow fix** (`src/output.cpp`, `src/helper_functions.{h,cpp}`)
    — `shout_setup()` built the Icecast mountpoint into a fixed `char mp[100]` stack buffer via
    `sprintf()`, with no length validation on the config-supplied `mountpoint` value. Replaced
    with `std::string` built by a new `make_icecast_mountpoint()` helper, removing the
    fixed-size buffer (and the overflow risk) entirely rather than adding an arbitrary cap.
    Unit tested in `test_helper_functions.cpp`, including a 500-char mountpoint that would have
    overflowed the old buffer.
11. **UDP stream bounds checks are real, not `assert()`** (`src/udp_stream.cpp`) —
    `udp_stream_write()`'s `len`-vs-buffer-size checks were `assert()`, which is stripped by
    `-DNDEBUG` in the default `Release` build — the same bug class behind item 3's historical
    4x-oversend incident had no runtime protection left in the binary that actually ships.
    Replaced with real conditionals that log and drop the packet on a mismatch instead of
    writing past the buffer. Verified the check still compiles in under a Release build.
12. **rdio-scanner config validation for `timeout_ms`/`max_retries`** (`src/config.cpp`) —
    `timeout_ms = 0` passed straight through to libcurl's `CURLOPT_TIMEOUT_MS`, where 0 means
    "never time out," risking an indefinitely stuck worker thread; `max_retries < 0` silently
    skipped the retry loop entirely instead of erroring, reporting every job "failed" without a
    single upload attempt. Both are now rejected at config parse time, matching the validation
    already in place for this block's other fields.
13. **rdio-scanner worker shutdown is interruptible mid-retry** (`src/rdio_scanner.cpp`) — the
    retry/backoff loop never checked the shutdown flag, so `rdio_scanner_shutdown()` could block
    for the full duration of any in-progress retry (backoff sleep plus curl connect/transfer
    timeout) — directly delaying the one-at-a-time systemd restarts this fork's deployment
    relies on. A `CURLOPT_XFERINFOFUNCTION` progress callback now aborts an in-flight
    `curl_easy_perform()` once shutdown is requested, and the retry backoff waits on the same
    condvar the shutdown path signals instead of sleeping the full interval. A job already
    in flight at shutdown always gets its first attempt; only further retries are skipped.
14. **`channel_t::axcindicate`/`freq_idx`/`state` made atomic** (`src/rtl_airband.h`) — each is
    written by one thread and read by others (demod/controller/mixer/output) with no lock or
    atomic between them — a data race under the C++ memory model, even though it never caused a
    torn read in practice on this project's target platforms. Switched to `std::atomic<T>`; the
    implicit `T` conversion meant every existing call site needed zero changes.
15. **`init_output()` checks `airlame_init()`/`malloc()` failures** (`src/rtl_airband.cpp`) —
    previously returned `true` unconditionally even when LAME init or the `lamebuf` allocation
    failed, which would silently leave `output->lame`/`lamebuf` null and crash the *next* time
    the output thread tried to encode on that output, instead of failing cleanly at startup like
    both call sites in `main()` already expect.
16. **RTL-SDR tuning failures are now fatal** (`src/input-rtlsdr.cpp`) — `rtlsdr_init()` logged
    failures from `rtlsdr_set_sample_rate()`/`rtlsdr_set_center_freq()`/
    `rtlsdr_set_freq_correction()` but always returned success, so a device could report itself
    initialized and start streaming while silently sampling the wrong rate or frequency. Now
    returns `-1` on these three failures so `input_init()`'s existing fail-fast path
    (`INPUT_FAILED` → `error()`) actually fires, matching how the runtime retune path already
    handles the same failure.
17. **System tests for Icecast and rdio-scanner outputs**
    (`system_tests/helpers/fake_icecast_server.py`, `fake_rdio_scanner_server.py`,
    `tests/test_icecast_output.py`, `tests/test_rdio_scanner_output.py`) — the only two output
    types this fork's production deployment actually uses had no end-to-end coverage. Getting
    the fake Icecast fixture working surfaced two real libshout protocol quirks (documented
    inline): it needs `Connection: Keep-Alive` echoed back or it won't stream, and it always
    sends a second `Authorization`-bearing request on the same connection regardless of how the
    first was answered.
18. **`rdio_scanner` rejected on `R_SCAN` channels at config time** (`src/config.cpp`) — an
    `R_SCAN` channel's `talkgroup_id`/`system_id`/labels are fixed at config time, but its
    actual frequency changes at runtime as it scans — so every scanned frequency's completed
    transmission would silently upload under one static, near-certainly-wrong `talkgroup_id`.
    This was already a documented limitation (see item 4); this makes the parser actually
    enforce it instead of relying on the docs alone, by threading `dev->mode` through to
    `parse_outputs()`.
19. **Structured JSON logging (`-j`)** (`src/logging.{h,cpp}`) — opt-in flag that switches
    `log()` from plain-text `vsyslog`/`vfprintf` output to one JSON object per line
    (`{timestamp, level, pid, message}`), to whichever destination was already configured.
    Makes correlating log lines across this fork's ~12 concurrent instances in a central
    pipeline (Loki/ELK/journald) a field lookup instead of a regex. Plain text remains the
    default. `build_json_log_line()` is a pure function, unit tested directly, including
    escaping of quotes/backslashes/newlines/control characters.
20. **Unit tests for `util.cpp`/`mixer.cpp` pure functions** (`src/test_util.cpp`,
    `src/test_mixer.cpp`) — `delta_sec()`, `atofs()`, the `sincosf` LUT,
    `dBFS_to_level()`/`level_to_dBFS()`, and `mixer.cpp`'s `mix_waveforms()` had no coverage.
    Most of the rest of these files (and `output.cpp`/`rtl_airband.cpp`/`config.cpp`) is tightly
    coupled to threading, real sockets/files, or `libconfig::Setting` and isn't a good
    unit-test target without a larger refactor — this extracts what was already genuinely pure.
21. **Four small magic-number/fragility cleanups** — `rdio_scanner`'s hardcoded
    `MAX_QUEUE_DEPTH = 64` is now a validated `rdio_scanner_queue_depth` config option
    (`src/rdio_scanner.cpp`); two `memcpy()` calls in `src/output.cpp` used a hardcoded float
    size of `4` instead of `sizeof(float)`; `LAMEBUF_SIZE` (`src/rtl_airband.h`) was a flat
    `22000` marked `// todo: calculate` — tracing actual usage found the real worst case is
    `LameTone`'s 1-second silence marker (`WAVE_RATE` samples in one
    `lame_encode_buffer_ieee_float()` call), and per LAME's own `1.25*num_samples + 7200`
    formula the old value was actually **insufficient** on NFM builds
    (`1.25*16000+7200 = 27200 > 22000`) — now computed from `WAVE_RATE`; and the `getopt()`
    optstring in `src/rtl_airband.cpp` was a manually-sized `char[16]` grown with `strcat()` for
    each conditional build flag, replaced with `std::string` to remove the silent-overflow risk.
22. **`buffer_underrun_count` metric and `process_cpu_seconds_total` metric** — added while
    investigating whether output-overrun events on this fork's deployment are compute-bound or
    USB/host-bound. `buffer_overflow_count` (the RX-thread-to-demod-thread ring buffer overflow
    counter) only shows *that* the demod thread fell behind, not why. Two additions close that
    diagnosability gap (they don't change *when* overruns happen, only what's observable about
    them):
    - `buffer_underrun_count{device}` (`src/rtl_airband.cpp`, the "not enough data yet" branch in
      the demod loop; counter added to `input_t` in `src/input-common.h`, reset alongside
      `overflow_count` in `src/config.cpp`) — counts how often the demod thread found insufficient
      samples to process a batch and had to wait. This increments frequently under normal, healthy
      load (the demod thread is *supposed* to spend time waiting between batches); read it as a
      trend, not an absolute value — a device whose `buffer_underrun_count` goes flat while its
      `buffer_overflow_count` climbs is the signature of the demod thread being CPU-saturated
      rather than starved for USB input.
    - `process_cpu_seconds_total` (`src/output.cpp`, `output_process_cpu_seconds()`, via
      `getrusage(RUSAGE_SELF, ...)`) — standard Prometheus-convention cumulative process CPU time
      (user+system seconds), so `rate()` over it in Grafana gives host CPU utilization
      correlatable by timestamp against the overrun counters above, without cross-referencing
      external host monitoring.
    Both are exposed via the existing `write_stats_file()` / HTTP metrics endpoint (item 8) with
    no new config options. The pure `rusage_cpu_seconds()` summation is unit tested in
    `test_helper_functions.cpp`; the counters themselves follow the same untested-by-design
    pattern as the pre-existing `overflow_count`/`output_overrun_count` (tightly coupled to
    `device_t`/threading, not a good unit-test target — see item 20).
23. **Failure/health counters for every output type** — item 22 covered the input side
    (RX→demod ring buffer) and process CPU; this extends the same "log it to the stats file,
    not just syslog" treatment to the output side, where several failure paths were previously
    log-only. New per-output counters, all zero-initialized for free (the structs below are
    either `XCALLOC`'d or `new`'d with no user constructor, so no explicit reset code was
    needed anywhere), all exposed via `write_stats_file()` with `device`/`channel`/`output` or
    `mixer`/`output` labels (icecast is also usable on mixer outputs — confirmed by checking
    `parse_outputs()` and `output_thread()`'s mixer loop):
    - `icecast_disconnect_count` / `icecast_backlog_exceeded_count` (`icecast_data` in
      `src/rtl_airband.h`; incremented in `src/output.cpp`'s `process_outputs()` on connection
      loss, and in `output_check_thread()` when the owning device fails). Backlog-exceeded is a
      subset of disconnect — called out separately since it means the local encode rate
      outpaced Icecast, not a network error.
    - `lame_encode_failure_count` (added to `output_t` itself, since both the icecast and file
      encode paths already share `output_t.lame`/`lamebuf`) — increments wherever
      `lame_encode_buffer_ieee_float()` returns negative. Deliberately does not cover the
      one-shot `LameTone` silence-marker encode at file-open time (item 21's `LAMEBUF_SIZE`
      fix): that path uses its own throwaway `lame_t`, not an `output_t`, and is a rare
      startup event already logged, not part of steady-state monitoring.
    - `file_write_failure_count` (`file_data` in `src/rtl_airband.h`) — increments on a short
      `fwrite()`/`ferror()` in `process_outputs()`; the output is disabled immediately after,
      so a healthy instance should see this stay at 0.
    - `udp_stream_dropped_packet_count` (`udp_stream_data`) — increments at all three
      bounds-check drop sites added in item 11. Also **added test coverage that was missing**:
      `test_udp_stream.cpp` had oversized-length tests for the S16LE and stereo-float bounds
      checks but none for the S8 path; added `mono_s8_oversized_len_does_not_overflow_buffer`
      and asserted the new counter on all three existing/added oversized-length tests.
    - `pulse_underflow_count` / `pulse_overflow_count` / `pulse_disconnect_count`
      (`pulse_data`) — the first two increment in the existing PulseAudio underflow/overflow
      callbacks; disconnect_count increments at the three write-path failure sites in
      `pulse_write_single_stream()` (latency-check failure, backlog-exceeded, write failure).
      Guarded by `#ifdef WITH_PULSEAUDIO` throughout, matching the existing pattern for this
      build option.
    - `rdio_scanner_queue_drop_count` / `rdio_scanner_upload_failure_count` — process-wide
      `std::atomic<size_t>` (not per-output), because the upload queue and its worker thread
      in `src/rdio_scanner.cpp` are already shared by every `rdio_scanner`-configured output;
      increments at the existing queue-full drop-oldest site and the existing
      max-retries-exhausted log site. Guarded by `#ifdef WITH_RDIO_SCANNER`.
    None of these are unit tested beyond the udp_stream case above, for the same reason item 22
    gives for `overflow_count`/`output_overrun_count`: they're tightly coupled to real network
    sockets, files, or PulseAudio/libcurl state, not pure logic. Verified instead by a full
    build + unit test pass across Debug, Debug+NFM, and `-DPULSEAUDIO=OFF` (all green).
24. **`-DRDIO_SCANNER=OFF` build fix** (`src/config.cpp`) — flagged as found-but-not-fixed in
    item 23: `parse_outputs()`'s `dev_mode` parameter (added by item 18) is only read inside an
    `#ifdef WITH_RDIO_SCANNER` block, so it's unused — and thus a `-Werror=unused-parameter`
    build failure — whenever `RDIO_SCANNER` is off. `RDIO_SCANNER` defaults to `ON` and CI
    doesn't build the `OFF` configuration, so this had gone unnoticed since item 18. Fixed with
    `(void)dev_mode;` guarded by the inverse `#ifndef WITH_RDIO_SCANNER`. Verified by configuring
    and building a scratch `-DRDIO_SCANNER=OFF -DBUILD_UNITTESTS=TRUE` tree from clean (fails
    without the fix, reproducing the exact reported error; builds and passes all 99 tests —
    5 fewer than the `ON` build's 104, consistent with rdio_scanner-specific tests being
    excluded — with it), and re-confirmed the default `RDIO_SCANNER=ON` Debug build still passes
    all 104 tests unaffected.
25. **Mixer file-output NULL-freqlist crash fix** (`src/output.cpp`) — `output_file_ready()`
    dereferenced `channel->freqlist[channel->freq_idx]` — unconditionally under
    `#ifdef WITH_RDIO_SCANNER` to populate `fdata->open_frequency`, and again when building the
    filename if `include_freq` is set — but a mixer's own `channel_t` (`mixer_t::channel`) has
    no `freqlist` at all — `parse_mixers()` never sets one, since a mixed stream has no single
    source frequency — leaving it `NULL` and crashing on the mixer's first file rotation. Never
    caught before because no automated test exercised a mixer with a `file`-type output that
    actually received signal (`test_multichannel.py` carried a "TODO: add mixer tests... once
    mixer support is implemented in the system tests" for exactly this gap); found independently
    twice, by two branches' own new mixer system tests hitting it at almost the same time — item
    26's `test_icecast_tx_tags.py` mixer case and item 27's `test_channel_add.py`/`test_channel_
    edit.py` mixer cases. Fixed by computing the frequency once, guarded by `channel->freqlist !=
    NULL`, defaulting to `0` for a mixer's own channel.
26. **`send_tx_tags`: per-transmission Icecast metadata for non-scanning channels/mixers**
    (`src/rtl_airband.h`, `src/config.cpp`, `src/output.cpp`, `src/mixer.cpp`,
    `src/helper_functions.{h,cpp}`) — the pre-existing `send_scan_freq_tags` only tags Icecast
    metadata when an `R_SCAN` device's `controller_thread` hops frequency; plain
    `R_MULTICHANNEL` channels and mixers had no way to surface an on-air indicator. Adds a new,
    independent `send_tx_tags` boolean on an icecast output block: pushes the channel's
    configured `label` as the "song" tag when a transmission starts (squelch opens) and clears
    it to an empty string when it ends (squelch closes). Rejected at config parse time on
    `R_SCAN` channels (`send_scan_freq_tags` already owns that case). Unlike
    `send_scan_freq_tags`'s device-scoped `tag_queue` ring buffer (needed because its tag event
    originates in a separate controller thread), `send_tx_tags` is self-contained inside
    `process_outputs()` (the output thread already safely reads `channel->axcindicate`, an
    `std::atomic`): `icecast_tx_tag_step()` (`helper_functions.cpp`) detects the on/off edge
    itself each tick and replicates just the `shout_metadata_delay` buffering-compensation
    behavior via a per-output deferred-apply state machine (`icecast_tx_tag_state`, embedded in
    `icecast_data` — which switched from `XCALLOC` to `new icecast_data()` since it now holds
    `std::string` members that calloc'd memory would never construct, matching the existing
    `file_data`/`rdio_scanner_data` precedent). A further signal flap while a change is pending
    updates the pending value but does not push the deadline back (bounds worst-case latency to
    one delay window); a revert to the already-applied value while pending cancels the change
    outright. For mixers, `mixinput_t` gained `source_device_idx`/`source_channel_idx` (resolved
    at config-parse time in the existing `"mixer"` output branch, stored as indices rather than
    a `channel_t*` since `dev->channels` is `XREALLOC`'d after parsing completes — the
    compaction index computed during parsing stays valid across that realloc, unlike a raw
    pointer) so a mixer's icecast output can look up whichever source channel is currently
    talking; `mixer_select_active_tag_input()` (`mixer.cpp`) breaks ties between simultaneously
    active inputs by lowest index. `mixinput_t::has_signal` changed `bool` → `std::atomic<bool>`
    since this feature adds a third reader (`process_outputs()`, potentially on a different
    output thread than `mixer_thread()` under `multiple_output_threads = true`) alongside the
    two that already existed. New `icecast_tx_tag_update_count` Prometheus counter, same
    `write_stats_file()` pattern as `icecast_disconnect_count`. Pure logic
    (`compute_tx_tag_content()`, `icecast_tx_tag_step()`, `mixer_select_active_tag_input()`) is
    unit tested (`test_helper_functions.cpp`, `test_mixer.cpp`); end-to-end wiring is system
    tested (`system_tests/tests/test_icecast_tx_tags.py`, one case for a plain channel and one
    for a mixer with two source channels) against an extended `fake_icecast_server.py` — real
    Icecast metadata updates are a *separate* `GET /admin/metadata?...` HTTP request per update
    (confirmed against the installed `libshout.so.3`'s strings output), not inline frames on the
    already-open SOURCE connection, so the fixture now accepts multiple concurrent connections
    (one thread per connection) instead of the single blocking `accept()` it used before this
    feature needed it to observe a second, independent connection type. The system tests force
    `shout_metadata_delay = 0` to keep assertions independent of the real/simulated-time
    interaction between that (wall-clock) delay and `speedup_factor` (which only accelerates IQ
    replay, not `gettimeofday()`).

27. **`dynamic_reload`: live retune/reconfiguration via a Unix domain control socket**
    (`src/control_socket.{cpp,h}`, `src/live_reconfig.{cpp,h}`, new) — the only reload mechanism
    before this was `SIGHUP` → full clean shutdown → `execvp()` re-exec (item 7), meaning any
    config change dropped the whole feed for a restart cycle. Adds a same-host-only (`0600`,
    `SO_PEERCRED`-checked) control socket, gated behind a new top-level `control_socket_path`
    config option, that accepts one JSON object per line and returns one JSON response line:
    `retune`, `set_gain`, `set_bandwidth`, `channel_enable`/`channel_disable`,
    `mixer_enable`/`mixer_disable`, `reload_diff`. The `SO_PEERCRED` check requires an exact UID
    match against the daemon's own `getuid()`, so a systemd unit that leaves `User=`/`Group=`
    unset (running as root) locks out any non-root control-socket client — discovered while
    testing this branch. The example unit (`init.d/rtl_airband.service`) now documents this.
    - **New `enabled = false/true` channel/mixer config keyword** (`src/config.cpp`), distinct
      from the pre-existing parse-time-permanent `disable` (which skips the config entry
      entirely — no array slot allocated). `enabled` still allocates everything (bins, `dm_dphi`,
      outputs) but starts the channel/mixer skipped by the hot loops, so the control socket can
      toggle it live with no array resize. Mixer/device add/remove is explicitly out of scope —
      declare a mixer up front (optionally `enabled = false`), then toggle. True dynamic *channel*
      add (a channel not present at startup at all) is supported — see item 28.
    - **Live centerfreq retune** only for `R_MULTICHANNEL` devices (`R_SCAN` keeps its existing
      controller-thread fixed-offset scheme unchanged — see item 2's "Device Modes"). Retuning
      recomputes every channel's `bins`/`base_bins`/`dm_dphi` for the new center, which — unlike
      `R_SCAN`'s single fixed-offset — touches state `AFC` (the per-channel automatic
      frequency-correction class in `rtl_airband.cpp`) also mutates continuously from inside the
      demod thread. To avoid racing AFC, the control socket only ever posts a request
      (`device_t::pending_centerfreq_request`, an `int` sentinel `-1`); the demod thread that
      already exclusively owns `bins`/`base_bins`/`dm_dphi` for that device polls and applies it
      in-thread (`device_apply_retune()`), so the recompute is always single-writer, same
      invariant AFC's own adjustments already relied on.
    - **Channel/mixer enable-disable is a request/apply split for the same reason** —
      `channel_t`/`mixer_t::pending_enable_request` (sentinel `-1`), posted by the control socket
      thread, consumed only by the `output_thread()` that owns that channel's/mixer's `outputs`.
      This split exists because the first implementation called `mixer_disable()` directly from
      the control socket thread and a system test (item 25's mixer test) reproducibly segfaulted
      it — a genuine data race against that same output thread's concurrent
      `process_outputs()`/`close_file()` on the same `output_t` structs. Known remaining gap:
      the pre-existing "last input died" auto-cascade (`mixer_disable_input()` →
      `mixer_disable()`, triggered when a device channel's own `O_MIXER` output is disabled) can
      in principle still run from a *different* output thread than the one owning the target
      mixer's range when `multiple_output_threads = true` is configured — not hit in the default
      single-output-thread topology this fork actually deploys, and not fixed here.
    - **`set_gain`/`set_bandwidth`** added as new *nullable* `input_t` vtable hooks
      (`src/input-common.h/.cpp`, mirroring the existing `set_centerfreq` hook's shape), returning
      `ENOTSUP` when a driver leaves the pointer null. `rtlsdr` gets `set_gain` only (no tuner
      bandwidth API exists in this driver at all); `soapysdr` gets both.
    - **`reload_diff`** re-reads the same `-c` config file (`cfgfile`, promoted from a `main()`
      local to a global) into a read-only snapshot (`parse_config_snapshot()` — deliberately not
      a full mirror of `parse_devices()`/`parse_channels()`/`parse_mixers()`, which can't be
      safely re-run against already-live state) and applies whatever's in v1 scope through the
      same primitives a single command would use. Device/channel/mixer count changes,
      `sample_rate`, driver type, and mode changes are detected and reported under
      `skipped_requires_restart`, never attempted.
    - Unit tested in `src/test_live_reconfig.cpp` (bins/`dm_dphi` formulas, request/apply
      bookkeeping, snapshot parsing, diff computation) and `src/test_control_socket.cpp`
      (wire-protocol parsing, command validation). System-tested end-to-end in
      `system_tests/tests/test_control_socket.py` (channel/mixer enable-disable audio gaps,
      malformed-command handling, socket permissions) using two new helpers,
      `helpers/control_socket_client.py` and `helpers/interactive_runner.py` (a `Popen`-based
      runner for tests that need to interact with a running instance mid-stream, unlike
      `conftest.run_rtl_airband()`'s blocking run-to-completion model). A live-retune system test
      that specifically confirms retuning one channel doesn't corrupt a sibling channel's bins on
      the same device — the highest-risk scenario for item 27's centerfreq-retune design — is a
      documented follow-up, not yet implemented; see `test_control_socket.py`'s module
      docstring.
28. **Dynamic channel add via `reload_diff`** (`src/config.cpp`, `src/live_reconfig.{cpp,h}`,
    `src/rtl_airband.h`, `src/logging.{h,cpp}`) — closes item 27's channel-add gap. The config
    file stays the single source of truth for a channel's definition: an operator adds a channel
    block to a device's `channels` list in the file and sends the existing `reload_diff` command
    (no new wire command); `compute_and_apply_diff()` detects the device's channel count grew as a
    pure tail append and applies it live. Only a *pure append* is handled — any other
    `channel_count` change (decrease, reorder, or an existing channel's fields changing) still
    falls into `skipped_requires_restart` exactly as before.
    - **`reserve_channels`** (new device-level config int, default `0`) — `dev->channels`/`bins`/
      `base_bins` are `XCALLOC`'d with this much extra headroom at startup and never resized
      again; `dev->channel_count` (now `std::atomic<int>`) can grow up to the new
      `dev->channel_capacity` field by writing into an already-allocated, already-zeroed slot and
      then publishing the new count. Deliberately not real runtime array growth (`realloc`-ing
      live while the demod and output threads are both mid-iteration through the old pointer was
      rejected as an unjustified hazard for two independent reader threads) — an operator must set
      `reserve_channels` on a device once (one restart) before dynamic add works on it, the same
      "declare capacity up front" idea item 27 already uses for `enabled = false`. Rejected at
      parse time on `R_SCAN` devices (a scan device always has exactly one channel; "add a
      frequency to its `freqlist`" is a different, unimplemented feature).
    - **`parse_channel()`** (`config.cpp`) — the per-channel body of the startup `parse_channels()`
      loop, extracted into its own function so the startup path and the live-append path share one
      implementation (including `parse_outputs()`, so a newly appended channel supports the exact
      same `outputs: (...)` block — any number/type — as a config-file channel always has). Returns
      `false` (silently not counted, matching pre-existing behavior) for two latent quirks found
      during the extraction: the legacy single-value forms of `squelch_snr_threshold == -1` and
      `bandwidth == 0` `continue`d out of the *entire* per-channel loop in the original code, not
      just the enclosing `if` — never fixed here, only preserved, since it's pre-existing behavior
      unrelated to this feature.
    - **`init_output()`** (`rtl_airband.cpp`) — declared in `rtl_airband.h` so the live-append path
      can call it too. A freshly appended channel's `output_t` structs are allocated by
      `parse_outputs()` but have no LAME encoder or open connection until `init_output()` runs;
      missing this call was caught by the system test below (the appended channel's MP3 file was
      created but stayed empty) rather than by the C++ unit tests, which stub `init_output()` to
      isolate the append/publish logic from real LAME/icecast/udp setup.
    - **Recoverable `error()`** (`logging.{h,cpp}`) — `config.cpp`'s ~50 `error()` call sites call
      `_Exit(1)`, correct for startup but fatal to a *running* process if reused verbatim for a
      malformed appended channel. A `thread_local` gate (`config_error_is_recoverable`) makes
      `error()` throw `ConfigApplyError` instead when set; the live-append path sets it, redirects
      `cerr` to capture the human-readable message each call site already prints, and catches
      `std::exception` broadly — not just `ConfigApplyError` — since some required-but-missing
      config keys (e.g. an absent `outputs` block) are indexed directly (`chan_setting["outputs"]`)
      without an `error()`-guarded `.exists()` check first and throw a raw
      `libconfig::SettingNotFoundException`; missing this case in the first pass crashed the whole
      process on exactly the malformed-channel test case it was meant to guard against. A batch of
      newly appended channels is all-or-nothing: `dev->channel_count` is only published after every
      new channel parses and connects successfully; a channel that individually succeeded earlier
      in a failed batch is leaked (its `strdup`'d labels / `XCALLOC`'d outputs are never freed)
      rather than unwound — acceptable for a rare, operator-triggered failure path.
    - Unit tested in `src/test_live_reconfig.cpp`'s `ChannelAppendTest` fixture (append one/many
      within capacity, capacity-exceeded, malformed-channel-doesn't-crash, `R_SCAN` guard,
      count-decrease still requires restart) with `init_output()` stubbed to isolate the
      append/publish logic. System-tested end-to-end in `system_tests/tests/test_channel_add.py`
      (a channel absent at startup captures real audio after a config edit + `reload_diff`,
      confirmed alongside its already-running sibling channel being unaffected; appending beyond
      `reserve_channels` is rejected without disrupting the existing channel) using a new
      `reserve_channels` parameter on `helpers/config_writer.write_config()`.
29. **Mixer-input live-append data race fix** (`src/mixer.cpp`, `src/rtl_airband.{cpp,h}`,
    `src/config.cpp`) — closes a gap flagged but not fixed while item 28 was built: a dynamically
    appended channel whose config declares a `type = "mixer"` output *did* reach
    `mixer_connect_input()` (`mixer.cpp`) on the live-append path, and that function grew
    `mixer_t::inputs`/`inputs_todo`/`input_mask` via an unconditional `XREALLOC` with a comment
    claiming it was "only run at startup" — no longer true once threads (`mixer_thread()`, the
    output thread's `mixer_put_samples()`/`mixer_disable_input()`/`mixer_enable_input()`) are
    already running and reading that pointer/count with no synchronization of their own. A real
    use-after-free, not theoretical.
    - Mirrors item 28's own fix for `device_t::channels`: a new `reserve_inputs` mixer-level
      config int (default `0`) sizes extra headroom, added on top of however many inputs actually
      connected during startup by a one-time `mixer_finalize_capacity()` (`mixer.cpp`), called
      from `main()` right after `parse_devices()` returns and before any thread is created — the
      same single-threaded window item 28 relies on. `mixer_t::input_count` is now
      `std::atomic<int>`; a new `input_capacity` field (plain `int`, write-once before
      finalization) mirrors `channel_capacity`. Unlike device channels, a mixer's eventual input
      count isn't known upfront (`parse_mixers()` runs *before* `parse_devices()`, and inputs are
      discovered incrementally as channel outputs connect to it) — so, unlike `reserve_channels`,
      the pre-reserved headroom can only be finalized once, after startup connections are done,
      not sized upfront.
    - `mixer_connect_input()` now branches on `mixer_capacity_finalized` (a plain global, not a
      `mixer.cpp` file-static — the unit test binary links every `test_*.cpp` into one process, and
      a file-static would leak `true` across unrelated tests in link/registration order): before
      finalization it grows exactly as before (still single-threaded, still safe); after
      finalization, exceeding `input_capacity` is rejected with a clear `mixer_get_error()` message
      instead of reallocating — which, via the same `config_error_is_recoverable` mechanism item 28
      already built, surfaces as an ordinary `skipped_requires_restart` entry for a live append,
      with no changes needed in `live_reconfig.cpp` at all.
    - Two adjacent bugs surfaced by the new system tests below, both fixed alongside the race
      itself:
      - A mixer declared with zero startup-connected inputs (the intended shape for a
        `reserve_inputs`-only, live-append-only mixer) never had its own output initialized —
        `main()`'s startup loop skipped `init_output()` for any mixer whose `enabled` was still
        `false` at that point ("no inputs connected = no need to initialize output"), correct
        before dynamic_reload (a zero-input mixer could never gain one) but not after. Fixed by
        always initializing a mixer's own outputs at startup regardless of `enabled`, matching how
        device channels already behave (no such skip exists for them). This incidentally also
        fixes a second, pre-existing latent instance of the same gap: a mixer declared with its own
        config-level `enabled = false` that *does* have startup-connected inputs was left with its
        outputs uninitialized too, since `mixer_disable()` (called for `config_wants_disabled`
        mixers right after `parse_devices()`) ran before the output-init loop and left `enabled`
        false by the time that loop checked it.
      - `control_socket.cpp`'s `json_escape()` didn't escape control characters, only `"` and `\`.
        Nearly every `error_response()` message is built from a captured `cerr` string that ends in
        `"\n"` (every `config.cpp` parse-error printout does), and the control socket's wire
        protocol is one JSON object per line — an unescaped, embedded newline in a response
        silently truncates it before the closing brace on every client, a latent bug for *any*
        config-parse error during a live append, not just this one. Only ever exercised by a real
        socket round-trip, not the pre-existing unit tests (which check `compute_and_apply_diff()`'s
        returned C++ string directly, never JSON-serialized) — this fork's first system test that
        actually reaches this path (the capacity-exceeded case below) is what surfaced it. Fixed by
        escaping `\n`/`\r`/`\t` and other control characters properly.
    - Unit tested in `src/test_mixer.cpp` (pre/post-finalize growth, headroom sizing, pointer
      stability across a post-finalize append, capacity-exceeded rejection, the zero-input
      finalize edge cases — `XREALLOC(ptr, 0)` can return `NULL`, which `xrealloc()` treats as
      fatal OOM, so finalizing a mixer with zero inputs and zero reserve must skip the realloc
      entirely), `src/test_live_reconfig.cpp` (a live-appended channel's `type = "mixer"` output
      connecting within/beyond `reserve_inputs`), and `src/test_control_socket.cpp` (a response
      never contains a raw newline). System-tested end-to-end in
      `system_tests/tests/test_channel_add.py` using a new `reserve_inputs` parameter on
      `helpers/config_writer.write_config()`'s mixer dicts: an appended channel's mixer output
      produces real audio through the mixer within its reserved headroom, and exceeding it is
      rejected while the mixer's already-connected input keeps producing correct, undisturbed
      audio throughout — the closest a system test (no sanitizer) can get to proving the original
      race is actually closed. Full unit test pass across Debug, Debug+NFM, and
      `-DRDIO_SCANNER=OFF` builds, plus the full system test suite.
30. **Live channel removal via `reload_diff`** (`src/live_reconfig.{cpp,h}`, `src/config.cpp`,
    `src/output.cpp`, `src/rtl_airband.h`) — the inverse of item 28's live channel add: deleting a
    channel from a device's config and calling `reload_diff` tears it down live instead of always
    falling into `skipped_requires_restart`. No new config option or control-socket command —
    same "the config file stays the source of truth, `reload_diff` just picks up the diff"
    contract as append. Easier than the item 29 mixer race fix in one respect (`dev->channels`/
    `bins`/`base_bins` never shrink or reallocate — array-slot safety was already solved by item 28's `reserve_channels`/`channel_capacity`, and `dev->channel_count` was already atomic and
    already re-read fresh everywhere), but adds two problems append never had, both because
    removal is destructive rather than additive:
    - **Which channel did the operator actually mean to delete?** Position-based diffing only
      sees "N fewer channels than before" — it can't tell "the last channel was deleted" apart
      from "some other channel was deleted, and the last one just happens to be one shorter" by
      count alone. Guessing wrong and tearing down the currently-last channel instead of the one
      actually removed would kill the wrong live audio feed — append has no equivalent risk
      (a wrong guess there can only fail to add something new). Closed by a new
      `DeviceConfigSnapshot::channel_freq_hz` (per-channel freq, captured in
      `parse_config_snapshot()` alongside the existing `channel_enabled`) that
      `compute_and_apply_diff()`'s new decrease branch checks against every surviving channel's
      live `freqlist[0].frequency` before touching anything; any mismatch — i.e. anything other
      than a pure tail deletion — falls into `skipped_requires_restart` with no attempt made.
      Only `R_MULTICHANNEL` is supported (same structural non-goal as append: `R_SCAN` always has
      exactly one channel). **Superseded by item 31**: `channel_freq_hz` was replaced by a general
      `channel_signature` field, and a signature mismatch is no longer flatly rejected — see below
      for why that turned out to be an unnecessary restriction once the underlying mechanism was
      generalized.
    - **The confirmation-then-decrement handshake needs a stronger guarantee than item 27's
      existing enable/disable pattern provides.** Enable/disable's `output_thread()` consumption
      (`exchange(-1, ...)` then apply) lets a waiter observe "consumed" *before* the apply
      function finishes — harmless there (nothing is freed), but wrong for removal: the caller
      (`try_remove_channels()`, `src/live_reconfig.cpp`) decrements `dev->channel_count` the
      moment it sees confirmation, and other threads that gate on that count (notably
      `output_check_thread()`, which has no synchronization with `output_thread()` beyond
      checking `channel->enabled`) would need that gate to already reflect a fully-torn-down
      channel, not one still mid-teardown. `channel_t::pending_remove_request` is a new, separate
      atomic field from `pending_enable_request` for exactly this reason: `output_thread()`
      (`src/output.cpp`) only resets it to `-1` *after* `channel_teardown_for_removal()`
      (`src/live_reconfig.cpp`) fully completes, not at the moment the request is observed.
    - **Deliberately narrower than a full free.** `channel_teardown_for_removal()` sets
      `enabled = false` first (same reason `channel_apply_disable()` does — it's what keeps
      `output_check_thread()`'s already-existing `!enabled` skip from reaching in during
      teardown), calls the existing `disable_channel_outputs()`, then additionally closes and
      frees each output's LAME encoder (`lame_close()` + `free(lamebuf)`) — the one resource that
      scales meaningfully with channel count and was never freed anywhere except whole-process
      shutdown (see item 21's `LAMEBUF_SIZE`). `channel->outputs`/`freqlist` and each output's own
      `data` struct are deliberately left allocated (leaked) rather than freed: freeing those
      would touch memory `output_check_thread()` reads with no synchronization beyond the same
      `enabled` check, and they're comparatively small (a few hundred bytes to low KB per
      channel) — the same "leaked, not unwound" tradeoff item 28 already accepts for a batch-
      append failure, just applied here to every removal rather than only a rare failure path.
    - **Not all-or-nothing, unlike append.** `try_remove_channels()` decrements
      `dev->channel_count` by one immediately after each individual channel's teardown is
      confirmed, rather than batching the count update to the end — so a mid-batch timeout (the
      output thread stuck or unusually slow) leaves a valid, still-tail-consistent smaller count
      instead of either an inconsistent one or losing an already-confirmed removal.
    - Unit tested in `src/test_live_reconfig.cpp` (`ChannelRemoveTest`: successful tail removal
      with a background thread standing in for `output_thread()`, timeout leaves a valid partial
      count; plus an `R_SCAN`-decrease guard alongside the existing append guard — the original
      mismatched-prefix-is-rejected test was superseded by item 31's rebuild-from-divergence
      behavior and replaced accordingly). One incidental discovery while writing these:
      constructing a real `freq_t` in a test (needed here to give channels a real freq to
      prefix-match against) runs `Squelch`'s constructor, which unconditionally calls
      `debug_print()` — fine in production, but segfaults in the unit test binary under a
      `-DDEBUG` build, since `debugf` (`src/logging.cpp`) is only ever `fopen()`'d by the real
      startup path. Existing tests never hit this because `mk_freqlist()` (`src/config.cpp`)
      allocates via `XCALLOC`, not a real C++ construction — worked around in the test the same
      way, not fixed at the source. System-tested end-to-end in
      `system_tests/tests/test_channel_remove.py` (a channel deleted from the tail stops
      capturing while its sibling channel keeps running the whole time).
31. **Live channel edit via `reload_diff` (tail-replace generalization)** (`src/config.cpp`,
    `src/live_reconfig.{cpp,h}`, `src/rtl_airband.h`) — items 28 and 29 could add or remove a
    channel live, but not edit one: changing an existing channel's `freq`/`modulation`/
    `bandwidth`/`squelch_snr_threshold`/`notch`/`ctcss`/`highpass`/`lowpass`/`outputs` was
    silently ignored by `reload_diff` — not even reported, since nothing compared those fields
    against what was live. Turned out to need no new mechanism: item 30's removal and item 28's
    append, run back to back on the same channel index, already *is* an edit — a channel is torn
    down and a freshly parsed replacement takes its place.
    - **Detecting *that* something changed, without hand-enumerating *what*.** Rather than adding
      a parallel snapshot field per config key (unbounded, and silently stale the next time a new
      channel option is added and someone forgets to wire up its diff), `channel_t` gains a
      `config_signature` field: a canonical string serialization of the channel's entire raw
      config block, computed once by a new `build_channel_identity_signature()`
      (`src/config.cpp`) and set unconditionally at the end of `parse_channel()` — so both the
      startup path and the live-append/replace path always have one. `serialize_setting()`
      recursively walks an arbitrary `libconfig::Setting` (scalar, group, list, or array), so
      nested structure — `outputs`, including per-output-type fields like a mixer connection's
      `balance` — is captured with no per-field knowledge needed, the same way a new output type
      or channel option added in the future is automatically covered without touching this code
      at all. `enabled` is deliberately excluded (see below). `DeviceConfigSnapshot::
      channel_freq_hz` (item 30) is retired in favor of the more general `DeviceConfigSnapshot::
      channel_signature`, built the same way from the freshly re-read config.
    - **The diff itself becomes "find the longest common prefix, replace the rest."**
      `compute_and_apply_diff()` no longer branches on `snap.channel_count > / < / ==
      dev->channel_count`; it computes `common_prefix` — the largest index such that every
      channel `[0, common_prefix)` has an identical signature on both sides — then removes
      whatever's live past that point (`try_remove_channels()`, now taking an explicit
      `target_count` parameter rather than deriving it from the snapshot, since a replace's
      target is the common-prefix length, not necessarily the snapshot's final count) and appends
      whatever the snapshot has past that point (`try_append_channels()`, unchanged — it already
      starts from `dev->channel_count`, which is `common_prefix` by the time it runs). A pure
      count increase or decrease is just the special case where one side of that gap is empty;
      `R_MULTICHANNEL`-only and the `reserve_channels`-capacity check apply exactly as before.
    - **Why `enabled` is excluded from the signature.** It already has a cheap, non-destructive
      live-apply path (`channel_request_enable`/`channel_request_disable`, item 27) that just
      flips a flag — folding it into the signature would make toggling it alone spuriously
      trigger a full tear-down/reconnect. The untouched common-prefix channels still get their
      `enabled` diffed and applied via that existing path, every `reload_diff` call, regardless of
      whether a tail-replace also happened.
    - **A deliberate, documented behavior change, not just a refactor**: item 30's removal
      shipped with a hard rule — a non-tail deletion (removing a channel from the middle of the
      list) was flatly rejected, because position-based diffing had no way to distinguish "the
      last channel was deleted" from "a different channel was deleted, coincidentally leaving the
      same count." The signature-based common-prefix approach doesn't have that ambiguity — it
      finds *where* the file and live state first disagree, by content, not just by count — so a
      non-tail deletion now succeeds: the deleted channel is removed, and any sibling channel
      after it in the array gets torn down and reconnected too (a brief interruption, not data
      loss) purely as a side effect of everything past the divergence point being rebuilt. This is
      a strictly better outcome than the old flat rejection (which demanded a full restart for the
      same edit), at the cost of the rebuilt sibling's brief interruption — considered an
      acceptable, documented tradeoff rather than something worth a more precise (and
      considerably more complex) reordering-aware diff. **Known limitation**: the comparison is a
      positional common *prefix*, not a set match — reordering two unchanged channels in the
      config file causes both to be rebuilt, since the mechanism can't tell "reordered" apart from
      "replaced" any more finely than that.
    - **A pre-existing gap this surfaces, not fixed here**: `mixer_disable_input()` (`src/
      mixer.cpp`) only masks a mixer input slot off (`input_mask[idx] = false`); it never releases
      it back to `input_capacity`. Editing an already-live channel whose output is `type =
      "mixer"` therefore burns one more `reserve_inputs` slot on every edit, permanently — an
      operator who expects to repeatedly tweak a mixer-connected channel's `balance` needs to size
      `reserve_inputs` for the number of edits they expect to make, not just the number of
      channels. Flagged, not fixed, matching this fork's existing practice of documenting a
      discovered-but-out-of-scope gap rather than silently expanding scope to cover it.
    - Unit tested directly (`ChannelIdentitySignatureTest`, `src/test_live_reconfig.cpp`: identical
      configs produce identical signatures, a changed scalar field or a changed nested output
      field produces a different one, `enabled` alone does not) and end-to-end through
      `compute_and_apply_diff()` (`replaces_tail_channel_when_its_definition_changes`: a bandwidth
      edit on an existing channel is picked up, the untouched sibling is provably untouched;
      `replaces_tail_channel_with_edited_mixer_output`: a mixer `balance` edit reconnects into a
      fresh `reserve_inputs` slot, demonstrating the capacity-consumption caveat above concretely;
      `deleting_a_non_tail_channel_rebuilds_from_the_point_of_divergence`: the new non-tail
      behavior, replacing item 30's old rejection test). Every existing hand-built
      `ChannelAppendTest`/`DiffApplyTest` case that keeps an "already-live, unchanged" channel at
      index 0 needed one added line (`chans[0].config_signature = strdup(...)`, mirroring what
      `parse_channel()` would have set at real parse time) — without it, the new common-prefix
      check has no signature to compare and correctly treats an untouched channel as diverged too,
      which is a good reason this field is required, not nullable-and-skipped, everywhere except a
      mixer's own non-replaceable embedded channel. System-tested end-to-end in
      `system_tests/tests/test_channel_edit.py` (a squelch-threshold edit on a live channel is
      picked up, with a brief gap around the edit, while an untouched sibling channel is
      unaffected the whole time) and `test_channel_remove.py`'s rewritten non-tail-deletion test.
32. **Live retune/gain/bandwidth failures no longer take down the whole process**
    (`src/input-common.{cpp,h}`, `src/live_reconfig.{cpp,h}`, `src/control_socket.cpp`,
    `src/rtl_airband.{cpp,h}`, `src/config.cpp`, `src/output.cpp`) — `input_set_centerfreq()`/
    `input_set_gain()`/`input_set_bandwidth()` (item 27) unconditionally set
    `input->state = INPUT_FAILED` on any nonzero return from the driver hook, including a single
    transient hardware error (e.g. an i2c write failure, `r82xx_set_freq: failed=-9`) on a device
    whose RX thread was running fine. `demodulate()`'s main loop (`rtl_airband.cpp`) treats
    `INPUT_FAILED` as fatal for that device — and if it's the last device running, exits the whole
    process ("All receivers failed, exiting"), found by hitting exactly this while testing a live
    `retune` control-socket command against a real device. Two call paths hit this: the
    dynamic_reload control socket's `retune`/`set_gain`/`set_bandwidth` commands, and `R_SCAN`'s
    pre-existing `controller_thread()` frequency-hopping loop, which also permanently abandoned
    that device's scan on the same failure (`break`). Fixed by no longer setting `INPUT_FAILED`
    from these three functions at all — that state now means only what it always should have: the
    RX stream itself died (`rtlsdr_rx_thread()`'s async-read-loop failure and the equivalents in
    the other drivers, `input_stop()`'s stop-failure path), not a single failed
    tuning/gain/bandwidth call. `controller_thread()` now logs and keeps scanning (retries the hop
    on its next ~200ms cycle) instead of abandoning the device. Startup's `rtlsdr_init()` (item 16)
    is unaffected — it calls `rtlsdr_set_center_freq()` directly, not through
    `input_set_centerfreq()`, so its fatal-on-init-failure behavior is untouched.
    - **A second, independent bug in the same area**: `handle_retune()`'s poll loop consumed
      `dev->pending_centerfreq_request` (reset it to `-1` via `exchange()`) *before*
      `device_apply_retune()` (and the `input_set_centerfreq()` call inside it) actually ran, so a
      caller polling for "is it done" could observe "consumed" before the hardware call — and its
      result — existed at all. Fixed the same way item 30's `pending_remove_request` already
      established for "apply does real work, not just a flag flip": a new
      `device_t::centerfreq_apply_failed` field is set by the demod thread from
      `device_apply_retune()`'s actual (now bool-returning) result, and
      `pending_centerfreq_request` is only reset to `-1` after that, not before —
      `handle_retune()` now checks this field instead of `dev->input->state`.
    - Adds `input_t::centerfreq_retune_failure_count`, incremented inside
      `input_set_centerfreq()`'s failure branch (covers both call paths with one line) and exposed
      via the stats/metrics endpoint (items 8/22) alongside `buffer_underrun_count`, so a
      recurring hardware tuning fault is visible without grepping syslog across this fork's ~12
      concurrent instances.
    - Unit tested in `src/test_live_reconfig.cpp` (`device_apply_retune()`'s failure path leaves
      `input->state` at `INPUT_RUNNING` and increments the new counter; a background-thread test
      proves the request/apply ordering itself, not just the state removal; a gain-failure case
      for `input_set_gain()`) and `src/test_control_socket.cpp` (`handle_retune()` returns an
      error keyed off `centerfreq_apply_failed`, not `input->state`, plus the success mirror).
      `controller_thread()`'s own retry-not-abandon behavior is not unit-tested — it's a real
      pthread loop with no existing test harness for it (see item 20's stated scope). A system
      test against a fault-injecting input driver is a documented follow-up, not yet implemented —
      no driver in `system_tests/helpers/` currently supports simulating a `set_centerfreq`
      failure (the only one exercised end-to-end, `type = "file"`, has a hard no-op
      `set_centerfreq()` that always succeeds).
33. **Live retune bins/dm_dphi correctness + `reload_diff` failure reporting fix**
    (`src/live_reconfig.{cpp,h}`, `src/control_socket.cpp`) — found while investigating a
    production report of "the same failure" persisting after item 32's fix. Extensive
    fault-injection and real-hardware stress testing (9,500+ forced consecutive retune failures;
    700+ real `reload_diff` channel-add/remove-plus-retune cycles, including a run that
    organically reproduced the actual production i2c fault) never reproduced a crash or a race in
    the request/apply/confirm machinery items 27/29/32 already built — the actual bugs were both
    correctness/reporting issues, not crashes or races:
    - `device_apply_retune()` previously recomputed every channel's `bins`/`base_bins`/`dm_dphi`
      for the *new* centerfreq unconditionally, before attempting the hardware retune. On a
      failed `input_set_centerfreq()` call, the radio stays physically on its old centerfreq (see
      item 32 — `input->centerfreq` is only updated on success), but the demod math had already
      moved on to the new, never-reached frequency: every channel on that device would demodulate
      the wrong bin offset relative to what the hardware was actually receiving, until the next
      successful retune. Fixed by reordering: attempt `input_set_centerfreq()` first, only
      recompute bins/`dm_dphi` on success.
    - Separately, `compute_and_apply_diff()`'s centerfreq branch (used by `reload_diff` — the
      only way rtl-airband-panel's `dynamic_reload` branch applies live changes) reported
      "applied" based solely on `device_request_retune()` successfully *posting* the request,
      never checking whether the demod thread's actual hardware call succeeded — unlike the
      standalone `retune` control-socket command, which already polled
      `pending_centerfreq_request`/`centerfreq_apply_failed` and reported real failures. A caller
      using `reload_diff` had no way to know a centerfreq change silently failed to reach the
      hardware. Fixed by extracting the poll-and-check logic `handle_retune()` already had into a
      shared `device_confirm_retune()`, used by both `handle_retune()` and
      `compute_and_apply_diff()` — a real hardware failure now surfaces in
      `skipped_requires_restart` with a message clarifying no restart is actually needed (retry
      `reload_diff`), matching the wording convention the channel-append failure case already
      used for the same "attempted and failed, not out-of-scope" distinction.
    - Unit tested: extended `device_apply_retune_returns_false_...` to assert bins/`base_bins`
      are left untouched on a failed retune (the existing test only checked `input->centerfreq`,
      never bins — the actual gap this bug lived in); new
      `device_confirm_retune_reports_success`/`_hardware_failure`/`_timeout_when_never_consumed`
      and `centerfreq_diff_posts_a_retune_request_and_waits_for_confirmation`/
      `_reports_transient_hardware_failure_instead_of_claiming_success`/
      `_reports_pending_when_not_yet_confirmed` (`src/test_live_reconfig.cpp`, the last superseding
      the old posts-and-returns-immediately test). All 186 tests (5 net new) pass across Debug and
      Debug+NFM. Verified end-to-end against real RTL-SDR hardware (temporary, reverted
      fault-injection build): a genuinely failing `retune` and `reload_diff` command both now
      correctly report failure instead of false success, with the device's live `centerfreq`
      confirmed unchanged; no crash or sanitizer report under either a real-hardware i2c fault
      (reproduced organically during this session's stress testing) or forced/injected failures.
    - Worth recording as a negative result: item 27/29/32's design (single-threaded control
      socket, blocking confirm-before-advance, no double-post hazard) held up under everything
      short of the actual production incident being reproduced end-to-end. If a genuine *crash*
      (not a wrong-frequency or under-reported-failure symptom) is seen again, it's unlikely to be
      in this code path — check the OS/systemd level first (OOM killer, watchdog, resource
      limits) before re-auditing this machinery.
    - Also noted, not fixed here: an unrelated pre-existing UBSan finding surfaced incidentally
      during this session's hardware testing — `src/ctcss.h:68`, "load of value 240, which is not
      a valid value for type 'bool'" (an uninitialized-read UB, not a new regression from this
      fix). Flagged as a discovered-but-out-of-scope follow-up per this fork's usual practice.
34. **Live RTL-SDR tuner bandwidth control** (`src/input-rtlsdr.{cpp,h}`, `src/input-common.h`,
    `src/live_reconfig.{cpp,h}`) — closes a gap found while scoping a live-bandwidth-adjustment
    request: `input->set_bandwidth` was hardcoded NULL for the rtlsdr driver with a comment
    claiming "this driver has no tuner-bandwidth API call anywhere" — verified false against the
    actual installed dependency (`librtlsdr2 2.0.1`, Ubuntu Noble's `rtl-sdr` package,
    osmocom-derived): `rtlsdr_set_tuner_bandwidth()` exists and returned success across the full
    practical range (0=auto, 200kHz-8MHz) against the R820T dongle used to develop and test this.
    Neither driver (rtlsdr or soapysdr) previously had a *startup*-time bandwidth config option
    either — only soapysdr had a live hook, reachable solely via the standalone `set_bandwidth`
    control-socket command, never via `reload_diff` (the only mechanism rtl-airband-panel's
    `dynamic_reload` branch actually uses).
    - New optional `bandwidth` device-level config key for rtlsdr (`rtlsdr_parse_config()`,
      integer Hz, 0 = automatic — librtlsdr's own convention), applied once at `rtlsdr_init()`
      right after `correction`. Omitting it is fully backward compatible: `dev_data->bandwidth`
      defaults to 0 (XCALLOC-zeroed), and explicitly calling `rtlsdr_set_tuner_bandwidth(rtl, 0)`
      at startup is the same "automatic" behavior this driver already had before this existed.
      Startup failure is fatal, matching item 16's precedent for other tuning parameters
      (centerfreq/sample_rate/correction) — no special-casing beyond that, since the real-hardware
      test above found no case where this call failed.
    - `rtlsdr_set_bandwidth()` implements the pre-existing nullable `input_t::set_bandwidth` hook
      (item 27) via the same API, so the pre-existing `set_bandwidth` control-socket command
      (`handle_set_bandwidth`, `input_set_bandwidth()`) now works for rtlsdr with zero changes
      needed there — only the driver-side NULL was ever missing.
    - `DeviceConfigSnapshot` gains `has_bandwidth`/`bandwidth` (`live_reconfig.h`), populated in
      `parse_config_snapshot()` and diffed/applied in `compute_and_apply_diff()` following the
      exact same pattern gain already uses: `input_t` has no generic live-readable "current
      bandwidth" (each driver's tracking, where it exists at all, is private to its own
      `dev_data`), so a `bandwidth` key present in the config file is reapplied unconditionally
      on every `reload_diff` rather than truly diffed — harmless/idempotent, matching gain's own
      documented tradeoff. Unlike centerfreq, `input_set_bandwidth()` is synchronous (no
      `pending_*`/apply-thread split), so its return value is already the real outcome and gets
      reported accurately (`applied` on success, `skipped_requires_restart` with a
      not-actually-restart-needed message on a transient hardware failure, silently skipped on
      `ENOTSUP` for drivers without the hook) — this is what actually makes it usable through the
      config-edit-then-`reload_diff` workflow the panel and this fork's own deployment rely on,
      not just the standalone command.
    - New default member initializers on `DeviceConfigSnapshot::has_bandwidth`/`bandwidth`
      (`= false`/`= 0`) — a real regression this addition hit during testing: several pre-existing
      hand-built `DeviceConfigSnapshot` test fixtures only set the fields they cared about, so the
      new fields were left as indeterminate garbage and intermittently tripped
      `input_set_bandwidth()`'s `assert(input->dev_data != NULL)` in unrelated tests. The other,
      pre-existing scalar fields on this struct don't have default initializers and were
      deliberately left alone (out of scope for this change) — every test that already touches
      them already sets them explicitly.
    - Deliberately rtlsdr-only: this fork's actual deployment is exclusively RTL-SDR, one device
      per instance (see the "Deployment Context" section), and soapysdr already had a live hook;
      adding soapysdr startup config was left out as unrequested scope.
    - Unit tested: `DiffApplyTest.bandwidth_not_supported_by_driver_is_silently_skipped_not_reported`/
      `_applied_via_input_set_bandwidth_is_reported`/`_set_hardware_failure_does_not_mark_input_failed`
      (mirroring the equivalent gain tests exactly) and `ConfigSnapshotTest.parses_basic_multichannel_device`
      (extended)/`bandwidth_absent_from_config_reports_has_bandwidth_false`
      (`src/test_live_reconfig.cpp`). All 190 tests (4 net new) pass across Debug and Debug+NFM.
      Verified end-to-end against real RTL-SDR hardware: startup `bandwidth = 2000000;` applied
      cleanly, a live `set_bandwidth` control-socket command changed it to 1000000 Hz, and a
      config-file edit + `reload_diff` picked up a further change to 3000000 Hz — all three
      confirmed via log output ("Device #0: bandwidth set to ... Hz") with no crash or sanitizer
      report.
35. **Live RTL-SDR frequency correction (PPM) control** (`src/input-rtlsdr.cpp`,
    `src/input-common.{cpp,h}`, `src/control_socket.cpp`, `src/live_reconfig.{cpp,h}`) — the
    cheapest item on a user-prioritized live-reconfiguration to-do list scoped in this same
    session (bandwidth done in item 34; live `sample_rate` change is the remaining, most
    expensive item — needs RX-thread stop/restart plus buffer/FFT-plan resizing, a different
    class of change from anything built so far, deliberately not started yet). Unlike bandwidth,
    `correction` already had startup config support (`rtlsdr_parse_config()`/`rtlsdr_init()`) —
    this only adds the *live* half, following the exact same three-layer template item 34
    established: driver hook, standalone control-socket command, `reload_diff` wiring.
    - New nullable `input_t::set_correction` hook (mirrors `set_gain`/`set_bandwidth` exactly) and
      `input_set_correction()` (`input-common.{cpp,h}`) — same "failed correction change doesn't
      mean the RX stream died" non-fatal handling as its siblings.
    - `rtlsdr_set_correction()` (`input-rtlsdr.cpp`) calls the same `rtlsdr_set_freq_correction()`
      API `rtlsdr_init()` already uses at startup, including that function's `-2` return code
      ("already at this value") being treated as success, not failure — matching the startup
      path's existing handling of the same quirk.
    - New standalone `set_correction` control-socket command (`handle_set_correction`,
      `device`/`correction` fields), matching `set_gain`/`set_bandwidth`'s shape exactly.
    - `DeviceConfigSnapshot` gains `has_correction`/`correction` (defaulted, same rationale as
      item 34's `has_bandwidth`/`bandwidth` — hand-built test fixtures that predate a field must
      not see indeterminate garbage), diffed/applied in `compute_and_apply_diff()` following
      gain/bandwidth's identical "no live-readable current value, reapply unconditionally,
      synchronous call so the return value is already the real outcome" pattern.
    - Unit tested: `DiffApplyTest.correction_not_supported_by_driver_is_silently_skipped_not_reported`/
      `_applied_via_input_set_correction_is_reported`/`_set_hardware_failure_does_not_mark_input_failed`
      and `ConfigSnapshotTest.parses_basic_multichannel_device` (further extended)/
      `bandwidth_absent_from_config_reports_has_bandwidth_false` (further extended to also assert
      `has_correction`) (`src/test_live_reconfig.cpp`). All 193 tests (3 net new) pass across
      Debug and Debug+NFM. Verified end-to-end against real RTL-SDR hardware: a live
      `set_correction` command changed correction to 50 ppm, and a config-file edit +
      `reload_diff` picked up a further change to 65 ppm — both confirmed via log output
      ("Device #0: freq correction set to ... ppm") with no crash or sanitizer report.
    - Deliberately out of scope, per explicit user direction when this to-do list was scoped:
      live `buffers`/`serial`/`index` changes, R_SCAN live reconfiguration, device/mixer count
      changes, driver `type`/`mode` changes, and all top-level (non-device) settings — none of
      these are planned.
36. **Live device `sample_rate` change** (`src/live_reconfig.{cpp,h}`, `src/rtl_airband.{cpp,h}`,
    `src/config.cpp`, `src/control_socket.cpp`, `src/input-rtlsdr.cpp`) — the last, most expensive
    item on the same user-prioritized to-do list (items 34/35), previously always
    `skipped_requires_restart`. Unlike every other item 27-35 live-set operation, changing
    `sample_rate` genuinely requires stopping and restarting the device's RX thread, resizing
    `input_t::buffer`/`buf_size`, and recomputing every channel's bins/`dm_dphi` for the new rate
    — a different class of change from anything mutated in place before this.
    - Scoping investigation found the FFTW plan/`fft_size` is a single process-wide global with
      zero dependency on any device's `sample_rate` (de-risks cross-device concerns entirely —
      changing one device's rate cannot touch another device's FFT processing), and that
      recomputing bins/`dm_dphi` for a new rate is mechanically identical to what live centerfreq
      retune already does (both already take `sample_rate` as an explicit parameter). The
      genuinely new work is narrower: safely quiescing/restarting one device's RX thread while
      sibling devices (and, in the default single-demod-thread topology, this one) keep running,
      and swapping the buffer out from under a demod thread that has no synchronization on the
      buffer *pointer/size* today (only on `bufs`/`bufe`).
    - `compute_input_buf_size()` (`live_reconfig.{cpp,h}`) extracts the sample-rate-dependent
      buffer-size formula out of `parse_devices()` (`config.cpp`) into a pure function shared by
      both the startup path and the new live path — same "must stay in sync by construction"
      rationale as `compute_channel_bin()`/`compute_channel_dm_dphi()`, and a real correctness
      requirement here (a mismatch would produce inconsistent `buf_size`/`bps`/`available`
      arithmetic), unlike e.g. `snapshot_parse_anynum2int()`'s deliberate duplication elsewhere in
      the same file.
    - `device_apply_sample_rate()` (`live_reconfig.cpp`) is the new heavy apply function, run
      synchronously within this device's own turn in the demod thread's round-robin
      (`demodulate()`, `rtl_airband.cpp`) — same single-writer-thread invariant
      `device_apply_retune()` already relies on for bins/`base_bins`/`dm_dphi`, extended here to
      also cover the RX thread and buffer:
      1. Computes and allocates the new buffer *first*, before touching the RX thread at all —
         the old buffer stays untouched until the new rate is confirmed working, which is what
         makes rollback cheap (nothing to reallocate on failure, just reopen and restart against
         what's already there).
      2. Stops the RX thread via the driver's `stop` hook directly (not `input_stop()` — that
         sets `input->state = INPUT_STOPPED` and is a one-shot, process-exit-only call, its only
         pre-existing caller being final shutdown) and joins it. If `stop()` itself fails, aborts
         immediately without joining (a join could hang forever if the driver failed to actually
         cancel the blocking read) — the device is presumably still fine at the old rate, so this
         is a rejected request, not a device failure.
      3. Reopens via `device_reopen_recoverable()` (new, anonymous-namespace helper) — wraps the
         driver's `init()` hook (e.g. `rtlsdr_init()`) with the same `config_error_is_recoverable`
         thread-local-gate mechanism item 28 already established for `parse_channel()`'s `error()`
         calls, so a startup-only fatal call inside `init()` becomes a catchable failure instead of
         killing the whole process. Deliberately does not call the generic `input_init()` wrapper
         (`input-common.cpp`): that function unconditionally re-runs `pthread_mutex_init()` on
         `buffer_lock` (undefined behavior on an already-live mutex) and unconditionally overwrites
         `input->state`, neither correct for reopening an already-running device.
      4. On success: frees the old buffer, adopts the new one, resets `bufs`/`bufe` to 0, restarts
         the RX thread, recomputes bins/`dm_dphi` for the new rate (reusing `device_apply_retune()`
         's per-channel loop, extracted into a shared private `recompute_channel_bins()` helper).
      5. On failure: per explicit user direction when this was scoped, attempts rollback to the
         *old* rate (reopen again, against the still-intact old buffer) rather than immediately
         giving up — restores full service in the common case (e.g. a transient i2c hiccup on the
         new rate) instead of leaving the device down for something recoverable.
      6. Only if that rollback *also* fails does this mark `input->state = INPUT_FAILED` — a
         genuine "won't come back up at any rate" case, which `demodulate()`'s existing
         `INPUT_FAILED` → `INPUT_DISABLED` handling already folds into `devices_running`/the "all
         receivers failed" exit path with no new machinery needed there.
    - `rtlsdr_init()` (`input-rtlsdr.cpp`) was refactored to guarantee this rollback path actually
      works: found via real-hardware testing (not caught by any fake-driver unit test, since the
      fakes don't hold real USB resources) that a failure partway through the original
      straight-line function left `dev_data->dev` open, so the rollback's own second
      `rtlsdr_open()` call collided with the still-claimed USB interface
      (`usb_claim_interface error -6`) and failed for an unrelated reason. Fixed by extracting
      everything after a successful `rtlsdr_open()` into `rtlsdr_configure_opened_device()` and
      wrapping its call in `rtlsdr_init()` with a try/catch plus a return-code check that always
      closes `dev_data->dev` (and resets it to NULL) on any failure exit, whether via a plain
      `return -1` or a thrown `ConfigApplyError` — a no-op difference at real startup (a failure
      there is always fatal anyway) but essential for a live reopen attempt to retry cleanly.
    - New `device_t::pending_sample_rate_request`/`sample_rate_apply_failed`
      (`rtl_airband.h`) — same request/apply/confirm shape as the centerfreq fields
      (`device_request_sample_rate()`/`device_confirm_sample_rate()`, mirroring
      `device_request_retune()`/`device_confirm_retune()` exactly), plus a new standalone
      `set_sample_rate` control-socket command (`handle_set_sample_rate`, matching
      `retune`/`set_gain`/`set_bandwidth`/`set_correction`'s shape) and `reload_diff` wiring.
      Unlike gain/bandwidth/correction, `sample_rate` *does* have a live-readable current value
      (`dev->input->sample_rate`), so `reload_diff` does a real diff rather than gain's
      "reapply unconditionally" pattern — re-triggering a full RX-thread stop/reopen/restart cycle
      on every `reload_diff` call regardless of whether the value actually changed would be a real
      cost, not a harmless idempotent no-op. Budgets a longer confirmation timeout (3s vs. plain
      retune's 500ms) given the apply side includes a full RX-thread join and hardware reopen, not
      just one API call.
    - Also required an explicit `pending_sample_rate_request = -1`/`sample_rate_apply_failed =
      false` initialization in `parse_devices()` (`config.cpp`), mirroring the pre-existing
      centerfreq initialization there — missing this was a second real bug caught only by
      real-hardware testing: a freshly `XCALLOC`'d (zero-initialized) `device_t` defaults
      `pending_sample_rate_request` to `0`, which every unit test happened to override explicitly
      but production startup did not, so the demod thread's `if (pending_sample_rate >= 0)` check
      (`rtl_airband.cpp`) treated a fresh device as having an immediate pending request for
      `sample_rate = 0` — dividing by zero in `compute_input_buf_size()` on the very first pass,
      a guaranteed SIGFPE crash at startup that no unit test exercised because every hand-built
      test fixture set the field explicitly.
    - Unit tested extensively: `ComputeInputBufSizeTest` (pure function), `device_confirm_sample_
      rate_reports_success`/`_failure`/`_timeout_when_never_consumed`, `device_request_sample_
      rate_rejects_r_scan`/`_rejects_driver_without_stop_hook`/`_accepts_r_multichannel_with_stop_
      hook`, and a dedicated `SampleRateApplyTest` fixture (`src/test_live_reconfig.cpp`) with real
      fake `init`/`stop`/`run_rx_thread` driver hooks and a genuinely spawned, joined
      `pthread_t` RX thread (not mocked away, since `device_apply_sample_rate()` calls
      `pthread_join()` directly and needs a real thread to join) covering the success path,
      rollback-on-failure, rollback-also-fails (`INPUT_FAILED`), and stop-fails-without-touching-
      state paths, plus `DiffApplyTest.sample_rate_diff_posts_request_and_reports_success`/
      `_reports_rollback_message_on_failure`/`sample_rate_unchanged_does_not_post_a_request`.
      Building this surfaced a third, pre-existing latent bug (documented in item 30 but not
      previously confirmed to actually manifest): a stack-constructed `freq_t` embeds a `Squelch`,
      whose constructor unconditionally calls `debug_print()` — safe in production, but `debugf`
      is never `fopen()`'d in the unit test binary, so this is undefined behavior under a `-DDEBUG`
      build. Under plain `Debug` it silently corrupted heap state that only crashed a later,
      unrelated test (`"malloc(): unaligned tcache chunk detected"`); under ASan it was caught
      immediately at the actual fault site. Fixed in the new `SampleRateApplyTest` fixture the same
      way `ChannelRemoveTest` already does (`calloc`, not a real C++ construction) — the other
      pre-existing tests using the unsafe pattern were left alone as out of scope for this change.
      All 207 tests (14 net new) pass across Debug, Debug+NFM, and under ASan/UBSan.
    - Verified end-to-end against real RTL-SDR hardware, including the two bugs above (both found
      *by* this hardware testing, not by unit tests): a live `set_sample_rate` command and a
      config-edit-plus-`reload_diff` both successfully changed the live rate (confirmed via
      "Found Rafael Micro R820T tuner" / "RTLSDR device 0 initialized" reappearing in the log,
      i.e. a genuine reopen occurred) with the device fully functional afterward (a follow-up
      `retune` succeeded); a fault-injected reopen failure (temporary, reverted, gated on
      `config_error_is_recoverable` so it can never fire at real startup) correctly triggered
      rollback to the previous rate with the device left fully functional; no crash or sanitizer
      report in either case once both bugs were fixed.
    - Known limitation, not addressed here: in the non-default `multiple_demod_threads=false`
      (single shared demod thread) topology with more than one device, this device's turn in the
      round-robin blocks synchronously for the whole stop/reopen/restart sequence (potentially
      hundreds of milliseconds for a real USB reopen), pausing every other device that thread
      services for that duration. An accepted tradeoff for this fork's actual one-device-per-
      instance deployment (see "Deployment Context"), where this concern doesn't arise at all, but
      worth reconsidering before this feature is used in a genuinely multi-device-per-thread
      deployment.
37. **`reload_diff` wording consistency: gain/bandwidth/correction failures now say "no restart
    needed" too** (`src/live_reconfig.cpp`) — found while checking rtl-airband-panel's
    `LiveApplyBanner` (which classifies `skipped_requires_restart` entries by matching the "no
    restart needed" substring, added for items 33/36's centerfreq/sample_rate wording) against
    every field that can land in that array. `gain`/`bandwidth`/`correction` failures (items 27/34/35) are the *same* kind of transient, retryable failure as a failed centerfreq/sample_rate
    change — no live-readable current value, reapplied unconditionally next `reload_diff`, no
    restart actually required — but their messages just said "present in config but failed to
    apply live, see logs", with no indication of that. A caller (this panel, or any other control-
    socket consumer) had no way to distinguish these from a genuinely restart-required entry other
    than by field name. Appended "- no restart needed, retry reload_diff" to all three messages,
    matching centerfreq/sample_rate's existing wording exactly. `gain_set_hardware_failure_does_
    not_mark_input_failed`/`bandwidth_set_hardware_failure_does_not_mark_input_failed`/`correction_
    set_hardware_failure_does_not_mark_input_failed` (`src/test_live_reconfig.cpp`) each gained an
    assertion locking in the new substring, mirroring item 33's regression test. All 207 tests
    pass; not separately re-verified against real hardware since this is a string-only change to
    an already-hardware-tested failure path (items 27/34/35).
38. **Control socket: per-connection recv timeout + buffer cap** (`src/control_socket.{cpp,h}`)
    — the first fix out of a pre-merge review pass across the whole `dynamic_reload` branch
    (items 27-37), specifically scoped to the easiest of four findings, ordered easiest to
    hardest. `handle_connection()` previously had no timeout on its `recv()` loop and no cap on
    how much it would buffer waiting for a newline — a single stalled or misbehaving client (a
    bug in a caller, or a `nc -U`/manual session left open) blocked *every* other control-socket
    operation on that instance indefinitely, since `control_main()`'s accept loop is single-
    threaded and doesn't service a new connection until `handle_connection()` returns. This also
    meant a stuck client could delay a clean process shutdown by the same amount, since
    `control_shutdown_requested` is only re-checked between `recv()` calls.
    - `handle_connection()` now takes `timeout_sec`/`max_buffered_bytes` parameters (defaulted in
      the header to 10s/16KiB for the production call site in `control_main()`, so that call site
      needed no changes) and sets `SO_RCVTIMEO` on the accepted fd; a client that sends more than
      `max_buffered_bytes` without a newline is disconnected immediately rather than buffered
      without limit.
    - Moved out of the anonymous namespace and declared in `control_socket.h` (matching the
      existing pattern for `control_socket_dispatch_command_line()`/`control_socket_parse_command_
      line()`) specifically so it's unit-testable — unlike those two, there's no way to exercise a
      real `recv()` timeout without a real socket, so the new tests use a short injected timeout
      rather than the production default to stay fast.
    - This does **not** make the control socket handle multiple clients concurrently — that
      remains a single-connection-at-a-time design, unchanged and out of scope for this fix.
      Verified via real-hardware testing: a stuck first client with no data sent still delays a
      second client's request, but now bounded to the configured timeout (~10s, confirmed via a
      live timed test) rather than blocking forever.
    - Unit tested in `src/test_control_socket.cpp`'s new `HandleConnectionTest` fixture, using a
      real `AF_UNIX` `socketpair()` (not mocked): closes on timeout with no data sent, closes when
      the buffer cap is exceeded without a newline, and a normal request/response round trip still
      works unchanged over the real socket. All 210 tests (3 net new) pass across Debug,
      Debug+NFM, and ASan/UBSan. Verified end-to-end against real RTL-SDR hardware: a normal
      `retune` command still works, and a stuck client no longer wedges a second client's request
      past the new timeout.
39. **Channel removal: tombstone to prevent a real crash on a stale index** (`src/rtl_airband.h`,
    `src/config.cpp`, `src/live_reconfig.cpp`, `src/control_socket.cpp`) — the second, harder
    finding from the same pre-merge review pass as item 38, and the highest-severity of the four:
    a genuine crash path, not just tech debt.
    - **The bug**: `try_remove_channels()` (`live_reconfig.cpp`) only decrements
      `dev->channel_count` after `channel_request_remove()` confirms the output thread has
      finished tearing a channel down. If that confirmation times out (plausible under real load —
      a congested Icecast connection, exactly this fork's actual Broadcastify-over-the-internet
      deployment shape), `try_remove_channels()` gives up and reports the channel as not removed —
      but the request it already posted is still live and *will* be processed by the output thread
      whenever it next catches up, asynchronously, with no further correlation back to the caller.
      `dev->channel_count` is never decremented for that index. In that window (which can be
      arbitrarily long, not just the ~500ms request timeout, if the output thread was genuinely
      stuck), the channel index looks "still live" to `get_device_and_channel()`
      (`control_socket.cpp`), the sole bounds check backing the standalone `channel_enable`/
      `channel_disable` commands — but the removal is either in flight or has already actually
      completed, freeing the channel's LAME encoder (`channel_teardown_for_removal()`). If a
      `channel_enable` command lands on that same index in this window, `channel_apply_enable()`
      reopens the channel's connections but never reallocates LAME (only `init_output()` does,
      called only at startup or for a freshly-appended channel) — the next `process_outputs()`
      pass then calls into LAME with a null encoder and crashes the entire `output_thread()`,
      taking down **every feed on that instance**, not just the one channel being edited.
    - **A second, quieter bug from the same root cause**: `channel_teardown_for_removal()` never
      updates `channel->config_signature`. If an operator reverts a channel deletion in the config
      file (restoring the exact original definition), `compute_and_apply_diff()`'s common-prefix
      signature match (item 31) would previously see "signature unchanged" and never re-diff that
      index again — the channel stays permanently, silently dead (torn down, `enabled = false`)
      with no way for `reload_diff` to ever revive it short of a full restart.
    - **The fix**: a new permanent tombstone, `channel_t::removed` (`rtl_airband.h`), set by
      `channel_teardown_for_removal()` once a removal has actually completed, and reset to `false`
      only by `parse_channel()` (`config.cpp`) populating a fresh channel into a reused slot
      (mirroring exactly how `pending_enable_request`/`pending_remove_request` are already reset
      there). Checked in two places:
      - `get_device_and_channel()` (`control_socket.cpp`) now rejects any index where
        `pending_remove_request != -1` (a removal is currently in flight - closes the window
        *before* teardown completes too, not just after) or `removed == true` (already torn
        down) — closing the crash path entirely, with a clear error instead of a silent time bomb.
      - `compute_and_apply_diff()`'s common-prefix walk (`live_reconfig.cpp`) now treats a
        tombstoned channel as always diverged, regardless of signature match — forcing a reverted
        delete to be correctly retried and revived instead of silently skipped forever.
    - Investigated and deliberately did *not* tombstone at the moment a removal is merely
      *requested* (i.e., inside `channel_request_remove()` itself) as a simpler alternative:
      checking `pending_remove_request != -1` already covers that entire in-flight window, so a
      separate request-time tombstone would be redundant - the two checks together (in-flight OR
      already-removed) close the full window with no gap, using one new field rather than two.
    - Unit tested in `src/test_control_socket.cpp`'s `ControlSocketDispatchTest` (four new cases:
      rejects a channel with a removal in flight, rejects an already-removed channel, confirms
      `channel_disable` is covered by the same fix, confirms a normal never-removed channel is
      unaffected — each asserting `pending_enable_request` is never posted against a rejected
      index, the actual mechanism that would otherwise lead to the crash) and
      `src/test_live_reconfig.cpp`'s new `ChannelAppendTest.tombstoned_channel_is_revived_even_
      when_its_signature_still_matches` (the full revert-a-delete scenario: a channel tombstoned
      with a config_signature that still matches the file is correctly torn down and re-appended,
      not silently skipped, and the tombstone is cleared on revival). All 215 tests (5 net new)
      pass across Debug, Debug+NFM, and ASan/UBSan. Verified end-to-end against real RTL-SDR
      hardware: normal `channel_enable`/`channel_disable`/`retune` all confirmed unaffected by the
      new check (this fix is pure software bounds-checking logic with no new hardware interaction,
      so hardware verification was scoped to confirming no regression, not reproducing the exact
      race - which is precisely and directly exercised by the unit tests' simulated stuck/working
      consumer threads instead).

40. **Mixer: reclaim an input slot on permanent channel removal, not just temporary disable**
    (`src/rtl_airband.h`, `src/mixer.cpp`, `src/output.cpp`, `src/live_reconfig.cpp`) — the third
    of the four pre-merge-review findings, and the last of item 31's own documented caveats still
    open: `mixer_disable_input()` only ever masked a slot off (`input_mask[idx] = false`); it never
    released the slot back to `input_capacity`. That's correct for an *ordinary* temporary disable
    (`mixer_disable()`'s "all inputs died" auto-cascade, `channel_apply_disable()`'s control-socket
    `channel_disable` command) — the same channel is expected to reconnect to the same index later
    via `mixer_enable_input()` — but item 31's live channel *edit* tears the old channel down and
    appends a fresh replacement, so every single live edit of a mixer-connected channel was
    permanently burning a new `reserve_inputs` slot it could never get back, exactly as item 31
    flagged but left unfixed at the time.
    - **The fix**: `disable_channel_outputs()` (`output.cpp`) and `mixer_disable_input()`
      (`mixer.cpp`) both gained a `permanent` bool parameter (default `false`, so every pre-existing
      call site is unchanged). `channel_teardown_for_removal()` (`live_reconfig.cpp` — used by both
      item 30's removal and item 31's tail-replace edit) is the only caller that passes `true`. A
      new parallel array, `mixer_t::input_removed` (`rtl_airband.h`), is tombstoned by
      `mixer_disable_input(..., permanent=true)` and checked first thing in `mixer_connect_input()`
      — a tombstoned slot is reused (fields reset, `input_mask`/`inputs_todo` republished, same
      "publish `input_mask` last" discipline the fresh-slot path already used) before any capacity
      check or growth is attempted, so reuse never touches `input_capacity`/`reserve_inputs` at all.
      The slot's `pthread_mutex_t` is deliberately never re-initialized or destroyed on reuse — only
      ever set up once, at the slot's first real allocation — since `mixer_thread()` locks every
      slot's mutex unconditionally every pass regardless of `input_mask` (see its per-input loop),
      so the mutex object must always remain valid for the mixer's entire lifetime.
    - **Reuse-scan safety**: the scan and reuse both run inside `mixer_connect_input()`, which is
      only ever called from the single control-socket thread's `reload_diff`-driven live-append
      path (`control_socket.cpp`'s accept loop handles one connection at a time — see item 27's own
      comment on this) — so there's no concurrent-writer race on the scan itself, matching every
      other live-reconfig primitive's single-writer-thread invariant.
    - Unit tested directly in `src/test_mixer.cpp` (`MixerCapacityTest`: permanent disable
      tombstones without disturbing other inputs, non-permanent disable does not tombstone, reuse
      pre- and post-`mixer_finalize_capacity()` without growing capacity, and a
      `mixer_put_samples()` round-trip through a reused slot proving the un-reinitialized mutex is
      still genuinely usable) and end-to-end through `test_live_reconfig.cpp`'s existing
      `replaces_tail_channel_with_edited_mixer_output` test, updated to assert the now-fixed
      behavior (`input_count` stays at 1, the old slot is reused, no `reserve_inputs` needed) rather
      than the old caveat it used to document. That test's coverage required upgrading
      `test_mixer.cpp`'s file-scope `disable_channel_outputs()` stub (previously a total no-op,
      since the real one in `output.cpp` needs shout/lame/real file I/O this test binary doesn't
      link in) to reproduce just the `O_MIXER` branch — pure `mixer.cpp` logic, already linked —
      so the reclaim path is genuinely exercised through the same call chain production uses, not
      simulated. All 220 tests (5 net new) pass across Debug, Debug+NFM, `-DRDIO_SCANNER=OFF`, and
      ASan/UBSan. No hardware interaction in this fix (pure in-process mixer bookkeeping), so
      verification was unit/system-test-only — `system_tests/tests/test_channel_add.py`,
      `test_channel_edit.py`, `test_channel_remove.py`, and `test_control_socket.py` all still pass
      against rebuilt `Release`/`Release_nfm` binaries, confirming no regression in the mixer output
      paths those tests already exercise end-to-end.
    - **Merge-time fix (main → `dynamic_reload`, this branch's merge into `main`)**: item 26's
      `send_tx_tags` added `mixinput_t::source_device_idx`/`source_channel_idx` (resolved by
      `parse_outputs()` right after `mixer_connect_input()` returns, on every connect - fresh or
      reused) on a separate line of history, so this item's reuse branch never anticipated them.
      The fresh-growth path already reset both to `-1` before publishing `input_mask[i] = true`
      (`mixer_tx_tag()` treats `-1` as "no tag", never dereferences it); the reuse branch didn't,
      so a reused slot briefly exposed the *previous* occupant's still-valid-but-wrong source
      indices to `mixer_tx_tag()` in the window between `mixer_connect_input()` publishing the
      slot and `parse_outputs()` setting the real values a few lines later - a stale-but-not-unsafe
      on-air tag for one tick, not a crash. Fixed by resetting both to `-1` in the reuse branch
      too, matching the fresh-growth path's discipline exactly.

41. **Channel teardown: safely reclaim leaked outputs/freqlist memory** (`src/live_reconfig.{cpp,h}`,
    `src/output.cpp`, `src/test_live_reconfig.cpp`) — the fourth and hardest of the pre-merge-review
    findings. `channel_teardown_for_removal()` (used by both item 30's removal and item 31's tail-
    replace edit) deliberately left `channel->outputs`/`freqlist`/`config_signature` and each
    output's own `data` struct allocated forever, because freeing them immediately risked a real
    use-after-free: `output_check_thread()` and the demod thread's per-channel loop both gate their
    access on `channel->enabled` (`std::atomic<bool>`, set false first) before touching this memory,
    but that only narrows the risk window, it doesn't close it — a reader that already observed
    `enabled == true` and is mid-access when the free happens has no synchronization stopping it.
    Investigating this surfaced a **third, unguarded reader** beyond the two originally scoped:
    `write_stats_file()`'s (`output.cpp`) per-channel metrics loop iterates `channel->freqlist`
    unconditionally, with no `enabled` check at all — the widest, least-protected exposure of the
    three, on a 15-second cycle (`STATS_FILE_TIMING`).
    - **Rejected approach**: a generation/epoch counter synchronized against every reader thread
      (demod, `output_check_thread`, `write_stats_file`, each with a different cadence and, for the
      stats writer, no existing gate to build on) — correct in principle, but would have required
      touching multiple hot-path loops for a fix whose entire point is closing a memory-safety gap,
      not a performance-critical primitive.
    - **The fix**: a deferred, time-based reclamation queue, entirely private to `live_reconfig.cpp`
      and drained by `output_thread()` (`output.cpp`) — the same thread that already exclusively
      owns this memory via `channel_teardown_for_removal()`. Teardown now captures
      `outputs`/`output_count`/`freqlist`/`freq_count`/`config_signature` into a
      `PendingChannelFree` entry (timestamped, pushed under a small `pthread_mutex_t`) instead of
      freeing them — `channel->outputs`/`freqlist`/`config_signature` are deliberately left pointing
      at this still-valid memory in the meantime, so a slot reused by a later `parse_channel()` call
      (an edit, not just a removal) simply overwrites the pointers with fresh ones; the old values
      are already safely captured. `reclaim_pending_channel_frees()`, called once per
      `output_thread()` pass (a lock-free `std::atomic<size_t>` size check makes this a no-op on the
      overwhelming majority of passes, when nothing is queued), actually frees anything whose
      `reclaim_grace_period_sec` (30s in production — comfortably more than double
      `STATS_FILE_TIMING`, since `write_stats_file()` itself runs to completion in well under a
      second, so any invocation in flight when a channel is torn down is long finished by the time
      this elapses) has passed. `lame`/`lamebuf` are unaffected — those still free immediately, as
      before this item, since neither `output_check_thread()` nor `write_stats_file()` ever touches
      them.
    - **`free_output_data()`** (`live_reconfig.cpp`, anonymous namespace) mirrors, in reverse, every
      heap allocation `parse_outputs()` (`config.cpp`) makes per output type: `icecast_data`/
      `udp_stream_data`/`pulse_data` are `XCALLOC`'d with individually `strdup()`'d string fields
      (plain `free()`, `const_cast` needed since the struct fields are `const char*`); `file_data`
      (and its nested `rdio_scanner_data`, if configured) are `new`'d with `std::string` members
      (`delete`, not `free()` — a free()/delete mismatch here would itself be a heap-corruption bug
      this fix exists to prevent); `mixer_data` owns no nested pointers (`mixer_t*` points at the
      process-wide `mixers[]` array, never freed here). `freqlist[k].label` (`strdup()`'d,
      `config.cpp`) is freed per-entry before the `freqlist` array itself.
    - Unit tested in `src/test_live_reconfig.cpp`'s new `ReclaimPendingChannelFreesTest` fixture:
      one test proves the queue/timing contract directly (`pending_channel_free_backlog()` — a new
      test-only introspection hook, `reclaim_grace_period_sec` — a new test-only overridable global,
      same rationale as `mixer_capacity_finalized`'s existing one — shrunk to make the grace period
      deterministic instead of a real 30-second sleep); a second builds a channel with real,
      fully-populated `icecast_data`/`file_data` (with nested `rdio_scanner_data`)/`udp_stream_data`/
      `pulse_data` outputs — everything except `O_MIXER`, whose reclaim path has no type-specific
      logic and is already covered by item 40's tests — and tears it down for real, with no
      explicit memory assertion needed: a double-free, an allocator mismatch, or a buffer overflow
      anywhere in `free_output_data()` would abort the whole test binary under ASan before reaching
      the final `EXPECT`. Confirmed clean under ASan/UBSan **with LeakSanitizer enabled** (the full
      suite otherwise runs with `detect_leaks=0` — unrelated pre-existing fixture leaks elsewhere —
      but these two tests specifically were run leak-detection-on and reported none, positively
      confirming the fix, not just "doesn't crash"). Existing tests that call
      `channel_teardown_for_removal()` directly (`ChannelRemoveTest`) needed their `TearDown()`
      updated to drain the pending-free queue (grace period forced to 0) before their own direct
      `free()` calls, to avoid a cross-test double-free once a later test (this item's own) shrinks
      `reclaim_grace_period_sec` and drains everything still queued from earlier tests in the same
      process. All 222 tests (7 net new across items 40–41) pass across Debug, Debug+NFM,
      `-DRDIO_SCANNER=OFF`, and ASan/UBSan. No hardware interaction (pure in-process memory
      management), so verification was unit/system-test-only —
      `system_tests/tests/test_channel_add.py`, `test_channel_edit.py`, `test_channel_remove.py`,
      `test_control_socket.py`, `test_icecast_output.py`, and `test_rdio_scanner_output.py` all
      still pass against rebuilt `Release`/`Release_nfm` binaries with `reclaim_pending_channel_
      frees()` now running on every `output_thread()` pass in the real binary, not just the test
      harness.

42. **Cross-instance remote mixer input** (`src/mixer_remote_wire.{h,cpp}`, `src/mixer_remote.{h,cpp}`,
    new; `src/rtl_airband.h`, `src/config.cpp`, `src/output.cpp`, `src/rtl_airband.cpp`, `src/mixer.cpp`) —
    a mixer in one `rtl_airband` instance can now absorb a live audio input streamed from a
    channel in a *different* instance's process, same host only. Investigated and scoped in a
    session that confirmed mixers were previously 100% in-process (no IPC/shared-memory/socket
    mechanism connected `mixers[]` across processes anywhere in this codebase). Deliberately
    extends each instance's own mixer (any mixer can absorb a remote input alongside its local
    channels) rather than introducing a standalone no-device relay-hub process — this fork's
    ~12 instances already run on one host (`10.0.50.31`), so a same-host, same-UID transport was
    chosen over real cross-host networking, which remains an explicit non-goal for now.
    - **Wire protocol** (`mixer_remote_wire.h`) — a fixed 32-byte header (`magic`, `version`,
      `format`, `sample_rate`, `stream_id`, `seq`, `sample_count`, `flags`) immediately followed
      by raw mono float32 PCM, one datagram per `WAVE_BATCH` tick. Rate mismatch is
      self-described per-packet rather than a separate handshake (simpler given the
      connectionless, fire-and-forget transport below); only `STREAM_FORMAT_FLOAT32`-equivalent
      is implemented (`MIXER_REMOTE_FORMAT_FLOAT32`) — same-host loopback has no real bandwidth
      constraint, so 16/8-bit variants (mirroring `udp_stream_format`) are deferred. Pure,
      no-I/O encode/decode/seq-classification functions, unit tested directly.
    - **Transport**: a new `AF_UNIX SOCK_DGRAM` socket, separate from the `dynamic_reload`
      control socket (`SOCK_STREAM`/JSON, control-plane only — audio and control traffic have
      different framing/latency needs). The *sending* side (`mixer_remote_send_init()`,
      `mixer_remote.cpp`) deliberately never `connect()`s: unlike a UDP socket, an `AF_UNIX
      SOCK_DGRAM` `connect()` requires the target socket file to already exist, but the
      receiving instance may start after this one does — connecting eagerly would make
      `init_output()` fail (fatal at startup, item 15) purely because a sibling instance hasn't
      started its listener yet. Every packet instead uses `sendto()` with the destination
      supplied explicitly; a missing/refused receiver is counted (`dropped_packet_count`,
      mirroring `udp_stream_data`'s counter exactly) and silently non-fatal, matching this
      output's own existing resilience pattern.
    - **Trust model**: `SCM_CREDENTIALS`/`SO_PASSCRED`, the connectionless analog of
      `control_socket.cpp`'s `SO_PEERCRED` check (which only applies to a connected/`SOCK_STREAM`
      socket) — a datagram is rejected unless its sender's UID matches the receiving process's
      own `getuid()`. Same-UID was chosen deliberately after checking against this deployment's
      *in-flight* per-instance service-account migration (see the `plugdev`/USB-permissions
      incident below in Deployment Context): any two instances meant to share a mixer must run
      under the same service account for this to work.
    - **Config surface**: a new `mixer_remote` output type (`type = "mixer_remote"; dest_path =
      "..."; stream_id = N;`), legal on any `channel_t` (device channel or a mixer's own embedded
      channel, matching `O_UDP_STREAM`'s precedent — not restricted like local `type = "mixer"`
      outputs) via the standard 3-file "adding an output" pattern this fork documents. Dispatched
      **unconditionally every tick** in `process_outputs()` (not gated on squelch like
      `O_UDP_STREAM`'s `continuous` check) — load-bearing: the receiver's `mixer_thread()`
      already silence-fills a slot that misses its ~1/16s window, but that only correctly
      distinguishes "sender alive, channel quiet" from "sender/process gone" if packets keep
      arriving every tick. No `ampfactor`/`balance` on the sending side — those describe how the
      *receiving* mixer treats the input, so (mirroring how a local `type = "mixer"` output
      already stores them mixer-side) they live in the receiver's config instead.
    - **Receiving side**: a new mixer-level `remote_inputs` config block (`listen_path`,
      `stream_id`, `ampfactor`, `balance`, optional `label`), parsed by `parse_mixers()`
      (`config.cpp`) at startup — the same single-threaded window `mixer_finalize_capacity()`
      already relies on — via the **existing** `mixer_connect_input()`, so no new
      capacity-safety mechanism was needed. Multiple `remote_inputs` entries may share one
      `listen_path`, multiplexed by `stream_id`. No live creation in this initial landing
      (matches item 27's "mixer/device add/remove out of scope, toggle only" precedent) —
      changing which sender feeds which slot needs a restart; the existing whole-mixer
      `mixer_enable`/`mixer_disable` control-socket commands already cover disabling every
      remote input on a mixer at once, so a granular per-remote-input live toggle was
      deliberately left out of scope (it would need `mixer_thread()` itself to grow a new
      poll-and-consume responsibility it doesn't have today, since a remote input has no local
      owning output thread the way a device channel does).
    - **Threading**: one receive thread per unique `listen_path` (not per `stream_id` — one
      socket, multiplexed by the packet header), calling `mixer_put_samples()` **directly**, with
      no sentinel request/apply split unlike `device_t`/`channel_t`'s pattern (items 27-36).
      Rationale: `mixinput_t` was already designed with its own per-slot `pthread_mutex_t`
      guarding exactly the producer-facing fields, specifically so *any* producer thread can call
      `mixer_put_samples()` safely — already proven today by `multiple_output_threads = true`,
      where several output threads already call it concurrently against the same mixer's
      different slots. The sentinel-request pattern exists for fields with no independent lock of
      their own (`device_t::bins`, `channel_t::outputs`); that's not the case here. The one
      invariant that still applies — slot creation being startup-only/single-threaded — is why
      registry/slot setup happens in `parse_mixers()`, not from this thread.
    - **Tag support** (`send_tx_tags`, item 26): `mixinput_t` gained a `remote_label` field
      (`strdup`'d from a `remote_inputs` entry's optional `label`), since a remote input has no
      local `source_device_idx`/`source_channel_idx` to look up. `mixer_tx_tag()` (`output.cpp`)
      falls back to it when those indices are unresolved. Required the same slot-reuse/
      fresh-growth zeroing discipline `source_device_idx`/`source_channel_idx` already follow
      (`mixer_connect_input()`'s tombstone-reuse branch, its fresh-growth branch, and
      `mixer_finalize_capacity()`'s realloc-tail-zeroing loop) — same class of bug item 40's
      merge-time fix caught for the mixer-tag-source-index fields, applied here proactively
      rather than found the hard way.
    - **Observability**: per-route counters (`rate_mismatch_count`, `sample_count_mismatch_count`,
      `malformed_payload_count`, `seq_gap_count`, `seq_reorder_or_duplicate_count`,
      `last_packet_time` gauge — lets an operator tell a quiet-but-alive sender apart from one
      whose process is gone entirely, something the existing local-input model never needed) and
      per-listener counters (`rejected_uid_count`, `malformed_header_count`, `unknown_stream_count`
      — a malformed *header* can't be attributed to a route at all, since decoding it is what
      would identify the route in the first place, so this is split from the route-level
      `malformed_payload_count`). All exposed via `write_stats_file()`/the HTTP metrics endpoint
      (item 8), following item 23's established per-output-counter pattern. A sender flooding
      faster than `WAVE_BATCH` cadence is covered for free by the existing
      `mixinput_t::input_overrun_count` — no new counter needed for that case.
    - **Deviation from the original plan**: the plan called for a listener `bind()` failure to be
      fatal at startup, mirroring `init_output()`. Implemented as non-fatal instead
      (`mixer_remote_recv_start()` logs and skips that listener, leaving its routes permanently
      silence-filled) after checking the actual precedent: `control_socket_start()`/
      `stats_http_start()` — the closest analogs, both optional background listeners — are
      already non-fatal in this codebase (their return values aren't even checked in `main()`).
      Taking down a mixer's other, working inputs/outputs over one failed optional listener was
      judged disproportionate, consistent with this fork's broader shift (items 32-36) toward
      "a transient failure in one subsystem shouldn't take down the whole process."
    - Unit tested extensively: wire encode/decode round-trip and bounds checks
      (`test_mixer_remote_wire.cpp`, matching item 11's "real check, not `assert()`" discipline);
      send-side against a real bound `AF_UNIX SOCK_DGRAM` socket including the "nothing listening
      yet is safe/inert" contract and a `dest_path`-too-long rejection (`test_mixer_remote.cpp`);
      receive-side registry logic, `mixer_remote_dispatch_packet()`'s full failure-mode matrix
      (wrong UID, malformed header, unknown stream, rate/sample-count mismatch, seq
      gap/duplicate/reorder classification), and a full round trip through the *real* listener
      thread and a real bound socket including its `SCM_CREDENTIALS` extraction
      (`test_mixer_remote.cpp`); `parse_mixers()`'s `remote_inputs` block including the
      duplicate-`stream_id`-on-one-`listen_path` and missing-field config-error paths, exercised
      via the `config_error_is_recoverable`/`ConfigApplyError` mechanism (item 28) rather than
      letting a malformed config `_Exit(1)` the test binary; and a dedicated regression test
      proving `"mixer_remote"` (which shares its first 5 characters with the pre-existing
      `"mixer"` output type's `strncmp()` match) is checked first in `parse_outputs()` and is
      never silently swallowed by the `"mixer"` branch (`test_live_reconfig.cpp`). All 279 tests
      (57 net new) pass across Debug, Debug+NFM, and `-DRDIO_SCANNER=OFF`.
    - **Manually validated end-to-end against two real RTL-SDR dongles** on this dev box
      (serials `SI02`/`SI03`, one sender instance + one receiver instance, real `Release`
      binary, `mixers.conf`-style config): zero `mixer_remote_route_failure_count`/
      `mixer_remote_listener_failure_count` across ~65s of continuous real transmission
      (no rate/sample-count mismatches, no seq gaps/reorders, no malformed packets, no
      rejected UIDs); killing the sending instance mid-stream left the receiver running
      with no crash, and `mixer_remote_last_packet_time_seconds` correctly froze at the
      last real packet instead of resetting or continuing to advance — confirming the
      "quiet sender" vs. "gone sender" distinction this metric exists for actually holds
      up against a real process death, not just the simulated one in `test_mixer_remote.cpp`.
    - **Real bug this testing caught, not a flaw in the feature**: the first attempt used a
      `dest_path`/`listen_path` under a deeply nested test-scratch directory (127 bytes) —
      `AF_UNIX` socket paths are capped at 107 bytes on Linux (`sizeof(sockaddr_un::sun_path)
      - 1`), and both `mixer_remote_send_init()` and `mixer_remote_recv_start()` correctly
      rejected it as "too long" (logged, non-fatal on the send side; logged and that
      listener skipped on the receive side) rather than truncating the path or crashing —
      exactly the designed behavior, just not something the unit tests (which use short
      `temp_dir`-relative paths) had exercised against a realistically long path. No code
      change needed; worth remembering that a deployment's `dest_path`/`listen_path` must
      stay well under 107 bytes (`/run/rtl_airband/*.sock`-style paths are safely short).
    - **Unrelated gotcha hit during this same validation session, not specific to this
      feature**: `-F` (foreground) alone does not disable syslog — `do_syslog` defaults to
      `1` regardless of `-F`, so every `log()` call (including all of this feature's own
      startup/diagnostic messages) went to `journalctl`/syslog, not the redirected
      stdout/stderr file, until `-e` was added too. Caused a long, confusing detour
      chasing a phantom "listener never starts" hypothesis before the real explanation
      (log destination, not a hang) was found. Worth remembering for any future manual
      rtl_airband testing session, not just this feature's.
    - **Still not done**: a two-instance system test (every existing system test drives
      exactly one running instance; this needs a `Popen()`-based two-process helper, closer
      to `helpers/interactive_runner.py`'s live-process pattern than
      `conftest.run_rtl_airband()`'s blocking model — flagged as the largest net-new test
      infrastructure item when this was scoped, still not built, so this manual validation
      is not a substitute for that automated coverage going forward). Per-remote-input live
      enable/disable and 16/8-bit wire formats remain deliberately out of scope unless a
      concrete need emerges.

Anything outside those areas should match upstream. If a diff against `upstream/main` shows
changes elsewhere, treat it as unintended drift and flag it.

### Do not import from `yegors`

The `yegors` fork also contains SRT output support and its own UDP path. The UDP path there is
non-functional — its `O_UDP_STREAM` branch in `process_outputs()` checks squelch and then never
calls `udp_stream_write()`. Neither the SRT work nor that UDP code should be pulled in.

### Upstreaming

Bug fixes that are not personal preference — the `sendto` fix in particular — are candidates for
PRs to `rtl-airband/RTLSDR-Airband`. Open an issue describing the bug before sending a PR.
The `post_write_script` / `min_rx_seconds` features originated with `yegors`; credit accordingly
if they are ever proposed upstream.

## Working Norms

- **Keep this file updated** as changes are made to the codebase.
- **Never guess or make assumptions** — ask clarifying questions when requirements are unclear.
- **All code changes should include unit tests.** When possible, write tests first and implement code to make them pass (TDD).
- **After writing code, review comments** and remove any that don't explain non-obvious behavior — don't comment what the code already says.
- **This fork feeds live public-safety audio.** Changes here affect production Broadcastify feeds.
  Test on a low-priority SDR instance before rolling out.

## Development Environment

The repo is set up for development in VS Code using a devcontainer (`.devcontainer/`). When working
inside the devcontainer, all compile, test, and run commands must be executed inside the container.

Note: the devcontainer has failed to build under GitHub Codespaces (`docker buildx build` exits 1
during container creation). Local builds on Ubuntu without the devcontainer work fine — install the
dependencies directly and build with CMake as below.

## Wiki Documentation

User-facing documentation lives in a separate repo: https://github.com/rtl-airband/RTLSDR-Airband/wiki

Flag when code changes require wiki updates and provide suggested content — do not edit the wiki directly.

## Code Review Guidelines

When reviewing code:
- Reference specific files and line numbers (`src/foo.cpp:42`)
- Start with architecture-level concerns before line-level feedback
- Consider SDR/DSP domain context (signal processing constraints, real-time threading, buffer management)
- Verify the testing approach covers the behavior being changed
- Structure feedback clearly: separate blocking issues from suggestions
- Be pragmatic — prefer working correct code over theoretical perfection
- Check for consistency with surrounding code style and conventions

## Project Overview

RTLSDR-Airband is a C++ SDR (Software-Defined Radio) application that receives analog radio voice
channels from SDR devices (RTL-SDR, SoapySDR, MiriSDR) and produces MP3 audio streams for Icecast,
file recording, UDP, and PulseAudio.

## Build Commands

Dependencies: libconfig++, libmp3lame, libshout, libfftw3f, librtlsdr, libsoapysdr, libpulse, libcurl (fork-only, for `RDIO_SCANNER`).
Install via `.github/install_dependencies`.

```bash
# Standard debug build with unit tests
cmake -B builds/Debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNITTESTS=TRUE
cmake --build builds/Debug -j4

# Release build with NFM and SoapySDR
cmake -B builds/Release -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DSOAPYSDR=ON
cmake --build builds/Release -j4

# Run unit tests
./builds/Debug/src/unittests
./builds/Release/src/unittests

# Run the binary
./builds/Debug/src/rtl_airband -c /path/to/config.conf
./builds/Release/src/rtl_airband -c /path/to/config.conf
```

Key CMake flags (all in `src/CMakeLists.txt`):

| Flag | Default | Purpose |
|------|---------|---------|
| `NFM` | OFF | Enable Narrow FM demodulation |
| `PLATFORM` | `native` | Optimization target: `native`, `generic`, `rpiv2` |
| `RTLSDR` | ON | RTL-SDR driver |
| `MIRISDR` | ON | Mirics SDR driver |
| `SOAPYSDR` | ON | SoapySDR (vendor-neutral) driver |
| `PULSEAUDIO` | ON | PulseAudio output |
| `RDIO_SCANNER` | ON | Native rdio-scanner call-upload output (fork-only, needs libcurl) |
| `BUILD_UNITTESTS` | OFF | Build Google Test unit tests |
| `BCM_VC` | OFF | Broadcom VideoCore GPU FFT (RPi v2 only) |

The flag names are `NFM` and `PULSEAUDIO`, **not** `WITH_NFM` / `WITH_PULSEAUDIO`. A `-DWITH_NFM=ON`
typo silently defines an unused cache variable and leaves NFM off. Confirm from the build rather
than from the command line that was typed:

```bash
grep -n "NFM" builds/Release/CMakeCache.txt
```

### `NFM` matters for anything consuming the UDP stream

`WAVE_RATE` is 16000 when NFM is enabled and 8000 when it is not. Any downstream decoder of the raw
UDP PCM must match, or audio plays back at the wrong rate. This fork's deployed builds use `NFM` on.
A config containing `modulation = "nfm"` fails to parse on an AM-only build, which is a useful
implicit confirmation that the flag took effect.

## Code Formatting and Pre-commit

Uses clang-format v14 with Chromium style (indent=4, column limit=200, config in `.clang-format`).

```bash
# Install pre-commit hooks (once, after cloning)
pre-commit install

# Run all pre-commit hooks manually
pre-commit run --all-files

# Format C++ source files manually (also used by CI)
./scripts/reformat_code
```

Pre-commit hooks (`.pre-commit-config.yaml`) run on every commit and check:
- YAML/JSON validity, trailing whitespace, EOF newlines, shebang permissions, large files, merge conflict markers, private keys
- clang-format on all `src/*.cpp` and `src/*.h` files
- shellcheck on all bash scripts (excluding `init.d/`)
- black, isort, and pylint on all `system_tests/**/*.py` files
- Build (AM and NFM) and C++ unit tests when `src/*.cpp`, `src/*.h`, or `CMakeLists.txt` are modified (`scripts/run_unit_tests`)
- Python system tests when `src/*.cpp`, `src/*.h`, `CMakeLists.txt`, or `system_tests/` are modified (`scripts/run_system_tests`); only runs if the build/unit-test step passes

## CI and Pull Request Checks

Three workflows run on every PR (`.github/workflows/`):

**`code_formatting.yml`** — runs `./scripts/reformat_code` and fails if any files differ.

**`ci_build.yml`** — builds and tests four configurations on Ubuntu (x86 and ARM) and macOS:
```bash
cmake -B builds/Debug          -DCMAKE_BUILD_TYPE=Debug   -DBUILD_UNITTESTS=TRUE
cmake -B builds/Debug_nfm      -DCMAKE_BUILD_TYPE=Debug   -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release        -DCMAKE_BUILD_TYPE=Release -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release_nfm    -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
```
Then runs `unittests` for all four, installs the Release+NFM build, and smoke-tests `rtl_airband -v`.

**`platform_build.yml`** — builds and tests an AM Release configuration (`PLATFORM=native`) on a Pi 4B runner and an `ubuntu-22.04-arm` runner, then runs unit tests and system tests. (Pi 3B runner is currently disabled.)

**Before submitting a PR**, the pre-commit hooks cover most checks automatically. For build system or config changes not touching `src/`, verify all four cmake configurations build cleanly by hand.

## System Tests

End-to-end tests live in `system_tests/`. They run the actual binary against generated IQ files and validate the audio output (MP3 duration, rawfile size). Managed with [uv](https://docs.astral.sh/uv/).

```bash
# Run system tests (requires Release binaries — run scripts/run_unit_tests first)
scripts/run_system_tests

# Run manually from the system_tests directory
cd system_tests
uv sync
uv run pytest tests/ \
    --binary ../builds/Release/src/rtl_airband \
    --nfm-binary ../builds/Release_nfm/src/rtl_airband \
    -v
```

Python tooling (formatter, import sorter, linter) is configured in `system_tests/pyproject.toml` under `[tool.black]`, `[tool.isort]`, and `[tool.pylint]`. Run them manually:

```bash
cd system_tests
uv run black .
uv run isort .
uv run pylint conftest.py helpers/ tests/
```

## Architecture

### Reception Pipeline

```
SDR device (input-*.cpp)
  → RX thread → circular sample buffer
  → demod thread: FFT (FFTW3) → demod (AM/NFM) → filter → CTCSS → squelch → AGC
  → channel output handlers
  → output thread: MP3 encode (lame) → Icecast / file / UDP / PulseAudio
```

### Key Source Files

| File | Purpose |
|------|---------|
| `src/rtl_airband.cpp` | Main entry point, demod loop, thread management |
| `src/rtl_airband.h` | All major struct/enum definitions (`device_t`, `channel_t`, `mixer_t`, `output_t`) |
| `src/config.cpp` | libconfig++ parsing for devices, channels, mixers, outputs |
| `src/output.cpp` | MP3 encoding, Icecast connections, file/UDP output |
| `src/udp_stream.cpp` | UDP socket setup and raw float PCM send |
| `src/mixer.cpp` | Multi-channel mixer with ampfactor/balance |
| `src/input-*.cpp` | SDR device drivers (rtlsdr, soapysdr, mirisdr, file) |
| `src/input-common.cpp/h` | Input device abstraction (`input_t` function-pointer interface) |
| `src/filters.cpp/h` | IIR lowpass and notch filters |
| `src/squelch.cpp/h` | Noise-power-based voice activity detection |
| `src/ctcss.cpp/h` | CTCSS tone detection |

### Adding or changing an output option

Three files, always:
1. The struct in `src/rtl_airband.h`
2. The parser in `parse_outputs()` in `src/config.cpp`
3. The per-tick dispatch in `process_outputs()` and the teardown in `disable_channel_outputs()`,
   both in `src/output.cpp`

Output types: `icecast`, `file`, `rawfile`, `udp_stream`, `mixer`, `pulse`.

### Device Modes

Each device operates in one of two modes, set via `mode = "multichannel"` (default) or `mode = "scan"` in config.

**`R_MULTICHANNEL`** — The SDR is tuned to a fixed center frequency and multiple channels are demodulated simultaneously from the same wideband capture. Each channel has a single `freq` value that must fall within the SDR's bandwidth. This is the common case for monitoring several frequencies at once.

**`R_SCAN`** — The device has exactly one channel, but that channel holds a `freqs` list of frequencies to cycle through. A controller thread monitors the squelch: after ~2 seconds of no signal (10 × 200 ms polls), it retunes the SDR hardware to the next frequency via `input_set_centerfreq()`. When a signal is detected, it stays on the current frequency. Per-frequency labels, squelch thresholds, modulation, notch filters, and CTCSS settings are all supported in the `freqs` list. (`rtl_airband.cpp:101-140`, `config.cpp:825`)

### Threading Model

- **RX thread** (1 per device, always) — reads SDR samples into the circular buffer (`input-common.cpp`)
- **Controller thread** (1 per device, `R_SCAN` mode only) — scanning/squelch state machine for devices that scan across frequencies; not created for `R_MULTICHANNEL` devices (`rtl_airband.cpp:1005-1013`)
- **Demod thread** (1 total by default; 1 per device if `multiple_demod_threads=true` in config) — FFT, demodulation, filter, CTCSS, squelch, AGC for all assigned devices (`rtl_airband.cpp:1044`)
- **Output thread** (1 total by default; 1 per device + 1 for mixers if `multiple_output_threads=true`) — MP3 encoding and streaming
- **Mixer thread** (1 total, only if any mixers are configured) — processes all mixers; not per-mixer (`rtl_airband.cpp:1091-1092`)

There is no wall-clock pacing anywhere in the audio production path — the only `SLEEP()` calls in
`rtl_airband.cpp` are connection retry and shutdown delays. Timing derives entirely from the SDR
sample rate driving the RX thread. Outputs with natural backpressure (lame encoding, disk writes,
Icecast) inherit that pacing implicitly; a bare `sendto()` does not. See the open issue below.

### Configuration Format

Config files use libconfig++ syntax. Sample configs in `config/`. Top-level sections:

```
devices: ( { type = "rtlsdr"; centerfreq = 120.0; gain = 25;
             channels: ( { freq = 119.5; modulation = "AM";
                           outputs: ( { type = "icecast"; ... } ); } ); } );
mixers: ( { name = "mix1"; inputs: ( { device=0; channel=0; ampfactor=1.0; } ); outputs: (...); } );
```

libconfig rejects an invalid boolean (`disable = ture;`) at parse time, but a misspelled or
misplaced *option name* is silently ignored. When a config change appears to have no effect, verify
the option name against `parse_outputs()` before assuming a code bug.

### Unit Tests

Tests use Google Test (fetched via CMake FetchContent). Test files in `src/test_*.cpp` cover filters, squelch, CTCSS, helper functions, and signal generation. `src/test_base_class.h` provides test utilities.

## Resolved Issue: UDP Stream "Pacing" Was a Byte-Length Bug

Previously tracked here as an open pacing mystery: a downstream `ffmpeg -f null` read of the
`udp_stream` output reported ~4x realtime throughput, and every Liquidsoap consumption strategy
tried — `input.ffmpeg` directly on the UDP socket, the same wrapped in `buffer()`, with and
without `self_sync`, and MP3 transcoded into `input.harbor` — produced choppy audio or a latency
snowball ending in a broken Icecast pipe.

**Root cause found without needing the tcpdump diagnostic**: item 3 under "Local changes carried
on top of upstream" above. The send side was emitting exactly 4x the correct byte count per
buffer (confirmed by code inspection, not packet capture), which fully accounts for the "~4x
realtime throughput" reading and the downstream jitter/latency symptoms — there was no pacing
gap to diagnose. Fixed in `src/output.cpp`, `src/rtl_airband.cpp`, and `src/udp_stream.cpp`.

If choppy audio or throughput anomalies reappear after this fix, re-open the tcpdump-based
inter-packet-timing diagnostic (`tcpdump -i eth0 -n -ttt udp port 9001`, not `-i any` — that
didn't capture this traffic on the deployment host) before assuming a new pacing bug and writing
send-side pacing code; confirm with a wire capture first.

## Deployment Context

- Builds happen on the `rtlsdr-airband` host (`10.0.50.31`), which also runs roughly a dozen
  concurrent `rtl_airband` instances — one per SDR, each with its own `~/rtl_<centerfreq>.conf`,
  invoked as `rtl_airband -FeQ -c <config>`.
- `sudo make install` places the binary at `/usr/local/bin/rtl_airband`, which is where the running
  units expect it. All instances pick up a new binary on their next restart; restart them one at a
  time and confirm each comes back clean.
- `rtl_airband -v` prints a git hash rather than a semantic version on this fork. That is expected —
  there are no release tags here.
- Audio consumers live on the `dsp` host (`10.0.50.30`) — Liquidsoap 2.4.0 (installed via opam)
  feeding Broadcastify. Same VLAN as the SDR host, direct L2, no firewall in the path.
- Per-instance channel configs live outside this repo and are not committed. Paste them into a
  session when they are relevant rather than adding them here.
- Once an instance's config sets `control_socket_path` (`dynamic_reload`, item 27), its systemd
  unit must set `User=`/`Group=` to a non-root service account — the control socket's
  `SO_PEERCRED` check rejects any client whose UID doesn't match the daemon's own, so a
  root-run daemon can only be controlled by root-run tooling.
- **That non-root service account also needs USB device permissions it doesn't get for free.**
  Hit this in production 2026-08-06: the `rtl_042` instance (RTL-SDR Blog V4, `serial = "17"`)
  was switched to `User=`/`Group=` to enable `control_socket_path` and immediately started
  failing every restart with `RTLSDR device with serial number 17 not found`, despite the
  device being present and working fine via `rtl_test` and previously via a root-run daemon.
  Root cause: every prior instance on this host had always run as root (via `sudo`), which
  bypasses USB device permissions entirely; the new non-root account was never added to
  `plugdev`, the group `librtlsdr2`'s own udev rule (`/lib/udev/rules.d/60-librtlsdr2.rules`)
  grants device access to. Fixed with `sudo usermod -aG plugdev <service-account>` +
  restarting the unit — no code change, no udev rule change, one-time per host (group
  membership, not per-service). Symptom is confusingly generic ("not found", not "permission
  denied") because `rtlsdr_find_device_by_serial()` (`src/input-rtlsdr.cpp`) silently discards
  the return code of `rtlsdr_get_device_usb_strings()` — a real robustness gap in that
  function, pre-existing (not introduced by `dynamic_reload`), not yet fixed; a future fix
  should log/surface a permission failure there distinctly from a genuine no-match. Documented
  in `init.d/rtl_airband.service` so this isn't rediscovered the hard way on the next
  control-socket rollout.
