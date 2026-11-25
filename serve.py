#!/usr/bin/env python3
"""Serves the dashboard and lets it call the C backend.

Run from the folder that holds index.html, crash_studio and the CSV:
    python3 serve.py          # http://127.0.0.1:8000
    python3 serve.py 9000     # custom port
Set CRASH_BIN to use a differently named binary.
"""
import csv
import json
import os
import re
import subprocess
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
try:
    PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
except ValueError:
    print("Port must be a whole number, e.g. python3 serve.py 8000", file=sys.stderr)
    sys.exit(1)

LOCK = threading.Lock()  # the backend writes files in ROOT, so run one job at a time
SAFE_TEXT_RE = re.compile(r"^[A-Za-z0-9\s\-\.\(\)/&,']{0,64}$")
ALLOWED_REPORTS = {
    "issues": "issues_log.txt",
    "trend": "trend_report.txt",
    "sector": "sector_report.txt",
    "cause": "cause_report.txt",
    "spotlight": "spotlight_report.txt",
    "top": "top_summary.txt",
}


def find_binary():
    for name in (os.environ.get("CRASH_BIN"), "crash_studio", "crash_studio.exe"):
        if name:
            path = os.path.join(ROOT, name)
            if os.path.isfile(path):
                return path
    return None


def int_param(qs, key, lo, hi, default_val=None):
    raw = qs.get(key, [None])[0]
    if raw is None or raw == "":
        if default_val is not None:
            return default_val
        raise ValueError("%s is required" % key)
    try:
        value = int(raw)
    except ValueError:
        raise ValueError("%s must be a whole number" % key)
    if not lo <= value <= hi:
        raise ValueError("%s must be between %d and %d" % (key, lo, hi))
    return value


def text_param(qs, key):
    raw = qs.get(key, [""])[0].strip()
    if len(raw) > 64 or not SAFE_TEXT_RE.match(raw):
        raise ValueError("%s contains unsupported characters or exceeds 64 chars" % key)
    return raw


def active_csv_path():
    for fname in ("filtered_crashes.csv", "cleaned_crashes.csv"):
        full = os.path.join(ROOT, fname)
        if os.path.isfile(full):
            return full, fname
    return None, None


def read_paginated_rows(offset=0, limit=25, query=""):
    csv_path, source_name = active_csv_path()
    if not csv_path:
        return [], 0, None
    q = query.strip().lower()
    matched = []
    with open(csv_path, "r", encoding="utf-8", errors="replace", newline="") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            if q:
                haystack = " ".join(str(v or "") for v in row.values()).lower()
                if q not in haystack:
                    continue
            matched.append(row)
    total = len(matched)
    return matched[offset:offset + limit], total, source_name


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def reply(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)

        if url.path == "/api/status":
            binary = find_binary()
            _, active_src = active_csv_path()
            return self.reply(200, {
                "ok": True,
                "port": PORT,
                "binary_ready": bool(binary),
                "dashboard_json_ready": os.path.isfile(os.path.join(ROOT, "dashboard_data.json")),
                "active_csv": active_src,
            })

        if url.path == "/api/rows":
            qs = parse_qs(url.query)
            try:
                offset = int_param(qs, "offset", 0, 1000000, 0)
                limit = int_param(qs, "limit", 1, 100, 25)
                q = text_param(qs, "q")
            except ValueError as err:
                return self.reply(400, {"ok": False, "error": str(err)})
            rows, total, source_name = read_paginated_rows(offset=offset, limit=limit, query=q)
            return self.reply(200, {
                "ok": True,
                "source": source_name,
                "total": total,
                "offset": offset,
                "limit": limit,
                "rows": rows,
            })

        if url.path == "/api/report":
            qs = parse_qs(url.query)
            name = qs.get("name", [""])[0].strip().lower()
            filename = ALLOWED_REPORTS.get(name)
            if not filename:
                return self.reply(400, {"ok": False, "error": "Unknown report name."})
            full = os.path.join(ROOT, filename)
            if not os.path.isfile(full):
                return self.reply(404, {"ok": False, "error": "%s not generated yet." % filename})
            with open(full, "r", encoding="utf-8", errors="replace") as fp:
                content = fp.read()
            return self.reply(200, {"ok": True, "name": name, "filename": filename, "content": content})

        if url.path != "/api/filter":
            return super().do_GET()

        binary = find_binary()
        if not binary:
            return self.reply(500, {"ok": False, "error": "crash_studio binary not found next to serve.py."})

        qs = parse_qs(url.query)
        is_reset = "reset" in qs
        try:
            if is_reset:
                cmd, stdin = [binary], "\n8\n0\n"
            else:
                start = int_param(qs, "start", 1900, 2100, 1908)
                end = int_param(qs, "end", 1900, 2100, 2019)
                minimum = int_param(qs, "min", 0, 1000000, 0)
                if start > end:
                    raise ValueError("start year must not be after end year")

                sector = qs.get("sector", ["all"])[0].strip().lower()
                if sector not in ("all", "military", "civil"):
                    raise ValueError("sector must be all, military, or civil")

                op = text_param(qs, "operator")
                ac = text_param(qs, "aircraft")
                loc = text_param(qs, "location")

                cmd = [binary, "--filter", str(start), str(end), str(minimum), sector, op, ac, loc]
                stdin = None
        except ValueError as err:
            return self.reply(400, {"ok": False, "error": str(err)})

        with LOCK:
            try:
                run = subprocess.run(cmd, cwd=ROOT, input=stdin, capture_output=True, text=True, timeout=60)
            except subprocess.TimeoutExpired:
                return self.reply(500, {"ok": False, "error": "The C backend took too long."})

            if run.returncode == 0 and is_reset:
                stale_filtered = os.path.join(ROOT, "filtered_crashes.csv")
                if os.path.isfile(stale_filtered):
                    try:
                        os.remove(stale_filtered)
                    except OSError:
                        pass

        if run.returncode != 0:
            detail = (run.stderr or run.stdout or "").strip()[-300:]
            return self.reply(500, {"ok": False, "error": detail or "Backend exited with code %d." % run.returncode})
        self.reply(200, {"ok": True, "message": run.stdout.strip().splitlines()[-1] if run.stdout.strip() else "Done."})


if __name__ == "__main__":
    print("Aviation Crash Data Studio: http://127.0.0.1:%d  (Ctrl+C to stop)" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
