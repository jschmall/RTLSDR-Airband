"""
test_fm_demod.py — the fm_demod config option through the real NFM binary.

A carrier modulated only by a CTCSS tone must not come out with harmonics of
that tone: 136.5 Hz would put one on the 410.8 Hz paging tone. Measured on the
udp_stream output, which is uncompressed.
"""

from __future__ import annotations

import socket
import threading
from pathlib import Path

import numpy as np
import pytest
from conftest import run_rtl_airband
from helpers import config_writer, iq_generator

CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
CTCSS_HZ = 136.5
DEVIATION_HZ = 350
DURATION_S = 10.0
WAVE_RATE = 16_000
TIMEOUT_S = (DURATION_S + 2 * iq_generator.NOISE_PAD_S) * 3 + 30


def _run(
    binary: Path, out_dir: Path, iq_file: Path, speedup: float, fm_demod: str | None
) -> np.ndarray:
    """Run rtl_airband on iq_file and return the udp_stream samples."""
    received: list[bytes] = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.2)
        done = threading.Event()

        def receive() -> None:
            while True:
                try:
                    received.append(sock.recv(65536))
                except socket.timeout:
                    if done.is_set():
                        return

        thread = threading.Thread(target=receive)
        thread.start()
        try:
            config_path = out_dir / f"{fm_demod}.conf"
            config_writer.write_config(
                config_path=config_path,
                iq_filepath=iq_file,
                sample_rate=iq_generator.SAMPLE_RATE,
                centerfreq_hz=CENTERFREQ_HZ,
                channels=[
                    {
                        "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                        "modulation": "nfm",
                        "bandwidth": 5000,
                        "extra_output_blocks": [
                            "{\n"
                            'type = "udp_stream";\n'
                            'dest_address = "127.0.0.1";\n'
                            f"dest_port = {sock.getsockname()[1]};\n"
                            "}"
                        ],
                    }
                ],
                output_dir=out_dir,
                speedup_factor=speedup,
            )
            if fm_demod is not None:
                config = config_path.read_text()
                config_path.write_text(f'fm_demod = "{fm_demod}";\n' + config)
            run_rtl_airband(binary, config_path, timeout_s=TIMEOUT_S)
        finally:
            done.set()
            thread.join()
    return np.frombuffer(b"".join(received), dtype="<f4")


def _harmonics_db(samples: np.ndarray) -> tuple[float, float, float]:
    """Fundamental amplitude, and the 2nd and 3rd harmonic relative to it in dB."""
    loud = np.flatnonzero(np.abs(samples) > 1e-4)
    assert loud.size, "no audio received"
    length = 4 * WAVE_RATE  # whole CTCSS cycles, so the tone falls on one bin
    middle = int((loud[0] + loud[-1]) // 2)
    window = np.hanning(length + 1)[:-1]
    segment = samples[middle - length // 2 : middle + length // 2] * window
    spectrum = np.abs(np.fft.rfft(segment)) * 2 / window.sum()
    level = [spectrum[round(CTCSS_HZ * k * 4)] for k in (1, 2, 3)]
    ratio = [20 * np.log10(v / level[0]) for v in level[1:]]
    return level[0], ratio[0], ratio[1]


def test_exact_atan2_removes_ctcss_harmonics(
    nfm_binary, test_output_dir: Path, cache_dir: Path, speedup_factor: float
) -> None:
    """The default demodulator adds a third harmonic; fm_demod = "atan2" must not."""
    if nfm_binary is None:
        pytest.skip("No NFM binary provided via --nfm-binary")
    iq_file = iq_generator.get_or_generate_nfm(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=CTCSS_HZ,
        duration_s=DURATION_S,
        cache_dir=cache_dir,
        deviation_hz=DEVIATION_HZ,
    )
    results = {}
    for name in (None, "fast_atan2", "atan2", "quadri"):
        samples = _run(nfm_binary, test_output_dir, iq_file, speedup_factor, name)
        results[name] = _harmonics_db(samples)
        print(name, results[name])

    assert results[None][2] > -45, results
    assert results["fast_atan2"][2] > -45, results
    assert results["atan2"][1] < -55, results
    assert results["atan2"][2] < -55, results
    expected = 2 * DEVIATION_HZ / WAVE_RATE
    assert results["atan2"][0] == pytest.approx(expected, rel=0.05)


@pytest.mark.parametrize("value", ['"atan"', '""', "1", "true"])
def test_invalid_fm_demod_is_rejected(
    nfm_binary, test_output_dir: Path, value: str
) -> None:
    """Anything but the known names is a configuration error."""
    if nfm_binary is None:
        pytest.skip("No NFM binary provided via --nfm-binary")
    config_path = test_output_dir / "rtl_airband.conf"
    config_path.write_text(f"fm_demod = {value};\n")
    result = run_rtl_airband(nfm_binary, config_path, timeout_s=30, check=False)
    assert result.returncode != 0, result.stderr
    assert "fm_demod must be" in result.stderr, result.stderr
