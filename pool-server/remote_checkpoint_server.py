#!/usr/bin/env python3
"""Simple HTTPS-ready checkpoint append server.

This script exposes an HTTP endpoint that accepts POST requests containing
JSON payloads with the following shape::

    {"filename": "CHECKPOINTS...TXT", "line": "..."}

The provided line is appended (with a trailing newline) to a file matching the
``filename`` in the local ``storage`` directory. The directory is created
automatically if it does not already exist.

The handler performs a minimal validation step by collapsing the filename to a
basename to avoid directory traversal and ensures the file lives underneath the
``storage`` folder.

Run with::

    python3 tools/remote_checkpoint_server.py --host 0.0.0.0 --port 8443 --cert cert.pem --key key.pem

If ``--cert``/``--key`` are omitted the server falls back to plain HTTP. For a
quick test you can generate a self-signed certificate using::

    openssl req -x509 -nodes -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365

"""

from __future__ import annotations

import argparse
import json
import os
import ssl
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Optional

STORAGE_DIR = Path("storage")


class AppendHandler(BaseHTTPRequestHandler):
    server_version = "CheckpointAppend/1.0"

    def do_POST(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler naming)
        self._raw_body = ""
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            self.send_error(HTTPStatus.LENGTH_REQUIRED, "Missing Content-Length")
            return

        try:
            length = int(length_header)
        except ValueError:
            self.send_error(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
            return

        payload = self.rfile.read(length)
        self._raw_body = payload.decode("utf-8", "replace")
        try:
            data = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_error(HTTPStatus.BAD_REQUEST, "Body must be JSON")
            return

        filename = data.get("filename")
        line = data.get("line")
        if not isinstance(filename, str) or not filename:
            self.send_error(HTTPStatus.BAD_REQUEST, "Missing filename field")
            return
        if not isinstance(line, str):
            self.send_error(HTTPStatus.BAD_REQUEST, "Missing line field")
            return

        safe_name = os.path.basename(filename)
        target_path = STORAGE_DIR / safe_name
        try:
            STORAGE_DIR.mkdir(parents=True, exist_ok=True)
            with target_path.open("a", encoding="utf-8") as fh:
                fh.write(line)
                fh.write("\n")
        except OSError as exc:  # pragma: no cover - filesystem failure path
            self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR, str(exc))
            return

        self.send_response(HTTPStatus.NO_CONTENT)
        self.end_headers()

    def log_message(self, format: str, *args: object) -> None:  # noqa: A003 - keep default signature
        # Reduce noise by emitting to stderr using the default implementation
        super().log_message(format, *args)
		
    def log_request(self, code: object = "-", size: object = "-") -> None:
        # Append the received body to the standard access log line
        body = getattr(self, "_raw_body", "")
        body_one_line = " ".join(body.split())
        self.log_message('"%s" %s %s %s', self.requestline, str(code), str(size), body_one_line)


def build_server(host: str, port: int, cert: Optional[str], key: Optional[str]) -> HTTPServer:
    httpd = HTTPServer((host, port), AppendHandler)
    if cert and key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=cert, keyfile=key)
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    elif cert or key:
        raise ValueError("Both certificate and key are required for TLS")
    return httpd


def main() -> None:
    parser = argparse.ArgumentParser(description="Checkpoint append server")
    parser.add_argument("--host", default="127.0.0.1", help="Address to bind (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Port to bind (default: 8080)")
    parser.add_argument("--cert", help="Path to TLS certificate (PEM)")
    parser.add_argument("--key", help="Path to TLS private key (PEM)")
    args = parser.parse_args()

    server = build_server(args.host, args.port, args.cert, args.key)
    protocol = "https" if args.cert and args.key else "http"
    print(f"Serving {protocol.upper()} on {args.host}:{args.port}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
