"""
test_mixer_file_include_freq.py — a mixer file output with include_freq = true.

A mixer's own channel_t has no freqlist, so output_file_ready() must not
dereference it (CLAUDE.md fork-delta item 25). Upstream #574 restructured that
function and reintroduced the unguarded dereference behind include_freq; this
test covers the re-applied guard. Without it, rtl_airband segfaults the first
time the mixer opens its file.
"""

from pathlib import Path

from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = +25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30
FREQ_TEMPLATE = "mix_freq"


def pytest_generate_tests(metafunc):
    """Parametrize over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_mixer_file_include_freq(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
    speedup_factor: float,
) -> None:
    """Mixer file output with include_freq → no crash, file named without a freq."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=cache_dir,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    freq_block = "\n".join(
        [
            "      {",
            '        type = "file";',
            f'        directory = "{test_output_dir}";',
            f'        filename_template = "{FREQ_TEMPLATE}";',
            "        continuous = false;",
            "        include_freq = true;",
            "      }",
        ]
    )

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "mixer_output": {"name": "mix1", "balance": 0.0},
            },
        ],
        mixers=[
            {
                "name": "mix1",
                "label": "mix1_output",
                "extra_output_blocks": [freq_block],
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode="multichannel",
    )

    run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

    # Duration isn't checked: a mixer's output under --mode fast's speedup doesn't
    # track the IQ duration, and this test only targets the crash and the name.
    matches = sorted(test_output_dir.glob(f"{FREQ_TEMPLATE}_[0-9]*.mp3"))
    assert matches, f"no {FREQ_TEMPLATE} MP3 written in {test_output_dir}"
    mp3 = matches[0]
    assert mp3.stat().st_size > 0, f"{mp3.name} is empty"
    # A mixer has no source frequency, so frequency 0 must be left out of the name
    # rather than appended as "_0" (or a garbage value read from a NULL freqlist).
    assert mp3.stem.count("_") == 3, f"unexpected frequency suffix in {mp3.name}"
