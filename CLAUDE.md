# CLAUDE.md

Working instructions for AI coding agents (Claude Code and others) on **LintasGuard**.
Read this file at the start of every session. `AGENTS.md` points here.

---

## 1. What this project is (30-second version)

LintasGuard is a **railway level crossing controller prototype running on QNX 8.0** (in a VirtualBox x86_64 VM on a Windows host), built for the **QNX Global Hackathon 2026** (category Automotive & Mobility, bonus target "Best use of QNX-specific Technologies").

Its idea: instead of "any fault, gate down forever", the controller **predicts sensor and gate failures early, keeps working on redundant sensors, falls back to the train timetable and station messages, notifies people**, and closes the road permanently only as a last resort. Five operating levels (0 Normal, 1 Early warning, 2 Degraded-redundant, 3 Uncertain detection, 4 Fail-safe).

**`README.md` is the single source of truth** for requirements, architecture, protocols, algorithms, the 15-step build plan and the timeline. This file tells you *how to work*; the README tells you *what to build*. If they conflict, the README wins for requirements and this file wins for working rules.

It is a prototype. It is **not** certified railway signalling equipment.

---

## 2. Hard rules (never break these)

1. **Never fabricate evidence.** Do not write, edit or "reconstruct" logs, metrics, screenshots or test outputs. Everything in `evidence/` must come from a real run. If you could not run something, say so.
2. **Never claim QNX execution you did not observe.** If the QNX SDP or the VirtualBox QNX target is not available in your environment, build and test on the host only, and record "host only, not verified on QNX" in `evidence/STATUS.md`.
3. **Wrong-side events must be 0.** A wrong-side event is a train on the island while the gate is not DOWN. Never weaken, skip or delete that assertion to make a test pass. If it fails, the code is wrong.
4. **Only `lg-controller` may command the gate.** No code path reachable from the network (`lg-bridge`, dashboard, station sim) may move the gate. Do not add one "for testing".
5. **No automatic gate opening under uncertainty.** In level 3 the gate rises only after island confirmation or an authorised guard confirmation. Level 4 is left only via `guard-ack` plus healthy components.
6. **No QNX-vs-other-OS performance comparisons** anywhere (code comments, docs, video script, commit messages). Report numbers from the QNX VM as functional evidence only.
7. **Safety wording.** Never write that the system is "safe", "certified" or "guarantees" anything about the real world. Use "in simulation", "in this campaign", "prototype".
8. **`core/` stays portable.** No QNX headers or QNX-only calls in `core/`. It must build and pass tests with plain gcc/clang on Linux/Windows.
9. **No secrets in the repo.** No myQNX credentials, licence keys, tokens or personal data.
10. **Stay inside scope order.** Do not start stretch goals (README section 14) before the MVP cut line (README step 9, tag `v0.1-mvp`) is reached, unless the user explicitly asks.

---

## 3. How to work on a task

1. **Locate the task in the README build plan** (section 9, steps 1 to 15). Read the step's Goal / Do / Done when, plus the sections it references (protocol in section 7, algorithms in section 8).
2. **Check `evidence/STATUS.md`** to see what is already done and verified, and where (host or QNX).
3. **Plan briefly**, then implement the smallest change that satisfies the step.
4. **Write or update tests first where possible** (unit tests for `core/`, scripts in `tests/target/` for QNX behaviour).
5. **Run the "Done when" check.** Host checks you can usually run yourself. QNX checks need the SDP environment and a running VirtualBox target; if either is unavailable (VM not booted, env not loaded), prepare the script and tell the user exactly what to run.
6. **Update `evidence/STATUS.md`**: step, what works, verified where, date, commit hash, known gaps.
7. **Commit** with a clear message (see section 9). Tag milestones.
8. **Report back to the user** in short form: what changed, what was verified, what the user must run on QNX, what is next.

When a requirement is ambiguous: pick the option that keeps the crossing in the safer state, note the decision in `docs/ARCHITECTURE.md` (Decisions log), and mention it to the user.

---

## 4. Environment and commands

### 4.1 Environment (Windows host)

| Item | Value |
|---|---|
| Host OS | Windows 10/11 |
| QNX SDP 8.0 | `C:\Users\acern\qnx800` |
| QNX workspace | **`E:\code\Heketon\QNX`**: env script, repo and VM all live here |
| VirtualBox | `D:\Program Files\Oracle\VirtualBox` (added to PATH per session by `qnx-env.cmd`) |
| Repository | **`E:\code\Heketon\QNX\lintasguard`** (Git Bash: `/e/code/Heketon/QNX/lintasguard`) |
| QNX target | **VirtualBox x86_64 VM**, built with `mkqnximage --type=vbox --arch=x86_64` in **`E:\code\Heketon\QNX\qnx-target`** |
| Shells | **CMD** for `qnx-env.cmd` and `mkqnximage`; **Git Bash** for `.sh` scripts, `ssh`, `scp` |
| Target login | `ssh root@<vm-ip>` (default mkqnximage users; password equals the user name; dev-only image) |

`qnx-env.cmd` (outside the repo) calls `C:\Users\acern\qnx800\qnxsdp-env.bat` and appends VirtualBox to the PATH of the current window. **Never edit the user's permanent PATH**: it is already at the Windows length limit. The user starts each session like this, so `qcc`, `make`, `mkqnximage` and `VBoxManage` should already be on your PATH:

```cmd
call E:\code\Heketon\QNX\qnx-env.cmd
cd /d E:\code\Heketon\QNX\lintasguard
claude
```

The VM runs in a separate CMD window (`call E:\code\Heketon\QNX\qnx-env.cmd`, `cd /d E:\code\Heketon\QNX\qnx-target`, `mkqnximage --run`, `mkqnximage --getip`); the user tells you its IP at the start of a session.

If `qcc -V` fails in your shell, the environment was not loaded: stop and ask the user to restart the session with the commands above. Do not try to hard-code SDP paths into the build.

Windows rules:
- No spaces in paths; keep them short. Use `cd /d` to change drives in CMD.
- Write `.sh` scripts for Git Bash and `.cmd` wrappers only where CMD is required (`mkqnximage`, loading the env).
- Line endings: keep `.sh` files LF (add a `.gitattributes` with `*.sh text eol=lf`), otherwise bash on the target and in Git Bash may fail.
- The VM IP comes from `mkqnximage --getip` (run in `E:\code\Heketon\QNX\qnx-target`). `lg-bridge` listens on 5555 on the target; the dashboard connects to `<vm-ip>:5555`.
- Linux/WSL2 + QEMU is a valid alternative host; avoid Windows-only assumptions in `core/`, `target/` and the Makefile where possible.

### 4.2 Commands

```bash
# Host
make host            # build core/ and host tools
make test            # unit tests + scenario replays (must pass before every commit)
make lint            # clang-format check + warnings-as-errors build

# Target (needs `call E:\code\Heketon\QNX\qnx-env.cmd` first, see 4.1)
make target          # qcc build of all target/ binaries (x86_64; aarch64le optional)
make image           # VirtualBox image in E:\code\Heketon\QNX\qnx-target with binaries + startup script
image\run_vm.cmd     # (CMD) boot the VirtualBox target and print its IP
make deploy          # copy binaries to the running target over scp if not baked into the image
make evidence        # run target tests over ssh and collect outputs into evidence/

# Host apps
pip install -r requirements.txt     # fastapi + uvicorn (pinned)
python host/dashboard/app.py --vm <vm-ip>   # http://localhost:8000, read-only view of lg-bridge (TCP 5555)
python tests/host/bridge_check.py <vm-ip>   # Step 10 host check (runs a stuck-clear case on the VM)
python host/station_sim/sim.py      # level 3 station messages
python host/campaign_runner/run.py campaigns/sensor_faults.json --ip <vm-ip>   # Step 9 campaign
python host/campaign_runner/run.py campaigns/full_campaign.json --ip <vm-ip>
```

Inside the QNX target:

```sh
lg-supervisor -c /etc/lintasguard/crossing.json -s /etc/lintasguard/scenarios/normal_day.json
lgctl status
cat /dev/lg/gate
cat /dev/lg/events            # level changes and maintenance tickets
lgctl sensor west/a fault drift 2
lgctl kill sensor west/b
lgctl kill bridge             # lg-bridge restarts; level 1 until T_stable, the gate is not affected
cat /dev/lg/bridge            # bridge clients, drops, rejected inbound lines, replayed events
cat /dev/lg/health            # lg-health: score, verdict, diagnosis, trend per component (lgctl health)
lgctl gate fault slow 150     # gate wear (also high-current <pct>, stuck-up, stuck-down, none)
lgctl battery drain 5         # backup battery drain in mV/s (0 = charger working)
lgctl health reset gate       # after a repair: lg-health forgets what it learned about the gate
lgctl kill health             # lg-health restarts; level 2 for the restart
lgctl guard-ack --id G01
```

If a command in this list does not exist yet, creating it is part of the relevant build step. Keep this list in sync with reality.

---

## 5. Architecture cheat sheet

| Process | Prio (SCHED_FIFO) | Role | Interface |
|---|---|---|---|
| `lg-supervisor` | 55 | spawn, heartbeat watch, restart budget (3 per 10 s) | pulses in, `PROC_STATUS` out |
| `lg-controller` | 50 | 10 ms tick, voting, mode manager (levels), gate commands, events, metrics | `name_attach("lg/controller")`, resmgr `/dev/lg/controller`, `/dev/lg/events` |
| `lg-gate` | 48 | barrier/lights/bell sim, **watchdog lowers gate if controller silent 100 ms**, starts DOWN | resmgr `/dev/lg/gate`, `name_attach("lg/gate")` |
| `lg-sensor` x6 | 45 | approach A/B per direction + island; noise and fault injection | resmgr `/dev/lg/sensor/<dir>/<id>` |
| `lg-plant` | 40 | simulated trains and ground truth (controller never sees ground truth) | scenario JSON |
| `lg-health` | 20 | EWMA/CUSUM health scores, verdicts OK/WARNING/FAILED + diagnosis + trend | `HEALTH_VERDICT` |
| `lg-schedule` | 15 | timetable + station messages for level 3 | `SCHED_QUERY/REPLY` |
| `lg-bridge` | 10 | NDJSON over TCP to the host; **no gate command path** | TCP 5555 |
| `lgctl` | n/a | CLI: faults, kill, guard-ack, status | devctl + messages |

Shared definitions: `common/include/lg_proto.h` (messages), `lg_names.h` (names, paths, priorities), `lg_config.h` (defaults). Runtime config: `config/*.json`. Change a message struct = bump `LG_PROTO_VERSION` and update every sender and receiver in the same commit.

---

## 6. Code conventions

**C (core/ and target/)**
- C11. Build with `-Wall -Wextra -Werror -std=c11`. Fixed-width integer types for anything in a message.
- Prefix public symbols with `lg_`. One process per directory under `target/`, each with its own `main.c` and a small number of modules.
- **Controller loop:** no dynamic allocation after init, no blocking calls other than `MsgReceive`, no logging that can block (buffer it), no floating-point surprises (document units: ms, ns, mm, mm/s).
- Every QNX call that returns an error must be checked; log with `slog2` (target) and continue into the safer state when in doubt.
- Timings and thresholds come from `config/` or `lg_config.h`, never magic numbers inline.
- Format with `clang-format` (config in repo root).

**Python (host/)**
- Python 3.11+. FastAPI + WebSocket for the dashboard, plain HTML/JS canvas for drawing (no heavy frontend framework). Type hints. Keep dependencies minimal and pinned in `requirements.txt`.

**Docs**
- English everywhere in the repo (hackathon rule). Keep `docs/QNX_EVIDENCE.md` updated whenever a QNX feature is added: file, function, why, evidence link.

---

## 7. QNX 8.0 notes and gotchas

Verify details against the SDP 8.0 documentation (https://www.qnx.com/developers/docs/8.0/) before relying on them; do not guess API signatures.

- **Toolchain:** `qcc -Vgcc_ntox86_64` (VirtualBox x86_64 target) or `-Vgcc_ntoaarch64le` (Raspberry Pi). `qnxsdp-env.bat` (Windows) / `qnxsdp-env.sh` (Linux) sets `QNX_HOST` and `QNX_TARGET`.
- **Linux-only APIs do not exist:** no `epoll`, `eventfd`, `timerfd`, `signalfd`, `inotify`, `/proc/<pid>/status` parsing. Use QNX patterns instead: `MsgReceive` loops, pulses, `timer_create` with a pulse `sigevent` for periodic ticks, `ionotify`/`select` for fds.
- **Networking** may need `-lsocket` at link time on QNX.
- **Message passing:** server `name_attach()` then `MsgReceive()` loop; handle system pulses (for example disconnect pulses) as well as your own pulse codes; client `name_open()` and reconnect after a server restart (this is how the restart demo works).
- **Resource managers:** `dispatch_create`, `iofunc_func_init`, `iofunc_attr_init`, `resmgr_attach`, custom `io_read` and `io_devctl` handlers. Attaching a pathname needs the right abilities/privileges; the simplest path in the prototype is to start resmgrs with sufficient privilege and drop what is not needed.
- **Priorities:** set with `pthread_setschedparam` (SCHED_FIFO). Keep all priorities within the range allowed for the user the process runs as; check abilities if a higher priority is refused.
- **Events delivered by other processes:** check whether SDP 8.0 requires event registration for your use case before sending sigevents across processes.
- **Timing:** `ClockCycles()` + `SYSPAGE_ENTRY(qtime)->cycles_per_sec` for fine measurements; `CLOCK_MONOTONIC` for timestamps in messages.
- **Logging:** `slog2` on target; read with `slog2info`.
- **A VM is not real hardware.** Timing numbers from the VirtualBox target are functional evidence only.

---

## 8. Testing rules

- `make test` must pass before every commit. CI runs host build, unit tests and scenario replays.
- Every bug fix comes with a test that fails before the fix.
- Scenario replays and campaigns use **fixed random seeds**; the seed is printed in every report.
- The wrong-side assertion (rule 3) is present in every scenario and campaign check.
- Target tests (`tests/target/*.sh`) print a clear PASS/FAIL line and exit non-zero on failure, so `make evidence` and the campaign runner can parse them.
- Campaign reports go to `evidence/campaign_report.md` with date, commit hash, seed and environment (host OS, VirtualBox version, SDP version).

---

## 9. Git conventions

- Branch per build step: `step-07-controller`, `step-11-health`.
- Commit message: `<area>: <what>` (for example `gate: add watchdog that lowers gate after 100 ms silence`).
- Tags: `v0.1-mvp` (end of step 9, target 7 Dec 2026), `v0.2-levels` (levels 1 and 3), `v1.0-submission`.
- The repository must be **new** (created after 2 Nov 2026), not a fork, with a licence file at the root.

---

## 10. Priorities and deadline

- Hackathon deadline: **15 Dec 2026, 2:00 pm ET = 16 Dec 2026, 02:00 WIB**. Internal submit target: **15 Dec 2026, 20:00 WIB**.
- MVP cut line: **7 Dec 2026** (levels 0, 2, 4 + supervisor + gate watchdog + resource managers + metrics, verified on QNX).
- If time is short, cut from the bottom of README section 13's priority list. Never cut evidence, the wrong-side check or the build/run instructions.

---

## 11. Working with the user

- The user is a solo developer working part-time (internship during weekdays). Prefer small, finishable increments over large rewrites.
- **Chat with the user in natural, casual Indonesian.** Keep code, comments, commit messages and repo docs in English.
- You normally run on the user's own Windows machine, where the SDP and the VirtualBox target exist. If the VM is not running or the env is not loaded, give the exact commands to fix it and what output to expect, then wait for the result before marking a step verified. If you run somewhere without the SDP (for example a cloud session), work host-only and say so.
- Ask before: changing the architecture or protocol, adding a dependency, deleting files, or starting a stretch goal.

---

## 12. Definition of done (per build step)

A step is done only when all of these are true:

- [ ] Code builds without warnings (host, and target if the step touches `target/`)
- [ ] `make test` passes
- [ ] The README step's "Done when" check passed, **on QNX if the step is about QNX behaviour**
- [ ] Real outputs saved in `evidence/` (or the gap recorded in `evidence/STATUS.md`)
- [ ] `docs/QNX_EVIDENCE.md` updated if a QNX feature was added
- [ ] `evidence/STATUS.md` updated
- [ ] Committed (and tagged if it is a milestone)
