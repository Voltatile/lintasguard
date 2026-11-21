#!/usr/bin/env python3
"""Fault campaign runner (README 9 step 9, 11, 11.1).

Reads a campaign JSON (campaigns/*.json), runs every case on the QNX target with
tests/target/lg_case.sh (one ssh session per case), and writes a markdown report plus the raw output
of every case. Exit code 0 only when every case passed.

    python host/campaign_runner/run.py campaigns/sensor_faults.json --ip 192.168.56.102
    python host/campaign_runner/run.py campaigns/sensor_faults.json --ip ... --cases crash_west_a_during_train

The binaries, config and scenarios must already be on the target (make deploy IP=<vm-ip>). The
report goes to the system temp directory unless --report is given: an archived copy in evidence/
must come from a real, complete run. Timings come from a VirtualBox VM: functional evidence only.
Standard library only (Python 3.11+).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CASE_SCRIPT = REPO / "tests" / "target" / "lg_case.sh"
CASE_ARGS = ("sensor", "fault", "inject_ms", "clear_ms", "end_ms", "expect_trains", "expect_level", "expect_diag")
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


@dataclass
class CaseResult:
    name: str
    exit_code: int
    passed: bool
    results: dict[str, str] = field(default_factory=dict)
    failed_checks: list[str] = field(default_factory=list)
    checks: int = 0
    output: str = ""
    error: str = ""


def run_text(cmd: list[str], timeout: float = 20.0) -> str:
    """Output of a local command, or a short note when it cannot run (the report says so)."""
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"(not available: {exc.__class__.__name__})"
    text = (out.stdout or out.stderr).strip()
    return text.splitlines()[0] if text else f"(no output, exit {out.returncode})"


def environment(ip: str) -> dict[str, str]:
    commit = run_text(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"])
    dirty = run_text(["git", "-C", str(REPO), "status", "--porcelain"])
    vbox = "VBoxManage not on PATH"
    if shutil.which("VBoxManage"):
        vbox = run_text(["VBoxManage", "--version"])
    qcc = "qcc not on PATH"
    if shutil.which("qcc"):
        qcc = f"qcc on PATH, QNX_HOST={os.environ.get('QNX_HOST', 'not set')}"
    return {
        "date (UTC)": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S"),
        "commit": commit + (" (working tree has uncommitted changes)" if dirty and not dirty.startswith("(") else ""),
        "host OS": platform.platform(),
        "python": platform.python_version(),
        "VirtualBox": vbox,
        "SDP": qcc,
        "target": run_text(["ssh", *SSH_OPTS, f"root@{ip}", "uname -a"]),
    }


def parse_output(name: str, code: int, text: str) -> CaseResult:
    res = CaseResult(name=name, exit_code=code, passed=False, output=text)
    for line in text.splitlines():
        if line.startswith("RESULT "):
            for key, _, value in (tok.partition("=") for tok in line[len("RESULT ") :].split(" ") if "=" in tok):
                res.results[key] = value
        elif line.startswith("FAIL: ") and not line.startswith("FAIL: case "):
            res.failed_checks.append(line[len("FAIL: ") :])
    m = re.search(r"^RESULT checks=(\d+) failed=(\d+)$", text, re.MULTILINE)
    res.checks = int(m.group(1)) if m else 0
    final_pass = re.search(rf"^PASS: case {re.escape(name)} ", text, re.MULTILINE) is not None
    res.passed = code == 0 and final_pass and not res.failed_checks
    return res


def cleanup_target(ip: str) -> None:
    """Stop every LintasGuard process on the target (the supervisor first, so it restarts nothing)."""
    names = "lg-supervisor lg-meter lg-controller lg-sensor lg-plant lg-gate"
    cmd = (f"for n in {names}; do for p in $(pidin -p $n -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); "
           "do kill -9 $p; done; done; true")
    try:
        subprocess.run(["ssh", *SSH_OPTS, f"root@{ip}", cmd], capture_output=True, timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired):
        pass


def run_case(ip: str, scenario: str, case: dict, script: bytes) -> CaseResult:
    args = [str(case["name"])] + [str(case[k]) for k in CASE_ARGS] + [scenario]
    remote = "sh -s -- " + " ".join(shlex.quote(a) for a in args)  # the remote shell parses this line
    timeout = int(case["end_ms"]) / 1000 + 180
    try:
        out = subprocess.run(
            ["ssh", *SSH_OPTS, f"root@{ip}", remote],
            input=script,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or b"").decode("utf-8", "replace")
        res = parse_output(case["name"], -1, partial)
        res.error = f"timeout after {timeout:.0f} s"
        res.passed = False
        cleanup_target(ip)  # the case script did not get to its own cleanup
        return res
    except OSError as exc:  # ssh missing or not startable
        res = CaseResult(name=case["name"], exit_code=-1, passed=False)
        res.error = f"could not start ssh: {exc}"
        return res
    text = out.stdout.decode("utf-8", "replace") + out.stderr.decode("utf-8", "replace")
    return parse_output(case["name"], out.returncode, text)


def report(campaign: dict, path: Path, env: dict[str, str], cases: list[dict], results: list[CaseResult]) -> str:
    passed = sum(1 for r in results if r.passed)
    lines = [
        f"# Campaign report: {campaign['name']}",
        "",
        f"{campaign.get('step', '')}",
        "",
        f"Result: **{passed} of {len(results)} cases passed**. Wrong-side events must be 0 in every case.",
        "",
        "VirtualBox VM timings: functional evidence only, not hardware performance.",
        "",
        "## Environment",
        "",
        "| Item | Value |",
        "|---|---|",
    ]
    lines += [f"| {k} | {v} |" for k, v in env.items()]
    lines += [
        f"| campaign | `{path.as_posix()}` |",
        f"| scenario | `{campaign['scenario']}` |",
        f"| seeds | {campaign.get('seeds', 'not stated')} |",
        "",
        "## Cases",
        "",
        "| Case | Result | Checks | Max level | Tickets | Level trail | Recovery from injection (ms) |"
        " Recovery from clear (ms) | Supervisor recovery (ms) | Trains | Wrong-side | Closure per hour (ms) |"
        " Late ticks |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        g = r.results.get
        lines.append(
            f"| {r.name} | {'PASS' if r.passed else 'FAIL'} | {r.checks - len(r.failed_checks)}/{r.checks} |"
            f" {g('max_level', '-')} | {g('tickets', '-')} | {g('level_trail', '-')} | {g('recovery_inject_ms', '-')} |"
            f" {g('recovery_clear_ms', '-')} | {g('supervisor_recovery_ms', '-')} | {g('trains', '-')} |"
            f" {g('wrong_side_events', '-')} | {g('closure_ms_per_hour', '-')} | {g('late_ticks', '-')}/{g('ticks', '-')} |"
        )
    lines += ["", "## Case definitions", "", "| Case | Sensor | Fault | Inject (ms) | Clear (ms) | End (ms) | Expected |",
              "|---|---|---|---|---|---|---|"]
    for c in cases:
        lines.append(
            f"| {c['name']} | {c['sensor']} | {c['fault']} | {c['inject_ms']} | {c['clear_ms']} | {c['end_ms']} |"
            f" level {c['expect_level']}, {c['expect_trains']} trains, ticket {c['expect_diag'].replace('|', ' or ')} |"
        )
    failed = [r for r in results if not r.passed]
    if failed:
        lines += ["", "## Failed checks", ""]
        for r in failed:
            lines.append(f"- **{r.name}** (exit {r.exit_code}){': ' + r.error if r.error else ''}")
            lines += [f"  - {f}" for f in r.failed_checks]
    lines += ["", "Raw output of every case: the `.txt` file next to this report.", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Run a LintasGuard fault campaign on the QNX target.")
    ap.add_argument("campaign", type=Path, help="campaign JSON, for example campaigns/sensor_faults.json")
    ap.add_argument("--ip", required=True, help="IP address of the QNX VM (mkqnximage --getip)")
    ap.add_argument("--cases", nargs="*", default=None, help="run only these case names")
    ap.add_argument("--report", type=Path, default=None, help="report path (.md); raw output goes next to it (.txt)")
    a = ap.parse_args(argv)

    campaign = json.loads(a.campaign.read_text(encoding="utf-8"))
    cases = campaign["cases"]
    if a.cases:
        unknown = set(a.cases) - {c["name"] for c in cases}
        if unknown:
            print(f"unknown case(s): {', '.join(sorted(unknown))}", file=sys.stderr)
            return 2
        cases = [c for c in cases if c["name"] in a.cases]
    script = CASE_SCRIPT.read_bytes().replace(b"\r\n", b"\n")
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    rep = a.report or Path(tempfile.gettempdir()) / f"lintasguard-{campaign['name']}-{stamp}.md"
    raw = rep.with_suffix(".txt")

    env = environment(a.ip)
    print(f"campaign {campaign['name']}: {len(cases)} case(s) on {a.ip}, commit {env['commit']}")
    results: list[CaseResult] = []
    with raw.open("w", encoding="utf-8", newline="\n") as rawf:
        for c in cases:
            print(f"  {c['name']} (plant time up to {int(c['end_ms']) // 1000} s) ...", flush=True)
            r = run_case(a.ip, campaign["scenario"], c, script)
            results.append(r)
            rawf.write(f"===== {r.name} (exit {r.exit_code}{', ' + r.error if r.error else ''})\n{r.output}\n")
            rawf.flush()
            summary = "PASS" if r.passed else f"FAIL ({len(r.failed_checks)} check(s)){' ' + r.error if r.error else ''}"
            print(f"    {summary}: max_level={r.results.get('max_level', '-')} tickets={r.results.get('tickets', '-')}"
                  f" recovery_inject_ms={r.results.get('recovery_inject_ms', '-')}"
                  f" wrong_side={r.results.get('wrong_side_events', '-')}")
    rep.write_text(report(campaign, a.campaign, env, cases, results), encoding="utf-8", newline="\n")
    passed = sum(1 for r in results if r.passed)
    print(f"{passed} of {len(results)} case(s) passed. Report: {rep}  Raw output: {raw}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
