# Campaign report: sensor_faults

README step 9: one approach sensor fails during traffic (stuck, crash, hang)

Result: **1 of 2 cases passed**. Wrong-side events must be 0 in every case.

VirtualBox VM timings: functional evidence only, not hardware performance.

## Environment

| Item | Value |
|---|---|
| date (UTC) | 2026-11-24 06:08:57 |
| commit | b94bd3c (working tree has uncommitted changes) |
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
| stuck_occupied_east_a_during_train | PASS | 23/23 | 2 | 1 | 4>0,0>2,2>0, | 125590 | 86553 | 0 | 5 | 0 | 2247287 | 161/21496 |
| crash_west_a_during_train | FAIL | 14/21 | 4 | 8 | 4>0,0>2,2>0,0>2,2>4, | 30108 | 30108 | 13 | 3 | 0 | 2813992 | 551/13109 |

## Case definitions

| Case | Sensor | Fault | Inject (ms) | Clear (ms) | End (ms) | Expected |
|---|---|---|---|---|---|---|
| stuck_occupied_east_a_during_train | east/a | stuck-occupied | 61000 | 100000 | 215000 | level 2, 5 trains, ticket STUCK_OCCUPIED |
| crash_west_a_during_train | west/a | crash | 21000 | 0 | 135000 | level 2, 3 trains, ticket SILENT or RESTARTING |

## Failed checks

- **crash_west_a_during_train** (exit 1)
  - lg-meter observed the whole run and saw no wrong side (exit 0)
  - every train had a gate closure (closure_count at least 3)
  - highest level after the boot release is 2 (never 3 or 4)
  - exactly one ticket (tickets 1)
  - the level trail is boot release, down to 2, back to 0
  - end: level 0
  - end: gate UP with the lights off

Raw output of every case: the `.txt` file next to this report.
