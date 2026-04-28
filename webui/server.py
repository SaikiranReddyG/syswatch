#!/usr/bin/env python3
"""Minimal SysWatch Web UI server.

- Receives batched events at POST /events (same schema emitted by syswatch).
- Serves dashboard at GET /
- Exposes JSON APIs:
  - GET /api/health
  - GET /api/latest
  - GET /api/history?event_type=...&limit=...

No third-party dependencies required.
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Deque
from urllib.parse import parse_qs, urlparse

WEBUI_DIR = Path(__file__).resolve().parent
INDEX_HTML = WEBUI_DIR / "index.html"


class EventStore:
    def __init__(self, max_events: int = 5000) -> None:
        self._lock = threading.Lock()
        self._history: Deque[dict[str, Any]] = deque(maxlen=max_events)
        self._latest_by_type: dict[str, dict[str, Any]] = {}
        self._received_batches = 0
        self._received_events = 0
        self._started_at = time.time()
        self._last_ingested_at: float | None = None

    def ingest(self, events: list[dict[str, Any]]) -> None:
        now = time.time()
        with self._lock:
            self._received_batches += 1
            self._last_ingested_at = now
            for event in events:
                event.setdefault("_ingested_at", now)
                etype = str(event.get("event_type", "unknown"))
                self._latest_by_type[etype] = event
                self._history.append(event)
                self._received_events += 1

    def health(self) -> dict[str, Any]:
        with self._lock:
            uptime = time.time() - self._started_at
            last_age = None
            if self._last_ingested_at is not None:
                last_age = round(time.time() - self._last_ingested_at, 2)
            return {
                "status": "ok",
                "uptime_seconds": round(uptime, 2),
                "received_batches": self._received_batches,
                "received_events": self._received_events,
                "history_size": len(self._history),
                "last_event_age_seconds": last_age,
                "expected_event_types": [
                    "system.metrics.cpu",
                    "system.metrics.memory",
                    "system.metrics.disk",
                    "system.metrics.network",
                ],
                "seen_event_types": sorted(self._latest_by_type.keys()),
            }

    def latest(self) -> dict[str, Any]:
        with self._lock:
            return {
                "events_by_type": self._latest_by_type,
                "history_size": len(self._history),
                "received_events": self._received_events,
            }

    def history(self, event_type: str | None = None, limit: int = 200) -> list[dict[str, Any]]:
        limit = max(1, min(limit, 2000))
        with self._lock:
            if event_type:
                items = [e for e in self._history if str(e.get("event_type", "")) == event_type]
            else:
                items = list(self._history)
        return items[-limit:]


class SysWatchHandler(BaseHTTPRequestHandler):
    store: EventStore

    def log_message(self, fmt: str, *args: Any) -> None:
        # Keep server output concise and structured.
        print(f"[webui] {self.address_string()} - {fmt % args}")

    def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, status: HTTPStatus, text: str) -> None:
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_index(self) -> None:
        if not INDEX_HTML.exists():
            self._send_text(HTTPStatus.NOT_FOUND, "index.html not found")
            return

        body = INDEX_HTML.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/":
            self._send_index()
            return

        if path == "/api/health":
            self._send_json(HTTPStatus.OK, self.store.health())
            return

        if path == "/api/latest":
            self._send_json(HTTPStatus.OK, self.store.latest())
            return

        if path == "/api/history":
            qs = parse_qs(parsed.query)
            event_type = qs.get("event_type", [None])[0]
            try:
                limit = int(qs.get("limit", ["200"])[0])
            except ValueError:
                limit = 200
            self._send_json(HTTPStatus.OK, {"events": self.store.history(event_type, limit)})
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found", "path": path})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/demo-seed":
            now_ts = time.strftime("%H:%M:%S")
            demo_events = [
                {
                    "schema_version": "1.0",
                    "timestamp": now_ts,
                    "source": "syswatch-demo",
                    "source_version": "0.1.0",
                    "host": "demo-host",
                    "event_type": "system.metrics.cpu",
                    "severity": "info",
                    "payload": {
                        "user_pct": 22.4,
                        "system_pct": 7.1,
                        "idle_pct": 70.5,
                        "usage_pct": 29.5,
                        "interval_seconds": 1,
                    },
                },
                {
                    "schema_version": "1.0",
                    "timestamp": now_ts,
                    "source": "syswatch-demo",
                    "source_version": "0.1.0",
                    "host": "demo-host",
                    "event_type": "system.metrics.memory",
                    "severity": "info",
                    "payload": {
                        "mem_used_bytes": 8450304000,
                        "mem_available_bytes": 4026531840,
                        "swap_used_bytes": 268435456,
                    },
                },
                {
                    "schema_version": "1.0",
                    "timestamp": now_ts,
                    "source": "syswatch-demo",
                    "source_version": "0.1.0",
                    "host": "demo-host",
                    "event_type": "system.metrics.disk",
                    "severity": "info",
                    "payload": {
                        "read_bps": 131072.0,
                        "write_bps": 262144.0,
                    },
                },
                {
                    "schema_version": "1.0",
                    "timestamp": now_ts,
                    "source": "syswatch-demo",
                    "source_version": "0.1.0",
                    "host": "demo-host",
                    "event_type": "system.metrics.network",
                    "severity": "info",
                    "payload": {
                        "rx_bps": 8192.0,
                        "tx_bps": 4096.0,
                    },
                },
            ]
            self.store.ingest(demo_events)
            self._send_json(HTTPStatus.OK, {"ok": True, "seeded": len(demo_events)})
            return

        if parsed.path != "/events":
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found", "path": parsed.path})
            return

        content_length = self.headers.get("Content-Length", "0")
        try:
            body_len = int(content_length)
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid_content_length"})
            return

        if body_len <= 0:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "empty_body"})
            return

        body = self.rfile.read(body_len)
        try:
            parsed_json = json.loads(body)
        except json.JSONDecodeError as exc:
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"error": "invalid_json", "detail": str(exc)},
            )
            return

        if isinstance(parsed_json, dict):
            events = [parsed_json]
        elif isinstance(parsed_json, list):
            events = [item for item in parsed_json if isinstance(item, dict)]
        else:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid_payload_type"})
            return

        if not events:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "no_event_objects"})
            return

        self.store.ingest(events)
        self._send_json(HTTPStatus.OK, {"ok": True, "accepted": len(events)})


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="SysWatch Web UI server")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8780, help="Bind port (default: 8780)")
    parser.add_argument("--max-events", type=int, default=5000, help="History size cap")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    store = EventStore(max_events=max(100, args.max_events))

    handler_cls = type("BoundSysWatchHandler", (SysWatchHandler,), {})
    handler_cls.store = store

    server = ThreadingHTTPServer((args.host, args.port), handler_cls)
    print(f"[webui] listening on http://{args.host}:{args.port}")
    print("[webui] ingest endpoint: POST /events")
    print("[webui] dashboard: GET /")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        print("[webui] stopped")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
