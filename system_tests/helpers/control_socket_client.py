"""
control_socket_client.py — minimal JSON-line client for the dynamic_reload
control socket (src/control_socket.cpp).

Unlike the fake server fixtures elsewhere in helpers/, there's no fake
counterpart here — this talks to the real control socket implementation in
the binary under test.
"""

from __future__ import annotations

import json
import socket
import time
from pathlib import Path


def send_command(socket_path: Path, command: dict, timeout_s: float = 5.0) -> dict:
    """Connect, send one JSON command line, read one JSON response line, disconnect."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout_s)
    try:
        sock.connect(str(socket_path))
        sock.sendall((json.dumps(command) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        assert data, f"no response received for command {command!r}"
        return json.loads(data.split(b"\n", 1)[0].decode())
    finally:
        sock.close()


def wait_for_socket(socket_path: Path, timeout_s: float = 10.0) -> None:
    """Poll until the control socket file appears, or raise TimeoutError."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if socket_path.exists():
            return
        time.sleep(0.05)
    raise TimeoutError(f"control socket never appeared at {socket_path}")
