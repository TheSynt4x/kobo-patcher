#!/usr/bin/env python3
"""Serve web/ locally, telling the browser to always check for newer files.

With --lan it's served to your network too, so a Kobo on the same Wi-Fi can check for updates
from it: put the address it prints in .adds/patches/update-url on the Kobo."""
import socket
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def lan_address():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("192.0.2.1", 9))  # no packet is sent, this only picks the outgoing interface
        return probe.getsockname()[0]


lan = "--lan" in sys.argv
print("http://localhost:8765")
if lan:
    print(f"for a Kobo: http://{lan_address()}:8765/mods/")
ThreadingHTTPServer(("0.0.0.0" if lan else "localhost", 8765), partial(Handler, directory=WEB)).serve_forever()
