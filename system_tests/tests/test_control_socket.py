"""
test_control_socket.py — dynamic_reload control socket end-to-end tests.

Unlike most system tests here, these need to interact with rtl_airband
*while it's running* (send a command mid-stream), not just wait for it to
finish and inspect the output - see helpers/interactive_runner.py for the
Popen-based runner this uses instead of conftest.run_rtl_airband(), and
helpers/control_socket_client.py for the JSON-line client.

These tests deliberately don't use the shared `speedup_factor` fixture:
--mode fast's 10x speedup would compress the whole run to about a second,
leaving no reliable window to send commands mid-stream with the gaps these
tests depend on to prove a real gap in captured audio. speedup_factor=1.0
throughout instead - these are correctness tests for the control socket,
not throughput/timing tests, so the extra wall-clock time is an acceptable
trade.

AF_UNIX socket paths are capped at ~108 bytes on Linux; system_tests'
test_output_dir path is routinely longer than that once nested under a
per-test subdirectory, so the socket itself is placed in its own short
tempfile.mkdtemp() directory rather than under test_output_dir. All other
artifacts (config, MP3 output) still go under test_output_dir as usual.

Not yet covered here (documented as a follow-up, not attempted): a live
centerfreq retune's cross-channel safety - i.e., confirming that retuning
one channel's device doesn't corrupt a sibling channel's bins on the same
device (the highest-risk scenario called out in the design plan for this
feature). That needs tight coordination between a mid-stream retune command
and two simultaneous known tones in the IQ fixture, which risks being
flaky under this test suite's existing timing model; get a reliable timing
approach validated (e.g. against a stats_filepath-driven readiness signal
instead of a fixed sleep) before attempting it.
"""

from __future__ import annotations

import shutil
import tempfile
import time
from pathlib import Path

import pytest
from conftest import CACHE_DIR, BinaryUnderTest
from helpers import config_writer, iq_generator, output_validator
from helpers.control_socket_client import send_command, wait_for_socket
from helpers.interactive_runner import run_rtl_airband_interactive

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
TONE_DURATION_S = 8.0
TOTAL_IQ_DURATION_S = TONE_DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 10 s
SPEEDUP_FACTOR = 1.0  # see module docstring - fixed, not the shared fixture
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30


@pytest.fixture
def short_socket_dir():
    """A short-path temp directory for just the control socket file (AF_UNIX length cap)."""
    d = tempfile.mkdtemp(prefix="rtla_")
    try:
        yield Path(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        # Non-NFM only - these tests exercise the control socket protocol and live-apply
        # primitives, not modulation-specific behavior already covered elsewhere.
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def test_channel_disable_then_enable_leaves_a_gap_in_captured_audio(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """channel_disable/channel_enable actually stop and resume output, live."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=TONE_DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    template = "ch_disable_enable"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": freq_hz, "output_filename_template": template}],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        control_socket_path=socket_path,
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)

        time.sleep(2.0)  # let some audio flow while the channel is still enabled
        resp = send_command(
            socket_path, {"cmd": "channel_disable", "device": 0, "channel": 0}
        )
        assert resp["ok"] is True, resp

        time.sleep(3.0)  # this window's tone must NOT make it into captured audio
        resp = send_command(
            socket_path, {"cmd": "channel_enable", "device": 0, "channel": 0}
        )
        assert resp["ok"] is True, resp

        # Let the rest of the fixture play out and the process exit naturally on EOF.
        time.sleep(TOTAL_IQ_DURATION_S)

    # A ~3s gap out of TONE_DURATION_S=8s of tone should show up as captured audio
    # meaningfully short of the full duration - generous bounds since exact squelch-open
    # timing around the disable/enable window isn't tightly controlled from Python.
    output_validator.validate_mp3_range(
        test_output_dir, template, min_duration_s=1.0, max_duration_s=6.5
    )


def test_channel_stays_disabled_without_enable(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """A channel_disable with no matching channel_enable produces no audio at all."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=TONE_DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    template = "ch_stays_disabled"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": freq_hz, "output_filename_template": template}],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        control_socket_path=socket_path,
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        # Disable well before the tone starts (it begins at NOISE_PAD_S=1.0s into the fixture) -
        # this test is asserting zero captured audio, so the disable has to land before squelch
        # ever has a chance to open, not just "soon".
        resp = send_command(
            socket_path, {"cmd": "channel_disable", "device": 0, "channel": 0}
        )
        assert resp["ok"] is True, resp
        time.sleep(TOTAL_IQ_DURATION_S)

    output_validator.assert_mp3_silent(test_output_dir, template)


def test_mixer_disable_then_enable_leaves_a_gap_in_captured_audio(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """mixer_disable/mixer_enable actually stop and resume the mixer's output, live."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=TONE_DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    mixer_label = "mix_disable_enable"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": freq_hz,
                "mixer_output": {"name": "mix1", "balance": 0.0},
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        mixers=[{"name": "mix1", "label": mixer_label}],
        control_socket_path=socket_path,
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)

        time.sleep(2.0)
        resp = send_command(socket_path, {"cmd": "mixer_disable", "mixer": "mix1"})
        assert resp["ok"] is True, resp

        time.sleep(3.0)
        resp = send_command(socket_path, {"cmd": "mixer_enable", "mixer": "mix1"})
        assert resp["ok"] is True, resp

        time.sleep(TOTAL_IQ_DURATION_S)

    output_validator.validate_mp3_range(
        test_output_dir, mixer_label, min_duration_s=1.0, max_duration_s=6.5
    )


def test_unknown_command_and_bad_index_are_rejected_without_crashing(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """Malformed/invalid commands get a clean error response, not a crash or hang."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=TONE_DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    template = "ch_bad_commands"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": freq_hz, "output_filename_template": template}],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        control_socket_path=socket_path,
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)

        resp = send_command(socket_path, {"cmd": "not_a_real_command"})
        assert resp["ok"] is False
        assert "unknown cmd" in resp["error"]

        resp = send_command(
            socket_path, {"cmd": "channel_enable", "device": 99, "channel": 0}
        )
        assert resp["ok"] is False
        assert "out of range" in resp["error"]

        resp = send_command(socket_path, {"cmd": "set_gain", "device": 0, "gain": 30.0})
        assert resp["ok"] is False
        assert "not supported" in resp["error"]  # file input driver has no gain hook

        # The instance must still be fully functional after rejecting all of the above.
        resp = send_command(
            socket_path, {"cmd": "channel_disable", "device": 0, "channel": 0}
        )
        assert resp["ok"] is True, resp
        resp = send_command(
            socket_path, {"cmd": "channel_enable", "device": 0, "channel": 0}
        )
        assert resp["ok"] is True, resp

        time.sleep(TOTAL_IQ_DURATION_S)

    output_validator.assert_mp3_present(test_output_dir, template)


def test_control_socket_file_permissions(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """The control socket is created with owner-only (0600) permissions."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=1.0,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": freq_hz, "output_filename_template": "ch_perms"}],
        output_dir=test_output_dir,
        speedup_factor=0.05,  # slow enough that the process is still up when we check
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        control_socket_path=socket_path,
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        mode = socket_path.stat().st_mode & 0o777
        assert mode == 0o600, f"expected control socket mode 0600, got {oct(mode)}"

    # Shutdown (interactive_runner's SIGTERM) must remove the socket file, not leak it.
    assert not socket_path.exists()
