"""Relay between lg-bridge (NDJSON over TCP on the QNX target) and the dashboard browsers.

No FastAPI import here, so the logic can be tested with the standard library alone
(tests/host/test_relay.py). The relay is read-only towards the crossing: the only line it ever
sends to lg-bridge is {"kind":"subscribe"}; nothing that a browser sends is forwarded
(CLAUDE.md rule 4, README 7.6).
"""

from __future__ import annotations

import asyncio
import json
import time
from collections import deque
from typing import Any

SUBSCRIBE = b'{"kind":"subscribe"}\n'
HISTORY = 50          # notifications and level changes kept for a browser that connects later
QUEUE_MAX = 400       # lines buffered per browser; a slow browser loses the oldest ones
MAX_LINE = 64 * 1024  # a longer line from the bridge is dropped


class Relay:
    def __init__(self, host: str, port: int = 5555, reconnect_s: float = 1.0) -> None:
        self.host = host
        self.port = port
        self.reconnect_s = reconnect_s
        self.connected = False
        self.last_line_at = 0.0
        self.lines_in = 0
        self.bad_lines = 0
        self.connects = 0
        self.state: dict[str, Any] | None = None
        self.metrics: dict[str, Any] = {}
        self.history: deque[dict[str, Any]] = deque(maxlen=HISTORY)
        self._seen_order: deque[tuple[str, int, float]] = deque(maxlen=4 * HISTORY)
        self._seen: set[tuple[str, int, float]] = set()
        self._subscribers: set[asyncio.Queue[str]] = set()
        self._stop = asyncio.Event()

    # ---- browsers ----------------------------------------------------------------------------
    def subscribe(self) -> asyncio.Queue[str]:
        q: asyncio.Queue[str] = asyncio.Queue(maxsize=QUEUE_MAX)
        self._subscribers.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue[str]) -> None:
        self._subscribers.discard(q)

    def hello(self) -> dict[str, Any]:
        """What a browser gets first: the latest state, recent events, metrics and the link status."""
        return {
            "kind": "hello",
            "link": self.link(),
            "state": self.state,
            "history": list(self.history),
            "metrics": dict(self.metrics),
        }

    def link(self) -> dict[str, Any]:
        age = time.monotonic() - self.last_line_at if self.last_line_at else None
        return {
            "kind": "link",
            "connected": self.connected,
            "target": f"{self.host}:{self.port}",
            "age_s": round(age, 3) if age is not None else None,
            "lines_in": self.lines_in,
            "bad_lines": self.bad_lines,
            "connects": self.connects,
        }

    def _broadcast(self, text: str) -> None:
        for q in list(self._subscribers):
            if q.full():
                try:
                    q.get_nowait()  # drop the oldest line of a slow browser
                except asyncio.QueueEmpty:
                    pass
            try:
                q.put_nowait(text)
            except asyncio.QueueFull:
                pass

    # ---- bridge ------------------------------------------------------------------------------
    def handle_line(self, raw: bytes) -> bool:
        """Takes one line from the bridge. Returns False (and counts it) when it is not valid."""
        if len(raw) > MAX_LINE:
            self.bad_lines += 1
            return False
        try:
            obj = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.bad_lines += 1
            return False
        if not isinstance(obj, dict) or not isinstance(obj.get("kind"), str):
            self.bad_lines += 1
            return False
        self.lines_in += 1
        self.last_line_at = time.monotonic()
        kind = obj["kind"]
        if kind == "state":
            self.state = obj
        elif kind in ("level", "notify"):
            # lg-bridge replays its recent events to every new client: after a reconnect the same event
            # (same seq and t) comes again and is kept and shown once.
            seq, t = obj.get("seq"), obj.get("t")
            if isinstance(seq, int) and isinstance(t, (int, float)):
                key = (kind, seq, t)
                if key in self._seen:
                    return True
                if len(self._seen_order) == self._seen_order.maxlen:
                    self._seen.discard(self._seen_order[0])
                self._seen_order.append(key)
                self._seen.add(key)
            self.history.append(obj)
        elif kind == "metric" and isinstance(obj.get("name"), str):
            self.metrics[obj["name"]] = obj.get("value")
        self._broadcast(json.dumps(obj, separators=(",", ":")))
        return True

    async def run(self) -> None:
        """Connect, read lines, reconnect after a loss, until stop() is called."""
        while not self._stop.is_set():
            writer = None
            try:
                reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), timeout=3)
                self.connected = True
                self.connects += 1
                self._broadcast(json.dumps(self.link()))
                writer.write(SUBSCRIBE)
                await writer.drain()
                while not self._stop.is_set():
                    raw = await asyncio.wait_for(reader.readline(), timeout=5)
                    if not raw:
                        break  # the bridge closed the connection
                    self.handle_line(raw.rstrip(b"\r\n"))
            except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
                pass
            finally:
                if writer is not None:
                    writer.close()
                if self.connected:
                    self.connected = False
                    self._broadcast(json.dumps(self.link()))
            if not self._stop.is_set():
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=self.reconnect_s)
                except asyncio.TimeoutError:
                    pass

    def stop(self) -> None:
        self._stop.set()
