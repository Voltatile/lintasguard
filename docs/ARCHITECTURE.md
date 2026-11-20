# Architecture notes

See README.md sections 4 to 8 for the design. This file records deeper notes.

## Decisions log

| Date | Decision | Reason |
|---|---|---|
| 2026-11-20 | Licence: Apache-2.0 | Recommended in README section 19; detectable by GitHub. |
| 2026-11-20 | Host unit tests use plain asserts/exit codes, built by the top-level Makefile | Keeps Step 2 dependency-free. |
| 2026-11-20 | Added `LG_MSG_ACK` (`lg_ack_t`) as a generic reply to `MsgSend` | README lists no reply type; every command needs an accept/reject answer (for example `EPERM` from the gate). |
| 2026-11-20 | Every struct carries explicit `reserved` fields so there is no implicit padding; sizes are `_Static_assert`ed | Fixed-size, versioned layout (README 7); the same bytes on host and target. |
| 2026-11-20 | Initial values not fixed by the README: `LG_TICK_TOLERANCE_MS` 2, `LG_T_BOOT_STABLE_MS` 5000, `LG_T_INFO_TIMEOUT_MS` 60000, `LG_PREWARNING_MS` 10000, `LG_MIN_GATE_DOWN_MS` 3000, `LG_T_DISAGREE_MS` 500 | Placeholders to be tuned in Steps 7 to 12; marked "initial" in `lg_config.h`. |
| 2026-11-20 | Pulse codes 1 to 4 | Still to be checked against `_PULSE_CODE_MINAVAIL`/`_PULSE_CODE_MAXAVAIL` in the SDP 8.0 headers when pulses are first used on target (Step 5). |
| 2026-11-20 | Mode manager: gate verdict FAILED, or controller/supervisor/gate process DEAD, requires level 4 | Without the gate or controller the crossing cannot be kept controlled; the gate watchdog already lowers the gate. |
| 2026-11-20 | Mode manager: island sensor lost requires level 3 (level 4 without timetable info) | The gate cannot be cleared by the island, so a guard or timetable is needed. Safer side. |
| 2026-11-20 | Mode manager: a RESTARTING or DEAD sensor driver counts as a lost sensor | Both drivers of a direction restarting at once must not be treated as one healthy sensor. |
| 2026-11-20 | Mode manager: health/schedule/plant process DEAD or RESTARTING gives level 2; bridge gives level 1; battery FAILED gives level 2 | README 8.5 says "level 2, 3 or 4 depending on the process" without a table. Bridge has no safety role. |
| 2026-11-20 | Level 4 release needs a `guard_ack` given while the required level is <= 1, then stable for `T_stable`; an ack given while unhealthy is void, and a pending ack is voided when the required level rises above 1 or level 4 is re-entered. (Revised after the review: the first version kept an ack given during a fault, which let level 4 release by itself later.) | README 3.1: "guard-ack and healthy sensors" and "never returns on its own". The guard attests to the state they see; the cost of a rejected ack is one more `lgctl` command. A WARNING-level component is still functional, so it does not block the release. |
| 2026-11-20 | De-escalation goes straight to the required level after `T_stable` of an unchanged requirement | README diagram shows single steps; this is the stricter hysteresis (any change restarts the timer) and is simpler to verify. |
| 2026-11-20 | Boot lock: released by all-healthy (required level 0) for `T_boot_stable`, no guard needed; a valid guard ack (required level <= 1) can also release it. A real level 4 event during boot (self check, controller/supervisor/gate DEAD, gate FAILED) removes the boot lock. A single failed sensor (level 2) keeps boot locked. | README 8.6 describes only the all-healthy path. Without the last rule a boot-time fault would be released without a guard, which README 3.1 forbids. "No information yet" and sensor faults at boot are not hard events. |
| 2026-11-20 | Metrics: "total closure" counts time the gate is not UP; "closure per train" is DOWN until the gate starts rising; boot DOWN period is counted as blocked time but not as a train closure | Road users are blocked while the gate lowers and raises too; per-train closure follows README 8.7 literally. |
| 2026-11-20 | Wrong side counts any gate state other than DOWN (LOWERING, RAISING, FAULT, UP) while a train is on the island | Strictest reading of README 8.7 ("gate is not DOWN"). |
| 2026-11-20 | Unknown enum values fail closed: verdict not OK/WARNING = FAILED, proc state not RUNNING/RESTARTING = DEAD, clearing level > 4 = level 4 | Inputs arrive in IPC messages; an out-of-range value must never read as healthy. |
| 2026-11-20 | Elapsed time uses `lg_elapsed_ge`: a timestamp in the future is "not elapsed" | `lg-gate` timestamps can be a few ms newer than the `now_ms` read at the start of a tick; plain unsigned subtraction would read that as a huge elapsed time and satisfy the minimum down time. |
| 2026-11-20 | Gate clearing needs an island sighting in level 3 and whenever axle counting is not healthy | Otherwise "island clear" can be true before the train has arrived. Trade-off: a false activation with an unhealthy axle counter keeps the gate down until a guard confirms (right-side, safer). Same-direction approach is not an input yet; to be revisited in Step 7. |
| 2026-11-20 | In level 3 a guard confirmation replaces the island condition only, not a healthy axle counter | If the axle counter is healthy and says in != out, wagons may still be on the crossing. |
| 2026-11-20 | Extra pre-warning margin applies in level 2 and above | A direction with working sensors in level 3 still has reduced redundancy elsewhere; the margin is the safer choice. |
| 2026-11-20 | `schedule_ok` must be 0 until `lg-schedule` reports usable information; `LG_T_INFO_TIMEOUT_MS` is applied by the controller or `lg-schedule` (Step 12), the mode manager only sees the debounced flag | Mode manager has no clock source for information age. README 8.4 timeout is not implemented in Step 4. |
| 2026-11-20 | Not covered by the mode manager inputs yet: sample freshness at boot (README 8.6 "valid samples") | Relies on the health verdict until Step 7 adds a freshness input. |
