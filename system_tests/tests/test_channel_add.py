"""
test_channel_add.py — dynamic channel add end-to-end tests.

Confirms rtl_airband can pick up a brand-new channel appended to a device's
config, live, via reload_diff - without a restart and without disturbing an
already-running sibling channel on the same device. This is the counterpart
to test_control_socket.py's enable/disable/retune coverage: those toggle a
channel that was already declared (optionally `enabled = false`) at startup;
these confirm a channel *not present at all* at startup can be added, which
requires the device to have been started with spare `reserve_channels`
capacity (see rtl_airband.h's device_t::channel_capacity comment and
config.cpp's parse_devices()).

Like test_control_socket.py, these use the Popen-based interactive runner
(helpers/interactive_runner.py) to send a command mid-stream, and a fixed
speedup_factor=1.0 rather than the shared fixture, for the same reason: a
reliable wall-clock window to rewrite the config file and call reload_diff
partway through the IQ replay.

Not yet covered here (documented as a follow-up, not attempted): appending a
channel whose output is a live network sink (icecast/rdio_scanner) rather
than a file output - the file-output case already exercises the full
parse_channel()/parse_outputs() path new channels go through, and a second
output-type-specific test would mostly duplicate test_icecast_output.py's
existing fixture-server machinery without adding new coverage of the append
mechanism itself.
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
CHANNEL_A_OFFSET_HZ = +25_000  # already declared at startup
CHANNEL_B_OFFSET_HZ = -25_000  # appended live, mid-run
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
SPEEDUP_FACTOR = 1.0  # see module docstring - fixed, not the shared fixture
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30
APPEND_AT_S = 2.0  # how far into the run reload_diff is sent


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        # Non-NFM only - this exercises the channel-add mechanism itself, not
        # modulation-specific demodulation already covered elsewhere.
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


def test_appended_channel_produces_audio_after_reload_diff(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """A channel absent at startup, appended via config edit + reload_diff, captures audio -
    and the already-running sibling channel is unaffected by the append."""
    # Both tones are present in the IQ stream from the start; only channel B's *declaration* is
    # added mid-run - this is what actually exercises "add a channel", as opposed to a channel
    # that was silently there all along.
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
    template_a = "ch_add_a"
    template_b = "ch_add_b"

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
            reserve_channels=1,
        )

    # Only channel A declared at startup - one reserved slot is left for channel B.
    write([{"freq_hz": freq_a_hz, "output_filename_template": template_a}])

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        time.sleep(APPEND_AT_S)

        # Now declare channel B too, and ask the running instance to pick it up.
        write(
            [
                {"freq_hz": freq_a_hz, "output_filename_template": template_a},
                {"freq_hz": freq_b_hz, "output_filename_template": template_b},
            ]
        )
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp
        assert not resp["skipped_requires_restart"], resp
        assert any("added 1 channel" in entry for entry in resp["applied"]), resp

        time.sleep(TOTAL_IQ_DURATION_S)

    # Channel A ran the whole time - full duration, same tolerance style as test_multichannel.py.
    output_validator.validate_mp3_range(
        test_output_dir, template_a, min_duration_s=8.0, max_duration_s=DURATION_S + 1.0
    )
    # Channel B only started capturing after the append landed partway through the tone -
    # generously bounded, same rationale as test_control_socket.py's disable/enable gap tests.
    output_validator.validate_mp3_range(
        test_output_dir, template_b, min_duration_s=3.0, max_duration_s=9.0
    )


def test_append_beyond_reserved_capacity_is_rejected_without_disrupting_existing_channel(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    """Appending more channels than reserve_channels allows is reported, not applied, and
    doesn't crash the instance or disturb the channel that's already running."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_A_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )
    config_path = test_output_dir / "rtl_airband.conf"
    socket_path = short_socket_dir / "control.sock"
    freq_a_hz = CENTERFREQ_HZ + CHANNEL_A_OFFSET_HZ
    freq_b_hz = CENTERFREQ_HZ + CHANNEL_B_OFFSET_HZ
    template_a = "ch_add_no_capacity"

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
            reserve_channels=0,  # no headroom at all
        )

    write([{"freq_hz": freq_a_hz, "output_filename_template": template_a}])

    with run_rtl_airband_interactive(
        binary_under_test.path, config_path, shutdown_timeout_s=TIMEOUT_S
    ):
        wait_for_socket(socket_path)
        time.sleep(APPEND_AT_S)

        write(
            [
                {"freq_hz": freq_a_hz, "output_filename_template": template_a},
                {
                    "freq_hz": freq_b_hz,
                    "output_filename_template": "ch_add_no_capacity_b",
                },
            ]
        )
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp
        assert not resp["applied"], resp
        assert any(
            "reserve_channels" in entry for entry in resp["skipped_requires_restart"]
        ), resp

        # The instance must still be fully functional after the rejected append.
        resp = send_command(socket_path, {"cmd": "reload_diff"})
        assert resp["ok"] is True, resp

        time.sleep(TOTAL_IQ_DURATION_S)

    output_validator.assert_mp3_present(test_output_dir, template_a)
