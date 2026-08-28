#!/usr/bin/env python3
"""Local web dashboard for razerpad (Razer Laptop Cooling Pad controller).

Serves a single-page UI on http://127.0.0.1:8737 that shows live CPU
temperature / fan RPM from the daemon's status file and edits the daemon's
config file (fan curve, interval, lighting). Stdlib only.
"""

import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8737
STATUS_PATH = "/tmp/razerpad-status.json"
CONFIG_PATH = os.path.expanduser("~/.config/razerpad/config")
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

STATIC_ROUTES = {
    "/": ("dashboard.html", "text/html; charset=utf-8"),
    "/index.html": ("dashboard.html", "text/html; charset=utf-8"),
    "/settings": ("settings.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}

LED_EFFECTS = {"off", "static", "spectrum", "wave", "wave2", "breath", "unmanaged"}


def read_status():
    try:
        with open(STATUS_PATH) as f:
            st = json.load(f)
        st["stale"] = (time.time() - st.get("ts", 0)) > 20
        return st
    except (OSError, ValueError):
        return {"stale": True, "connected": False, "temp": -1, "rpm": 0}


def read_config():
    cfg = {}
    try:
        with open(CONFIG_PATH) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k] = v
    except OSError:
        pass
    return cfg


def write_config(cfg):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w") as f:
        for k, v in cfg.items():
            f.write(f"{k}={v}\n")
    os.replace(tmp, CONFIG_PATH)


def validate(body):
    """Validate incoming JSON config; returns (config_dict, error)."""
    cfg = read_config()

    if "interval" in body:
        iv = float(body["interval"])
        if not 0.5 <= iv <= 60:
            return None, "interval must be 0.5-60 seconds"
        cfg["interval"] = f"{iv:g}"

    if "curve" in body:
        pts = body["curve"]
        if isinstance(pts, str):
            pts = [p.split(":") for p in pts.split(",")]
        pts = [(float(t), float(p)) for t, p in pts]
        if len(pts) < 2 or len(pts) > 16:
            return None, "curve needs 2-16 points"
        pts.sort()
        if any(not (0 <= t <= 110) or not (0 <= p <= 100) for t, p in pts):
            return None, "curve points out of range (temp 0-110, percent 0-100)"
        cfg["curve"] = ",".join(f"{t:g}:{p:g}" for t, p in pts)

    if "led" in body:
        if body["led"] not in LED_EFFECTS:
            return None, f"led must be one of {sorted(LED_EFFECTS)}"
        cfg["led"] = body["led"]

    if "led_color" in body:
        color = body["led_color"].lstrip("#").lower()
        if not re.fullmatch(r"[0-9a-f]{6}", color):
            return None, "led_color must be RRGGBB hex"
        cfg["led_color"] = color

    if "brightness" in body:
        b = int(body["brightness"])
        if not 0 <= b <= 100:
            return None, "brightness must be 0-100"
        cfg["brightness"] = str(b)

    return cfg, None


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in STATIC_ROUTES:
            fname, ctype = STATIC_ROUTES[self.path]
            try:
                with open(os.path.join(BASE_DIR, fname), "rb") as f:
                    data = f.read()
            except OSError:
                self.send_error(500, f"{fname} missing")
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path == "/api/status":
            self._json({"status": read_status(), "config": read_config()})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/api/config":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length))
            cfg, err = validate(body)
        except (ValueError, TypeError, KeyError) as e:
            cfg, err = None, f"bad request: {e}"
        if err:
            self._json({"ok": False, "error": err}, 400)
            return
        write_config(cfg)
        self._json({"ok": True, "config": cfg})

    def log_message(self, *args):
        pass  # keep the launchd log quiet


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"razerpad dashboard: http://127.0.0.1:{PORT}")
    server.serve_forever()
