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
    dereferenced `channel->freqlist[channel->freq_idx]` twice (building the filename when
    `include_freq` is set, and unconditionally under `WITH_RDIO_SCANNER` to populate
    `fdata->open_frequency`), but a mixer's own `channel_t` (`mixer_t::channel`) has no
    `freqlist` at all — `parse_mixers()` never sets one, since a mixed stream has no single
    source frequency — leaving it `NULL` and crashing on the mixer's first file rotation. Never
    caught before because no automated test exercised a mixer with a `file`-type output
    (`test_multichannel.py` carried a "TODO: add mixer tests... once mixer support is
    implemented in the system tests" for exactly this gap); item 26's new mixer system tests are
    what first hit it. Fixed by computing the frequency once, guarded by
    `channel->freqlist != NULL`, defaulting to `0` for a mixer's own channel.
26. **`dynamic_reload`: live retune/reconfiguration via a Unix domain control socket**
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
      add (a channel not present at startup at all) is supported — see item 27.
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
      the same device — the highest-risk scenario for item 26's centerfreq-retune design — is a
      documented follow-up, not yet implemented; see `test_control_socket.py`'s module
      docstring.
27. **Dynamic channel add via `reload_diff`** (`src/config.cpp`, `src/live_reconfig.{cpp,h}`,
    `src/rtl_airband.h`, `src/logging.{h,cpp}`) — closes item 26's channel-add gap. The config
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
      "declare capacity up front" idea item 26 already uses for `enabled = false`. Rejected at
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
28. **Mixer-input live-append data race fix** (`src/mixer.cpp`, `src/rtl_airband.{cpp,h}`,
    `src/config.cpp`) — closes a gap flagged but not fixed while item 27 was built: a dynamically
    appended channel whose config declares a `type = "mixer"` output *did* reach
    `mixer_connect_input()` (`mixer.cpp`) on the live-append path, and that function grew
    `mixer_t::inputs`/`inputs_todo`/`input_mask` via an unconditional `XREALLOC` with a comment
    claiming it was "only run at startup" — no longer true once threads (`mixer_thread()`, the
    output thread's `mixer_put_samples()`/`mixer_disable_input()`/`mixer_enable_input()`) are
    already running and reading that pointer/count with no synchronization of their own. A real
    use-after-free, not theoretical.
    - Mirrors item 27's own fix for `device_t::channels`: a new `reserve_inputs` mixer-level
      config int (default `0`) sizes extra headroom, added on top of however many inputs actually
      connected during startup by a one-time `mixer_finalize_capacity()` (`mixer.cpp`), called
      from `main()` right after `parse_devices()` returns and before any thread is created — the
      same single-threaded window item 27 relies on. `mixer_t::input_count` is now
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
      instead of reallocating — which, via the same `config_error_is_recoverable` mechanism item 27
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
- Once an instance's config sets `control_socket_path` (`dynamic_reload`, item 26), its systemd
  unit must set `User=`/`Group=` to a non-root service account — the control socket's
  `SO_PEERCRED` check rejects any client whose UID doesn't match the daemon's own, so a
  root-run daemon can only be controlled by root-run tooling.
