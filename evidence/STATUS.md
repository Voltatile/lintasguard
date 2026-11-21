# Evidence status

Updated: 2026-11-21 (Step 8). Commit: see `git log` (first commit = Step 1 and Step 2).

## Step 1: Accounts and tools

| Check | Result | Verified where |
|---|---|---|
| `qcc -V` | targets listed: gcc_ntox86_64 (default), gcc_ntoaarch64le, 12.2.0 | Windows host, SDP 8.0 env loaded |
| `make --version` | GNU Make 4.4.1 (x86_64-w64-mingw32) | Windows host |
| `python --version` | Python 3.13.7 | Windows host |
| `git --version` | git 2.37.0.windows.1 | Windows host |
| `bash --version` | GNU bash 4.4.23 (msys, Git Bash) | Windows host |
| `VBoxManage --version` | 7.2.22r175245 | Windows host |
| QNX VM boots | yes, `mkqnximage --type=vbox --arch=x86_64 --hostname=lintasguard` | VirtualBox VM |
| VM answers ssh | yes, key login as root (image rebuilt with `--ssh-ident`) | VirtualBox VM |
| Hello world on QNX | built with `qcc -Vgcc_ntox86_64`, run on the VM | **QNX 8.0 VM** |

Raw output: `evidence/step1_hello.txt` (`uname -a` on the target and the program output).

VM IP is assigned by DHCP and changed after the rebuild (192.168.56.101 to 192.168.56.102). Always re-check with `mkqnximage --getip`.

## Step 2: Repository skeleton

| Check | Result | Verified where |
|---|---|---|
| `make host` | builds `core/` and unit test with `-Wall -Wextra -Werror -std=c11` | Windows host (gcc 14.2.0, MSYS2) |
| `make test` | `PASS: test_smoke` | Windows host |
| `make target` | `qcc -Vgcc_ntox86_64` builds `build/target/lg-stub` | Windows host, SDP 8.0 |
| `lg-stub` runs | prints `lg-stub 0.0.1-skeleton` (`evidence/step2_stub.txt`) | **QNX 8.0 VM** |
| LICENSE | Apache-2.0 (201 lines) | n/a |

Not done in Step 2: `clang-format` is not installed on this host, so `make lint` skips the format check and only runs the warnings-as-errors build.

## Step 3: Shared protocol

| Check | Result | Verified where |
|---|---|---|
| `common/include/lg_proto.h`, `lg_names.h`, `lg_config.h` compile with `-Wall -Wextra -Werror -std=c11` | yes, via `make test` (gcc 14.2.0) and `make target` (qcc gcc_ntox86_64) | Windows host, SDP 8.0 |
| `gcc -Wpadded` on `lg_proto.h` | no padding reported | Windows host |
| `tests/unit/test_proto.c` | `PASS: test_proto` (header position, ids, priority order, timing relations) | Windows host |
| Stub including the headers runs on target | `lg-stub 0.0.1-skeleton proto=1 hdr=16 event=96 gate=/dev/lg/gate` (`evidence/step3_proto.txt`) | **QNX 8.0 VM** |

Not verified yet: message passing with these structs (first used in Step 5); pulse code range on the target.

## Step 4: Portable core with unit tests

| Check | Result | Verified where |
|---|---|---|
| `core/voting.c`, `mode_manager.c`, `metrics.c` (+ `health.c`, `timetable.c` stubs) | implemented, portable C11, no QNX headers, no dynamic allocation | Windows host |
| `make test` | 7 test binaries pass: `test_proto`, `test_smoke`, `test_voting`, `test_mode_manager`, `test_metrics`, `test_core_extra`, `test_review_fixes` | Windows host (gcc 14.2.0) |
| Mutation check by the `tester` agent | Reported by the agent: 34 of 34 mutants caught before the review fixes; 5 more mutants checked against `test_review_fixes`, all caught. The mutant copies were kept in a scratchpad, not archived in the repo, so this is not reproducible from the repository | Windows host |
| `make target` | core compiles with `qcc -Vgcc_ntox86_64 -Wall -Wextra -Werror` | Windows host, SDP 8.0 |
| Independent review (`reviewer` agent) | 5 defects fixed (clearing without island sighting, boot lock hiding a real level 4, negative elapsed time, unknown enum values read as healthy, shared boot/guard timer), plus the guard-ack latch rule tightened; see `docs/ARCHITECTURE.md` Decisions log | Windows host |

**Host only, not verified on QNX.** Step 4 has no QNX behaviour; nothing here has been run as a QNX process. The core only gets exercised on the target once the controller (Step 7) calls it.

Open items carried to later steps: same-direction approach as a clearing input and sample freshness at boot (Step 7), `LG_T_INFO_TIMEOUT_MS` handling and real timetable logic (Step 12), health statistics (Step 11).

## Step 5: `lg-gate` resource manager

| Check | Result | Verified where |
|---|---|---|
| `core/gate_model.c` (motor, lights, bell, watchdog, faults) | `test_gate_model.c` and `test_gate_model_extra.c` pass on the host (`make test`, 9 test binaries). The extra suite (from the `tester` agent) holds a seeded random walk with invariants. The agent also reported a mutation check of the model (38 mutants, 37 caught, the 1 survivor judged equivalent); the mutant copies are not archived in the repo. Its findings were fixed and re-tested | Windows host |
| `make target` | builds `lg-gate`, `lg-gate-test`, `lg-prio-demo`, `lg-stub` with `qcc -Vgcc_ntox86_64 -D_QNX_SOURCE -Wall -Wextra -Werror -std=c11` | Windows host, SDP 8.0 |
| README check: `cat /dev/lg/gate` shows the state | yes: `state=DOWN position=90 ...`, then UP | **QNX 8.0 VM**, `evidence/step05_gate.txt` |
| README check: a client can lower and raise the gate | yes: raise reaches UP in about 4.0 s, explicit lower reaches DOWN | **QNX 8.0 VM**, `evidence/step05_gate.txt` |
| README check: killing the client makes the gate lower itself | yes: after `kill -9` the gate left UP (seen after 2 polls of the state file), `watchdog_trips=1`, it reached DOWN. The gate logged `no controller command or heartbeat for 109 ms (limit 100 ms)` | **QNX 8.0 VM**, `evidence/step05_gate.txt` |
| `tests/target/step05_gate.sh` | 36 checks, 0 failed (boot DOWN, raise, no write path, watchdog and its measured silence, SLOW fault, devctl refusals, rejected messages, slog2, crash and restart). Each check is a single command; the first version of the script had two checks that tested only part of their condition, found in review and fixed | **QNX 8.0 VM** |
| `tests/target/step05_priority.sh` | 3 checks, 0 failed: default channel pulls a FIFO-48 server down to the client priority 10, `_NTO_CHF_FIXED_PRIORITY` keeps 48 | **QNX 8.0 VM**, `evidence/step05_priority.txt` |

Notes:
- Timing numbers of the recorded run (raise 4004 ms, lowering after the trip 4001 ms, SLOW 150 percent 6005 ms against 4000 and 6000 nominal; trip after 109 ms of silence) come from a VirtualBox VM: functional evidence only, not hardware performance. Other runs are not archived; only the recorded files count.
- The watchdog is checked on a 10 ms tick, so the gate starts lowering 100 to 110 ms after the last keepalive.
- `lg-gate` runs SCHED_FIFO 48 as root. The process read this back itself (`sched_policy=1 sched_priority=48`).

Not done or not claimed in Step 5:
- Only the controller may command the gate: credential checks are Step 13. Today any process can open the message channel and send `GATE_CMD`, and any process can send the heartbeat pulse that feeds the watchdog (a live non-controller process could keep a gate UP). `devctl` fault injection needs a write-opened descriptor, which in practice means root with mode 0644, but is not authenticated. `/dev/lg/gate` has no working write path (`EPERM`).
- `deadline_ms` of `GATE_CMD` is ignored, and the gate state is only returned as a reply (no status query); both are decided before Step 7. Both are in the Decisions log.
- No supervisor: after the crash test the gate was restarted by hand. Automatic restart is Step 8.
- No controller yet: `lg-gate-test` stands in for it.
- No startup script or image baking: the gate is started by hand over ssh.

## Step 6: plant and sensors

| Check | Result | Verified where |
|---|---|---|
| `core/json_mini.c`, `scenario.c`, `plant_model.c`, `sensor_model.c` | host unit tests pass (17 test binaries in `make test`), including a 300,000-case mutation fuzz of the parser and random walks of the models against independent oracles. The `tester` agent also reported a mutation check (69 mutants, 7 survivors, all judged equivalent); the mutant copies are not archived in the repo. Host sanitizers are not available on this toolchain, so there is no automatic memory checking | Windows host |
| `make target` | builds `lg-plant`, `lg-sensor`, `lgctl`, `lg-sample-sink`, `lg-jitter`, `lg-chan-demo` (and the Step 5 binaries) with `qcc -Vgcc_ntox86_64 -D_QNX_SOURCE -Wall -Wextra -Werror -std=c11` | Windows host, SDP 8.0 |
| README check: `cat /dev/lg/sensor/west/a` changes as a train passes | yes: occupied readings at plant time 5234 to 9012 ms for the west train; the model puts the rise at 5001 ms and the fall at 9067 ms, and the script checks that the model time lies inside the interval the trace brackets it with. Axles 0 to 8 | **QNX 8.0 VM**, `evidence/step06_sensors.txt` |
| README check: `lgctl sensor west/a noise 50` visibly changes the samples | yes: raw spread 46 counts healthy, 592 counts with noise 50, quality 97 to 50 | **QNX 8.0 VM** |
| `tests/target/step06_sensors.sh` | 82 checks, 0 failed: idle state, stream, noise, stuck, dropout, hang, drift, refusals and exit codes, crash and restart of one sensor, a silent controller (SIGSTOP), controller gone and a new one found, plant gone and back, two trains (west and east, with a 1000 ms delay on `east/b`), device and worker priority 45 for all six sensors | **QNX 8.0 VM** |
| `tests/target/step06_channels.sh` | 10 checks, 0 failed: `SIGEV_PULSE_PRIO_INHERIT` runs the receiver at the creation priority (10), an explicit priority gives 45; on an ordinary channel another process can attach and its pulse arrives as a tick, on a `_NTO_CHF_PRIVATE` channel the attach is refused | **QNX 8.0 VM**, `evidence/step06_channels.txt` |
| Silent controller (SIGSTOP) | `cat`, `lgctl stats` of `west/a` and a read of `east/island` answered within 2 s while the controller was stopped; the ticks of `west/a` went from 4692 to 4823 in 2 s; after the controller was killed `no_controller` grew from 2498 to 2614 and a new controller was found (`connects` 4 to 5) and received samples | **QNX 8.0 VM** |
| Sample stream (10 s, six sensors, controller stand-in) | 986 to 1000 samples per sensor, `gaps` 0 (1 on `east/a`), `reorder=0`, average sample age 25 to 78 microseconds, largest sample age 669 to 2053 microseconds | **QNX 8.0 VM** |
| Timer baseline (`lg-jitter`, nothing else running, 10 s, real priority 45, same run) | 9 percent of the gaps over 12 ms, 11 of 1000 over 20 ms, largest gap 43249 microseconds | **QNX 8.0 VM** |
| Tick lateness of a sensor | `west/a`: 8 percent of its ticks late in the stream phase, against 9 percent for the bare timer; 93 to 105 overruns per sensor in about 5300 ticks in the second phase | **QNX 8.0 VM** |
| Dropout and hang | dropout 50: 163 of 328 ticks without a sample (49 percent), 185 sequence gaps seen by the sink; hang: `samples_sent` stayed at 1172 while `no_sample` went from 22 to 356, 380 gaps at the sink | **QNX 8.0 VM** |
| Delay fault | `east/b` with delay 1000 ms followed `east/a` by 1072 ms in the trace; the script accepts 1000 minus/plus one round of the trace (the largest round was 312 ms) | **QNX 8.0 VM** |

How this evidence came about (the archived files are the last of several runs):
- The first run had 2 failing checks that were faults of the script (a check made after the sink had finished, a window that ignored the reading skew). Fixed in the script.
- The `reviewer` found that a controller that is alive but silent could freeze the sensor. A manual experiment on the VM (SIGSTOP of the controller) confirmed it twice: first through `name_open`, then through the reply phase. The sensor was rebuilt with a worker thread and connector threads, and the experiment became part of the script.
- After that rebuild the script failed 4 checks that I first put down to VM jitter. A second review then found the real cause of part of it: the worker, and my jitter baseline, ran at priority 10 instead of 45, because a timer pulse made with `SIGEV_PULSE_PRIO_INHERIT` gives the receiver the priority the process was created with. This was measured on the VM (`evidence/step06_channels.txt`), the pulse priority was made explicit, the worker priority is now read back and checked, and the baseline was measured again at a real 45. Numbers from the version that ran at 10 were discarded.
- The same review showed that another process could attach to the worker channel and fake a tick; the channel is now `_NTO_CHF_PRIVATE`, also measured.
- Some checks were changed from fixed numbers to what the code can guarantee: transition times are bracketed by the trace itself, overruns and sequence gaps are compared with the bare-timer baseline of the same run, the average sample age is asserted and the largest one is reported.
- The README 10.3 limit of 20 ms for the sample age was met in the recorded run (largest 2053 microseconds) but cannot be guaranteed on this VM: the bare timer alone had a gap of 43 ms in that run.

Not done or not claimed in Step 6:
- The controller does not exist yet: `lg-sample-sink` plays it. `lgctl` only has the sensor subcommands.
- A server that has received a sensor message and never replies blocks the sensor worker; the device still answers. Step 8 restarts silent processes. The timeout path of `TimerTimeout` was not provoked deterministically (`timeouts=0` in the recorded run).
- Access gaps as for the gate (Step 13), plus: any process can register `lg/plant` or `lg/controller`.
- Open decisions for Step 7 and Step 8 are in the Decisions log (truth for the metrics, island redundancy, HANG versus heartbeat, a plant restart replaying the scenario, how a controller refuses a sample).

## Step 7: `lg-controller` (level 0 and level 4) and `lg-meter`

Commits 9f8c1ec to d1c38e5 on branch `step-07-controller` (base e08420e). The VM run in `evidence/step07_controller.txt` was made from the working tree before it was committed; the code has not changed since (only documentation did).

| Check | Result | Verified where |
|---|---|---|
| `core/controller_logic.c` (verdicts, voting, clearing, gate target, lights) | host unit tests (`test_controller_logic`, `test_controller_logic_extra`, `test_controller_stale`) and the replay below pass in `make test` (21 test binaries). `test_controller_logic_extra` is from the `tester` agent: same-direction hold, two directions overlapping, level 3 and 4, silent gate, wrap of the 32-bit clock, and a seeded property test (seeds 20261120 to 20261123) with five invariants. No mutation check was archived | Windows host (gcc) |
| Host replay `tests/scenario/replay_normal_day.c` (plant, six sensors, gate, controller, seed 20261120) | `normal_day.json`: boot release at about 5 s, 6 trains, 6 gate cycles, 0 wrong-side events. Negative control: with the old 10 s pre-warning the same replay reports wrong-side events (the metric sees them). `test_prewarning_budget` checks pre-warning + gate travel + 1 s against every train. `test_restart_sweep` restarts the controller every 997 ms over the whole day (281 runs): 0 wrong-side events; with the axle-count rule removed the same sweep fails in 4 runs | Windows host, **host only, not verified on QNX** |
| `make target` | builds `lg-controller` and `lg-meter` (and the earlier binaries) with `qcc -Vgcc_ntox86_64 -D_QNX_SOURCE -Wall -Wextra -Werror -std=c11` | Windows host, SDP 8.0 |
| README check: `normal_day.json` runs end to end with gate cycles for every train and 0 wrong-side events; metrics CSV produced | yes: `trains=6`, `controller_cycles=6`, `closure_count=6`, `wrong_side_events=0`, `wrong_side_ticks=0`, `trains_without_warning=0`, `plant_end_ms=285004`, the controller ends in level 0 with the gate UP. CSV in the file below | **QNX 8.0 VM**, `evidence/step07_controller.txt` |
| `tests/target/step07_controller.sh` | 56 checks, 0 failed. Parts: boot without sensors (level 4, gate DOWN, no release without information), boot release and idle, controller killed (gate watchdog lowers the gate, new controller boots in level 4 and is released), gate stopped with SIGSTOP and 25 stop/continue cycles (the status device answers every time), plant gone (blind sensors: level 4, no release without a guard), `normal_day` with `lg-meter`. Each check is one command | **QNX 8.0 VM** |
| Metrics of the recorded `normal_day` run | warning time (lights on until the train reaches the island) average 16291 ms, largest 19990 ms; closure per train average 14835 ms, largest 21100 ms; total closed 145956 ms of 284230 ms observed (1848649 ms per hour of observed time, including the boot closure); controller late ticks 170 of 28507 ticks (a tick more than 12 ms after the previous one) | **QNX 8.0 VM**: functional evidence, not hardware performance |
| Coverage of the meter in that run | every one of the meter periods gave a sample (coverage 100 percent), largest gap between two samples 61 ms, `meter_plant_fail=0`, `meter_gate_fail=0` | **QNX 8.0 VM** |
| Frozen gate | after the 25 stop/continue cycles the controller reports `gate_timeouts=746` and `gate_dropped=224`: requests piled up while the gate worker was blocked on a reply (224 were overwritten) and the main thread kept ticking and answering | **QNX 8.0 VM**, `evidence/step07_controller.txt` |
| Scheduling | the controller reads its own policy and priority back: `sched_policy=1 sched_priority=50` | **QNX 8.0 VM** |

How this evidence came about (the archived file is the last of several runs):
- The first run had 18 failing checks. Cause: `MsgSend` returns the status given to `MsgReply`, not the reply length; the controller and the meter compared it with the size of the reply and rejected every answer of the gate. Fixed in both, and the same trap was checked in the other senders.
- The `tester` found sensors whose samples arrive on time but are older than the usable age for good; the verdict stayed OK while the sensor was never used. The interim verdict now fails them after 200 ms in a row (`test_controller_stale`).
- The `reviewer` found, and a repro confirmed, a wrong-side event after a controller restart while a train was between the approach zone and the island (fixed with the axle-count rule, with the sweep as regression test), a gate that answered 150 ms late removing the boot lock for good (fixed with a boot grace period), a sensor dead for more than 2^31 ms looking alive again (fixed with a latch), a meter that could pass without looking (fixed: exit 4 when the observation is incomplete), and a log of one line per gate command that overwrote the watchdog lines (fixed in `lg-gate`).
- The first official run of the target test hung for almost an hour in part 4: a gate stopped after it had received a command froze the controller in the reply phase of `MsgSend`. That is a defect of the controller, not of the test. The gate IO now runs in a worker thread, the test runs 25 stop/continue cycles and has a 1500 s guard.
- The Decisions log has the rows of all of these.
- The script was changed once more after that run (the guard of the test no longer holds the ssh session open after the test has finished), so the whole target test was run again; the archived file is that last run. `tests/target/step05_gate.sh` was run again on the VM after the log change in `lg-gate` (one line only when a command changes the state): 36 checks, 0 failed. That output went to a scratch file and is NOT archived; `evidence/step05_gate.txt` is still the earlier run, made with the earlier gate binary.

Not done or not claimed in Step 7:
- No supervisor and no heartbeats (Step 8): a hung or silent controller is only handled by the gate watchdog, and a controller restart is manual.
- Level 4 is only left by restarting the controller (boot check): there is no guard acknowledgement path yet (Step 8 and 13).
- Level 3 (timetable) is a hold-down; the interim health verdicts are replaced by `lg-health` in Step 11.
- A malformed or out-of-range sample sent to the controller on the target was not provoked (host unit tests only).
- The target test uses `test_idle.json` for the boot parts and `normal_day.json` for the run; `closure_count` is accepted as 5 or 6 because the first train can arrive while the boot release is still raising the gate (the cycle count is exact).
- The plant restart replaying the scenario (Step 6 open decision) is still open for Step 8.
- The sender of a `GATE_CMD` or of a tick pulse is not authenticated (Step 13).
- `deadline_ms` of `GATE_CMD` is still ignored, `lgctl status` was not added (the status is `cat /dev/lg/controller`).

## Step 8: `lg-supervisor`, heartbeats, restart

Branch `step-08-supervisor` (from `step-07-controller`), not committed yet. The VM run in `evidence/step08_supervisor.txt` was made from the working tree before the commit.

| Check | Result | Verified where |
|---|---|---|
| `core/supervisor_logic.c` (stages, grace, hang kill, budget 3 per 10 s, DEAD absorbing, stall rule) | `test_supervisor_logic` and `test_supervisor_extra` (from the `tester`: 27 functions, budget clamps, window edges, pid reuse, text buffer sweep): pass. Two seeded random walks of 200000 steps. By-hand mutation check on a temporary copy: 11 of 12 mutants caught, the survivor is equivalent for timestamps that are not in the future. Copies not archived | Windows host |
| Controller side (`lg_ctl_on_proc_status`, supervisor silence, guards, restart flag `-r`, gate RESTARTING = level 4), protocol v4 | `test_controller_proc`, `test_guards`, `test_proto`, `test_core_extra`: pass | Windows host |
| Host replay with the real supervisor logic | `replay_normal_day`: sensor kill sweep, 852 runs (6 sensors, back after 300 ms or 2 s, every 3989 ms of the day), 0 wrong-side events; a fast restart of an approach sensor gives level 2 for at most 30340 ms (T_stable 30 s); slow restarts (2 s, longer than the 1.5 s start grace) end DEAD in 192 runs; island kills lock level 4 (gate DOWN) | Windows host, **host only, not verified on QNX** |
| `make test`, `make target` | 25 test binaries pass; target builds with `qcc -Vgcc_ntox86_64 -Wall -Wextra -Werror` | Windows host, SDP 8.0 |
| README check: `lgctl kill sensor west/a` restarts within a measured recovery time | yes: `recovery_ms` 17 (detection of the exit to the first heartbeat of the new process) | **QNX 8.0 VM**, `evidence/step08_supervisor.txt` part 2 |
| README check: the controller stays in level 2 during the restart and returns to level 0 after `T_stable` | yes: level 2 held for 20 s, level 0 31 s after the kill; level trail `4>0 0>2 2>0`, gate UP, `watchdog_trips` 0 | **QNX 8.0 VM**, part 2 |
| README check: killing the controller makes the gate watchdog lower the gate | yes, in part 10 (supervisor and controller killed together): the gate left UP, reached DOWN, `watchdog_trips` grew, nobody restarted the controller. In part 5 (supervisor alive) `watchdog_trips` stayed 0: the supervisor starts a new controller faster than the 100 ms watchdog, the gate still left UP and reached DOWN | **QNX 8.0 VM**, parts 5 and 10 |
| `tests/target/step08_supervisor.sh` | 149 checks, 0 failed. Parts: boot order, kill sensor, hang and crash faults, budget to DEAD, kill controller (restart with `-r`, guard needed), kill gate, kill island sensor, kill plant (epoch), kill supervisor, supervisor and controller killed together, controller stopped with SIGSTOP, `normal_day` with `lg-meter` and three kills | **QNX 8.0 VM** |
| Guard acknowledgement after a controller, gate, island-sensor or plant restart | level 4 holds (15 s with everything healthy after a controller restart), `G99` refused (exit 4), malformed id exit 3, `G01` accepted, level 0 thirty seconds later; no re-acknowledgement was needed in this run (the script allows 3 and prints the count) | **QNX 8.0 VM**, parts 5 to 8, 11, 12 |
| Plant epoch | plant time 10322 ms before the kill, 13370 ms three seconds after the restart: continued, not replayed | **QNX 8.0 VM**, part 8 |
| `normal_day` with kills of `west/a` (2 s before train 3), `east/b` (before train 4) and the controller (5 s before train 5, then a guard acknowledgement) | `wrong_side_events=0`, `wrong_side_ticks=0`, 6 trains, `trains_without_warning=0`, meter coverage 99 percent, largest meter gap 70 ms, plant end 285004 ms, `closure_count` 5 (the gate stays DOWN until the guard acknowledges), west/a, east/b and the controller restarted once each, nothing DEAD, gate UP at the end | **QNX 8.0 VM**, part 12: functional evidence, not hardware performance |

How this evidence came about (the archived file is the last of several runs):
- The first smoke test (not archived) showed every heartbeat rejected: QNX pids need up to 31 bits (seen: 355848218), the first design packed the pid into the pulse value. The beat now carries the process id and the supervisor takes the pid from the kernel (`ConnectClientInfo`); the controller accepts `PROC_STATUS` only from its parent (the supervisor), the supervisor an exit pulse only from its own waiter thread.
- Review (`reviewer`) findings fixed: gate RESTARTING is level 4 by rule (it depended on a reconnect timing), `PROC_STATUS` from other processes is refused, weak or mislabelled checks in the target test and the replay, evidence numbers only from archived files. Decided with the user: a controller restarted by the supervisor starts with `-r` (level 4 without the boot release, a guard has to acknowledge).
- Runs on a busy host failed for reasons of the environment: the whole VM stalled for about 190 ms, the supervisor judged a healthy controller hung and killed it, a sensor looked silent and voided a pending guard acknowledgement, the meter refused a verdict above a 200 ms gap. The supervisor now restarts the heartbeat clocks when its own step comes more than 50 ms late (`stalls` in `/dev/lg/supervisor`, 5 in the archived run). The controller's own 200 ms silence rule for sensors is not given the same treatment (it guards the gate): a stall can still void a pending acknowledgement, so the script allows up to 3 and prints the count.
- The run right before the archived one, on a VM that had just been started, had 149 checks and 3 failures, all three read `slog2info` in parts 1 and 2 (start order, the supervisor's priority line, the level trail); everything else passed, wrong-side 0. The next run on the same VM passed all of them. The cause is not established (it may be the first use of slog2 after a boot); that output was kept only in a scratch file.
- The meter reads `/dev/lg/controller` once, through a descriptor that dies with the first controller: its `controller_*` fields are void whenever the controller is restarted in the run (part 12), and the script reads the level live instead.

Part 11 (controller stopped with SIGSTOP) is information plus "nothing DEAD": in some runs a sensor blocked on the stopped controller was restarted as well, in others not.

Not done or not claimed in Step 8:
- Regression of the earlier target tests after the protocol bump and the heartbeat changes, run on the VM with this working tree (output kept in scratch files only, the archived `evidence/step05_gate.txt`, `step06_sensors.txt`, `step07_controller.txt` are still the earlier runs): `step05_gate.sh` 36 checks, 0 failed; `step06_sensors.sh` 82 checks, 0 failed; `step07_controller.sh` 56 checks, 1 failed: part 4, "the controller counted the gate timeouts" (`gate_timeouts` at least 1 right after the SIGSTOP of the gate). That check is timing dependent by design (whether the controller counts a timeout or stays blocked in the reply phase depends on whether the gate had already received a command when it was stopped, see the Step 6 and 7 notes); the same script passed it in its archived run, and nothing Step 8 changed sits on the controller-to-gate path. A second run of `step07_controller.sh` (host memory about 1.2 GB free, 26 Opera processes) failed 5 checks, all in part 6 (`normal_day`): six gate cycles (4 counted), closures (4), level 0 at the end (level 4), gate UP at the end, and "only level change is the boot release". The run itself shows a starved VM: the controller counted `late_ticks=2839` of 27833 ticks (the archived run: 170 of 28507), the meter saw a gap of 169 ms, and the controller went 0 to 4 (reason 8, no trustworthy sensor: a sensor looked silent for 200 ms, which latches and locks level 4; `step07_controller.sh` does not acknowledge). `wrong_side_events` was 0 in that run. This script starts the processes by hand, without the supervisor, so only the heartbeat connector threads (a failed `name_open` every 200 ms per process) are new on its path. I cannot tell a starved VM from a regression with these two runs: the first had one failing check, the second five, different ones. **`step07_controller.sh` has not passed on this working tree on a quiet host yet; this is open and must be rerun (with Opera and other heavy programs closed) before the merge to `main`.**
- Sender authentication and the PIN check of the guard acknowledgement (Step 13): any local process can send `GUARD_ACK` with a listed id, a heartbeat flood is possible, `lgctl kill` needs only root.
- A supervisor that never starts is not detected by the controller (not armed before the first status).
- Review findings left as they are (Decisions log, 2026-11-21): children inherit SCHED_FIFO 55 from the supervisor until they set their own priority, no spawn backoff, a lost exit pulse is dropped, `lgctl guard-ack` has no timeout on `name_open`, a second supervisor started while orphaned children still run was not tried.
- `lg-health` (Step 11), timetable (Step 12), `lg-bridge`, image baking and a startup script. A malformed `PROC_STATUS` or `GUARD_ACK` on the target was not provoked (host unit tests only).

## Known gaps

- `clang-format` missing on the host (format check skipped in `make lint`).
- No CI yet (GitHub Actions comes with the public repo).
- Host-only network DHCP shows "Disabled" in `VBoxManage list hostonlyifs`, yet the VM obtained an address; not investigated further.
