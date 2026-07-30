"""
test_rdio_scanner_output.py — native rdio_scanner call-upload output.

Fork-specific feature (src/rdio_scanner.cpp, requires -DRDIO_SCANNER=ON):
uploads completed split_on_transmission files to a rdio-scanner instance's
/api/call-upload endpoint. Runs a fake HTTP server standing in for
rdio-scanner and asserts rtl_airband successfully uploads the transmission
with the expected fields once the squelch closes.

split_on_transmission's idle-close detection (close_if_necessary() in
src/output.cpp) is driven by real wall-clock time (gettimeofday), not IQ
sample time - unlike other system tests, this one hardcodes
speedup_factor=1.0 rather than using the shared fixture, so fast mode's
10x IQ speedup can't shrink the trailing noise pad's real-world duration
below the 0.5s idle-close threshold and leave the transmission file open
(and never uploaded) when the process hits EOF and exits.

Parametrized over all provided binaries (non-NFM and NFM if available).
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator
from helpers.fake_rdio_scanner_server import FakeRdioScannerServer

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 66 s

API_KEY = "test-api-key"
TALKGROUP_ID = 12345
SYSTEM_ID = 99
SYSTEM_LABEL = "TestSys"
TALKGROUP_LABEL = "TestTG"
SOURCE_ID = 1


def pytest_generate_tests(metafunc):
    """Parametrize test_rdio_scanner_output over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_rdio_scanner_output(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
) -> None:
    """One transmission → exactly one call-upload POST with the expected fields."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ

    with FakeRdioScannerServer() as server:
        rdio_scanner_block = "\n".join(
            [
                "        {",
                '          type = "file";',
                f'          directory = "{test_output_dir}";',
                '          filename_template = "rdio_test";',
                "          split_on_transmission = true;",
                "          rdio_scanner: {",
                '            server = "127.0.0.1";',
                f"            port = {server.port};",
                f'            api_key = "{API_KEY}";',
                f"            talkgroup_id = {TALKGROUP_ID};",
                f"            system_id = {SYSTEM_ID};",
                f'            system_label = "{SYSTEM_LABEL}";',
                f'            talkgroup_label = "{TALKGROUP_LABEL}";',
                f"            source_id = {SOURCE_ID};",
                "            delete_after_upload = false;",
                "            timeout_ms = 5000;",
                "            max_retries = 0;",
                "          };",
                "        }",
            ]
        )

        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=[
                {
                    "freq_hz": freq_hz,
                    "extra_output_blocks": [rdio_scanner_block],
                }
            ],
            output_dir=test_output_dir,
            # Real-time only - see module docstring.
            speedup_factor=1.0,
            mode="multichannel",
        )

        run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

        requests = server.received_requests

    assert (
        len(requests) == 1
    ), f"Expected exactly 1 call-upload request, got {len(requests)}"
    fields = requests[0]

    assert fields.get("audioType") == b"audio/mpeg"
    assert fields.get("key") == API_KEY.encode()
    assert fields.get("talkgroup") == str(TALKGROUP_ID).encode()
    assert fields.get("system") == str(SYSTEM_ID).encode()
    assert fields.get("systemLabel") == SYSTEM_LABEL.encode()
    assert fields.get("talkgroupLabel") == TALKGROUP_LABEL.encode()
    assert fields.get("source") == str(SOURCE_ID).encode()
    assert fields.get("frequency") == str(freq_hz).encode()

    date_time = fields.get("dateTime")
    assert date_time is not None, "Missing dateTime field"
    assert (
        date_time.isdigit()
    ), f"Expected dateTime to be a plain epoch-seconds integer, got {date_time!r}"
    assert int(date_time) > 0

    audio = fields.get("audio")
    assert audio is not None, "Missing audio field"
    assert len(audio) > 0, "Uploaded audio content is empty"

    local_mp3 = list(test_output_dir.glob("rdio_test_*.mp3"))
    assert local_mp3, "Local split_on_transmission MP3 file not found on disk"
    assert local_mp3[0].stat().st_size > 0
