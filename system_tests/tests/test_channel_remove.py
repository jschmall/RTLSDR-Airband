"""
test_channel_remove.py — live channel removal (dynamic_reload) end-to-end tests.

Counterpart to test_channel_add.py: confirms rtl_airband can pick up a channel being REMOVED
from a device's config, live, via reload_diff - tearing down that channel's output (closing its
connections and freeing its LAME encoder, live_reconfig.cpp's channel_teardown_for_removal())
while an already-running sibling channel is unaffected.

Only a pure TAIL removal (deleting from the end of the config's channels list) is supported.
compute_and_apply_diff()'s decrease branch validates the surviving prefix by frequency before
touching anything - see DeviceConfigSnapshot::channel_freq_hz's comment (live_reconfig.h) for why:
position-based removal has no way to tell "the operator deleted the last channel" apart from "the
operator deleted a different channel, coincidentally leaving the same final count" without that
check, and guessing wrong would tear down and free the WRONG channel's live audio feed. Unlike
test_channel_add.py, no reserve_channels headroom is needed here - shrinking dev->channel_count
never touches the (fixed-size, never-reallocated) channels/bins/base_bins arrays.

Uses the same Popen-based interactive runner and fixed speedup_factor=1.0 as test_channel_add.py,
for the same reason: a reliable wall-clock window to rewrite the config file and call reload_diff
partway through the IQ replay.
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

SAMPLE_RATE = iq_generator.SAMPLE_RATE
CENTERFREQ_HZ = 120_000_000
CHANNEL_A_OFFSET_HZ = +25_000
CHANNEL_B_OFFSET_HZ = -25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
SPEEDUP_FACTOR = 1.0  # see module docstring - fixed, not the shared fixture
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30
REMOVE_AT_S = 4.0  # how far into the run reload_diff is sent


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        # Non-NFM only - this exercises the removal mechanism itself, not modulation-specific
        # demodulation already covered elsewhere.
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


@pytest.fixture
def short_socket_dir():
    """A short-path temp directory for just the control socket file (AF_UNIX length cap)."""
    d = tempfile.mkdtemp(prefix="rtla_")
    try:
        yield Path(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_removed_channel_stops_capturing_after_reload_diff(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """Deleting the last (tail) channel from the config and calling reload_diff tears down its
    output live - the sibling channel keeps running the whole time, unaffected."""
    iq_file = iq_generator.get_or_generate_multichannel(
        offset_a_hz=CHANNEL_A_OFFSET_HZ,
        offset_b_hz=CHANNEL_B_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_a_hz = CENTERFREQ_HZ + CHANNEL_A_OFFSET_HZ
    freq_b_hz = CENTERFREQ_HZ + CHANNEL_B_OFFSET_HZ
    template_a = "ch_remove_a"
    template_b = "ch_remove_b"

    def write(channels: list[dict]) -> None:
        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=channels,
            output_dir=test_output_dir,
            speedup_factor=SPEEDUP_FACTOR,
            mode="multichannel",
            mp3_tmp_dir=test_output_dir,
            control_socket_path=socket_path,
        )

    # Both channels declared at startup.
    write(
        [
            {"freq_hz": freq_a_hz, "output_filename_template": template_a},
            {"freq_hz": freq_b_hz, "output_filename_template": template_b},
        ]
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        time.sleep(REMOVE_AT_S)

        # Delete channel B (the tail entry) from the config, then ask the running instance to
        # pick it up.
        write([{"freq_hz": freq_a_hz, "output_filename_template": template_a}])
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp
        assert not resp["skipped_requires_restart"], resp
        assert any("removed 1 channel" in entry for entry in resp["applied"]), resp

        time.sleep(TOTAL_IQ_DURATION_S)

    # Channel A ran the whole time, untouched by the removal.
    output_validator.validate_mp3_range(
        test_output_dir, template_a, min_duration_s=8.0, max_duration_s=DURATION_S + 1.0
    )
    # Channel B only captured audio up until the removal landed partway through the tone -
    # generously bounded, same rationale as test_control_socket.py's disable/enable gap tests.
    output_validator.validate_mp3_range(
        test_output_dir, template_b, min_duration_s=1.0, max_duration_s=6.0
    )


def test_non_tail_removal_is_rejected_without_disrupting_existing_channels(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """Deleting a channel from the start of the config (rather than the tail) is reported, not
    applied - both channels keep running, undisturbed, since reload_diff can't safely guess which
    one the operator actually meant to remove from a count change alone."""
    iq_file = iq_generator.get_or_generate_multichannel(
        offset_a_hz=CHANNEL_A_OFFSET_HZ,
        offset_b_hz=CHANNEL_B_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_a_hz = CENTERFREQ_HZ + CHANNEL_A_OFFSET_HZ
    freq_b_hz = CENTERFREQ_HZ + CHANNEL_B_OFFSET_HZ
    template_a = "ch_remove_reject_a"
    template_b = "ch_remove_reject_b"

    def write(channels: list[dict]) -> None:
        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=channels,
            output_dir=test_output_dir,
            speedup_factor=SPEEDUP_FACTOR,
            mode="multichannel",
            mp3_tmp_dir=test_output_dir,
            control_socket_path=socket_path,
        )

    write(
        [
            {"freq_hz": freq_a_hz, "output_filename_template": template_a},
            {"freq_hz": freq_b_hz, "output_filename_template": template_b},
        ]
    )

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        time.sleep(REMOVE_AT_S)

        # Delete channel A (the FIRST entry) instead of the tail - channel B's definition is now
        # alone at index 0, so a naive count-only removal would incorrectly tear down channel B
        # (currently the last live channel) instead of channel A, which is what was actually
        # deleted from the file.
        write([{"freq_hz": freq_b_hz, "output_filename_template": template_b}])
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp
        assert not resp["applied"], resp
        assert any(
            "pure tail removal" in entry for entry in resp["skipped_requires_restart"]
        ), resp

        # The instance must still be fully functional after the rejected removal.
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp

        time.sleep(TOTAL_IQ_DURATION_S)

    # Both channels must have kept running the whole time, undisturbed by the rejected removal.
    output_validator.validate_mp3_range(
        test_output_dir, template_a, min_duration_s=8.0, max_duration_s=DURATION_S + 1.0
    )
    output_validator.validate_mp3_range(
        test_output_dir, template_b, min_duration_s=8.0, max_duration_s=DURATION_S + 1.0
    )
