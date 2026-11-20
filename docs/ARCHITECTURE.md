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
