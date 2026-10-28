#!/usr/bin/env python3
"""
Groove local web server.

Serves the player in ./mobile-app and nothing else.

    python3 server.py              # http://localhost:8080  (this computer only)
    python3 server.py 9000         # different port
    python3 server.py --lan        # also reachable from your phone on the same Wi-Fi

Security notes
  * Only files inside ./mobile-app can be served. Paths are resolved and checked,
    so "../" tricks and symlinks pointing outside are rejected.
  * Hidden files (dotfiles) are never served.
  * By default the server only listens on 127.0.0.1. Use --lan deliberately.
  * HTTP Range requests are supported, so seeking inside large audio files works.
"""

import argparse
import mimetypes
import os
import posixpath
import re
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "mobile-app"))

EXTRA_TYPES = {
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".webmanifest": "application/manifest+json",
    ".svg": "image/svg+xml",
    ".flac": "audio/flac",
    ".m4a": "audio/mp4",
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
    ".aif": "audio/aiff",
    ".aiff": "audio/aiff",
    ".ogg": "audio/ogg",
    ".opus": "audio/ogg",
}

CSP = (
    "default-src 'self'; "
    "script-src 'self'; "
    "style-src 'self'; "
    "img-src 'self' blob: data:; "
    "media-src 'self' blob:; "
    "worker-src 'self' blob:; "
    "connect-src 'self'; "
    "object-src 'none'; "
    "base-uri 'none'; "
    "form-action 'none'; "
    "frame-ancestors 'none'"
)

RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")
CHUNK = 256 * 1024


def resolve_path(url_path):
    """Map a URL path to a real file inside ROOT, or return None."""
    path = unquote(urlsplit(url_path).path)
    if "\x00" in path:
        return None
    path = posixpath.normpath("/" + path.lstrip("/"))
    parts = [p for p in path.split("/") if p]
    if any(p.startswith(".") for p in parts):
        return None  # no dotfiles, no traversal segments
    candidate = os.path.realpath(os.path.join(ROOT, *parts)) if parts else ROOT
    if os.path.commonpath([candidate, ROOT]) != ROOT:
        return None
    if os.path.isdir(candidate):
        candidate = os.path.join(candidate, "index.html")
    if not os.path.isfile(candidate):
        return None
    return candidate


def content_type(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in EXTRA_TYPES:
        return EXTRA_TYPES[ext]
    guessed, _ = mimetypes.guess_type(path)
    return guessed or "application/octet-stream"


class Handler(BaseHTTPRequestHandler):
    server_version = "Groove"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.serve(send_body=True)

    def do_HEAD(self):
        self.serve(send_body=False)

    def _reject(self, *_args):
        self.send_error(HTTPStatus.METHOD_NOT_ALLOWED)

    do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _reject

    def common_headers(self):
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")

    def serve(self, send_body):
        path = resolve_path(self.path)
        if path is None:
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        size = os.path.getsize(path)
        start, end = 0, size - 1
        status = HTTPStatus.OK

        range_header = self.headers.get("Range")
        if range_header and size > 0:
            m = RANGE_RE.match(range_header.strip())
            if not m or (m.group(1) == "" and m.group(2) == ""):
                return self.range_not_satisfiable(size)
            if m.group(1) == "":  # suffix range: last N bytes
                length = int(m.group(2))
                if length == 0:
                    return self.range_not_satisfiable(size)
                start = max(0, size - length)
            else:
                start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
            if start >= size or start > end:
                return self.range_not_satisfiable(size)
            status = HTTPStatus.PARTIAL_CONTENT

        length = end - start + 1 if size > 0 else 0
        self.send_response(status)
        self.send_header("Content-Type", content_type(path))
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.common_headers()
        self.end_headers()

        if not send_body or length == 0:
            return
        try:
            with open(path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(CHUNK, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass  # browser cancelled the request (normal when seeking)

    def range_not_satisfiable(self, size):
        self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
        self.send_header("Content-Range", f"bytes */{size}")
        self.send_header("Content-Length", "0")
        self.common_headers()
        self.end_headers()

    def send_error(self, code, message=None, explain=None):
        # Minimal error page without echoing the request path back.
        body = f"{int(code)} {HTTPStatus(code).phrase}\n".encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.common_headers()
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s  %s\n" % (self.log_date_time_string(), fmt % args))


def main():
    parser = argparse.ArgumentParser(description="Serve the Groove web player.")
    parser.add_argument("port", nargs="?", type=int, default=8080)
    parser.add_argument("--lan", action="store_true",
                        help="listen on all network interfaces (reachable from other devices on your Wi-Fi)")
    args = parser.parse_args()

    host = "0.0.0.0" if args.lan else "127.0.0.1"
    httpd = ThreadingHTTPServer((host, args.port), Handler)
    httpd.daemon_threads = True
    print(f"\n  Groove is running at http://localhost:{args.port}/")
    if args.lan:
        print("  --lan: other devices on your network can open it too (http://<this-computer's-IP>:%d/)" % args.port)
    print("  Press Ctrl+C to stop.\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n  Stopped.")
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
