"""
fake_rdio_scanner_server.py — minimal fake rdio-scanner /api/call-upload server.

Implements just enough of rdio-scanner's HTTP call-upload endpoint
(https://github.com/chuot/rdio-scanner) to let system tests verify
rtl_airband's native rdio_scanner upload path end-to-end: accepts a
multipart/form-data POST, parses the fields libcurl sends (see
rdio_scanner_build_fields() in src/rdio_scanner.cpp), and records each
request for the test to assert against.

Not a multipart parsing library - a small manual parser tailored to what
libcurl's mime API produces, to avoid depending on the deprecated `cgi`
module.
"""

from __future__ import annotations

import re
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


def _parse_multipart(body: bytes, boundary: bytes) -> dict[str, bytes]:
    fields: dict[str, bytes] = {}
    delimiter = b"--" + boundary
    for part in body.split(delimiter):
        part = part.strip(b"\r\n")
        if not part or part == b"--":
            continue
        header_blob, sep, content = part.partition(b"\r\n\r\n")
        if not sep:
            continue
        name_match = re.search(rb'name="([^"]+)"', header_blob)
        if not name_match:
            continue
        name = name_match.group(1).decode("utf-8")
        if content.endswith(b"\r\n"):
            content = content[:-2]
        fields[name] = content
    return fields


class FakeRdioScannerServer:
    """Context manager wrapping a background HTTP server for /api/call-upload."""

    def __init__(self) -> None:
        self._received: list[dict[str, bytes]] = []
        lock = self._lock = threading.Lock()
        received = self._received

        class _Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802 - stdlib method name
                if self.path != "/api/call-upload":
                    self.send_response(404)
                    self.end_headers()
                    return

                content_type = self.headers.get("Content-Type", "")
                boundary_match = re.search(r"boundary=([^;]+)", content_type)
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)

                if boundary_match:
                    boundary = boundary_match.group(1).strip('"').encode("utf-8")
                    fields = _parse_multipart(body, boundary)
                    with lock:
                        received.append(fields)

                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"ok")

            # pylint: disable-next=redefined-builtin
            def log_message(self, format: str, *args) -> None:
                pass  # silence default request logging to stderr; base class's param is named "format"

        self._httpd = HTTPServer(("127.0.0.1", 0), _Handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self._httpd.server_address[1]

    @property
    def received_requests(self) -> list[dict[str, bytes]]:
        with self._lock:
            return list(self._received)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._httpd.shutdown()
        self._thread.join(timeout=5)
        self._httpd.server_close()

    def __enter__(self) -> "FakeRdioScannerServer":
        self.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.stop()
