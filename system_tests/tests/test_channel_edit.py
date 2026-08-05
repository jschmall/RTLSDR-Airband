"""
test_channel_edit.py — live channel field-edit (dynamic_reload) end-to-end tests.

Covers item 30: editing an already-live channel's definition (freq/modulation/bandwidth/squelch/
notch/ctcss/outputs) and calling reload_diff picks up the change, not just a pure channel-count
change (item 27/29). Mechanically this is a tail-replace: compute_and_apply_diff() (live_reconfig.
cpp) detects the edited channel's raw config no longer matches its live
channel_t::config_signature and tears it down (item 29's channel_teardown_for_removal()) then
re-appends a freshly parsed replacement (item 27's try_append_channels()) at the same index - so
there's a brief interruption around the edit, not a live in-place field update. The sibling
channel, whose definition didn't change, is untouched throughout.

No reserve_channels headroom is needed here (unlike test_channel_add.py) - a same-count edit
never grows dev->channel_count, only tears down and rebuilds within the existing array.
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
EDIT_AT_S = 4.0  # how far into the run reload_diff is sent


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        # Non-NFM only - this exercises the replace mechanism itself, not modulation-specific
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


def test_edited_channel_keeps_capturing_after_reload_diff(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """Editing channel B's squelch threshold (same freq, same output) and calling reload_diff
    tears it down and rebuilds it live - it keeps producing audio before and after the edit (a
    brief gap around the edit itself), and channel A - whose definition never changes - is
    unaffected the whole time."""
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
    template_a = "ch_edit_a"
    template_b = "ch_edit_b"

    def write(squelch_b: float) -> None:
        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=[
                {"freq_hz": freq_a_hz, "output_filename_template": template_a},
                {
                    "freq_hz": freq_b_hz,
                    "output_filename_template": template_b,
                    "squelch": squelch_b,
                },
            ],
            output_dir=test_output_dir,
            speedup_factor=SPEEDUP_FACTOR,
            mode="multichannel",
            mp3_tmp_dir=test_output_dir,
            control_socket_path=socket_path,
        )

    write(squelch_b=3.0)

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        time.sleep(EDIT_AT_S)

        # Channel B's squelch threshold changes - same freq, same output, different field.
        write(squelch_b=6.0)
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp
        assert not resp["skipped_requires_restart"], resp
        assert any("removed 1 channel" in entry for entry in resp["applied"]), resp
        assert any("added 1 channel" in entry for entry in resp["applied"]), resp

        time.sleep(TOTAL_IQ_DURATION_S)

    # Channel A's definition never changed - full duration, unaffected by B's edit.
    output_validator.validate_mp3_range(
        test_output_dir, template_a, min_duration_s=8.0, max_duration_s=DURATION_S + 1.0
    )
    # Channel B captured audio both before and after the edit - a brief gap around the edit
    # itself (teardown + re-append) means less than the full duration, generously bounded, same
    # rationale as test_control_socket.py's disable/enable gap tests.
    output_validator.validate_mp3_range(
        test_output_dir, template_b, min_duration_s=5.0, max_duration_s=DURATION_S + 1.0
    )
