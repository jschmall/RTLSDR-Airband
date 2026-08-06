"""
interactive_runner.py — run rtl_airband as a live background process for
tests that need to interact with it mid-run (control socket commands),
unlike conftest.run_rtl_airband() which blocks until the process exits on
its own after the IQ fixture is exhausted.

Sends SIGTERM on context exit and waits for the same clean-shutdown path
every other system test relies on (join all threads, tear down outputs,
exit 0) - asserts a clean exit the same way run_rtl_airband() does.
"""

from __future__ import annotations

import signal
import subprocess
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def run_rtl_airband_interactive(
    binary: Path, config_path: Path, shutdown_timeout_s: float = 30.0
):
    """Start rtl_airband in the background; SIGTERM + wait for clean exit on context exit."""
    cmd = [str(binary), "-F", "-e", "-c", str(config_path)]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        yield proc
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            stdout, _ = proc.communicate(timeout=shutdown_timeout_s)
        except subprocess.TimeoutExpired as exc:
            proc.kill()
            stdout, _ = proc.communicate(timeout=5.0)
            raise AssertionError(
                f"rtl_airband did not exit within {shutdown_timeout_s}s of SIGTERM.\n"
                f"Command: {' '.join(cmd)}\nOutput so far:\n{stdout}"
            ) from exc
        assert proc.returncode == 0, (
            f"rtl_airband exited with code {proc.returncode} after SIGTERM.\n"
            f"Command: {' '.join(cmd)}\nOutput:\n{stdout}"
        )
