"""Unit tests for host/station_sim/sim.py (Step 12). Standard library only, no target needed."""

from __future__ import annotations

import importlib.util
import json
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("station_sim", REPO / "host" / "station_sim" / "sim.py")
sim = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules["station_sim"] = sim
_spec.loader.exec_module(sim)


def write(doc) -> Path:
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
    f.write(doc if isinstance(doc, str) else json.dumps(doc))
    f.close()
    return Path(f.name)


class LoadTests(unittest.TestCase):
    def test_repo_file_loads(self) -> None:
        deps = sim.load_departures(REPO / "scenarios" / "level3_station.departures.json")
        self.assertEqual([(d.at_ms, d.dir, d.eta_s) for d in deps], [(150000, "west", 60), (330000, "west", 190)])

    def test_rejects(self) -> None:
        bad = [
            "not json",
            [],
            {"departures": {}},
            {"departures": [], "extra": 1},
            {"departures": [{"at_s": 1, "dir": "north", "eta_s": 5}]},
            {"departures": [{"at_s": 1, "dir": "west", "eta_s": 5.5}]},
            {"departures": [{"at_s": 1, "dir": "west", "eta_s": -1}]},
            {"departures": [{"at_s": 1, "dir": "west", "eta_s": 86401}]},
            {"departures": [{"at_s": -1, "dir": "west", "eta_s": 5}]},
            {"departures": [{"at_s": True, "dir": "west", "eta_s": 5}]},
            {"departures": [{"at_s": 1, "dir": "west", "eta_s": 5, "cmd": "raise"}]},
            {"departures": [{"at_s": 1, "dir": "west"}]},
        ]
        for doc in bad:
            with self.subTest(doc=doc):
                with self.assertRaises(ValueError):
                    sim.load_departures(write(doc))


class LineTests(unittest.TestCase):
    def test_exact_whitelist_line(self) -> None:
        self.assertEqual(sim.departure_line("west", 60), '{"kind":"station_departure","dir":"west","eta_s":60}')
        self.assertLess(len(sim.departure_line("east", 86400)), 256)  # lg-bridge line limit
        for d, e in (("north", 1), ("west", -1), ("west", 86401)):
            with self.assertRaises(ValueError):
                sim.departure_line(d, e)

    def test_due_and_rounding(self) -> None:
        d = sim.Departure(at_ms=150000, dir="west", eta_s=60)
        self.assertIsNone(sim.due_eta(d, 149999, 5000))
        self.assertEqual(sim.due_eta(d, 150000, 5000), 60)
        self.assertEqual(sim.due_eta(d, 150001, 5000), 59)  # remaining time rounded down: earlier window
        self.assertEqual(sim.due_eta(d, 151000, 5000), 59)
        late = sim.Departure(at_ms=150000, dir="west", eta_s=60)
        self.assertIsNone(sim.due_eta(late, 155001, 5000))
        self.assertTrue(late.skipped)
        short = sim.Departure(at_ms=0, dir="east", eta_s=1)
        self.assertEqual(sim.due_eta(short, 4000, 5000), 0)  # never negative
        sent = sim.Departure(at_ms=0, dir="east", eta_s=1, sent=True)
        self.assertIsNone(sim.due_eta(sent, 10, 5000))
        sim.rearm([late, sent])
        self.assertFalse(late.skipped or sent.sent)


class FakeBridge(threading.Thread):
    """Sends state lines with plant_ms advancing 500 ms per line, answers station_departure with ack."""

    def __init__(self, start_ms: int, step_ms: int = 500, lines: int = 40, reply_kind: str = "ack") -> None:
        super().__init__(daemon=True)
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.start_ms, self.step_ms, self.n, self.reply_kind = start_ms, step_ms, lines, reply_kind
        self.received: list[str] = []

    def run(self) -> None:
        conn, _ = self.srv.accept()
        conn.settimeout(0.05)
        buf = b""
        for i in range(self.n):
            line = {"kind": "state", "plant_ms": self.start_ms + i * self.step_ms}
            try:
                conn.sendall(json.dumps(line).encode() + b"\n")
            except OSError:
                break
            try:
                buf += conn.recv(4096)
            except (socket.timeout, OSError):
                pass
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode()
                self.received.append(text)
                obj = json.loads(text)
                if obj.get("kind") == "station_departure":
                    if self.reply_kind == "ack":
                        reply = {"kind": "ack", "what": "station_departure"}
                    else:
                        reply = {"kind": "error", "what": "station_departure", "reason": "schedule_unavailable"}
                    conn.sendall(json.dumps(reply).encode() + b"\n")
        try:
            conn.close()
        finally:
            self.srv.close()


class RunTests(unittest.TestCase):
    def test_sends_on_time_and_passes(self) -> None:
        fb = FakeBridge(start_ms=148000)
        fb.start()
        deps = [sim.Departure(at_ms=150000, dir="west", eta_s=60), sim.Departure(at_ms=152000, dir="east", eta_s=30)]
        rc = sim.run("127.0.0.1", fb.port, deps, 10.0, 5000)
        fb.join(5)
        self.assertEqual(rc, 0)
        self.assertEqual(fb.received[0], '{"kind":"subscribe"}')
        self.assertIn('{"kind":"station_departure","dir":"west","eta_s":60}', fb.received)
        self.assertIn('{"kind":"station_departure","dir":"east","eta_s":30}', fb.received)

    def test_late_start_skips_and_fails(self) -> None:
        fb = FakeBridge(start_ms=200000, lines=6)
        fb.start()
        deps = [sim.Departure(at_ms=150000, dir="west", eta_s=60)]
        rc = sim.run("127.0.0.1", fb.port, deps, 5.0, 5000)
        fb.join(5)
        self.assertEqual(rc, 1)
        self.assertTrue(deps[0].skipped)
        self.assertEqual(fb.received, ['{"kind":"subscribe"}'])  # nothing with a wrong eta

    def test_unavailable_schedule_fails(self) -> None:
        fb = FakeBridge(start_ms=150000, lines=10, reply_kind="error")
        fb.start()
        deps = [sim.Departure(at_ms=150000, dir="west", eta_s=60)]
        rc = sim.run("127.0.0.1", fb.port, deps, 5.0, 5000)
        fb.join(5)
        self.assertEqual(rc, 1)
        self.assertEqual(deps[0].answer, "error schedule_unavailable")


if __name__ == "__main__":
    unittest.main()
