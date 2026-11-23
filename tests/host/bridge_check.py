#!/usr/bin/env python3
"""Step 10 host check of lg-bridge on the QNX target (README 9 step 10, 7.6; CLAUDE.md rule 4).

    python tests/host/bridge_check.py <vm-ip> [--out FILE]

Runs one fault case on the target in the background (tests/target/lg_case.sh: sensor_faults.json,
stuck-clear on west/a from 10 s to 50 s, lg-meter watching the island truth), and while it runs talks
to lg-bridge on TCP 5555 from the host:

  - the stream: every line is JSON; state lines arrive about every 100 ms and carry trains with
    positions; the fault shows up as a level line 0 -> 2 and a MISSED_TRAIN notify line for west/a;
    metric lines arrive;
  - inbound whitelist: subscribe and station_departure are acknowledged; a gate command, garbage, an
    oversized line, unknown and extra fields are answered with an error; the bridge rejected counter
    rises by exactly that number; the gate rejected counter does not move and the gate target equals
    the controller target (read over ssh);
  - clients: a ninth client is refused while eight are connected; a client that never reads is
    dropped while a reading client keeps getting state lines;
  - the case itself passes on the target, including wrong_side_events 0 from lg-meter.

Standard library only. Prints one PASS:/FAIL: line per check and exits non-zero on any failure.
Timings come from a VirtualBox VM: functional evidence only.
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CASE_SCRIPT = REPO / "tests" / "target" / "lg_case.sh"
# name sensor fault inject_ms clear_ms end_ms expect_trains expect_level expect_diag scenario
CASE_ARGS = ["bridge_check", "west/a", "stuck-clear", "10000", "50000", "145000", "3", "2",
             "MISSED_TRAIN", "sensor_faults.json"]
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=30"]

FORBIDDEN = [
    ("gate command", '{"kind":"gate","cmd":"raise"}'),
    ("controller command", '{"kind":"guard_ack","id":"G01"}'),
    ("garbage", "hello"),
    ("not an object", "[1,2,3]"),
    ("oversized line", '{"kind":"subscribe","pad":"' + "x" * 400 + '"}'),
    ("extra field on station_departure", '{"kind":"station_departure","dir":"west","eta_s":30,"cmd":"raise"}'),
    ("bad direction", '{"kind":"station_departure","dir":"north","eta_s":30}'),
    ("negative eta", '{"kind":"station_departure","dir":"east","eta_s":-1}'),
]

results: list[tuple[bool, str]] = []


def check(name: str, ok: bool) -> None:
    results.append((ok, name))
    print(f"{'PASS' if ok else 'FAIL'}: {name}", flush=True)


def ssh_text(ip: str, cmd: str, timeout: float = 40.0) -> str:
    try:
        out = subprocess.run(SSH + [f"root@{ip}", cmd], capture_output=True, text=True,
                             timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return out.stdout


def kv(text: str) -> dict[str, str]:
    d: dict[str, str] = {}
    for word in text.split():
        if "=" in word:
            k, v = word.split("=", 1)
            d.setdefault(k, v)
    return d


class Reader(threading.Thread):
    """One client that reads every line and keeps what the checks need."""

    def __init__(self, ip: str, port: int) -> None:
        super().__init__(daemon=True)
        self.sock = socket.create_connection((ip, port), timeout=5)
        self.sock.settimeout(1.0)
        self.lock = threading.Lock()
        self.lines = 0
        self.bad = 0
        self.kinds: dict[str, int] = {}
        self.state_times: list[float] = []  # host clock of each state line
        self.trains_seen: set[int] = set()
        self.trains_with_positions = 0
        self.level_lines: list[dict] = []
        self.notify_lines: list[dict] = []
        self.closed = False
        self.stop = False

    def run(self) -> None:
        buf = b""
        while not self.stop:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._line(raw)
        self.closed = True

    def _line(self, raw: bytes) -> None:
        with self.lock:
            self.lines += 1
            try:
                obj = json.loads(raw)
            except ValueError:
                self.bad += 1
                return
            if not isinstance(obj, dict):
                self.bad += 1
                return
            kind = str(obj.get("kind"))
            self.kinds[kind] = self.kinds.get(kind, 0) + 1
            if kind == "state":
                self.state_times.append(time.monotonic())
                for t in obj.get("trains") or []:
                    if isinstance(t, dict) and "id" in t and "front_mm" in t and "rear_mm" in t:
                        self.trains_seen.add(int(t["id"]))
                        self.trains_with_positions += 1
            elif kind == "level":
                self.level_lines.append(obj)
            elif kind == "notify":
                self.notify_lines.append(obj)

    def states_since(self, t0: float) -> int:
        with self.lock:
            return sum(1 for t in self.state_times if t >= t0)

    def max_gap_since(self, t0: float) -> float:
        with self.lock:
            ts = [t for t in self.state_times if t >= t0]
        return max((b - a for a, b in zip(ts, ts[1:])), default=99.0)


def exchange(ip: str, port: int, line: str, timeout: float = 4.0) -> dict | None:
    """Send one line on a fresh client; return the first ack/error reply (state lines are skipped)."""
    with socket.create_connection((ip, port), timeout=5) as s:
        s.sendall(line.encode() + b"\n")
        s.settimeout(0.5)
        buf = b""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                return None
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                try:
                    obj = json.loads(raw)
                except ValueError:
                    continue
                if isinstance(obj, dict) and obj.get("kind") in ("ack", "error"):
                    return obj
    return None


def closed_by_peer(sock: socket.socket, timeout: float) -> bool:
    """True when the peer closes the connection within the timeout (data before the close is read)."""
    sock.settimeout(0.5)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return True
        if not chunk:
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--out", help="also write the target case output to this file")
    args = ap.parse_args()
    ip, port = args.ip, args.port

    print(f"bridge_check: target {ip}:{port}, case {' '.join(CASE_ARGS)}", flush=True)
    case_lines: list[str] = []
    with open(CASE_SCRIPT, "rb") as script:
        case = subprocess.Popen(SSH + [f"root@{ip}", "sh", "-s", "--"] + CASE_ARGS, stdin=script,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    booted = threading.Event()

    def pump() -> None:
        assert case.stdout is not None
        for line in case.stdout:
            case_lines.append(line)
            if line.startswith("PASS: boot:"):
                booted.set()

    threading.Thread(target=pump, daemon=True).start()

    # The case starts a fresh system; connect only after its boot check, never to an old bridge.
    check("the case booted a fresh system (level 0, gate UP) within 60 s", booted.wait(60))

    reader = Reader(ip, port)
    reader.start()
    t0 = time.monotonic()
    time.sleep(3)
    n = reader.states_since(t0)
    check(f"state lines arrive about every 100 ms (3 s: {n}, want at least 20)", n >= 20)

    # ---- inbound whitelist ------------------------------------------------------------------
    before = kv(ssh_text(ip, "cat /dev/lg/bridge; cat /dev/lg/gate"))
    reply = exchange(ip, port, '{"kind":"subscribe"}')
    check("subscribe is acknowledged", bool(reply) and reply.get("kind") == "ack"
          and reply.get("what") == "subscribe")
    reply = exchange(ip, port, '{"kind":"station_departure","dir":"west","eta_s":120}')
    check("station_departure is acknowledged", bool(reply) and reply.get("kind") == "ack"
          and reply.get("what") == "station_departure")
    for name, line in FORBIDDEN:
        reply = exchange(ip, port, line)
        check(f"rejected with an error: {name} ({reply.get('reason') if reply else 'no reply'})",
              bool(reply) and reply.get("kind") == "error")
    after_text = ssh_text(ip, "cat /dev/lg/bridge; cat /dev/lg/gate; echo; cat /dev/lg/controller")
    lines = after_text.splitlines()
    after = kv(" ".join(lines[:2]))
    ctl = kv(" ".join(lines[2:]))
    d_rej = int(after.get("in_rejected", "-1")) - int(before.get("in_rejected", "0"))
    check(f"bridge in_rejected rose by exactly {len(FORBIDDEN)} (got {d_rej})", d_rej == len(FORBIDDEN))
    d_station = int(after.get("in_station", "-1")) - int(before.get("in_station", "0"))
    check(f"bridge in_station rose by 1 (got {d_station})", d_station == 1)
    check(f"gate rejected counter unchanged ({before.get('rejected')} -> {after.get('rejected')})",
          before.get("rejected") is not None and before.get("rejected") == after.get("rejected"))
    check(f"gate target equals the controller target (gate {after.get('target')}, controller "
          f"{ctl.get('target')})", after.get("target") is not None and after.get("target") == ctl.get("target"))

    # ---- client limits ----------------------------------------------------------------------
    # The reader is one client; a non-reading client and six idle readers make eight.
    hog = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hog.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    hog.connect((ip, port))
    idle = [socket.create_connection((ip, port), timeout=5) for _ in range(6)]
    time.sleep(1)
    ninth = socket.create_connection((ip, port), timeout=5)
    check("a ninth client is refused (closed within 3 s) while eight are connected",
          closed_by_peer(ninth, 3))
    ninth.close()
    for s in idle:
        s.close()
    b0 = kv(ssh_text(ip, "cat /dev/lg/bridge"))
    t_hog = time.monotonic()
    dropped = False
    while time.monotonic() - t_hog < 90:
        b1 = kv(ssh_text(ip, "cat /dev/lg/bridge"))
        if int(b1.get("dropped_slow", "0")) > int(b0.get("dropped_slow", "0")):
            dropped = True
            break
        time.sleep(2)
    check(f"the client that never reads is dropped (dropped_slow rose, after {time.monotonic() - t_hog:.0f} s)",
          dropped)
    hog.close()
    gap = reader.max_gap_since(t_hog)
    check(f"the reading client kept getting state lines meanwhile (largest gap {gap:.2f} s, want < 2 s)",
          gap < 2.0)

    # ---- the fault on the stream, and the case result -----------------------------------------
    case.wait(timeout=400)
    reader.stop = True
    reader.join(timeout=3)
    check(f"every line from the bridge is JSON ({reader.lines} lines, {reader.bad} bad)",
          reader.lines > 0 and reader.bad == 0)
    check(f"state lines carry trains with positions (trains seen {sorted(reader.trains_seen)})",
          len(reader.trains_seen) >= 2 and reader.trains_with_positions > 0)
    check("a level line 0 -> 2 (SENSOR_FAILED) arrived",
          any(o.get("from") == 0 and o.get("to") == 2 for o in reader.level_lines))
    check("a notify line for west/a with diag MISSED_TRAIN arrived",
          any(o.get("sensor") == "west/a" and o.get("diag") == "MISSED_TRAIN" for o in reader.notify_lines))
    check(f"metric lines arrived ({reader.kinds.get('metric', 0)})", reader.kinds.get("metric", 0) > 0)
    case_text = "".join(case_lines)
    check("on the target: wrong_side_events is 0 (lg-meter)", "PASS: wrong_side_events is 0" in case_text)
    check(f"the target case passed (exit {case.returncode})", case.returncode == 0)

    print("--- target case output:")
    print(case_text, end="")
    print("--- stream summary: kinds " + json.dumps(reader.kinds, sort_keys=True))
    if args.out:
        Path(args.out).write_text(case_text, encoding="utf-8")
    fails = sum(1 for ok, _ in results if not ok)
    print(f"=== RESULT: {len(results)} checks, {fails} failed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
