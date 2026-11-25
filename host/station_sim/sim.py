#!/usr/bin/env python3
"""Station simulator (README 4.1, 8.4, Step 12): tells the crossing when trains depart towards it.

    python host/station_sim/sim.py --ip <vm-ip> --departures scenarios/level3_station.departures.json
                                   [--port 5555] [--duration 600] [--late-window 5]

It connects to lg-bridge on the QNX target (TCP 5555), subscribes, follows the scenario clock from the
"plant_ms" field of the state lines, and when a departure is due sends

    {"kind":"station_departure","dir":"west","eta_s":60}

eta_s = seconds from now until the train reaches the crossing (rounded down: an earlier window is the safe
side). It uses only the existing inbound whitelist of lg-bridge (README 7.6): there is no message that can
move the gate, and lg-schedule can only move a level 3 window earlier or add one (CLAUDE.md rule 4).

Departures file:
    {"name": "...", "departures": [{"at_s": 150, "dir": "west", "eta_s": 60}, ...]}
at_s = scenario time of the message (s), eta_s = seconds from then until the train reaches the crossing.
A departure is sent once, when plant time reaches at_s; one more than --late-window seconds overdue (the
simulator started late) is skipped and reported, never sent with a wrong eta. If the plant clock goes
back (a new scenario run), everything is armed again.

Standard library only. Prints one line per departure sent and the bridge's answer. Exit code 0 when every
departure was sent and acknowledged, 1 otherwise, 2 for bad arguments or a bad file.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path

DIRS = ("west", "east")
ETA_MAX_S = 86400  # lg-bridge whitelist


@dataclass
class Departure:
    at_ms: int
    dir: str
    eta_s: int
    sent: bool = False
    skipped: bool = False
    answer: str = ""


def load_departures(path: Path) -> list[Departure]:
    """Read and check a departures file. Raises ValueError with a reason."""
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    if not isinstance(doc, dict) or set(doc) - {"name", "departures"} or "departures" not in doc:
        raise ValueError("top level must be an object with name (optional) and departures")
    deps = doc["departures"]
    if not isinstance(deps, list):
        raise ValueError("departures must be an array")
    out: list[Departure] = []
    for i, d in enumerate(deps):
        if not isinstance(d, dict) or set(d) != {"at_s", "dir", "eta_s"}:
            raise ValueError(f"departure {i}: needs exactly at_s, dir and eta_s")
        at, direction, eta = d["at_s"], d["dir"], d["eta_s"]
        if isinstance(at, bool) or not isinstance(at, (int, float)) or not 0 <= at <= 86400:
            raise ValueError(f"departure {i}: at_s must be a number from 0 to 86400")
        if direction not in DIRS:
            raise ValueError(f"departure {i}: dir must be west or east")
        if isinstance(eta, bool) or not isinstance(eta, int) or not 0 <= eta <= ETA_MAX_S:
            raise ValueError(f"departure {i}: eta_s must be an integer from 0 to {ETA_MAX_S}")
        out.append(Departure(at_ms=int(round(at * 1000)), dir=direction, eta_s=eta))
    return out


def departure_line(direction: str, eta_s: int) -> str:
    """The exact inbound line lg-bridge accepts (no extra fields, compact)."""
    if direction not in DIRS or not 0 <= eta_s <= ETA_MAX_S:
        raise ValueError("bad departure")
    return json.dumps({"kind": "station_departure", "dir": direction, "eta_s": eta_s}, separators=(",", ":"))


def due_eta(dep: Departure, plant_ms: int, late_window_ms: int) -> int | None:
    """eta_s to send now, or None when the departure is not due (or too late, then it is marked skipped)."""
    if dep.sent or dep.skipped or plant_ms < dep.at_ms:
        return None
    late = plant_ms - dep.at_ms
    if late > late_window_ms:
        dep.skipped = True
        return None
    return max(0, dep.eta_s - (late + 999) // 1000)  # round the remaining time down


def rearm(deps: list[Departure]) -> None:
    for d in deps:
        d.sent = d.skipped = False
        d.answer = ""


class Link:
    """Line-based TCP client of lg-bridge."""

    def __init__(self, ip: str, port: int) -> None:
        self.sock = socket.create_connection((ip, port), timeout=10)
        self.sock.settimeout(0.5)
        self.buf = b""

    def send(self, line: str) -> None:
        self.sock.sendall(line.encode("ascii") + b"\n")

    def lines(self) -> list[dict]:
        out: list[dict] = []
        try:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("lg-bridge closed the connection")
            self.buf += data
        except socket.timeout:
            pass
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if isinstance(obj, dict):
                out.append(obj)
        return out

    def close(self) -> None:
        self.sock.close()


def run(ip: str, port: int, deps: list[Departure], duration_s: float, late_window_ms: int) -> int:
    link = Link(ip, port)
    link.send('{"kind":"subscribe"}')
    waiting: list[Departure] = []  # sent, answer not seen yet (answers come in order)
    last_plant = -1
    end = time.monotonic() + duration_s
    try:
        while time.monotonic() < end:
            for obj in link.lines():
                kind = obj.get("kind")
                if kind == "state" and isinstance(obj.get("plant_ms"), int):
                    plant = obj["plant_ms"]
                    if plant < last_plant:
                        print(f"plant clock went back ({last_plant} -> {plant} ms): new run, departures armed again")
                        rearm(deps)
                        waiting.clear()
                    last_plant = plant
                    for d in deps:
                        eta = due_eta(d, plant, late_window_ms)
                        if d.skipped and not d.answer:
                            d.answer = "skipped (simulator started too late)"
                            print(f"plant {plant} ms: departure {d.dir} at {d.at_ms} ms skipped (late)")
                        if eta is not None:
                            link.send(departure_line(d.dir, eta))
                            d.sent = True
                            waiting.append(d)
                            print(f"plant {plant} ms: sent station_departure dir={d.dir} eta_s={eta}", flush=True)
                elif obj.get("what") == "station_departure" and waiting:
                    d = waiting.pop(0)
                    d.answer = "ack" if kind == "ack" else f"error {obj.get('reason')}"
                    print(f"  lg-bridge: {d.answer}", flush=True)
            if deps and all(d.skipped or (d.sent and d.answer) for d in deps):
                break
    finally:
        link.close()
    ok = all(d.sent and d.answer == "ack" for d in deps)
    print(f"RESULT departures={len(deps)} acked={sum(d.answer == 'ack' for d in deps)} "
          f"skipped={sum(d.skipped for d in deps)} {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--departures", type=Path, required=True)
    ap.add_argument("--duration", type=float, default=600.0, help="give up after this many seconds")
    ap.add_argument("--late-window", type=float, default=5.0, help="skip a departure this many seconds overdue")
    args = ap.parse_args(argv)
    try:
        deps = load_departures(args.departures)
    except ValueError as exc:
        print(f"station_sim: {exc}", file=sys.stderr)
        return 2
    try:
        return run(args.ip, args.port, deps, args.duration, int(args.late_window * 1000))
    except OSError as exc:
        print(f"station_sim: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
