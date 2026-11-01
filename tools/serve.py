#!/usr/bin/env python3
"""Serve web/ locally, telling the browser to always check for newer files."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


print("http://localhost:8765")
ThreadingHTTPServer(("localhost", 8765), partial(Handler, directory=WEB)).serve_forever()
