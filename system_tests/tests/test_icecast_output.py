"""
test_icecast_output.py — Icecast output streams continuous MP3 audio.

Icecast (Broadcastify delivery in this fork's deployment) is not gated by
squelch - process_outputs() encodes and shout_send()s channel->waveout on
every tick regardless of axcindicate, so the connection carries a
continuous live stream (silence when squelch is closed, demodulated audio
when it's open). Runs a fake Icecast source server and asserts rtl_airband
connects with a source request for the expected mountpoint (observed as
legacy "SOURCE /mountpoint HTTP/1.0" from this libshout build, even with
SHOUT_PROTOCOL_HTTP configured - accept either that or a modern "PUT"),
sends Basic Auth credentials and the configured stream name (icecast->name,
confirming shout_setup()'s metadata plumbing reaches the wire), and streams
a plausible amount of real MP3 data for the run's duration. See
helpers/fake_icecast_server.py for the request/response cycle this needs
to complete before real streaming starts.

Parametrized over all provided binaries (non-NFM and NFM if available).
"""

from pathlib import Path

from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator
from helpers.fake_icecast_server import FakeIcecastServer

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 8.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 10 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 60 s

MOUNTPOINT = "test.mp3"
USERNAME = "source"
PASSWORD = "test-password"
STREAM_NAME = "Test Stream"

# LAME is configured for a constant 16 kbps target (airlame_init() in
# src/output.cpp) - over TOTAL_IQ_DURATION_S that's ~20 KB. Use a much
# lower floor to comfortably absorb encoder framing/VBR overhead variance
# without losing this as a "did real streaming actually happen" check.
MIN_EXPECTED_BYTES = 4000


def pytest_generate_tests(metafunc):
    """Parametrize test_icecast_output over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_icecast_output(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
    speedup_factor: float,
) -> None:
    """A channel with an icecast output connects and streams continuous MP3 audio."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=cache_dir,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ

    with FakeIcecastServer() as server:
        icecast_block = "\n".join(
            [
                "        {",
                '          type = "icecast";',
                '          server = "127.0.0.1";',
                f"          port = {server.port};",
                f'          mountpoint = "{MOUNTPOINT}";',
                f'          username = "{USERNAME}";',
                f'          password = "{PASSWORD}";',
                f'          name = "{STREAM_NAME}";',
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
                    "extra_output_blocks": [icecast_block],
                }
            ],
            output_dir=test_output_dir,
            speedup_factor=speedup_factor,
            mode="multichannel",
        )

        run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

        request_line = server.request_line
        headers = server.headers_received
        bytes_received = server.bytes_received

    assert request_line.startswith(
        ("PUT ", "SOURCE ")
    ), f"Expected an HTTP PUT or legacy SOURCE connection, got request line: {request_line!r}"
    assert (
        f"/{MOUNTPOINT}" in request_line
    ), f"Expected mountpoint '/{MOUNTPOINT}' in request line: {request_line!r}"
    assert (
        f"ice-name: {STREAM_NAME}" in headers
    ), f"Expected 'ice-name: {STREAM_NAME}' (from icecast->name) in request headers:\n{headers}"
    assert (
        "Authorization:" in headers
    ), f"Expected Basic Auth credentials in request headers:\n{headers}"
    assert bytes_received >= MIN_EXPECTED_BYTES, (
        f"Expected at least {MIN_EXPECTED_BYTES} bytes of streamed MP3 data, "
        f"got {bytes_received}"
    )
