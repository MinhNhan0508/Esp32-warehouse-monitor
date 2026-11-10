"""Generate public Firebase configuration/Rules, or serve the local cloud dashboard."""

import argparse
from collections import deque
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import re
import socket
import threading
import urllib.error
import urllib.request
import webbrowser
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web/warehouse"
DEVICE_ID = "warehouse-01"


def identifiers(api_key, database_url, device_uid, viewer_uid):
    if not re.fullmatch(r"[A-Za-z0-9_-]{10,128}", api_key):
        raise ValueError("Use the Firebase Web API key, not an ID token or service-account key")
    parsed = urlsplit(database_url.rstrip("/"))
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.port
        or parsed.path
        or parsed.query
        or parsed.fragment
        or not parsed.hostname.endswith((".firebaseio.com", ".firebasedatabase.app"))
    ):
        raise ValueError("Use the HTTPS Realtime Database origin without path/query")
    for uid in (device_uid, viewer_uid):
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", uid):
            raise ValueError("Copy each UID from Firebase Authentication Users")
    if device_uid == viewer_uid:
        raise ValueError("Device writer and dashboard viewer must be different users")
    return dict(
        apiKey=api_key,
        databaseUrl=database_url.rstrip("/"),
        deviceUid=device_uid,
        viewerUid=viewer_uid,
        deviceId=DEVICE_ID,
    )


def record_rules(device_uid):
    required = [
        "schema_version",
        "boot_id",
        "sequence",
        "uptime_ms",
        "captured_at_ms",
        "received_at_ms",
        "sensor_valid",
        "sample_age_ms",
        "door_open",
        "door_open_ms",
        "alarm_mask",
        "muted",
        "sensor_errors",
        "storage_state",
        "storage_samples",
        "storage_skipped",
        "storage_queue_dropped",
        "test_sensor_fault",
    ]
    rule = {
        ".write": f"auth != null && auth.uid == '{device_uid}' && newData.exists()",
        ".validate": "newData.hasChildren(" + json.dumps(required) + ") && "
        "newData.child('captured_at_ms').val() >= now - 60000 && "
        "newData.child('captured_at_ms').val() <= now + 30000 && "
        "newData.child('received_at_ms').val() >= now - 60000 && "
        "newData.child('received_at_ms').val() <= now + 1000 && "
        "(newData.child('sensor_valid').val() == true ? "
        "newData.hasChildren(['temperature_c','humidity_rh_pct']) && "
        "newData.child('sample_age_ms').val() < 6000 : "
        "!newData.child('temperature_c').exists() && !newData.child('humidity_rh_pct').exists())",
        "$other": {".validate": False},
    }
    for field, maximum in {
        "sequence": 4294967295,
        "uptime_ms": 4294967295,
        "sample_age_ms": 4294967295,
        "door_open_ms": 4294967295,
        "alarm_mask": 15,
        "sensor_errors": 4294967295,
        "storage_state": 7,
        "storage_samples": 4294967295,
        "storage_skipped": 4294967295,
        "storage_queue_dropped": 4294967295,
    }.items():
        rule[field] = {
            ".validate": f"newData.isNumber() && newData.val() >= 0 && newData.val() <= {maximum} && newData.val() % 1 == 0"
        }
    for field in ("sensor_valid", "door_open", "muted", "test_sensor_fault"):
        rule[field] = {".validate": "newData.isBoolean()"}
    for field in ("captured_at_ms", "received_at_ms"):
        rule[field] = {".validate": "newData.isNumber()"}
    rule["schema_version"] = {".validate": "newData.isNumber() && newData.val() == 1"}
    rule["boot_id"] = {".validate": "newData.isString() && newData.val().matches(/^[0-9a-f]{8}$/)"}
    rule["temperature_c"] = {
        ".validate": "newData.isNumber() && newData.val() >= -40 && newData.val() <= 80"
    }
    rule["humidity_rh_pct"] = {
        ".validate": "newData.isNumber() && newData.val() >= 0 && newData.val() <= 100"
    }
    return rule


def database_rules(device_uid, viewer_uid):
    return {
        "rules": {
            ".read": False,
            ".write": False,
            "devices": {
                DEVICE_ID: {
                    ".read": f"auth != null && auth.uid == '{viewer_uid}'",
                    "latest": record_rules(device_uid),
                    "history": {
                        "$slot": {
                            **record_rules(device_uid),
                            ".write": f"auth != null && auth.uid == '{device_uid}' && newData.exists() && $slot.matches(/^(0|[1-5]?[0-9])$/)",
                        }
                    },
                }
            },
        }
    }


def configure(config, output=ROOT):
    output = Path(output)
    header = output / "targets/esp32-wokwi/warehouse-local/warehouse_cloud_config.h"
    rules = output / "firebase/database.rules.json"
    web = output / "web/warehouse/config.json"
    for path in (header, rules, web):
        path.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(
        "#ifndef WAREHOUSE_CLOUD_CONFIG_H\n#define WAREHOUSE_CLOUD_CONFIG_H\n"
        "// Public identifiers only; credentials are entered at runtime.\n"
        "#ifndef WAREHOUSE_CLOUD_ENABLED\n#define WAREHOUSE_CLOUD_ENABLED 1\n#endif\n"
        + "\n".join(
            f"static const char {name}[] = {json.dumps(config[key])};"
            for name, key in (
                ("WM_FIREBASE_API_KEY", "apiKey"),
                ("WM_FIREBASE_DATABASE_URL", "databaseUrl"),
                ("WM_DEVICE_UID", "deviceUid"),
                ("WM_DEVICE_ID", "deviceId"),
            )
        )
        + "\n#endif\n",
        encoding="utf-8",
    )
    rules.write_text(
        json.dumps(database_rules(config["deviceUid"], config["viewerUid"]), indent=2) + "\n",
        encoding="utf-8",
    )
    web.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return header, rules, web


class DashboardHandler(SimpleHTTPRequestHandler):
    def diagnostic_request(self):
        port = self.server.server_port
        allowed = (f"127.0.0.1:{port}", f"localhost:{port}")
        return self.headers.get("Host") in allowed and self.headers.get("Origin") in (
            None,
            *(f"http://{host}" for host in allowed),
        )

    def do_GET(self):
        if self.path != "/diagnostics":
            return super().do_GET()
        if not self.diagnostic_request():
            self.send_error(403)
            return
        with self.server.diagnostics_lock:
            records = list(self.server.diagnostics)
        body = json.dumps({"records": records}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        # Bounded local diagnostic metadata only. Never accept credentials,
        # tokens, request URLs, arbitrary strings or complete Firebase responses.
        if self.path != "/diagnostics":
            self.send_error(404)
            return
        if not self.diagnostic_request():
            self.send_error(403)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 1024 or self.headers.get("Content-Type") != "application/json":
                raise ValueError()
            item = json.loads(self.rfile.read(size))
            integers = {
                "browser_ms",
                "captured_at_ms",
                "received_at_ms",
                "sequence",
                "uptime_ms",
                "sample_age_ms",
                "read_ms",
            }
            required = integers | {"boot_id", "badge"}
            if (
                not isinstance(item, dict)
                or not required <= set(item)
                or set(item) - required - {"source", "telemetry"}
            ):
                raise ValueError()
            if any(
                type(item[key]) is not int or not 0 <= item[key] <= 2**53 - 1 for key in integers
            ):
                raise ValueError()
            if not isinstance(item["boot_id"], str) or not re.fullmatch(
                r"[a-f0-9]{8}", item["boot_id"]
            ):
                raise ValueError()
            if item["badge"] not in ("LIVE", "STALE", "CLOCK MISMATCH", "CONNECTION LOST"):
                raise ValueError()
            if item.get("source", "POLL") not in ("POLL", "STREAM"):
                raise ValueError()
            if "telemetry" in item:
                values = item["telemetry"]
                flags = {"sensor_valid", "door_open", "muted", "test_sensor_fault"}
                counts = {
                    "door_open_ms",
                    "storage_samples",
                    "storage_skipped",
                    "storage_queue_dropped",
                    "sensor_errors",
                }
                fields = (
                    flags
                    | counts
                    | {"alarm_mask", "storage_state", "temperature_c", "humidity_rh_pct"}
                )
                if not isinstance(values, dict) or set(values) != fields:
                    raise ValueError()
                if any(type(values[key]) is not bool for key in flags):
                    raise ValueError()
                if any(
                    type(values[key]) is not int or not 0 <= values[key] <= 4294967295
                    for key in counts
                ):
                    raise ValueError()
                for key, limit in (("alarm_mask", 15), ("storage_state", 7)):
                    if type(values[key]) is not int or not 0 <= values[key] <= limit:
                        raise ValueError()
                for key, low, high in (("temperature_c", -40, 80), ("humidity_rh_pct", 0, 100)):
                    value = values[key]
                    if values["sensor_valid"]:
                        if (
                            type(value) not in (int, float)
                            or not math.isfinite(value)
                            or not low <= value <= high
                        ):
                            raise ValueError()
                    elif value is not None:
                        raise ValueError()
        except (ValueError, TypeError, KeyError):
            self.send_error(400)
            return
        with self.server.diagnostics_lock:
            self.server.diagnostics.append(item)
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' https://identitytoolkit.googleapis.com https://securetoken.googleapis.com https://*.firebaseio.com https://*.firebasedatabase.app; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'",
        )
        super().end_headers()


def server(port=8080):
    http = ThreadingHTTPServer(
        ("127.0.0.1", port), partial(DashboardHandler, directory=str(WEB)), bind_and_activate=False
    )
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            # Windows SO_REUSEADDR can permit a second listener on an occupied
            # port. Own this address exclusively instead of sharing it.
            http.allow_reuse_address = False
            http.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        http.server_bind()
        http.server_activate()
    except OSError:
        http.server_close()
        raise
    http.diagnostics = deque(maxlen=120)
    http.diagnostics_lock = threading.Lock()
    return http


def dashboard_running(port):
    """Only reuse a service serving this dashboard, not any occupied TCP port."""
    expected = (WEB / "index.html").read_bytes()
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=2) as response:
            return response.status == 200 and response.read(len(expected) + 1) == expected
    except (OSError, urllib.error.URLError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("configure")
    for option in ("api-key", "database-url", "device-uid", "viewer-uid"):
        setup.add_argument("--" + option, required=True)
    setup.add_argument("--output-dir", type=Path, default=ROOT)
    serve = commands.add_parser("serve")
    serve.add_argument("--port", type=int, default=8080)
    launch = commands.add_parser(
        "open", help="Start or reuse the dashboard and open its browser page"
    )
    launch.add_argument("--port", type=int, default=8080)
    launch.add_argument(
        "--no-browser", action="store_true", help="Start/reuse without opening a browser"
    )
    args = parser.parse_args()
    if args.command == "configure":
        for path in configure(
            identifiers(args.api_key, args.database_url, args.device_uid, args.viewer_uid),
            args.output_dir,
        ):
            print(path)
        print(
            "No cloud operation was performed. You publish Rules and configure Firebase yourself."
        )
    else:
        opening = args.command == "open"
        url = f"http://127.0.0.1:{args.port}"
        if opening and dashboard_running(args.port):
            print(f"Warehouse dashboard already running: {url}", flush=True)
            if not args.no_browser:
                webbrowser.open(url)
            return
        try:
            http = server(args.port)
        except OSError as error:
            raise SystemExit(
                f"Cannot start dashboard on {url}: {error}. Another service may be using this port."
            ) from error
        print(f"Warehouse dashboard: http://127.0.0.1:{http.server_port}", flush=True)
        url = f"http://127.0.0.1:{http.server_port}"
        print("Keep this task/window running. Press Ctrl+C to stop the dashboard.", flush=True)
        try:
            if opening and not args.no_browser:
                webbrowser.open(url)
            http.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            http.server_close()


if __name__ == "__main__":
    main()
