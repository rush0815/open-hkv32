#!/usr/bin/env python3
"""Serves src/net/web/index.html with example data - develop the web UI without a device.

Usage:  python3 tools/web_preview/mock_server.py [port] [mode]
        mode: connected (default) | ap   (setup mode)
Then open http://127.0.0.1:<port>/  (default port 8080)

All values are made-up examples (no real device, network or person).
"""
import json
import os
import random
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HTML = os.path.join(ROOT, "src", "net", "web", "index.html")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
MODE = sys.argv[2] if len(sys.argv) > 2 else "connected"

STATUS = {
    "id": "12345678", "manufacturer": "XXX", "fw": "0.3.0", "uptime_s": 93784,
    "measurements": 412, "measurements_total": 2817,
    "local_time": "2026-11-26T20:31:05", "dst": False, "period_start": "2026-01-01",
    "units": 123.4, "units_current": 123, "units_lifetime": 241.9,
    "due_date": "2025-12-31", "units_due_date": 118.7,
    "radiator_c": 48.6, "room_c": 21.4, "pressure_hpa": 1013.2, "humidity_pct": None,
    "delta_k": 27.2, "rate_units_h": 0.45, "state": "counting", "room_fallback": False,
    "battery_valid": True, "battery_mv": 3950, "battery_pct": 78, "charging": False,
    "errors": 0, "error_history": 0,
    "wifi": {"mode": "connected", "ssid": "MeinWLAN", "ap_ssid": "HKV-12345678",
             "ip": "192.168.178.42", "rssi": -58, "channel": 6},
    "rssi": -58, "mqtt": "connected",
    "power": {"wifi_mode": 0, "wake_left_s": None, "cpu_mhz": 80},
    "xfer": {"schedule": "15", "last_s": 200, "next_s": 700},
    "mac": "AA:BB:CC:DD:EE:FF", "offset": None,
    "time_gap": {"start": "2026-11-20T14:02", "end": "2026-11-20T14:20", "units": 0.3},
    "monthly": [{"month": "2026-%02d" % m, "units": u} for m, u in
                [(10, 96), (9, 71), (8, 55), (7, 52), (6, 49), (5, 46), (4, 38), (3, 27), (2, 16), (1, 8)]],
    "telegram": "2A4418637856341201087A2A000000046D250E583B036ED20400426C3F3C436E2E16000259721402656608",
}
CONFIG = {
    "meterId": 12345678, "mfct": "XXX", "ver": 1, "scale": 0, "kc": 1, "kq": 1.18, "exp": 1.3, "dtn": 50,
    "dts": 4.5, "tmin": 25, "tsum": 40, "sumS": 6, "sumE": 8, "offR": 0, "offA": 0, "dueD": 31, "dueM": 12,
    "emis": 0.95, "rcpl": 0.43, "rtau": 10, "mint": 120, "tint": 240, "utco": 60, "dst": 1, "batlow": 3400, "dto": 15, "sec": 0,
    "key_set": False, "wifi": 1, "ssid": "MeinWLAN", "pass_set": True, "host": "hkv-12345678",
    "ntp": "pool.ntp.org", "mqtt": 1, "mhost": "192.168.178.10", "mport": 1883, "muser": "hkv",
    "mpass_set": True, "mtopic": "hkv/12345678", "mdisc": 1, "admin_set": False, "xfer": "15",
    "wmode": 0, "wps": 1, "wake": 10, "cpu": 80, "lsleep": 1, "dinv": 1, "dflip": 0,
}
NETS = {"networks": [{"ssid": "MeinWLAN", "rssi": -48, "secure": True},
                     {"ssid": "Nachbar", "rssi": -71, "secure": True},
                     {"ssid": "Gast", "rssi": -80, "secure": False}]}

if MODE == "ap":
    STATUS.update(local_time=None, period_start=None, errors=8, mqtt="disabled")
    STATUS["wifi"] = {"mode": "ap", "ssid": "", "ap_ssid": "HKV-12345678"}
    CONFIG.update(ssid="", pass_set=False, mqtt=0)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/config"):
            with open(HTML, "rb") as f:
                self.send(200, f.read(), "text/html; charset=utf-8")
        elif path == "/api/status":
            self.send(200, json.dumps(STATUS))
        elif path == "/api/config":
            self.send(200, json.dumps(CONFIG))
        elif path == "/api/scan":
            self.send(200, json.dumps(NETS))
        elif path == "/api/live":
            self.send(200, json.dumps({"valid": True, "radiator_c": round(48.6 + random.uniform(-0.3, 0.3), 1),
                                       "room_c": 21.4, "delta_k": 27.2, "pressure_hpa": 1013.2,
                                       "humidity_pct": None, "age_ms": 5, "interval_ms": 2000}))
        elif path == "/api/telegram":
            self.send(200, STATUS["telegram"], "text/plain")
        else:
            self.send(404, "{}")

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send(200, json.dumps({"ok": True, "message": "Gespeichert (Vorschau)"}))


if __name__ == "__main__":
    print(f"Web-UI-Vorschau ({MODE}): http://127.0.0.1:{PORT}/")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
