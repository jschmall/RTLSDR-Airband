"""
test_auto_ppm.py — auto_ppm config validation through the real binary.

The closed loop (measure, adjust, re-measure) needs a driver with a live
correction hook, which the file driver used by these tests doesn't have; the
decision logic is unit tested in src/test_auto_ppm.cpp and the loop was
validated on RTL-SDR hardware. What this covers is that the parser refuses
auto_ppm where it can't work, instead of silently ignoring it.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator

SAMPLE_RATE = iq_generator.SAMPLE_RATE
CENTERFREQ_HZ = 120_000_000


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


@pytest.mark.parametrize(
    "extra_lines, expected",
    [
        (["auto_ppm = true;"], "auto_ppm is not supported by the file driver"),
        (["auto_ppm = false;"], "auto_ppm is not supported by the file driver"),
    ],
)
def test_auto_ppm_is_rejected_on_a_driver_without_live_correction(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    extra_lines: list[str],
    expected: str,
) -> None:
    iq_file = test_output_dir / "silence.iq"
    iq_file.write_bytes(bytes([128]) * SAMPLE_RATE * 2)
    config_path = test_output_dir / "rtl_airband.conf"
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": CENTERFREQ_HZ + 25_000}],
        output_dir=test_output_dir,
        extra_device_lines=extra_lines,
    )
    result = run_rtl_airband(
        binary_under_test.path, config_path, timeout_s=30, check=False
    )
    assert result.returncode != 0, result.stderr
    assert expected in result.stderr, result.stderr


def write_rtlsdr_config(config_path: Path, output_dir: Path, device_body: str) -> None:
    config_path.write_text(f"""
devices: (
  {{
    type = "rtlsdr";
    index = 0;
    gain = 25;
    auto_ppm = true;
    {device_body}
  }}
);
""".replace("OUTDIR", str(output_dir)))


FILE_OUTPUT = """outputs: ( { type = "file"; directory = "OUTDIR"; filename_template = "out"; } );"""


@pytest.mark.parametrize(
    "device_body, expected",
    [
        (
            f"""auto_ppm_min_voters = 3; reserve_channels = 1; centerfreq = 120.0;
            channels: ( {{ freq = 120.025; {FILE_OUTPUT} }} );""",
            "auto_ppm_min_voters (3) is more than the device's 2 channel(s)",
        ),
        (
            f"""auto_ppm_min_voters = 2; mode = "scan";
            channels: ( {{ freqs = ( 120.025, 121.5 ); {FILE_OUTPUT} }} );""",
            "auto_ppm_min_voters must be 1 in scan mode",
        ),
        (
            f"""auto_ppm_deadband_hz = 2000; centerfreq = 120.0;
            channels: ( {{ freq = 120.025; {FILE_OUTPUT} }} );""",
            "auto_ppm_deadband_hz must be between 0 and 1000",
        ),
    ],
    ids=[
        "min_voters_over_capacity",
        "min_voters_in_scan_mode",
        "deadband_out_of_range",
    ],
)
def test_auto_ppm_voting_settings_are_validated(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    device_body: str,
    expected: str,
) -> None:
    # Parsing runs before any device is opened, so this needs no dongle.
    config_path = test_output_dir / "rtl_airband.conf"
    write_rtlsdr_config(config_path, test_output_dir, device_body)
    result = run_rtl_airband(
        binary_under_test.path, config_path, timeout_s=30, check=False
    )
    assert result.returncode != 0, result.stderr
    assert expected in result.stderr, result.stderr
