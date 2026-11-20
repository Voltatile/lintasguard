# Operating levels

Implementation: `core/mode_manager.c` (`lg_mode_step`, `lg_mode_required`). Requirements: README section 3. Tested on the host only (`tests/unit/test_mode_manager.c`); not yet verified on QNX.

| Level | Name | Road impact |
|---|---|---|
| 0 | Normal | Normal |
| 1 | Early warning | None |
| 2 | Degraded, redundant | Almost none (extra pre-warning margin) |
| 3 | Uncertain detection | Gate closes per expected train (timetable windows) |
| 4 | Fail-safe | Gate down until a guard takes over |

## How the level is chosen

`lg_mode_required()` computes the level the inputs demand. The mode manager then applies hysteresis and the level-4 lock.

| Condition | Required level | Reason code |
|---|---|---|
| Self check failed | 4 | `LG_REASON_SELF_CHECK` |
| Controller, supervisor or gate process DEAD | 4 | `LG_REASON_CRITICAL_PROC_DEAD` |
| Gate verdict FAILED | 4 | `LG_REASON_GATE_FAILED` |
| Both approach sensors of a direction lost, timetable or station info available for that direction | 3 | `LG_REASON_DETECT_LOST_SCHEDULE` |
| Island sensor of a direction lost, info available | 3 | `LG_REASON_DETECT_LOST_SCHEDULE` |
| Same two cases, no info for that direction | 4 | `LG_REASON_DETECT_LOST_NO_INFO` |
| One approach sensor of a direction lost, the other usable | 2 | `LG_REASON_SENSOR_FAILED` |
| Health, schedule, plant or gate process DEAD or RESTARTING (gate DEAD is level 4, see above) | 2 | `LG_REASON_PROC_DEAD` / `LG_REASON_PROC_RESTART` |
| Controller or supervisor RESTARTING | 2 | `LG_REASON_PROC_RESTART` |
| Battery verdict FAILED | 2 | `LG_REASON_WARNING` |
| Bridge process DEAD or RESTARTING | 1 | `LG_REASON_PROC_DEAD` / `LG_REASON_PROC_RESTART` |
| Any component verdict WARNING | 1 | `LG_REASON_WARNING` |
| none of the above | 0 | none |

A sensor is "lost" when its verdict is FAILED or its driver process is RESTARTING or DEAD. Directions are evaluated independently; the highest requirement wins.

Inputs come from IPC messages, so unknown values fail closed: a verdict other than OK or WARNING counts as FAILED, a process state other than RUNNING or RESTARTING counts as DEAD, and an out-of-range level in the clearing check counts as level 4. The controller must pass `schedule_ok = 0` until `lg-schedule` reports usable information (the test helper `lg_mode_in_all_healthy` sets it to 1).

## Transitions

- **Escalation is immediate** and may skip levels (0 to 3, 0 to 4).
- **De-escalation** needs the required level to stay unchanged for `LG_T_STABLE_MS`. The timer restarts whenever the required level changes. After the window the level drops straight to the required level (not step by step).
- **Boot** starts in level 4 with the gate down. The boot lock is released when everything is healthy (required level 0) for `LG_T_BOOT_STABLE_MS`; the level then becomes 0. "Not ready yet" conditions (for example no schedule information yet) are covered by this path. A real level 4 event during boot (self check failed, controller, supervisor or gate process DEAD, gate verdict FAILED) removes the boot lock and then needs a guard acknowledgement like any other level 4 event.
- **Level 4 lock-in.** Level 4 is left only through a guard acknowledgement (`guard_ack`) that was given while the inputs demanded at most level 1, and only if that still holds for `LG_T_STABLE_MS` afterwards. Healthy components alone never release it. An acknowledgement given while a component is failed is void, and any later tick that demands more than level 1 voids a pending one, so the guard must acknowledge again after the cause is gone. Acknowledgements are ignored in levels 0 to 3 and cleared when level 4 is entered again. The same rule lets a guard release a boot that never reached level 0 (for example with a WARNING component); a failed component keeps it locked. The boot timer and the guard timer are separate (the timer restarts when the release type changes).
- **Clearing the gate** (`core/voting.c`, `lg_clear_step`) is separate from the level: see README 8.2. Level 4 never clears through that path. "Island clear" requires that the island sensor saw the train and then stayed clear for `T_clear`; the sighting is required in level 3 and whenever axle counting is not healthy (with healthy axle counting, equal in/out counts cover it). In level 3 a guard confirmation replaces the island condition only: a healthy axle counter, other-direction traffic and the minimum down time still apply.

## Implementation notes

- Pure functions: the caller passes the inputs and `now_ms`; elapsed time uses `lg_elapsed_ge` (`core/include/lg_time.h`): safe across a 32-bit counter wrap, and a timestamp that lies in the future counts as not elapsed. Spans up to about 24 days are supported.
- Activation margin: the extra pre-warning margin applies in level 2 and above.
- Defaults come from `common/include/lg_config.h`; runtime values can be passed through `lg_mode_cfg_t`.
