# Campaign report: sensor_faults

README step 9: one approach sensor fails during traffic (stuck, crash, hang)

Result: **5 of 7 cases passed**. Wrong-side events must be 0 in every case.

VirtualBox VM timings: functional evidence only, not hardware performance.

## Environment

| Item | Value |
|---|---|
| date (UTC) | 2026-11-23 07:49:50 |
| commit | 5da0006 (working tree has uncommitted changes) |
| host OS | Windows-11-10.0.26200-SP0 |
| python | 3.13.7 |
| VirtualBox | 7.2.22r175245 |
| SDP | qcc not on PATH |
| target | QNX lintasguard 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64 |
| campaign | `campaigns/sensor_faults.json` |
| scenario | `sensor_faults.json` |
| seeds | sensor noise seeds are the lg-sensor defaults, (dir * 3 + id + 1) * 2654435761, the same in every run |

## Cases

| Case | Result | Checks | Max level | Tickets | Level trail | Recovery from injection (ms) | Recovery from clear (ms) | Supervisor recovery (ms) | Trains | Wrong-side | Closure per hour (ms) | Late ticks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | PASS | 16/16 | 0 | 0 | 4>0, |  |  | 0 | 6 | 0 | 1814540 | 404/26937 |
| stuck_occupied_west_a | PASS | 23/23 | 2 | 1 | 4>0,0>2,2>0, | 123903 | 83992 | 0 | 3 | 0 | 2279929 | 14/14525 |
| stuck_clear_west_a | PASS | 23/23 | 2 | 1 | 4>0,0>2,2>0, | 123864 | 83960 | 0 | 3 | 0 | 2016244 | 217/14470 |
| stuck_clear_east_b_during_train | FAIL | 18/23 | 4 | 3 | 4>0,0>2,2>0,0>4, | 193547 | 89497 | 0 | 6 | 0 | 2064995 | 311/26929 |
| stuck_occupied_east_a_during_train | FAIL | 13/23 | 4 | 4 |  |  |  | 0 | 5 | 0 | 3600000 | 411/21373 |
| crash_west_a_during_train | PASS | 21/21 | 2 | 1 | 4>0,0>2,2>0, | 30119 | 30119 | 14 | 3 | 0 | 2004199 | 17/13518 |
| hang_east_a_during_train | PASS | 22/22 | 2 | 1 | 4>0,0>2,2>0, | 30303 | 30303 | 24 | 3 | 0 | 1998468 | 13/13523 |

## Case definitions

| Case | Sensor | Fault | Inject (ms) | Clear (ms) | End (ms) | Expected |
|---|---|---|---|---|---|---|
| baseline | west/a | none | 0 | 0 | 270000 | level 0, 6 trains, ticket NONE |
| stuck_occupied_west_a | west/a | stuck-occupied | 10000 | 50000 | 145000 | level 2, 3 trains, ticket STUCK_OCCUPIED |
| stuck_clear_west_a | west/a | stuck-clear | 10000 | 50000 | 145000 | level 2, 3 trains, ticket MISSED_TRAIN |
| stuck_clear_east_b_during_train | east/b | stuck-clear | 61000 | 165000 | 270000 | level 2, 6 trains, ticket MISSED_TRAIN |
| stuck_occupied_east_a_during_train | east/a | stuck-occupied | 61000 | 100000 | 215000 | level 2, 5 trains, ticket STUCK_OCCUPIED |
| crash_west_a_during_train | west/a | crash | 21000 | 0 | 135000 | level 2, 3 trains, ticket SILENT or RESTARTING |
| hang_east_a_during_train | east/a | hang | 61000 | 0 | 135000 | level 2, 3 trains, ticket SILENT or RESTARTING |

## Failed checks

- **stuck_clear_east_b_during_train** (exit 1)
  - highest level after the boot release is 2 (never 3 or 4)
  - exactly one ticket (tickets 1)
  - the level trail is boot release, down to 2, back to 0
  - end: level 0
  - end: gate UP with the lights off
- **stuck_occupied_east_a_during_train** (exit 1)
  - boot: level 0 and gate UP within 25 s
  - every train had a gate closure (closure_count at least 5)
  - highest level after the boot release is 2 (never 3 or 4)
  - exactly one ticket (tickets 1)
  - the level trail is boot release, down to 2, back to 0
  - level 2 came after the injection (watched from the boot release)
  - level 0 again before the end (recovery recorded)
  - back to level 0 not before T_stable after the fault ended
  - end: level 0
  - end: gate UP with the lights off

Raw output of every case: the `.txt` file next to this report.
