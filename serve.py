#!/usr/bin/env python3
"""Serves the dashboard and lets it call the C backend.

Run from the folder that holds index.html, crash_studio and the CSV:
    python3 serve.py          # http://localhost:8000
    python3 serve.py 9000     # custom port
Set CRASH_BIN to use a differently named binary.
"""
import json
import os
import subprocess
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
LOCK = threading.Lock()  # the backend writes files in ROOT, so run one job at a time


def find_binary():
    for name in (os.environ.get("CRASH_BIN"), "crash_studio", "crash_studio.exe"):
        if name:
            path = os.path.join(ROOT, name)
            if os.path.isfile(path):
                return path
    return None


def int_param(qs, key, lo, hi):
    try:
        value = int(qs.get(key, [""])[0])
    except ValueError:
        raise ValueError("%s must be a whole number" % key)
    if not lo <= value <= hi:
        raise ValueError("%s must be between %d and %d" % (key, lo, hi))
    return value


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def reply(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path != "/api/filter":
            return super().do_GET()

        binary = find_binary()
        if not binary:
            return self.reply(500, {"ok": False, "error": "crash_studio binary not found next to serve.py."})

        qs = parse_qs(url.query)
        try:
            if "reset" in qs:
                # Same as choosing Option 8 in the menu: default CSV, export everything, exit.
                cmd, stdin = [binary], "\n8\n0\n"
            else:
                start = int_param(qs, "start", 1900, 2100)
                end = int_param(qs, "end", 1900, 2100)
                minimum = int_param(qs, "min", 0, 1000000)
                if start > end:
                    raise ValueError("start year must not be after end year")
                cmd, stdin = [binary, "--filter", str(start), str(end), str(minimum)], None
        except ValueError as err:
            return self.reply(400, {"ok": False, "error": str(err)})

        with LOCK:
            try:
                run = subprocess.run(cmd, cwd=ROOT, input=stdin, capture_output=True, text=True, timeout=60)
            except subprocess.TimeoutExpired:
                return self.reply(500, {"ok": False, "error": "The C backend took too long."})

        if run.returncode != 0:
            detail = (run.stderr or run.stdout or "").strip()[-300:]
            return self.reply(500, {"ok": False, "error": detail or "Backend exited with code %d." % run.returncode})
        self.reply(200, {"ok": True, "message": run.stdout.strip().splitlines()[-1] if run.stdout.strip() else "Done."})


if __name__ == "__main__":
    print("Aviation Crash Data Studio: http://localhost:%d  (Ctrl+C to stop)" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
