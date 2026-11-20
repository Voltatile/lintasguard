# Evidence status

Updated: 2026-11-20. Commit: see `git log` (first commit = Step 1 and Step 2).

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

## Known gaps

- `clang-format` missing on the host (format check skipped in `make lint`).
- No CI yet (GitHub Actions comes with the public repo).
- Host-only network DHCP shows "Disabled" in `VBoxManage list hostonlyifs`, yet the VM obtained an address; not investigated further.
