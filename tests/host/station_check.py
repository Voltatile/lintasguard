#!/usr/bin/env python3
"""Step 12 host check: station messages through lg-bridge to lg-schedule (README 8.4, 7.6; CLAUDE.md rule 4).

    python tests/host/station_check.py <vm-ip> [--port 5555]

Starts scenarios/level3_station.json on the QNX target under lg-supervisor with its timetable (lg-schedule
installed), fails both West approach sensors at 20 s (DROPOUT 100), runs lg-meter to 450 s, and from the host
runs host/station_sim/sim.py with scenarios/level3_station.departures.json:

  - train 1 is planned at 300 s but runs 90 s early (210 s): only the station message at 150 s protects it.
    /dev/lg/schedule must show entry 1 moved earlier (source STATION, eta at most 210 s);
  - the message at 330 s claims train 3 arrives 2 minutes late: entry 3 must keep its planned 400 s
    (a station message can never delay a window) and the station counter says ignored=1;
  - a departure with an extra field is rejected by the bridge whitelist;
  - both West trains are confirmed by the island (win_island=2,0), level 3 throughout, and lg-meter reports
    wrong_side_events 0 with complete observation.

The negative control (the same day without station messages gives a wrong-side event) is in the host replay
tests/scenario/replay_level3.c, never on the target. Standard library only. Prints one PASS:/FAIL: line per
check and exits non-zero on any failure. Timings come from a VirtualBox VM: functional evidence only.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=30"]
BIN = "/tmp/lintasguard"
END_MS = 450000

_spec = importlib.util.spec_from_file_location("station_sim", REPO / "host" / "station_sim" / "sim.py")
sim = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules["station_sim"] = sim
_spec.loader.exec_module(sim)

results: list[tuple[bool, str]] = []


def check(name: str, ok: bool) -> None:
    results.append((ok, name))
    print(f"{'PASS' if ok else 'FAIL'}: {name}", flush=True)


def ssh_text(ip: str, cmd: str, timeout: float = 40.0) -> str:
    try:
        out = subprocess.run(SSH + [f"root@{ip}", cmd], capture_output=True, text=True, timeout=timeout,
                             check=False)
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


def plant_ms(ip: str) -> int:
    v = kv(ssh_text(ip, "cat /dev/lg/plant")).get("t_ms", "")
    return int(v) if v.isdigit() else -1


def wait_plant(ip: str, ms: int, limit_s: float = 900.0) -> bool:
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        p = plant_ms(ip)
        if p >= ms:
            return True
        time.sleep(1.0 if p < 0 or ms - p > 3000 else 0.2)
    return False


def entry(schedule_text: str, entry_id: int) -> dict[str, str]:
    for line in schedule_text.splitlines():
        if line.startswith(f"entry id={entry_id} "):
            return kv(line)
    return {}


CLEANUP = ("for p in lg-supervisor lg-bridge lg-schedule lg-health lg-meter lg-controller lg-sensor lg-plant "
           "lg-gate; do for q in $(pidin -p $p -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do kill -9 $q; "
           "done; done; true")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=5555)
    args = ap.parse_args()
    ip = args.ip

    print(f"=== station_check against {ip}", flush=True)
    ssh_text(ip, CLEANUP)
    time.sleep(1.5)
    ssh_text(ip, f"cd {BIN}; rm -f /tmp/lg_sc_metrics.csv; nohup ./lg-supervisor -b {BIN} -c config/crossing.json "
                 f"-s scenarios/level3_station.json -T scenarios/level3_station.timetable.json "
                 f"</dev/null >/tmp/lg_sc_sup.log 2>&1 &")
    ok = False
    for _ in range(60):
        c = kv(ssh_text(ip, "cat /dev/lg/controller; cat /dev/lg/gate"))
        if c.get("level") == "0" and c.get("state") == "UP":
            ok = True
            break
        time.sleep(0.5)
    check("boot release: level 0 and gate UP", ok)
    ssh_text(ip, f"cd {BIN}; rm -f /tmp/lg_sc_meter.out; nohup sh -c './lg-meter -e {END_MS} -m {END_MS // 1000 + 60} "
                 f"-o /tmp/lg_sc_metrics.csv; echo meter_exit=$?' </dev/null >/tmp/lg_sc_meter.out 2>&1 &")

    deps = sim.load_departures(REPO / "scenarios" / "level3_station.departures.json")
    sim_rc: list[int] = []
    t = threading.Thread(target=lambda: sim_rc.append(sim.run(ip, args.port, deps, 420.0, 5000)), daemon=True)
    t.start()

    wait_plant(ip, 20000)
    ssh_text(ip, f"{BIN}/lgctl sensor west/a dropout 100; {BIN}/lgctl sensor west/b dropout 100")
    time.sleep(3)
    c = kv(ssh_text(ip, "cat /dev/lg/controller"))
    check("level 3 after both West sensors failed", c.get("level") == "3")

    wait_plant(ip, 153000)
    text = ssh_text(ip, "cat /dev/lg/schedule")
    e1 = entry(text, 1)
    eta1 = int(e1.get("eta_ms", "999999999"))
    print(text, flush=True)
    check(f"the station moved train 1 earlier (source STATION, eta {eta1} ms <= 210000, planned 300000)",
          e1.get("source") == "STATION" and eta1 <= 210000 and e1.get("planned_ms") == "300000")
    check("the window of train 1 is open by 153 s (the controller wants the gate DOWN)",
          kv(ssh_text(ip, "cat /dev/lg/controller")).get("target") == "DOWN")

    # A departure with an extra field: the bridge whitelist rejects it.
    try:
        with socket.create_connection((ip, args.port), timeout=10) as s:
            s.settimeout(3)
            s.sendall(b'{"kind":"station_departure","dir":"west","eta_s":30,"cmd":"raise"}\n')
            buf = b""
            reply = None
            end = time.monotonic() + 3
            while time.monotonic() < end and reply is None:
                try:
                    buf += s.recv(65536)
                except socket.timeout:
                    break
                for raw in buf.split(b"\n")[:-1]:
                    try:
                        obj = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    if obj.get("kind") in ("ack", "error"):
                        reply = obj
                        break
    except OSError:
        reply = None
    check("a departure with an extra field is rejected (error rejected)",
          bool(reply) and reply.get("kind") == "error" and reply.get("what") == "rejected")

    wait_plant(ip, 333000)
    text = ssh_text(ip, "cat /dev/lg/schedule")
    e3 = entry(text, 3)
    print(text, flush=True)
    check("the 'train 3 is late' message did not delay entry 3 (TIMETABLE, eta 400000)",
          e3.get("source") == "TIMETABLE" and e3.get("eta_ms") == "400000")
    st = kv(next((ln for ln in text.splitlines() if ln.startswith("station ")), ""))
    check(f"station counters: rx=2 moved=1 ignored=1 (got rx={st.get('rx')} moved={st.get('moved')} "
          f"ignored={st.get('ignored')})", st.get("rx") == "2" and st.get("moved") == "1" and st.get("ignored") == "1")

    t.join(timeout=200)
    check("station_sim sent both departures and lg-bridge acknowledged them", sim_rc == [0])

    wait_plant(ip, END_MS + 2000)
    meter_out = ""
    for _ in range(90):
        meter_out = ssh_text(ip, "cat /tmp/lg_sc_meter.out 2>/dev/null")
        if "meter_exit=" in meter_out:
            break
        time.sleep(1)
    csv = dict(ln.split(",", 1) for ln in ssh_text(ip, "cat /tmp/lg_sc_metrics.csv").splitlines() if "," in ln)
    print(meter_out, flush=True)
    for k in ("trains", "trains_without_warning", "closure_ms_per_hour", "wrong_side_events", "wrong_side_ticks",
              "meter_coverage_pct", "meter_max_gap_ms"):
        print(f"RESULT station_{k}={csv.get(k, '')}", flush=True)
    check("lg-meter: wrong_side_events is 0", csv.get("wrong_side_events") == "0")
    check("lg-meter: wrong_side_ticks is 0", csv.get("wrong_side_ticks") == "0")
    check("lg-meter: 3 trains, all with the lights on", csv.get("trains") == "3"
          and csv.get("trains_without_warning") == "0")
    check("lg-meter observed the whole run and saw no wrong side (exit 0)", "meter_exit=0" in meter_out)
    c = kv(ssh_text(ip, "cat /dev/lg/controller"))
    check("both West windows confirmed by the island (win_island=2,0), still level 3",
          c.get("win_island") == "2,0" and c.get("level") == "3")
    bridge = kv(ssh_text(ip, "cat /dev/lg/bridge"))
    check(f"lg-bridge forwarded both departures to lg-schedule (fwd_sent={bridge.get('fwd_sent')})",
          bridge.get("fwd_sent") == "2" and bridge.get("fwd_failed") == "0")
    ssh_text(ip, CLEANUP)

    fails = sum(1 for ok, _ in results if not ok)
    print(f"RESULT checks={len(results)} fails={fails}")
    print(f"RESULT station_check {'PASS' if fails == 0 else 'FAIL'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
