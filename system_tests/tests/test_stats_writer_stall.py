"""
test_stats_writer_stall.py — a stats file write that blocks must not stall the audio.

In production the stats file was written from the output thread. A nightly VM backup froze the
filesystem for ~12 s, the write blocked, and every instance lost that much audio at once.

Here stats_filepath is a FIFO with no reader for the first STALL_S seconds, so opening it for
writing blocks just as a frozen filesystem does. The channel's MP3 must still hold all of the
transmission.
"""

import os
import subprocess
import threading
import time
from pathlib import Path

import pytest
from conftest import BinaryUnderTest, rtl_airband_cmd
from helpers import config_writer, iq_generator, output_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
STALL_S = 5.0
# The output thread runs on the wall clock relative to the replay, so replay in real time.
SPEEDUP_FACTOR = 1.0
TIMEOUT_S = DURATION_S * 3 + 30


def pytest_generate_tests(metafunc):
    """Run against one binary only; the stats writer is modulation-independent."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def _drain(fifo: Path, stop: threading.Event) -> None:
    """Read every stats write until stop is set."""
    while not stop.is_set():
        with open(fifo, "rb") as f:
            while f.read(65536):
                pass


def test_blocked_stats_write_does_not_stall_audio(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=cache_dir,
    )
    fifo = test_output_dir / "stats.fifo"
    os.mkfifo(fifo)
    os.chmod(fifo, 0o666)
    filename_template = "stats_stall"

    config_path = test_output_dir / "rtl_airband.conf"
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "output_filename_template": filename_template,
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        stats_filepath=fifo,
        stats_update_interval=1,
    )

    stop = threading.Event()
    drainer = threading.Thread(target=_drain, args=(fifo, stop), daemon=True)
    log_path = test_output_dir / "rtl_airband.log"
    with open(log_path, "w", encoding="utf-8") as log_file:
        with subprocess.Popen(
            rtl_airband_cmd(binary_under_test.path, config_path),
            stdout=log_file,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        ) as proc:
            time.sleep(STALL_S)
            drainer.start()
            try:
                proc.wait(timeout=TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                pytest.fail(f"rtl_airband did not exit within {TIMEOUT_S}s")
            finally:
                stop.set()
                # Unblock a drainer waiting in open() for the next writer.
                try:
                    os.close(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK))
                except OSError:
                    pass
                drainer.join(timeout=5)

    log = log_path.read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"rtl_airband exited with {proc.returncode}:\n{log}"
    recorded = output_validator.total_mp3_duration_s(test_output_dir, filename_template)
    assert recorded >= DURATION_S - 1.0, (
        f"expected ~{DURATION_S:.0f} s of audio with the stats write blocked for "
        f"{STALL_S:.0f} s, recorded {recorded:.1f} s"
    )
