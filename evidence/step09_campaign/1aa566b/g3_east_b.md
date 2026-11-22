# Campaign report: sensor_faults

README step 9: one approach sensor fails during traffic (stuck, crash, hang)

Result: **0 of 1 cases passed**. Wrong-side events must be 0 in every case.

VirtualBox VM timings: functional evidence only, not hardware performance.

## Environment

| Item | Value |
|---|---|
| date (UTC) | 2026-11-21 14:23:34 |
| commit | 1aa566b |
| host OS | Windows-11-10.0.26200-SP0 |
| python | 3.13.7 |
| VirtualBox | 7.2.22r175245 |
| SDP | qcc on PATH, QNX_HOST=C:/Users/acern/qnx800/host/win64/x86_64 |
| target | QNX lintasguard 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64 |
| campaign | `campaigns/sensor_faults.json` |
| scenario | `sensor_faults.json` |
| seeds | sensor noise seeds are the lg-sensor defaults, (dir * 3 + id + 1) * 2654435761, the same in every run |

## Cases

| Case | Result | Checks | Max level | Tickets | Level trail | Recovery from injection (ms) | Recovery from clear (ms) | Supervisor recovery (ms) | Trains | Wrong-side | Closure per hour (ms) | Late ticks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| stuck_clear_east_b_during_train | FAIL | 22/23 | 2 | 2 | 4>0,0>2,2>0, | 193475 | 89379 | 0 | 6 | 0 | 1953474 | 1886/26587 |

## Case definitions

| Case | Sensor | Fault | Inject (ms) | Clear (ms) | End (ms) | Expected |
|---|---|---|---|---|---|---|
| stuck_clear_east_b_during_train | east/b | stuck-clear | 61000 | 165000 | 270000 | level 2, 6 trains, ticket MISSED_TRAIN |

## Failed checks

- **stuck_clear_east_b_during_train** (exit 1)
  - exactly one ticket (tickets 1)

Raw output of every case: the `.txt` file next to this report.
