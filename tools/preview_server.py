#!/usr/bin/env python3
"""Serve main/web/index.html with mocked /api/* responses.

Flashing the board just to look at a CSS tweak is slow. This runs the real
dashboard file locally and answers the REST calls with plausible fake data, so
the whole UI can be exercised in a browser in a couple of seconds.

    python tools/preview_server.py            # http://127.0.0.1:8765
    python tools/preview_server.py 9000

This is a development helper only. Nothing here ships to the device.
"""
import json
import math
import os
import random
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX_PATH = os.path.join(PROJECT_DIR, "main", "web", "index.html")

BOOT_TIME = time.time()

WIFI_NETWORKS = [
    {"ssid": "Yao Yao Ling Xian !", "rssi": -38, "channel": 1,
     "security": "WPA2-PSK", "bssid": "24:0A:C4:11:22:33"},
    {"ssid": "TP-LINK_5F2A", "rssi": -57, "channel": 6,
     "security": "WPA/WPA2-PSK", "bssid": "A0:2B:B8:5F:2A:10"},
    {"ssid": "CMCC-Home-9d3", "rssi": -71, "channel": 11,
     "security": "WPA2-PSK", "bssid": "8C:53:C3:9D:3A:04"},
    {"ssid": "<hidden>", "rssi": -83, "channel": 3,
     "security": "WPA3-PSK", "bssid": "DC:A6:32:00:1B:77"},
]

BLE_DEVICES = [
    {"name": "AirPods Pro", "address": "5C:8D:2B:41:0A:9E", "rssi": -52,
     "seen": 34, "address_type": "RANDOM"},
    {"name": "Mi Band 7", "address": "C8:47:8C:19:7F:03", "rssi": -63,
     "seen": 21, "address_type": "PUBLIC"},
    {"name": "Unknown", "address": "7A:11:04:CC:8B:2D", "rssi": -78,
     "seen": 6, "address_type": "RANDOM"},
    {"name": "ESP32-DASH", "address": "3C:71:BF:44:88:01", "rssi": -31,
     "seen": 58, "address_type": "PUBLIC"},
]

SCAN_START = {"at": 0.0, "duration": 8000}


def status_payload():
    uptime = int(time.time() - BOOT_TIME) + 137
    return {
        "device": {
            "chip": "esp32", "cores": 2, "revision": 1,
            "flash_bytes": 2 * 1024 * 1024,
            "free_heap": 148 * 1024 + random.randint(-4096, 4096),
            "min_free_heap": 131 * 1024,
            "uptime_seconds": uptime,
            "idf_version": "v5.5.5",
            "reset_reason": "POWER-ON",
            "mac_sta": "3C:71:BF:44:88:00",
            "mac_ap": "3C:71:BF:44:88:01",
            "mac_bt": "3C:71:BF:44:88:02",
        },
        "ap": {"ssid": "Yao Yao Ling Xian !", "clients": 1,
               "max_clients": 4, "channel": 1},
        "sta": {"connected": False, "configured": False, "ssid": "",
                "ip": "0.0.0.0", "gateway": "0.0.0.0", "rssi": 0},
        "led": {"gpio": 2, "mode": 2, "brightness": 70, "period_ms": 1000,
                "active_high": True},
        "ble": {"advertising": False, "adv_name": "ESP32-DASH",
                "host_ready": True, "unique_devices": 0, "adv_reports": 0},
        "total_networks": len(WIFI_NETWORKS),
        "scan_id": 3,
        "networks": WIFI_NETWORKS,
        "log_lines": 42,
    }


def ble_payload():
    elapsed = int((time.time() - SCAN_START["at"]) * 1000) if SCAN_START["at"] else 0
    running = SCAN_START["at"] and elapsed < SCAN_START["duration"]
    state = "running" if running else ("done" if SCAN_START["at"] else "idle")
    devices = BLE_DEVICES if not running else BLE_DEVICES[:2]
    return {
        "ble_count": len(devices),
        "ble_scan_id": 1 if SCAN_START["at"] else 0,
        "advertising": False,
        "adv_name": "ESP32-DASH",
        "scan_state": state,
        "ble_devices": devices,
        "diag": {
            "host_ready": True, "controller_ok": True, "controller_status": 2,
            "last_sync_rc": 0, "last_scan_rc": 0, "last_adv_rc": 0,
            "host_resets": 0,
            "adv_reports": 118 if SCAN_START["at"] else 0,
            "adv_reports_scan": 118 if SCAN_START["at"] else 0,
            "scan_runs": 1 if SCAN_START["at"] else 0,
            "scan_timeouts": 0, "adv_starts": 0,
            "scan_duration_ms": SCAN_START["duration"],
            "scan_elapsed_ms": min(elapsed, SCAN_START["duration"]),
            "own_address": "3C:71:BF:44:88:02",
            "own_address_type": "PUBLIC",
        },
    }


LOGS = """    1204  I (1204) device_manager: Booting device dashboard on v5.5.5
    1211  I (1211) led_control: Restored LED config: GPIO2 mode=2 brightness=70%
    1213  I (1213) led_control: LED driver ready on GPIO2
    1220  I (1220) gpio_panel: Pin toolbox ready (ADC1 calibrated=1)
    1290  I (1290) ble_scanner: BLE identity address 3C:71:BF:44:88:02 (type PUBLIC)
    1291  I (1291) ble_scanner: NimBLE observer/broadcaster ready
    1502  I (1502) device_manager: Wi-Fi power save disabled for BLE coexistence (ESP_OK)
    1503  I (1503) device_manager: Wi-Fi channel 1
    3108  I (3108) device_manager: Found 4 network(s); saved 4.
    3110  I (3110) device_manager: ========================================
    3111  I (3111) device_manager: Wi-Fi:    Yao Yao Ling Xian !
    3112  I (3112) device_manager: Open:     http://192.168.4.1
    3113  I (3113) device_manager: ========================================
    8420  I (8420) ble_scanner: BLE scan started (8000 ms, active scan, 50% duty)
   16435  I (16435) ble_scanner: BLE scan finished: 118 raw report(s), 4 unique device(s), rc=0
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, payload, content_type="application/json; charset=utf-8"):
        body = payload if isinstance(payload, bytes) else payload.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _route(self, method):
        path = self.path.split("?")[0]

        if path in ("/", "/index.html"):
            with open(INDEX_PATH, "rb") as handle:
                return self._send(handle.read(), "text/html; charset=utf-8")
        if path == "/favicon.ico":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path in ("/api/status", "/api/scan"):
            return self._send(json.dumps(status_payload()))
        if path == "/api/ble/status":
            return self._send(json.dumps(ble_payload()))
        if path == "/api/ble/scan/start":
            SCAN_START["at"] = time.time()
            SCAN_START["duration"] = 8000
            return self._send(json.dumps({"ok": True}))
        if path == "/api/ble/scan/stop":
            SCAN_START["at"] = 0.0
            return self._send(json.dumps({"ok": True}))
        if path == "/api/logs":
            return self._send(json.dumps(
                {"ok": True, "total_lines": 42, "text": LOGS}))
        if path == "/api/adc":
            volts = 1.65 + 1.55 * math.sin(time.time() * 1.7)
            return self._send(json.dumps({
                "ok": True, "channel": 0, "gpio": 36,
                "raw": int(volts / 3.3 * 4095),
                "millivolts": int(volts * 1000), "volts": round(volts, 4),
            }))
        if path.startswith("/api/"):
            return self._send(json.dumps({"ok": True}))

        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        self._route("POST")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"dashboard preview  ->  http://127.0.0.1:{port}/")
    print("Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
