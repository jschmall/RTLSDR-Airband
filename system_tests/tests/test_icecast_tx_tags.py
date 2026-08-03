"""
test_icecast_tx_tags.py — send_tx_tags: on-air metadata tag per transmission.

Unlike send_scan_freq_tags (R_SCAN only, driven by controller_thread hopping
between scan frequencies), send_tx_tags applies to R_MULTICHANNEL channels and
mixers: it pushes the channel's configured label as the Icecast "song" tag
when squelch opens (a transmission starts) and clears it to an empty string
when squelch closes (the transmission ends). See icecast_tx_tag_step() in
src/helper_functions.cpp and compute_tx_tag_content()/
mixer_select_active_tag_input() in src/output.cpp / src/mixer.cpp.

shout_metadata_delay (the real-time buffering-compensation delay before a tag
change is applied) runs on wall-clock time via gettimeofday(), not on the IQ
file's simulated time - it does not scale with speedup_factor. To keep these
tests independent of that real-time/speedup interaction, shout_metadata_delay
is forced to 0 here (immediate apply). icecast_tx_tag_step()'s deferred-apply
behavior is already covered directly by its own unit tests
(src/test_helper_functions.cpp); this system test's job is to prove the
end-to-end wiring (axcindicate / mixer input -> label -> Icecast
"GET /admin/metadata" request), not the delay arithmetic itself.

Parametrized over all provided binaries (non-NFM and NFM if available).
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator
from helpers.fake_icecast_server import FakeIcecastServer

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
MIXER_CHANNEL_B_OFFSET_HZ = -25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 8.0
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 10 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 60 s

MOUNTPOINT = "test_tx_tags.mp3"
USERNAME = "source"
PASSWORD = "test-password"
LABEL = "Fire Dispatch"


def pytest_generate_tests(metafunc):
    """Parametrize over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def _icecast_block(port: int, mountpoint: str) -> str:
    return "\n".join(
        [
            "        {",
            '          type = "icecast";',
            '          server = "127.0.0.1";',
            f"          port = {port};",
            f'          mountpoint = "{mountpoint}";',
            f'          username = "{USERNAME}";',
            f'          password = "{PASSWORD}";',
            "          send_tx_tags = true;",
            "        }",
        ]
    )


def test_icecast_tx_tags_channel(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    speedup_factor: float,
) -> None:
    """A transmission on a plain R_MULTICHANNEL channel sets then clears the tag."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ

    with FakeIcecastServer() as server:
        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=[
                {
                    "freq_hz": freq_hz,
                    "label": LABEL,
                    "extra_output_blocks": [_icecast_block(server.port, MOUNTPOINT)],
                }
            ],
            output_dir=test_output_dir,
            speedup_factor=speedup_factor,
            mode="multichannel",
            shout_metadata_delay=0,
        )

        run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

        updates = sorted(server.metadata_updates, key=lambda u: u.received_at)

    assert updates, "Expected at least one Icecast metadata update, got none"

    for u in updates:
        assert (
            MOUNTPOINT in u.mount
        ), f"Unexpected mount in metadata update: {u.mount!r}"

    label_updates = [u for u in updates if u.song == LABEL]
    clear_updates = [u for u in updates if u.song == ""]

    assert label_updates, (
        f"Expected a metadata update with song={LABEL!r}, "
        f"got: {[u.song for u in updates]}"
    )
    assert clear_updates, (
        "Expected a metadata update clearing the tag (song=''), "
        f"got: {[u.song for u in updates]}"
    )
    assert updates.index(label_updates[0]) < updates.index(clear_updates[-1]), (
        "Expected the label update to precede the clearing update, "
        f"got order: {[u.song for u in updates]}"
    )
    # only ever these two values - no stray/garbled content
    assert {u.song for u in updates} == {LABEL, ""}


def test_icecast_tx_tags_mixer(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    speedup_factor: float,
) -> None:
    """A mixer's icecast output tags with whichever source channel is talking."""
    # Channel A carries the only signal in this fixture; channel B demodulates
    # a different offset in the same wideband capture and sees only the
    # ambient noise pad, so it never transmits - proving the mixer's tag
    # reflects the *specific* talking input (channel A's label), not a
    # coincidental default or the wrong source index.
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"

    with FakeIcecastServer() as server:
        config_writer.write_config(
            config_path=config_path,
            iq_filepath=iq_file,
            sample_rate=SAMPLE_RATE,
            centerfreq_hz=CENTERFREQ_HZ,
            channels=[
                {
                    "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                    "label": LABEL,
                    "mixer_output": {"name": "mix1", "balance": 0.0},
                },
                {
                    "freq_hz": CENTERFREQ_HZ + MIXER_CHANNEL_B_OFFSET_HZ,
                    "label": "Channel B",
                    # get_or_generate_am()'s tone->noise-pad splice is a direct
                    # concatenation (no fade), so the abrupt discontinuity when
                    # channel A's tone ends briefly radiates broadband energy
                    # across the whole spectrum, including this channel's bin.
                    # A much higher-than-default squelch threshold keeps that
                    # transient from being mistaken for a real transmission on
                    # a channel that otherwise never carries one.
                    "squelch": 30.0,
                    "mixer_output": {"name": "mix1", "balance": 0.0},
                },
            ],
            mixers=[
                {
                    "name": "mix1",
                    "label": "mix1_output",
                    "extra_output_blocks": [_icecast_block(server.port, MOUNTPOINT)],
                }
            ],
            output_dir=test_output_dir,
            speedup_factor=speedup_factor,
            mode="multichannel",
            shout_metadata_delay=0,
        )

        run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

        updates = sorted(server.metadata_updates, key=lambda u: u.received_at)

    assert updates, "Expected at least one Icecast metadata update, got none"

    label_updates = [u for u in updates if u.song == LABEL]
    clear_updates = [u for u in updates if u.song == ""]

    assert label_updates, (
        f"Expected a metadata update tagging the talking input ({LABEL!r}), "
        f"got: {[u.song for u in updates]}"
    )
    assert clear_updates, (
        "Expected a metadata update clearing the tag (song=''), "
        f"got: {[u.song for u in updates]}"
    )
    assert "Channel B" not in {u.song for u in updates}, (
        "Channel B never transmitted in this fixture - it must never appear "
        "as a tag value"
    )
