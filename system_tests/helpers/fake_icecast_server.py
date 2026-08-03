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

Metadata updates (SHOUT_SET_METADATA -> shout_set_metadata_utf8() in
src/output.cpp, used by send_scan_freq_tags and send_tx_tags) are NOT sent
inline on the SOURCE connection's body stream - confirmed against the
installed libshout.so.3 (`strings` shows "mode=updinfo&mount=%s&%s" and
"/admin/metadata") - libshout opens a *separate* HTTP connection per
update and sends "GET /admin/metadata?mode=updinfo&mount=...&song=...".
That means this server has to accept more than one connection at once (the
long-lived SOURCE stream plus however many short-lived metadata GETs), so
the single blocking accept()-then-serve loop from the original version of
this fixture is replaced with an accept loop that hands each connection to
its own thread. The first request line on a connection decides how it's
handled; everything else about the SOURCE/PUT path is unchanged.
"""

from __future__ import annotations

import socket
import threading
import time
from dataclasses import dataclass
from urllib.parse import parse_qs, urlparse

_REQUEST_METHODS = (b"SOURCE ", b"PUT ")
_MAX_REQUEST_CYCLES = 4  # defensive bound; 2 is what's been observed in practice

_METADATA_RESPONSE_BODY = (
    b'<?xml version="1.0"?><iceresponse><message>Metadata update successful</message>'
    b"<return>1</return></iceresponse>"
)


@dataclass
class MetadataUpdate:
    """One "GET /admin/metadata" request observed by the fake server."""

    mount: str
    song: str
    received_at: float  # time.monotonic() when the request was fully read


class FakeIcecastServer:
    """Context manager wrapping a background fake Icecast source server."""

    def __init__(self) -> None:
        self._listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen_sock.bind(("127.0.0.1", 0))
        self._listen_sock.listen(8)
        self._listen_sock.settimeout(1.0)

        self.port: int = self._listen_sock.getsockname()[1]
        self.request_line: str = (
            ""  # the last (most authoritative) SOURCE/PUT request seen
        )
        self.headers_received: str = ""
        self.bytes_received: int = 0

        self._metadata_lock = threading.Lock()
        self.metadata_updates: list[MetadataUpdate] = []

        self._stop = threading.Event()
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._conn_threads: list[threading.Thread] = []
        self._conn_threads_lock = threading.Lock()

    def start(self) -> None:
        self._accept_thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._accept_thread.join(timeout=5)
        with self._conn_threads_lock:
            threads = list(self._conn_threads)
        for t in threads:
            t.join(timeout=5)
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

    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self._listen_sock.accept()
            except socket.timeout:
                continue
            conn.settimeout(1.0)
            t = threading.Thread(
                target=self._handle_connection, args=(conn,), daemon=True
            )
            with self._conn_threads_lock:
                self._conn_threads.append(t)
            t.start()

    def _handle_metadata_request(self, conn: socket.socket, request_line: str) -> None:
        # e.g. "GET /admin/metadata?mode=updinfo&mount=%2Ftest.mp3&song=... HTTP/1.0"
        parts = request_line.split(" ")
        path = parts[1] if len(parts) >= 2 else ""
        params = parse_qs(urlparse(path).query)
        mount = params.get("mount", [""])[0]
        song = params.get("song", [""])[0]
        with self._metadata_lock:
            self.metadata_updates.append(
                MetadataUpdate(mount=mount, song=song, received_at=time.monotonic())
            )
        conn.sendall(
            b"HTTP/1.0 200 OK\r\n"
            b"Content-Type: text/xml\r\n"
            b"Content-Length: " + str(len(_METADATA_RESPONSE_BODY)).encode() + b"\r\n"
            b"\r\n" + _METADATA_RESPONSE_BODY
        )

    def _handle_connection(self, conn: socket.socket) -> None:
        try:
            pending = b""
            first_request = True
            for _ in range(_MAX_REQUEST_CYCLES):
                result = self._read_headers(conn, pending)
                if result is None:
                    return
                request_line, headers, pending = result

                if first_request and request_line.startswith("GET /admin/metadata"):
                    self._handle_metadata_request(conn, request_line)
                    return
                first_request = False

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
