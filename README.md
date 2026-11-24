# LintasGuard

**A fail-operational railway level crossing controller for QNX 8.0: it warns before sensors fail, keeps working when they do, and closes the road only as a last resort.**

> Entry for the **QNX Global Hackathon 2026**, category **Automotive & Mobility**.
> Bonus target: **Best use of QNX-specific Technologies**.
> Status: *planning / pre-implementation*. This README is the project specification and the build plan.
> **Prototype only. This is not certified railway signalling equipment and must never be used to protect a real crossing.**

---

## Table of contents

0. [How to use this README (for AI agents and humans)](#0-how-to-use-this-readme-for-ai-agents-and-humans)
1. [The problem](#1-the-problem)
2. [What LintasGuard does](#2-what-lintasguard-does)
3. [The five operating levels](#3-the-five-operating-levels)
4. [System architecture](#4-system-architecture)
5. [Why QNX: feature mapping](#5-why-qnx-feature-mapping)
6. [Repository layout](#6-repository-layout)
7. [Interfaces and protocols](#7-interfaces-and-protocols)
8. [Core algorithms](#8-core-algorithms)
9. [Step-by-step build guide](#9-step-by-step-build-guide)
10. [Integration guide](#10-integration-guide)
11. [Testing strategy](#11-testing-strategy)
12. [Demo script (video under 3 minutes)](#12-demo-script-video-under-3-minutes)
13. [Timeline (20 Nov to 15 Dec 2026)](#13-timeline-20-nov-to-15-dec-2026)
14. [Stretch goals](#14-stretch-goals)
15. [Risks and mitigations](#15-risks-and-mitigations)
16. [Hackathon submission checklist](#16-hackathon-submission-checklist)
17. [Rules for contributors and AI agents](#17-rules-for-contributors-and-ai-agents)
18. [References](#18-references)
19. [License](#19-license)

---

## 0. How to use this README (for AI agents and humans)

This file is the **single source of truth** for the project. If you are an AI coding agent:

1. Read sections 1 to 8 before writing any code. They define *what* to build and *why*.
2. Follow section 9 (build guide) **in order**. Each step lists its inputs, outputs and a "done when" check.
3. Never skip the "done when" check. If a check cannot be run (for example, the QNX SDP is not installed), say so explicitly and record it in `evidence/STATUS.md`. **Do not claim something runs on QNX unless it was actually run on QNX.**
4. Keep the **portable core** (`core/`) free of QNX-specific calls so it can be unit-tested on a normal Linux/Windows host.
5. Respect the priorities in section 13: finish the **MVP cut line** before touching anything marked *stretch*.
6. Section 17 lists hard rules (safety wording, licensing, evidence). They override everything else.

Terminology used everywhere:

| Term | Meaning |
|---|---|
| Crossing | A railway level crossing (road and track at the same level). In Indonesian: *perlintasan sebidang*. |
| Strike-in point | Location on the track, before the crossing, where an approaching train is first detected. |
| Island | The section of track that crosses the road. |
| Approach sensor | A sensor at the strike-in point. Each direction has two independent ones (A and B). |
| Island sensor | Detects whether a train is occupying the island. Used to confirm the train has cleared. |
| Gate | The barrier (*palang*), plus warning lights and bell. |
| Right-side failure | A failure that leaves the crossing in the safe state (gate down) even when no train is coming. Safe, but causes delays and erodes trust. |
| Wrong-side failure | A failure that leaves the crossing open while a train is coming. **Must never happen.** |
| Level | The current degradation level of the system (0 to 4, see section 3). |
| Plant | The simulated physical world: trains, track, sensors' ground truth. |

---

## 1. The problem

### 1.1 Level crossings kill people, especially at unguarded ones

Indonesia (data from PT KAI, reported in national media):

- About **3,703 level crossings** nationwide; **901** of the official ones are still **unguarded**.
- **1,808 accidents and 1,522 deaths since 2020** at level crossings.
- Around **81%** of accidents happen at unguarded locations.
- Up to 22 June 2026 there were **134 accidents**, and **88%** were caused by road users **running the crossing**.

### 1.2 "Just keep the gate down" is not a good enough failure strategy

The classic fail-safe answer to any fault is: lower the gate and keep it down. It prevents wrong-side failures, but research and field reports show it has a cost:

- Willful non-compliance at crossings is linked to the **perception that the warning is unreliable**, caused by false activations (US FRA research).
- The longer the warning time, the more violations occur (US National Academies human factors guidelines, 2025).
- Where false activations are frequent, drivers and pedestrians start **ignoring the warning and driving around lowered gates** (ABC13 Houston report).

So a crossing that closes for hours after a single sensor fault teaches road users to ignore it, which feeds directly into the "88% ran the crossing" statistic above.

### 1.3 Existing solutions

- Commercial remote condition monitoring products exist for level crossings (for example TraxSentinel, Tracsis RCM, DB InfraGO's Cam.LevelCrossing). They are **add-on monitoring systems**, typically proprietary and costly.
- Open-source level crossing controllers that combine **control + health prediction + graded degradation** on a real-time OS were **not found** during our research (as of 20 Nov 2026).

**LintasGuard's position:** an open-source, low-cost controller where health monitoring and graded fallback are *built into* the controller itself, aimed at the hundreds of crossings that today have no protection at all.

---

## 2. What LintasGuard does

LintasGuard is a set of QNX 8.0 processes that together control one level crossing:

1. **Detects trains** with two independent approach sensors per direction plus island sensors.
2. **Controls the gate** (barrier motor, lights, bell) with a guaranteed reaction deadline.
3. **Monitors the health** of every sensor, the gate motor and the backup battery, and predicts failures **before** they happen.
4. **Degrades gracefully** through five levels (section 3) instead of jumping straight to "gate down forever".
5. **Mitigates faults immediately**: restarts crashed drivers, switches to redundant sensors, uses the train timetable and station messages when track sensors are unreliable, and notifies maintenance staff, the station and the train driver.
6. **Proves its behaviour**: every run produces a log and metrics (warning time, closure time, recovery time), and a fault-injection campaign produces a pass/fail report.

Everything runs on **QNX 8.0 in a VirtualBox x86_64 virtual machine on a Windows host** (no hardware needed; see section 9.0 for the exact environment). A host-side web dashboard visualises the crossing, the trains, the system level and notifications.

### 2.1 Non-goals

- Not a certified safety product. No SIL / EN 50126/50128/50129 claims.
- No real train or real road traffic is ever controlled.
- No claims comparing QNX performance against other operating systems (see section 17).

---

## 3. The five operating levels

The heart of the project. The **mode manager** in `lg-controller` owns the current level. Transitions are driven by the health monitor, the supervisor and the voting logic.

| Level | Name | Entry condition | Automatic actions | Road impact |
|---|---|---|---|---|
| **0** | Normal | All components healthy | Normal detection and gate cycle | Normal |
| **1** | Early warning | Any component's health score below the *warning* threshold, but still functional | Notify maintenance with diagnosis and trend; increase health sampling rate | None |
| **2** | Degraded, redundant | One approach sensor in a direction declared failed, the other healthy; **or** a driver process crashed and is being restarted | Isolate the failed sensor; operate on the healthy one; attempt driver restart; add extra safety margin (activate earlier); urgent maintenance ticket | Almost none |
| **3** | Uncertain detection | No trustworthy approach sensor in a direction, but timetable and station messages are available | Close the gate in **timetable windows** around each expected train; request station to warn train drivers (reduce speed, sound horn); dispatch a crossing guard; show "crossing fault, take care" on the road sign | Gate closes per expected train, not permanently |
| **4** | Fail-safe | No trustworthy detection at all (sensors failed **and** timetable/station unavailable), **or** restart budget exhausted, **or** controller cannot verify its own state | Gate down, lights flashing, bell on; notify everyone; wait for an authorised guard | Gate stays down until a guard takes over |

### 3.1 Transition rules

- **Escalation is immediate**; **de-escalation requires stability** (condition clear for `T_stable`, default 30 s simulated) to avoid oscillation.
- From level 4, the system **never** returns to a lower level on its own. It needs an **authorised guard confirmation** (`lgctl guard-ack`) *and* healthy sensors.
- Opening the gate under uncertainty is **never automatic**. In level 3, the gate opens after a timetable window only when the island sensor confirms the train passed, or a guard confirms.
- On boot, the system starts in level 4 (gate down) and descends only when every component has reported healthy (section 8.6).

### 3.2 State diagram

```
             health warning               sensor failed / driver crash
   +-----+  ---------------->  +-----+  ------------------------------>  +-----+
   |  0  |                     |  1  |                                   |  2  |
   +-----+  <----------------  +-----+  <------------------------------  +-----+
      ^      stable 30 s          |        restart ok + stable 30 s         |
      |                           |                                         | no trustworthy approach sensor
      |                           +-----------------------------------------+ (timetable available)
      |                                                                     v
      |                                                                  +-----+
      |                                                                  |  3  |
      |                                                                  +-----+
      |                                                                     | timetable/station lost,
      |   guard-ack + all healthy + stable                                  | restart budget exhausted,
      +--------------------------------------------- +-----+  <-------------+ self-check failure
                                                     |  4  |  <---- boot starts here
                                                     +-----+
```

---

## 4. System architecture

### 4.1 Process overview

All processes below run on the QNX 8.0 target. Only the dashboard and the station simulator run on the host.

```
                QNX 8.0 target (VirtualBox x86_64 VM)
  +-------------------------------------------------------------------------+
  |                                                                         |
  |   lg-supervisor (prio 55)  spawns, watches heartbeats, restarts         |
  |        |  heartbeat pulses from every process                           |
  |        v                                                                |
  |   +-----------+   samples    +----------------+   commands   +---------+|
  |   | lg-sensor | -----------> | lg-controller  | -----------> | lg-gate ||
  |   | x6 (45)   |  (msg pass)  | (50, 10 ms tick)|  (resmgr)   |  (48)   ||
  |   +-----------+              |  mode manager   | <---------- +---------+|
  |      ^  /dev/lg/sensor/...   |  voting         |   state      /dev/lg/gate
  |      |                       +----------------+                         |
  |   ground truth                  ^        |  events, metrics             |
  |   +-----------+                 |        v                              |
  |   | lg-plant  |     health      |   +-----------+                       |
  |   | (40)      |     verdicts    |   | lg-bridge | ---- TCP ----+        |
  |   +-----------+                 |   | (10)      |              |        |
  |                     +-----------+   +-----------+              |        |
  |                     | lg-health |        ^                     |        |
  |                     | (20)      |   +-------------+            |        |
  |                     +-----------+   | lg-schedule |            |        |
  |                                     | (15)        |            |        |
  |   lgctl (CLI): fault injection,     +-------------+            |        |
  |   guard-ack, status                                            |        |
  +----------------------------------------------------------------|--------+
                                                                   |
                   Host (Windows 10/11)                            v
  +-------------------------------------------------------------------------+
  |  dashboard (Python FastAPI + WebSocket + HTML/JS canvas)                |
  |  station simulator (sends "train departed" messages)                    |
  |  notification sink (maintenance / station / guard inboxes)              |
  +-------------------------------------------------------------------------+
```

### 4.2 Process responsibilities

| Process | Priority (SCHED_FIFO) | Responsibility |
|---|---|---|
| `lg-supervisor` | 55 | Spawns all other processes in the right order, receives heartbeat pulses, restarts crashed or hung processes within a restart budget, reports to the controller. |
| `lg-controller` | 50 | 10 ms periodic tick. Reads sensor samples, runs voting, owns the mode manager (levels 0 to 4), commands the gate, emits events and metrics. **The only process allowed to command the gate.** |
| `lg-gate` | 48 | Resource manager `/dev/lg/gate`. Simulates the barrier motor (position, current, travel time), lights and bell. Accepts commands only from the controller. Supports fault injection (slow motor, stuck, high current). |
| `lg-sensor` | 45 | One binary, six instances: approach A and B for West and East, island West and East. Resource manager `/dev/lg/sensor/<dir>/<id>`. Turns ground truth from the plant into samples, with configurable noise and faults. |
| `lg-plant` | 40 | Simulated world: trains (position, speed, length, axle count) from a scenario file; publishes ground truth to the sensors; reports the true train position for metrics only (the controller never sees it). |
| `lg-health` | 20 | Computes a health score per component from statistics (noise, dropouts, response delay, A/B disagreement, motor travel time and current, battery voltage). Sends verdicts (OK / WARNING / FAILED) and diagnoses to the controller. |
| `lg-schedule` | 15 | Holds the timetable and station messages; answers "when is the next train expected in direction X?" Used in level 3. |
| `lg-bridge` | 10 | TCP link to the host: streams state, events and metrics; receives station messages and dashboard commands (read-only commands only; no gate control from the network). |
| `lgctl` | n/a | Command line tool: inject faults, kill processes, guard acknowledge, print status. |

### 4.3 Data flow for one train (level 0)

1. `lg-plant` moves the train; it reaches the West strike-in point.
2. `lg-sensor` West-A and West-B report `occupied=1` (with their own noise and delay).
3. `lg-controller` (next 10 ms tick) sees at least one healthy sensor occupied, so it **activates**: lights and bell on immediately, gate lowering after the pre-warning time.
4. `lg-gate` lowers the barrier and reports position until `DOWN`.
5. The train enters and leaves the island; island sensor goes 1 then 0, and axle counts in and out match.
6. `lg-controller` raises the gate; lights and bell off.
7. `lg-bridge` sends the events and metrics (warning time, closure time) to the dashboard.

---

## 5. Why QNX: feature mapping

This mapping is the backbone of the *Best use of QNX-specific Technologies* bonus. Every item must have a **reason tied to the crossing problem**, a **code location**, and **evidence**.

| QNX capability | Where | Why the crossing needs it | Evidence to produce |
|---|---|---|---|
| Native message passing (`MsgSend` / `MsgReceive` / `MsgReply`) | sensor to controller, controller to gate, health to controller | Synchronous, bounded, priority-inherited communication between safety-relevant parts | Code references + trace of one train cycle |
| Pulses (`MsgSendPulse`, timer pulses) | heartbeats, 10 ms controller tick, health notifications | Non-blocking liveness signals; a periodic tick that does not drift | Heartbeat log; tick jitter histogram |
| Resource managers (`resmgr_attach`, `iofunc_*`, `devctl`) | `/dev/lg/sensor/...`, `/dev/lg/gate` | Each device is an independent driver process with a file-like interface, inspectable from the shell (`cat /dev/lg/gate`) | Shell session showing reads and devctl fault injection |
| Microkernel isolation and process restart | supervisor + every driver | A crashed sensor driver must not take down the crossing; it must come back on its own | Fault campaign: kill sensor driver, measure detection and recovery time, gate never wrong-side |
| Real-time priority scheduling (`SCHED_FIFO`, priority inheritance) | all processes | The decision to close the gate must meet its deadline even under heavy CPU load from monitoring, logging or networking. Measured on QNX 8.0: a server thread on a default channel takes the priority of the sender, even a lower one, so servers that must keep their own priority (the gate) use `_NTO_CHF_FIXED_PRIORITY` (see `docs/ARCHITECTURE.md`, `evidence/step05_priority.txt`) | Run with a CPU hog process; controller deadline misses = 0 |
| High-resolution timing (`ClockCycles`, `CLOCK_MONOTONIC`) | controller, metrics | Measure warning time, reaction time and recovery time precisely | Metrics CSV and plots |
| Name service (`name_attach` / `name_open`) | all processes | Restarted processes are found again without hard-coded IDs | Restart test shows clients reconnect |
| Client credentials / access control | `lg-gate` | Only the controller may command the gate; the network bridge must be unable to, even if compromised | Test: `lgctl` or `lg-bridge` writing to the gate gets `EPERM` |
| Security policies (stretch, verify tooling in SDP 8.0) | image build | Run non-safety processes with reduced abilities | Policy file + denial log |
| Startup / boot configuration (`mkqnximage`, startup script) | image | On power-up the crossing starts in the safe state and the supervisor launches everything in order | Boot log: gate DOWN before any other activity |
| System logging (`slog2`) | all processes | Durable, structured event log for incident analysis | `slog2info` excerpt in evidence |

---

## 6. Repository layout

```
lintasguard/
├── README.md                  # this file
├── LICENSE                    # Apache-2.0 (or MIT), must be detectable by GitHub
├── docs/
│   ├── ARCHITECTURE.md        # deeper design notes, diagrams
│   ├── LEVELS.md              # full state machine spec and transition table
│   ├── QNX_EVIDENCE.md        # "where QNX is used": files, functions, outputs (required by the rules)
│   ├── SAFETY_NOTES.md        # assumptions, limits, what is NOT claimed
│   └── CODELAB.md             # stretch: tutorial for the codelab bonus
├── common/
│   └── include/
│       ├── lg_proto.h         # message types and structs shared by all processes
│       ├── lg_names.h         # name_attach names, /dev paths, priorities
│       └── lg_config.h        # compile-time defaults (timings, thresholds)
├── core/                      # PORTABLE C11, no QNX headers, unit-tested on the host
│   ├── include/
│   ├── voting.c               # 1oo2 activation, clearing logic, axle count check
│   ├── mode_manager.c         # levels 0..4, transitions, hysteresis
│   ├── health.c               # EWMA, CUSUM, dropout rate, disagreement, scores
│   ├── timetable.c            # expected arrival windows for level 3
│   └── metrics.c              # warning time, closure time, recovery time
├── target/                    # QNX 8.0 processes (use core/ + QNX APIs)
│   ├── lg-supervisor/
│   ├── lg-controller/
│   ├── lg-gate/
│   ├── lg-sensor/
│   ├── lg-plant/
│   ├── lg-health/
│   ├── lg-schedule/
│   ├── lg-bridge/
│   ├── lgctl/
│   └── tools/lg-hog/          # CPU load generator for priority tests
├── host/
│   ├── dashboard/             # FastAPI + WebSocket + static HTML/JS
│   ├── station_sim/           # sends "train departed" messages
│   └── campaign_runner/       # runs fault campaigns on the VM over ssh, builds reports
├── config/
│   ├── crossing.json          # geometry: strike-in distance, island length, speeds
│   ├── timetable.json         # expected trains (GAPEKA-like)
│   └── thresholds.json        # health thresholds, timings
├── scenarios/                 # train sequences for lg-plant
│   ├── normal_day.json
│   ├── slow_freight.json
│   └── two_trains_crossing.json
├── campaigns/                 # fault injection campaigns
│   ├── sensor_faults.json
│   ├── gate_faults.json
│   └── full_campaign.json
├── tests/
│   ├── unit/                  # host unit tests for core/
│   ├── scenario/              # deterministic host replays of core/ logic
│   └── target/                # on-target integration test scripts
├── image/
│   ├── mkqnximage/            # image options, startup script, file list
│   ├── build_vm.cmd           # mkqnximage --type=vbox --arch=x86_64 in E:\code\Heketon\QNX\qnx-target
│   └── run_vm.cmd             # boot / stop the VirtualBox target, print its IP
├── scripts/
│   ├── build_host.sh          # run from Git Bash
│   ├── build_target.sh        # run from Git Bash after env is loaded
│   ├── deploy.sh              # copy binaries to the VirtualBox target over scp
│   └── collect_evidence.sh
├── evidence/                  # REAL captured outputs only (logs, CSV, screenshots)
│   └── STATUS.md              # what was verified, where, when, commit hash
└── Makefile                   # top-level: host, target, test, image, evidence
```

---

## 7. Interfaces and protocols

All shared definitions live in `common/include/lg_proto.h`. Keep structs fixed-size and versioned.

### 7.1 Message header

```c
#define LG_PROTO_VERSION 1

typedef struct {
    uint16_t type;      /* LG_MSG_* */
    uint16_t version;   /* LG_PROTO_VERSION */
    uint32_t seq;       /* per-sender sequence number, detects loss */
    uint64_t t_ns;      /* CLOCK_MONOTONIC timestamp at send */
} lg_hdr_t;
```

### 7.2 Message types (initial set)

| Type | Direction | Payload (summary) |
|---|---|---|
| `LG_MSG_SENSOR_SAMPLE` | sensor to controller | `dir`, `id`, `occupied`, `quality` (0..100), `axle_count`, `raw_level` |
| `LG_MSG_GATE_CMD` | controller to gate | `cmd` (LOWER, RAISE, LIGHTS_ON, LIGHTS_OFF, BELL_ON, BELL_OFF), `deadline_ms` |
| `LG_MSG_GATE_STATE` | gate to controller | `state` (UP, LOWERING, DOWN, RAISING, FAULT), `position_deg`, `current_ma`, `travel_ms` |
| `LG_MSG_HEALTH_VERDICT` | health to controller | `component_id`, `score` (0..100), `verdict` (OK, WARNING, FAILED), `diagnosis_code` (`lg_diag_t`), `trend_s_to_failure` (seconds of simulated time, -1 unknown), `flags` (`LG_HV_ASSESSED`: the verdict rests on data or a maintenance reset; an unassessed OK never replaces an earlier WARNING or FAILED) (`LG_PROTO_VERSION` 5, Step 11) |
| `LG_MSG_PROC_STATUS` | supervisor to controller | `proc_id`, `state` (RUNNING, RESTARTING, DEAD), `restarts_in_window` |
| `LG_MSG_SCHED_QUERY` / `LG_MSG_SCHED_REPLY` | controller to schedule | next expected train per direction: `eta_ns`, `confidence`, `source` (TIMETABLE, STATION) |
| `LG_MSG_EVENT` | controller to bridge | level change, activation, clear, notification, metric |
| `LG_MSG_GUARD_ACK` | lgctl to controller | guard id, PIN hash, requested action |

Pulse codes (8-bit): `LG_PULSE_TICK`, `LG_PULSE_HEARTBEAT`, `LG_PULSE_HEALTH_RECHECK`, `LG_PULSE_SHUTDOWN`.

### 7.3 Resource manager paths

| Path | read() returns | write() accepts | devctl |
|---|---|---|---|
| `/dev/lg/sensor/west/a` (and `west/b`, `east/a`, `east/b`, `west/island`, `east/island`) | latest sample as text (`occupied=1 quality=93 axles=8`) | not supported | `DCMD_LG_SENSOR_FAULT` (set fault mode), `DCMD_LG_SENSOR_STATS` (noise, dropouts) |
| `/dev/lg/gate` | gate state as text | not supported (commands go via message passing) | `DCMD_LG_GATE_FAULT`, `DCMD_LG_GATE_STATS` (movements, peak current, battery voltage since Step 11), `DCMD_LG_GATE_BATTERY` (battery drain) |
| `/dev/lg/health` (Step 11) | one line per component: score, verdict, diagnosis, trend and the statistics behind them | not supported | `DCMD_LG_HEALTH_RESET` (forget a component after a repair; needs a descriptor opened for writing) |

Text reads make the system inspectable from the QNX shell, which is great for the demo video and for judges.

### 7.4 Sensor fault modes (for injection)

`NONE`, `NOISE(level)`, `DROPOUT(rate)`, `STUCK_OCCUPIED`, `STUCK_CLEAR`, `DELAY(ms)`, `DRIFT(rate)` (gradual degradation, used to demonstrate early warning), `CRASH` (driver calls `abort()`), `HANG` (driver stops sending heartbeats).

### 7.5 Gate fault modes

`NONE`, `SLOW(factor)` (mechanical wear), `HIGH_CURRENT(factor)`, `STUCK_UP`, `STUCK_DOWN`, `CRASH`.

### 7.6 Host protocol (lg-bridge to dashboard)

Newline-delimited JSON over TCP (port 5555 by default), one ASCII line per object. `t` is the target clock in seconds (CLOCK_MONOTONIC, 3 decimals); `plant_ms` in the state line is the scenario time. As built in Step 10 (`core/include/lg_bridge_proto.h`):

```json
{"t":16550.120,"kind":"state","stale":0,"ctl_ok":1,"gate_ok":1,"plant_ok":1,"level":2,"gate":"DOWN","gate_position":90,"lights":1,"bell":1,"active":[1,0],"sensor_failed":1,"tickets":1,"events":3,"stalls":0,"watchdog_trips":0,"plant_ms":105300,"island":1,"trains":[{"id":3,"dir":"west","front_mm":-2000,"rear_mm":-102000}]}
{"t":16550.000,"seq":3,"kind":"level","from":0,"to":2,"reason":"SENSOR_FAILED"}
{"t":16550.000,"seq":2,"kind":"notify","to":"maintenance","severity":"urgent","sensor":"west/a","diag":"MISSED_TRAIN","text":"Sensor west/a: MISSED_TRAIN, urgent maintenance"}
{"t":16551.000,"kind":"metric","name":"warning_time_ms","value":13400}
{"kind":"ack","what":"subscribe"}
{"kind":"error","what":"rejected","reason":"unknown_kind"}
```

- A state line every 100 ms. `level` is `null` when the controller could not be read, `gate` and `gate_position` are `null` when the gate could not be read (never a 0 that reads as level 0 or gate UP). `stale` is 1 when the device reader has not delivered for 1 s.
- Level and notify lines carry the controller's `seq` and time; `reason` is a mode-manager reason code name. A client that connects later first gets the last 32 of them again (a dashboard opened after a ticket still shows it); deduplicate by `kind`, `seq` and `t`.
- Metrics (`warning_time_ms`, `closure_ms_per_hour`, `trains`) are for display only; evidence numbers come from `lg-meter`.
- Since Step 11 a notify line also carries `component` (`west/a` ... `gate`, `battery`; `sensor` is `none` for the gate and the battery) and, for an early warning, `trend_s` (seconds to failure estimated by `lg-health`). A WARNING episode that ends in FAILED adds a controller event `{"t":...,"seq":...,"kind":"metric","name":"lead_time_ms","component":"west/a","value":21900}`; it has a `seq` and is replayed and deduplicated like the other events.

Inbound from the host, a strict whitelist: `{"kind":"subscribe"}` and `{"kind":"station_departure","dir":"west"|"east","eta_s":0..86400}`. Everything else (unknown kinds, extra fields, broken JSON, lines over 256 bytes) is answered with an `error` line and never acted on. **No inbound message can move the gate**: there is no such message type, and `lg-bridge` has no connection to the gate or the controller (checked by `tests/static/check_bridge_no_gate.sh` in `make test`). At most 8 clients; a client that does not read is dropped.

---

## 8. Core algorithms

All in `core/` (portable C, unit-tested on the host).

### 8.1 Activation (1oo2 per direction, safe side)

- A direction is **approaching** if **any healthy** approach sensor (A or B) reports occupied.
- A sensor marked FAILED is ignored; a sensor marked WARNING is still used.
- If **both** sensors in a direction are FAILED, that direction has **no trustworthy detection**, so go to level 3 (timetable available) or level 4.
- In level 2 add `extra_margin_s` (default +5 s) to the pre-warning time.

### 8.2 Clearing (gate may rise only when all hold)

1. The island sensor has been clear for `T_clear` (default 2 s).
2. The axle count in equals the axle count out for that train (when the axle-counting sensor is healthy).
3. No other direction is approaching.
4. Minimum gate-down time elapsed.
5. In level 3: island confirmed clear **or** guard confirmation.

### 8.3 Health scoring (`lg-health`)

Per sensor, over a sliding window:

- **Noise**: variance of `raw_level` while the track is known clear, tracked with an EWMA.
- **Dropouts**: rate of missing samples (sequence gaps or timeouts).
- **Response delay**: time between ground-truth arrival (available in simulation only for evaluation) or the *other* sensor's activation, and this sensor's activation.
- **Disagreement**: how often A and B disagree for longer than `T_disagree`.
- **Drift detection**: CUSUM on noise and delay to detect slow degradation early.

Per gate: travel time (lowering and raising) and peak current vs. baseline; CUSUM for slow mechanical wear.
Battery (simulated): voltage trend; time-to-empty estimate.

Score: weighted combination mapped to 0..100. Verdict: `OK` (score ≥ 70), `WARNING` (40 to 69), `FAILED` (< 40, or hard failures such as stuck, crash, hang). All thresholds in `config/thresholds.json`.

Each verdict carries a human-readable **diagnosis** (for example, "noise rising for 3 days, likely connector corrosion or loose cable") and a **trend** (estimated days to failure), which go straight into maintenance notifications.

**As built (Step 11, `core/health.c`, `target/lg-health`).** `lg-health` reads the cumulative counters of each device (`DCMD_LG_SENSOR_STATS`, `DCMD_LG_GATE_STATS`, read-only) every 200 ms, every 50 ms for a component that is not OK, so every statistic covers all 10 ms samples and no ground truth is used:

- noise: mean |raw level - nominal level of the reading| / 4 per window = noise units; EWMA (1/8) and a one-sided CUSUM (slack 1 unit, alarm at 2 unit-seconds) against a baseline learned in the first 10 s (between 3 and 5 units). Score 100 at the baseline down to 40 at 30 units (FAILED before the noise can read a phantom train at about 37 units);
- dropouts: ticks without a usable sample (none, or quality 0) per mille, EWMA; WARNING from 50, FAILED from 300;
- approach pairs: response delay (rise later than the partner's rise of the same train) and disagreement (the partner completed a plausible run of 0.5 to 20 s and this sensor did not rise; only a sensor that is itself OK can accuse, a sensor stuck occupied accuses nobody; forgotten after one agreeing train);
- gate: travel time and peak current of every completed movement against nominal, EWMA and CUSUM; FAILED at 175 % travel (5 s pre-warning + 7 s travel stays below the 13.3 s of the fastest train) or 200 % current, one such movement is enough;
- battery (simulated in `lg-gate`, `lgctl battery drain`): voltage EWMA, slope, time to empty; WARNING below 600 s or 12.0 V, FAILED below 60 s or 11.0 V.

A CUSUM alarm caps the score at 69: the early warning comes before the level threshold. A verdict gets worse at once and better only after 2 s at a score of 75 or more. The trend is in seconds of simulated time (degradation in the simulation takes seconds, not days). The controller merges the verdicts: the effective verdict of a component is the worse of its own fast rules (silence, staleness, blindness, stuck, gate FAULT) and the `lg-health` verdict, so `lg-health` can only make a component look worse. Level 1 changes nothing in the gate cycle. A component that was OK after the boot release and gets a WARNING raises one WARNING notification with diagnosis and trend; if the episode ends in FAILED the controller also records the metric `lead_time_ms`. `lg-health` silent for 2 s, or not heard yet when installed (`-H`), counts as a lost health monitor (level 2, boot lock kept).

*Stretch:* replace or complement the statistical score with a small learned anomaly model (section 14).

### 8.4 Level 3 timetable windows

- `lg-schedule` merges `config/timetable.json` with live `station_departure` messages (a live message overrides the timetable entry and raises confidence).
- For each expected train: window = `[ETA - lead_s, ETA + tolerance_s]` (defaults 60 s and 180 s).
- Gate goes down at window start; rises only after island confirmation or guard confirmation.
- If no information is available for a direction for longer than `T_info_timeout`, go to level 4.
- Notifications in level 3: station ("crossing X degraded, instruct drivers to slow down and sound horn"), guard dispatch, road sign message.

### 8.5 Supervisor restart policy

- Every process sends `LG_PULSE_HEARTBEAT` every 50 ms.
- Missed heartbeats for 200 ms, or process exit: kill (if hung) and respawn.
- Restart budget: max 3 restarts per process in 10 s. Exceeding the budget sends `PROC_STATUS=DEAD` to the controller, which escalates (level 2, 3 or 4 depending on the process).
- If the **controller itself** dies, the gate process detects missing commands and heartbeats (watchdog inside `lg-gate`, timeout 100 ms) and **lowers the gate on its own**. This is the last line of defence and must be tested.

### 8.6 Boot sequence

1. Startup script launches `lg-gate` first; its initial state is DOWN, lights flashing.
2. `lg-supervisor` starts and spawns: `lg-plant`, sensors, `lg-health`, `lg-schedule`, `lg-controller`, `lg-bridge`.
3. The controller starts in level 4 and descends to level 0 only after every sensor has sent valid samples for `T_boot_stable` and the health monitor reports OK.

### 8.7 Metrics (`core/metrics.c`)

- **Warning time**: lights on until the train reaches the island (from plant ground truth). Target: consistent, not excessive.
- **Closure time**: gate fully down until gate starts rising, per train.
- **Total closure per hour**: the key metric for the "closes only when needed" claim.
- **Recovery time**: fault injected until normal operation resumes.
- **Deadline misses**: controller ticks later than 10 ms + tolerance.
- **Wrong-side events**: train on the island while the gate is not DOWN. **Must always be 0.**

---

## 9. Step-by-step build guide

Each step: **Goal / Do / Done when.** Steps are ordered; do not start step N+1 before step N's check passes (unless marked parallel).

### 9.0 Development environment (the one this project is built on)

| Item | Value |
|---|---|
| Host OS | Windows 10/11 (x86_64) |
| QNX SDP 8.0 | Installed with QNX Software Center at `C:\Users\acern\qnx800` |
| QNX workspace | **`E:\code\Heketon\QNX`**: everything for this project lives here (env script, repo, VM) |
| VirtualBox | `D:\Program Files\Oracle\VirtualBox` (not on the system PATH; added per session by `qnx-env.cmd`) |
| Repository | **`E:\code\Heketon\QNX\lintasguard`** (Git Bash path: `/e/code/Heketon/QNX/lintasguard`) |
| QNX target | **VirtualBox x86_64 VM** built with `mkqnximage --type=vbox --arch=x86_64`, kept in **`E:\code\Heketon\QNX\qnx-target`** |
| Shells | **CMD** to load the QNX environment and run `mkqnximage`; **Git Bash** for `.sh` scripts, `ssh`, `scp` |
| Other tools | VirtualBox, Git for Windows, Python 3.11+ (dashboard), Claude Code (Windows native) |

Why VirtualBox and not QEMU: the `mkqnximage` reference lists VMware and VirtualBox as the supported VM types on a Windows host, and QEMU on Ubuntu. Running the whole stack on Linux/WSL2 with QEMU remains a valid alternative; scripts should not hard-code Windows-only paths where avoidable.

**Start of every work session (CMD):**

```cmd
call E:\code\Heketon\QNX\qnx-env.cmd
cd /d E:\code\Heketon\QNX\lintasguard
claude
```

`E:\code\Heketon\QNX\qnx-env.cmd` is a machine-specific helper kept **outside the repository**. It loads the SDP environment and adds VirtualBox to the PATH of the current CMD window only (the user PATH is already at the Windows length limit, so it must not be edited permanently):

```cmd
@echo off
call C:\Users\acern\qnx800\qnxsdp-env.bat
set "PATH=%PATH%;D:\Program Files\Oracle\VirtualBox"
```

It must be called **before** starting the agent so that `qcc`, `make`, `mkqnximage` and `VBoxManage` are on the PATH. Use a second CMD window, also starting with `call E:\code\Heketon\QNX\qnx-env.cmd`, to run the VM (`cd /d E:\code\Heketon\QNX\qnx-target`, `mkqnximage --run`, `mkqnximage --getip`).

Workspace layout:

```
E:\code\Heketon\QNX\
├── qnx-env.cmd      # per-machine session helper (not in git)
├── lintasguard\     # this repository
└── qnx-target\      # VirtualBox VM built by mkqnximage
```

**Path rules:** no spaces in any project path; keep paths short; use `cd /d` to change drives in CMD; in scripts prefer forward slashes or Git Bash paths (`/e/...`).

**Host to target networking:** find the VM IP with `mkqnximage --getip` (from `E:\code\Heketon\QNX\qnx-target`). `ssh`/`scp` from Git Bash to that IP. `lg-bridge` **listens** on TCP 5555 on the target and the dashboard **connects to `<vm-ip>:5555`**, so no port forwarding is needed as long as the host can reach the VM IP. If it cannot, check the VirtualBox network adapter mode (host-only or bridged) chosen by `mkqnximage`.

**Before the first build, verify (and record in `evidence/STATUS.md`):** `qcc -V`, `make --version` (from the SDP environment), `python --version`, `git --version`, `bash --version` (Git Bash), and that the VM boots and answers `ssh`.

### Step 1: Accounts and tools (Day 1)

- **Goal:** working QNX toolchain and target on the Windows host described in section 9.0.
- **Do:**
  1. myQNX account + free **QNX Everywhere** licence: **done** (licence accepted and deployed to the user's own account).
  2. **QNX SDP 8.0** installed via QNX Software Center (`C:\Users\acern\qnx800`): **done**. Verify: open CMD, run `call E:\code\Heketon\QNX\qnx-env.cmd`, then `qcc -V` lists the QNX compilers and `VBoxManage --version` prints a version.
  3. **VirtualBox** installed at `D:\Program Files\Oracle\VirtualBox`: **done**; create a host-only network with DHCP enabled (Tools > Network > Create) before the first `mkqnximage`. Install **Git for Windows** (provides Git Bash) and **Python 3.11+** if missing.
  4. Create the target VM: in CMD after `call E:\code\Heketon\QNX\qnx-env.cmd`, `mkdir E:\code\Heketon\QNX\qnx-target`, `cd /d E:\code\Heketon\QNX\qnx-target`, `mkqnximage --type=vbox --arch=x86_64 --hostname=lintasguard`, then `mkqnximage --run` (or start the generated VM from the VirtualBox UI). Get its IP with `mkqnximage --getip`.
  5. Build a hello-world with `qcc -Vgcc_ntox86_64 -o hello hello.c`, copy it to the VM with `scp` (from Git Bash), run it over `ssh`.
- **Done when:** hello-world prints inside the QNX VirtualBox target; output saved to `evidence/step1_hello.txt`.

### Step 2: Repository skeleton (Day 1 to 2)

- **Do:** create the public repo **new** (created during the submission period, not a fork), add `LICENSE`, the layout from section 6, `Makefile` targets (`host`, `target`, `test`, `image`, `deploy`, `evidence`), and `evidence/STATUS.md`.
- **Done when:** `make host` and `make test` run (even if tests are empty); `make target` compiles a stub for QNX.

### Step 3: Shared protocol (Day 2)

- **Do:** write `lg_proto.h`, `lg_names.h`, `lg_config.h` per section 7.
- **Done when:** headers compile on host (gcc/clang) and target (qcc) with `-Wall -Wextra -Werror`.

### Step 4: Portable core with unit tests (Day 2 to 5)

- **Do:** implement `core/voting.c`, `core/mode_manager.c`, `core/metrics.c` first; `health.c` and `timetable.c` can start as stubs.
- Write unit tests in `tests/unit/` (a minimal C test framework such as Unity, or plain asserts).
- **Done when:** unit tests cover: 1oo2 activation, clearing rules, every level transition in section 3, hysteresis, level 4 lock-in until guard-ack. All pass on the host.

### Step 5: Gate resource manager (Day 4 to 6)

- **Do:** `lg-gate` as a resource manager at `/dev/lg/gate`; motor simulation (travel time, position, current); message-passing command channel via `name_attach("lg/gate")`; watchdog that lowers the gate if controller commands stop; initial state DOWN; fault injection via devctl.
- **Done when:** on QNX, `cat /dev/lg/gate` shows state; a test client can lower and raise it; killing the client triggers the watchdog and the gate lowers. Save shell output to evidence.

### Step 6: Plant and sensors (Day 5 to 8)

- **Do:** `lg-plant` reads a scenario and moves trains; `lg-sensor` (one binary, instance chosen by arguments) registers `/dev/lg/sensor/<dir>/<id>`, receives ground truth, applies noise and faults, sends samples to the controller.
- **Done when:** with the plant running a scenario, `cat /dev/lg/sensor/west/a` changes as a train passes; fault injection with `lgctl sensor west/a noise 50` visibly changes the samples.

### Step 7: Controller, level 0 and level 4 (Day 7 to 10) **MVP core**

- **Do:** `lg-controller` with a 10 ms timer pulse; reads samples, runs core voting and the mode manager, commands the gate; starts in level 4; implements level 0 cycle.
- **Done when:** `normal_day.json` runs end to end on QNX with gate cycles for every train and **0 wrong-side events**; metrics CSV produced.

### Step 8: Supervisor and restart (Day 9 to 11) **MVP core**

- **Do:** `lg-supervisor` spawns processes in order, heartbeats, restart budget, PROC_STATUS to controller.
- **Done when:** `lgctl kill sensor west/a` causes a restart within the measured recovery time; the controller stays in level 2 during the restart and returns to level 0 after `T_stable`; killing the controller makes the gate watchdog lower the gate.

### Step 9: Level 2, redundancy (Day 10 to 12) **MVP core**

- **Do:** health verdict FAILED for one sensor makes the controller isolate it, apply extra margin, raise an urgent ticket event.
- **Done when:** `campaigns/sensor_faults.json` (stuck, crash, hang on one sensor) passes on QNX: trains still protected, 0 wrong-side events, recovery recorded.

> **MVP cut line.** At this point the project is already a valid, demonstrable submission: real QNX processes, resource managers, message passing, restart, levels 0, 2 and 4, metrics. Commit, tag `v0.1-mvp`, record a rough demo video as a backup.

### Step 10: Bridge and dashboard (Day 11 to 14, can run in parallel with step 9)

- **Do:** `lg-bridge` streams NDJSON over TCP; `host/dashboard` (FastAPI + WebSocket) draws the crossing on an HTML canvas: track, trains, gate angle, lights, current level (color-coded), notification inbox (maintenance, station, guard), live metrics (warning time, closure per hour).
- **Done when:** a scenario on QNX is visible live in the browser on the host.

### Step 11: Level 1, early warning (Day 13 to 16)

- **Do:** implement `core/health.c` (EWMA, CUSUM, dropouts, disagreement, gate travel and current trend, battery); `lg-health` sends verdicts with diagnosis and trend; controller handles level 1 and sends maintenance notifications.
- **Done when:** injecting `DRIFT` on a sensor or `SLOW` on the gate produces a WARNING and a maintenance notification **before** the component reaches FAILED, with traffic unaffected. Lead time (warning before failure) is recorded as a metric.

### Step 12: Level 3, timetable and station (Day 15 to 18)

- **Do:** `lg-schedule` + `core/timetable.c`; `host/station_sim` sends departures; controller level 3 windows; notifications to station, guard, road sign.
- **Done when:** with both West sensors failed, trains from the West are still protected using timetable windows; total closure per hour is far lower than "gate down permanently"; level 4 is entered when schedule info times out.

### Step 13: Access control and priority proof (Day 17 to 19)

- **Do:** `lg-gate` checks client credentials (only the controller's user/process may command); run non-safety processes (`lg-bridge`, `lg-health`, `lg-schedule`) as an unprivileged user; `lg-hog` CPU load test.
- **Done when:** a write attempt from `lg-bridge` or `lgctl` to the gate is denied; under `lg-hog` load the controller records 0 deadline misses (record the numbers as observed, without comparisons to other OSes).

### Step 14: Full fault campaign and evidence (Day 19 to 21)

- **Do:** `host/campaign_runner` executes `campaigns/full_campaign.json` over ssh, collects logs, metrics, and produces `evidence/campaign_report.md` (pass/fail per fault, recovery times, wrong-side count, closure time comparison).
- **Done when:** the report is generated from a real QNX run and committed with the commit hash and date.

### Step 15: Docs, video, submission (Day 22 to 25)

- **Do:** finalize `docs/QNX_EVIDENCE.md`, build and run instructions, record the video (section 12), fill in the Devpost form (section 16).
- **Done when:** checklist in section 16 is fully ticked.

---

## 10. Integration guide

### 10.1 Build

```bash
# Host (core + unit tests + dashboard deps)
make host
make test
pip install -r host/dashboard/requirements.txt

# Target (run `call E:\code\Heketon\QNX\qnx-env.cmd` in CMD first, see 9.0)
make target            # builds all target/ binaries with qcc for x86_64 (and aarch64le optionally)
make image             # rebuilds the VirtualBox image in E:\code\Heketon\QNX\qnx-target with LintasGuard binaries and startup script
```

### 10.2 Run

```bash
image\run_vm.cmd                       # boot the QNX 8.0 VirtualBox target and print its IP
bash scripts/deploy.sh <vm-ip>         # (Git Bash) if not baked into the image: copy binaries over scp
python host/dashboard/app.py           # open http://localhost:8000
python host/station_sim/sim.py         # optional, for level 3
```

Inside the QNX target:

```sh
lg-supervisor -c /etc/lintasguard/crossing.json -s /etc/lintasguard/scenarios/normal_day.json
lgctl status
cat /dev/lg/gate
lgctl sensor west/a fault drift 2       # early warning demo
lgctl kill sensor west/b                # restart demo
lgctl guard-ack --id G01                # leave level 4 after a fail-safe
```

### 10.3 Integration order (and what to check at each join)

| Join | Check |
|---|---|
| sensor ↔ controller | sequence numbers continuous; sample age < 20 ms |
| controller ↔ gate | command-to-motion latency recorded; watchdog fires when commands stop |
| supervisor ↔ all | each process restarts individually; clients reconnect via `name_open` |
| health ↔ controller | verdict changes cause the correct level transitions |
| schedule ↔ controller | level 3 windows match timetable plus station overrides |
| bridge ↔ dashboard | dashboard state equals `lgctl status` at all times |

---

## 11. Testing strategy

| Layer | Where | Tool | What |
|---|---|---|---|
| Unit | host | C test framework | `core/` functions: voting, transitions, health math, timetable windows, metrics |
| Scenario replay | host | `tests/scenario/` | Feed recorded or synthetic sample streams into `core/`; assert level sequences and 0 wrong-side events. Fully deterministic, runs in CI. |
| Target integration | QNX in VirtualBox VM | `tests/target/*.sh` over ssh | Processes, resource managers, message passing, restart, watchdog, access control |
| Fault campaign | QNX in VirtualBox VM | `host/campaign_runner` | All faults in section 7.4 and 7.5, one at a time and in combinations; pass/fail report |
| Timing | QNX in VirtualBox VM | controller metrics + `lg-hog` | Tick jitter, deadline misses, reaction time under load. **Report VM numbers as functional evidence, not as hardware performance.** |
| Long run | QNX in VirtualBox VM | `normal_day.json` x N | Stability over several simulated hours; memory does not grow; no missed trains |

### 11.1 Pass criteria (every campaign run)

- Wrong-side events = **0** (hard fail otherwise).
- Every injected fault is detected (level change or notification) within its documented bound.
- Every recoverable fault recovers to level 0 or 1 without guard action.
- Level 4 is reached only for the faults that are supposed to cause it.
- Total closure per hour in level 2 and 3 scenarios is reported next to the "permanent closure" baseline.

### 11.2 CI

GitHub Actions on every push: host build, unit tests, scenario replays, lint. Target tests run manually (they need the SDP licence) and their outputs are committed to `evidence/`.

---

## 12. Demo script (video under 3 minutes)

Screen layout: dashboard (left), QNX serial/ssh console (right). No copyrighted music. No third-party trademarks.

| Time | Scene | What viewers see |
|---|---|---|
| 0:00 to 0:20 | Problem | Indonesian crossing statistics; "closing forever on every fault teaches drivers to ignore the gate" |
| 0:20 to 0:40 | Normal | Train passes; gate down and up; `cat /dev/lg/gate` in the console |
| 0:40 to 1:05 | Early warning (level 1) | `lgctl ... drift`; health score falls; maintenance notification appears; next train handled normally |
| 1:05 to 1:35 | Sensor crash (level 2) | `lgctl kill sensor west/a`; restart in milliseconds; train still protected; level returns to 0 |
| 1:35 to 2:05 | Both sensors lost (level 3) | Timetable windows; station and guard notifications; gate closes only around trains |
| 2:05 to 2:25 | Controller dies | Gate watchdog lowers the gate on its own (level 4); guard-ack restores |
| 2:25 to 2:50 | Proof | Campaign report: 0 wrong-side events; closure-per-hour chart vs permanent closure; list of QNX features used |
| 2:50 to 3:00 | Close | Repo link, "prototype, not certified" note |

---

## 13. Timeline (20 Nov to 15 Dec 2026)

**Deadline:** 15 Dec 2026, 2:00 pm ET = **16 Dec 2026, 02:00 WIB**. Internal target: submit by **15 Dec 2026, 20:00 WIB** (6 hours of buffer).
Assumed capacity: about 2 to 3 hours on weekdays, 6 to 8 hours on weekends (part-time, alongside an internship).

| Week | Dates | Steps | Milestone |
|---|---|---|---|
| Week 0 | Thu 20 to Sun 23 Nov | 1, 2, 3, start 4 | QNX hello-world in the VirtualBox target; repo skeleton; protocol headers |
| Week 1 | Mon 24 to Sun 30 Nov | 4, 5, 6, start 7 | Core unit tests pass; gate resmgr and sensors run on QNX |
| Week 2 | Mon 1 to Sun 7 Dec | 7, 8, 9, start 10 | **MVP cut line (tag v0.1-mvp) by Sun 7 Dec**; backup demo video |
| Week 3 | Mon 8 to Sat 13 Dec | 10, 11, 12, 13, 14 | Dashboard, early warning, level 3, access control, full campaign report |
| Final | Sun 14 to Mon 15 Dec | 15 | Video, docs, Devpost submission by 15 Dec 20:00 WIB |

Priority order if time runs short (cut from the bottom):

1. Levels 0, 2, 4 + supervisor + gate watchdog + resource managers (**must have**)
2. Dashboard (**must have** for a good video)
3. Level 1 early warning (**should have**, core of the "warn before failure" story)
4. Level 3 timetable mode (**should have**)
5. Access control + CPU load proof (**should have**, bonus prize evidence)
6. Everything in section 14 (**nice to have**)

---

## 14. Stretch goals

Pick only after the MVP cut line, in roughly this order of value per effort:

1. **Codelab** (`docs/CODELAB.md`, bonus prize): "Build a self-healing driver on QNX 8.0: resource manager + heartbeat + supervisor restart, tested in a QNX virtual machine".
2. **Constant warning time**: estimate train speed from the time between strike-in sensors and delay the gate so every train gets a similar warning time; this reduces closure time for slow trains (long closures are a known cause of drivers bypassing barriers).
3. **Obstacle on the crossing**: simulated vehicle stuck on the island (for example a stalled motorbike); the system notifies the station so the approaching train can be stopped.
4. **Learned anomaly detection**: small model trained on simulated sensor data (host), inference on QNX (plain C or ONNX Runtime, which is already ported to QNX); compare against the statistical score.
5. **QNX security policies**: formal policy file restricting abilities of non-safety processes (verify available tooling in SDP 8.0 first).
6. **Multi-crossing view**: several crossings on one line; a central maintenance dashboard ranking crossings by predicted risk.
7. **Real notification channel**: Telegram bot or email from the host bridge (never from the safety processes).
8. **Raspberry Pi 5 target**: run the same image on real hardware with a servo as the barrier and buttons/IR sensors as track sensors.
9. **Indonesian/English bilingual dashboard and road sign messages.**

---

## 15. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| myQNX licence or SDP install delayed | Cannot prove "runs on QNX" | Do Step 1 on day 1; meanwhile develop `core/` and host tests |
| VM timing is not representative of hardware | Weak timing claims | Present timing as functional evidence in the VM; focus claims on behaviour (0 wrong-side, recovery) |
| Windows path or tooling issues (spaces, drive letters, missing bash) | Build scripts fail | Paths without spaces (section 9.0), `cd /d`, Git Bash for scripts, `make` from the SDP environment |
| Scope too big for 3.5 weeks part-time | Unfinished project | Strict MVP cut line on 7 Dec; section 13 priority list |
| Similar idea submitted by others | Lower novelty score | Emphasis on graded degradation + early warning + Indonesian data; execution quality and evidence |
| Licence terms restrict publishing benchmarks | Rule violation | Read the QNX Everywhere licence; avoid any QNX-vs-other-OS performance comparison |
| Unsafe wording ("guarantees safety") | Credibility loss | Section 17 wording rules; `docs/SAFETY_NOTES.md` |
| Network process compromise moves the gate | Design flaw | Gate accepts commands only from the controller; bridge has no command path |

---

## 16. Hackathon submission checklist

From the official rules (verify again before submitting):

- [ ] Public repository on GitHub, GitLab or Bitbucket, **created during the submission period** (after 2 Nov 2026)
- [ ] Open-source licence file detectable in the repository "About" section
- [ ] All source, assets, build and run instructions included
- [ ] Repository **shows** the project builds for / runs on QNX 8.0 (evidence folder, logs, screenshots), not just mentions it
- [ ] Build and run instructions state: QNX version, target (VirtualBox x86_64 VM on a Windows host, optionally RPi 5), toolchain (`qcc`, SDP 8.0)
- [ ] Written description of features and functionality
- [ ] **Evidence of QNX use**: files, functions, outputs (see `docs/QNX_EVIDENCE.md`)
- [ ] Public YouTube video **under 3 minutes**, showing the project running on its target; no copyrighted music or third-party trademarks
- [ ] Category selected: **Automotive & Mobility**
- [ ] All materials in English (or with English translation)
- [ ] (Bonus) Codelab URL, if the codelab was written
- [ ] Project free to test until the end of judging
- [ ] Submitted before **16 Dec 2026, 02:00 WIB** (internal target 15 Dec 20:00 WIB)

---

## 17. Rules for contributors and AI agents

1. **Evidence or it did not happen.** Anything claimed to run on QNX must have a captured output in `evidence/` with date and commit hash. If something only ran on the host, say "host only".
2. **Safety wording.** Never write "safe", "certified", "guaranteed" about the real world. Use "in simulation", "in this campaign", "prototype". Keep the disclaimer at the top of this README.
3. **No performance comparisons** between QNX and other operating systems.
4. **Portable core stays portable.** No QNX headers in `core/`.
5. **Only the controller commands the gate.** No network-reachable code path may move the gate.
6. **Wrong-side events are a hard failure** in every test. Never relax that assertion to make a test pass.
7. **Configuration, not magic numbers.** Timings and thresholds live in `config/` and `lg_config.h`.
8. **Original work.** The repo is new; third-party code only with compatible licences, credited in `docs/THIRD_PARTY.md`.
9. **Keep `evidence/STATUS.md` current**: what works, where it was verified (host or QNX VM), what is not done.
10. **Small, reviewable commits** with clear messages; tag milestones (`v0.1-mvp`, `v0.2-levels`, `v1.0-submission`).

---

## 18. References

Problem data:

- KAI crossing counts and unguarded crossings (2025): https://otodriver.com/berita/2025/ratusan-lintasan-ka-ditutup-untuk-tekan-kecelakaan-fatal-ratedccatal
- 1,808 accidents and 1,522 deaths since 2020: https://otodriver.com/berita/2026/kepedulian-akan-bahayanya-perlintasan-kereta-api-masih-rendah-kepeeagddah
- 134 accidents to 22 June 2026, 88% caused by running the crossing: https://oto.detik.com/berita/d-8549555/miris-88-kecelakaan-kereta-di-perlintasan-dipicu-pengendara-nerobos
- 81% of accidents at unguarded locations (MTI): https://www.jawapos.com/ekonomi/2508220011/kemenhub-ingatkan-pengguna-jalan-waspada-di-perlintasan-sebidang-kereta-api

Why permanent closure is not enough:

- US FRA, motorist behaviour and perceived unreliability of warnings: https://railroads.dot.gov/sites/fra.dot.gov/files/fra_net/2008/rr0904.pdf
- National Academies, Human Factors Guidelines for Road Systems (2025), warning time and violations: https://www.nationalacademies.org/read/29158/chapter/24
- False activations leading people to ignore signals (ABC13 Houston): https://abc13.com/post/malfunctioning-railroad-crossing-signals-east-houston-raise-safety-concerns/19559784/

Condition monitoring background:

- New methods for the condition monitoring of level crossings: https://research.birmingham.ac.uk/en/publications/new-methods-for-the-condition-monitoring-of-level-crossings/
- TraxSentinel (commercial example): https://www.psa.inc/enterprise-iot/level-crossing-monitoring-solution/

QNX:

- Hackathon page and rules: https://qnx.devpost.com/ and https://qnx.devpost.com/rules
- QNX Everywhere (free licence): https://qnx.software/en/developers/get-started/qnx-everywhere
- QNX SDP 8.0 documentation (resource managers, message passing, mkqnximage): https://www.qnx.com/developers/docs/8.0/

---

## 19. License

To be chosen at repository creation (Apache-2.0 recommended). The licence file must be at the repository root so GitHub detects it.
