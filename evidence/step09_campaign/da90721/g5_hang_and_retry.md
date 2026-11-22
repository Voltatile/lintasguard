# Campaign report: sensor_faults

README step 9: one approach sensor fails during traffic (stuck, crash, hang)

Result: **2 of 2 cases passed**. Wrong-side events must be 0 in every case.

VirtualBox VM timings: functional evidence only, not hardware performance.

## Environment

| Item | Value |
|---|---|
| date (UTC) | 2026-11-22 03:57:34 |
| commit | da90721 (working tree has uncommitted changes) |
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
| stuck_clear_west_a | PASS | 23/23 | 2 | 1 | 4>0,0>2,2>0, | 123807 | 83807 | 0 | 3 | 0 | 2017304 | 25/14516 |
| hang_east_a_during_train | PASS | 22/22 | 2 | 1 | 4>0,0>2,2>0, | 30318 | 30318 | 22 | 3 | 0 | 1995652 | 2215/12918 |

## Case definitions

| Case | Sensor | Fault | Inject (ms) | Clear (ms) | End (ms) | Expected |
|---|---|---|---|---|---|---|
| stuck_clear_west_a | west/a | stuck-clear | 10000 | 50000 | 145000 | level 2, 3 trains, ticket MISSED_TRAIN |
| hang_east_a_during_train | east/a | hang | 61000 | 0 | 135000 | level 2, 3 trains, ticket SILENT or RESTARTING |

Raw output of every case: the `.txt` file next to this report.
