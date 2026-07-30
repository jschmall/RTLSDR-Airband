"""
fake_icecast_server.py — minimal fake Icecast source server.

libshout's source-client protocol (shout_setup() in src/output.cpp
configures SHOUT_PROTOCOL_HTTP, though the request line observed on the
wire from this libshout build is legacy "SOURCE /mountpoint HTTP/1.0"
rather than "PUT") sends an unauthenticated probe request on the
connection, and - regardless of this server's response to it - always
follows up with a second "SOURCE ..." request carrying an Authorization
header on the *same* kept-alive connection before it ever starts
streaming audio. This server loops absorbing however many such
request/response cycles arrive (responding "200 OK" to each) until it
sees something that isn't a new request line, which it then treats as the
start of the real MP3 body stream. Getting this wrong (responding to only
one request and treating everything after as body) silently captures the
second HTTP request's raw bytes as if they were audio - which is exactly
what an earlier version of this fixture did.

It also doesn't send Content-Length on any response, matching how libshout
streams frames indefinitely once connected - not well-formed HTTP/1.1
framing, which is why this uses a raw socket rather than Python's
http.server (built around bounded/well-framed request bodies). Counts
streamed body bytes until the client disconnects (or stop() is called),
for a system test to assert real MP3 audio was actually streamed
continuously.
"""

from __future__ import annotations

import socket
import threading

_REQUEST_METHODS = (b"SOURCE ", b"PUT ")
_MAX_REQUEST_CYCLES = 4  # defensive bound; 2 is what's been observed in practice


class FakeIcecastServer:
    """Context manager wrapping a background fake Icecast source server."""

    def __init__(self) -> None:
        self._listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen_sock.bind(("127.0.0.1", 0))
        self._listen_sock.listen(1)
        self._listen_sock.settimeout(1.0)

        self.port: int = self._listen_sock.getsockname()[1]
        self.request_line: str = ""  # the last (most authoritative) request seen
        self.headers_received: str = ""
        self.bytes_received: int = 0

        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self._listen_sock.close()

    def __enter__(self) -> "FakeIcecastServer":
        self.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.stop()

    def _read_headers(
        self, conn: socket.socket, initial: bytes
    ) -> tuple[str, str, bytes] | None:
        """Read one request's headers off conn, starting from any already-read bytes.

        Returns (request_line, headers_text, leftover_bytes_after_headers), or
        None on EOF/stop.
        """
        buf = initial
        while b"\r\n\r\n" not in buf:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                if self._stop.is_set():
                    return None
                continue
            if not chunk:
                return None
            buf += chunk
        header_text, _, leftover = buf.partition(b"\r\n\r\n")
        decoded = header_text.decode("utf-8", errors="replace")
        lines = decoded.split("\r\n")
        request_line = lines[0] if lines else ""
        return request_line, decoded, leftover

    def _serve(self) -> None:
        conn = None
        while not self._stop.is_set() and conn is None:
            try:
                conn, _ = self._listen_sock.accept()
            except socket.timeout:
                continue
        if conn is None:
            return

        conn.settimeout(1.0)
        try:
            pending = b""
            for _ in range(_MAX_REQUEST_CYCLES):
                result = self._read_headers(conn, pending)
                if result is None:
                    return
                request_line, headers, pending = result
                self.request_line = request_line
                self.headers_received = headers

                # The client's request has "Connection: Keep-Alive"; per HTTP/1.0
                # semantics that only takes effect if the server echoes it back
                # too, or libshout treats the response as connection: close and
                # closes its end immediately without ever streaming audio.
                conn.sendall(b"HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\n\r\n")

                if not pending:
                    try:
                        pending = conn.recv(4096)
                    except socket.timeout:
                        pending = b""

                if not pending.startswith(_REQUEST_METHODS):
                    break

            self.bytes_received += len(pending)

            while not self._stop.is_set():
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                self.bytes_received += len(chunk)
        finally:
            conn.close()
