#!/bin/sh
# Step 12 target test (README 9 step 12, level 3 timetable windows). Runs ON the QNX target, detached:
#
#   scp tests/target/step12_level3.sh root@<vm-ip>:/tmp/ && ssh root@<vm-ip> \
#       'nohup sh /tmp/step12_level3.sh A >/tmp/step12_A.out 2>&1 &'      (then poll; the same with B)
#
# Done when: with both West sensors failed, trains from the West are still protected using timetable
# windows; total closure per hour is far lower than "gate down permanently"; level 4 is entered when
# schedule info times out. Wrong-side must be 0.
#
# Two runs (split after the first two 25-minute runs were broken by VM stalls under host memory pressure,
# evidence/step12_level3/run1_vmstall.out and run2_vmstall.out; the user chose the split):
#
# A  scenarios/level3_day.json with its timetable (lead 60 s, tolerance 180 s), about 17 min of plant time:
#    part 1  boot: level 0, lg-schedule RUNNING, /dev/lg/schedule lists the timetable, sched_ok=1,1
#    part 2  DROPOUT 100 on west/a and west/b at 20 s: level 3 (DETECT_LOST_SCHEDULE) with notifications to
#            station, guard and road sign; the gate is UP between windows and DOWN in each window before the
#            West train reaches the island: train 1 on time, train 3 60 s late, train 5 30 s early; it rises
#            after the island confirmed each train; East trains as usual. lg-meter up to 1000 s: wrong-side 0
#            and closure_ms_per_hour at most 40 % of 3 600 000 (permanent closure; plan, Step 12)
#    part 4  (inside part 2, at 260 s) lg-schedule killed: the supervisor restarts it, level stays 3
# B  scenarios/level3_outage.json with short windows (lead 20 s, tolerance 30 s), about 4 min:
#    part 3  DROPOUT 100 on west/a and west/b at 10 s: level 3; entry 7 is a cancelled West train (window 50 ..
#            100 s): the gate goes DOWN, stays DOWN past the end, overdue notification to the guard, an unknown
#            guard id is refused, lgctl guard-confirm --dir west raises it
#    part 5  lgctl schedule outage all: still level 3 after 50 s, level 4 (DETECT_LOST_NO_INFO) by 65 s with
#            the gate DOWN and notifications; repair (outage none, faults none, health reset, guard-ack): level 4
#            left. lg-meter over the whole run: wrong-side 0
#
# Prints one PASS:/FAIL: line per check, RESULT key=value lines, and exits non-zero when any check fails.
# Every check is ONE command. Needs everything in /tmp/lintasguard (bash scripts/deploy.sh <vm-ip>). Plant
# time runs with the real clock; timings come from a VirtualBox VM: functional evidence only.

BIN=/tmp/lintasguard
CSV=/tmp/lg_s12_metrics.csv
PERMANENT=3600000
LIMIT_PCT=40
MODE=${1:-}
fails=0
checks=0

check() { # check <name> <command...>
    name=$1
    shift
    checks=$((checks + 1))
    if "$@"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        fails=$((fails + 1))
    fi
}

cfield() { cat /dev/lg/controller 2>/dev/null | tr ' ' '\n' | sed -n "s/^$1=//p"; }
gfield() { cat /dev/lg/gate 2>/dev/null | tr ' ' '\n' | sed -n "s/^$1=//p"; }
sfield() { grep "^proc=$1 " /dev/lg/supervisor 2>/dev/null | tr ' ' '\n' | sed -n "s/^$2=//p"; }
sched_state() { sfield schedule state; } # one argument, for wait_eq
plant_ms() { cat /dev/lg/plant 2>/dev/null | tr ' ' '\n' | sed -n 's/^t_ms=//p'; }
csv_get() { sed -n "s/^$1,//p" $CSV; }
killproc() { for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do kill -9 $p; done; }
cleanup() { # the supervisor first, so that it does not start anything again
    killproc lg-supervisor
    killproc lg-bridge
    killproc lg-schedule
    killproc lg-health
    killproc lg-meter
    killproc lg-controller
    killproc lg-sensor
    killproc lg-plant
    killproc lg-gate
}
eq() { [ -n "$1" ] && [ "$1" = "$2" ]; }
less_than() { [ -n "$1" ] && [ -n "$2" ] && [ "$1" -lt "$2" ]; }
has_event() { # has_event <pattern...>: a line of /dev/lg/events contains every fixed string
    l=$(cat /dev/lg/events 2>/dev/null)
    for p in "$@"; do
        l=$(echo "$l" | grep -F -- "$p")
    done
    [ -n "$l" ]
}
sched_has() { grep -qF -- "$1" /dev/lg/schedule 2>/dev/null; }
wait_eq() { # wait_eq <fn> <arg> <value> <tenths>
    i=0
    while [ $i -lt "$4" ]; do
        [ "$($1 "$2")" = "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_plant() { # wait_plant <ms>: until the plant time reaches ms (or the plant is gone)
    while :; do
        p=$(plant_ms)
        [ -z "$p" ] && return 1
        [ "$p" -ge "$1" ] && return 0
        sleep 0.1
    done
}
wait_level0() {
    i=0
    while [ $i -lt 250 ]; do
        [ "$(cfield level)" = "0" ] && [ "$(gfield state)" = "UP" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
level_below4_within() { # level_below4_within <tenths>
    i=0
    while [ $i -lt "$1" ]; do
        l=$(cfield level)
        [ -n "$l" ] && [ "$l" -lt 4 ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
start_meter() { # start_meter <end_ms>
    rm -f $CSV
    $BIN/lg-meter -e "$1" -m $(($1 / 1000 + 60)) -o $CSV </dev/null >/tmp/lg_s12_meter.out 2>&1 &
    MPID=$!
}
finish_meter() { # waits at most 120 s; a meter still running then is killed and its run counts as failed
    i=0
    while [ $i -lt 1200 ] && kill -0 $MPID 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    if kill -0 $MPID 2>/dev/null; then
        echo "--- lg-meter did not finish within 120 s of its end: killed"
        kill -9 $MPID 2>/dev/null
    fi
    wait $MPID 2>/dev/null
    METER_RC=$?
    echo "--- lg-meter output (exit $METER_RC):"
    cat /tmp/lg_s12_meter.out
}
safety_checks() { # safety_checks <part> <expected trains>
    for k in trains trains_without_warning closure_count closure_max_ms closure_ms_per_hour warning_avg_ms \
        wrong_side_events wrong_side_ticks meter_coverage_pct meter_max_gap_ms; do
        echo "RESULT $1_$k=$(csv_get $k)"
    done
    check "$1: wrong_side_events is 0" eq "$(csv_get wrong_side_events)" 0
    check "$1: wrong_side_ticks is 0" eq "$(csv_get wrong_side_ticks)" 0
    check "$1: lg-meter observed the whole run and saw no wrong side (exit 0)" [ "$METER_RC" = "0" ]
    check "$1: every train reached the island with the lights on" eq "$(csv_get trains_without_warning)" 0
    check "$1: trains seen by the meter: $2" eq "$(csv_get trains)" "$2"
}
gate_down_now() { [ "$(gfield state)" = "DOWN" ]; }
target_down_now() { [ "$(cfield target)" = "DOWN" ]; }
gate_up_within() { wait_eq gfield state UP "$1"; }
gate_down_within() { wait_eq gfield state DOWN "$1"; }
stall_report() { # the controller's own stall counter: a VM pause shows here (functional evidence, see run1/run2)
    echo "RESULT $1_controller_stalls=$(cfield stalls) late_ticks=$(cfield late_ticks)"
}

boot() { # boot <scenario> <timetable>
    echo "=== part 1: boot under lg-supervisor with lg-schedule ($1, timetable $(basename $2))"
    cleanup
    rm -f $CSV /tmp/lg_s12_meter.out
    sleep 1.5
    $BIN/lg-supervisor -b $BIN -c $BIN/config/crossing.json -s $BIN/scenarios/$1 -T $2 </dev/null >/tmp/lg_s12_sup.log 2>&1 &
    check "boot release: level 0 and gate UP within 25 s" wait_level0
    check "lg-schedule is RUNNING under the supervisor" eq "$(sfield schedule state)" RUNNING
    check "the controller has schedule information for both directions (sched_ok=1,1)" wait_eq cfield sched_ok 1,1 30
}

west_fails() { # west_fails <plant ms>
    wait_plant "$1"
    $BIN/lgctl sensor west/a dropout 100
    $BIN/lgctl sensor west/b dropout 100
    check "level 3 within 3 s" wait_eq cfield level 3 30
    check "the level change says DETECT_LOST_SCHEDULE (reason 7)" has_event "kind=LEVEL" "to=3" "reason=7"
    check "West is uncertain, East is not (uncertain=1,0)" eq "$(cfield uncertain)" 1,0
}

run_a() {
    TT=$BIN/scenarios/level3_day.timetable.json
    boot level3_day.json $TT
    check "/dev/lg/schedule: West has information and lists train 1" sh -c "grep -q 'dir=west info=1 outage=0' /dev/lg/schedule && grep -q 'entry id=1 dir=west source=TIMETABLE' /dev/lg/schedule"
    check "lgctl schedule prints the same device (exit 0)" sh -c "$BIN/lgctl schedule | grep -q 'dir=east info=1'"
    cat /dev/lg/schedule
    start_meter 1000000

    echo "=== part 2: both West approach sensors fail at 20 s (DROPOUT 100)"
    west_fails 20000
    check "station notified: drivers from the West slow down (to=station DETECTION_LOST dir=west)" has_event "to=station" "diag=DETECTION_LOST" "dir=west"
    check "guard dispatched (to=guard DETECTION_LOST)" has_event "to=guard" "diag=DETECTION_LOST" "dir=west"
    check "road sign shows the fault (to=road_sign DETECTION_LOST)" has_event "to=road_sign" "diag=DETECTION_LOST"
    wait_plant 100000
    check "100 s: still level 3" eq "$(cfield level)" 3
    check "100 s, no window open: the gate is UP (not down permanently)" eq "$(gfield state)" UP
    wait_plant 140000
    check "140 s, window of train 1 open (136 s): the controller wants the gate DOWN" target_down_now
    check "the West window is open (win_open=1,0)" eq "$(cfield win_open)" 1,0
    check "the gate is DOWN within 10 s, before train 1 reaches the island at 196 s" gate_down_within 100
    wait_plant 194000
    check "194 s: the gate is still DOWN just before train 1 arrives" gate_down_now
    wait_plant 203000
    check "train 1 confirmed by the island: the gate is UP again within 15 s" gate_up_within 150
    check "one West window confirmed by the island (win_island=1,0)" eq "$(cfield win_island)" 1,0

    echo "=== part 4: lg-schedule is killed at 260 s (the supervisor restarts it, level stays 3)"
    wait_plant 260000
    check "lgctl kill schedule (exit 0)" sh -c "$BIN/lgctl kill schedule >/dev/null"
    check "lg-schedule is RUNNING again within 5 s" wait_eq sched_state x RUNNING 50
    sleep 2
    check "the restart was counted (total=1)" eq "$(sfield schedule total)" 1
    check "the level is still 3 (not 4: the information did not time out)" eq "$(cfield level)" 3
    check "schedule information again for both directions (sched_ok=1,1)" wait_eq cfield sched_ok 1,1 30
    check "the restarted lg-schedule uses the same scenario clock (train 3 still listed at 520 s)" sched_has "entry id=3 dir=west source=TIMETABLE eta_ms=520000"

    echo "=== part 2 (continued): train 3 runs 60 s late, train 5 comes 30 s early"
    wait_plant 465000
    check "465 s, window of train 3 open (460 s): the controller wants the gate DOWN" target_down_now
    check "the gate is DOWN within 10 s" gate_down_within 100
    wait_plant 578000
    check "578 s: train 3 is 58 s late and the gate is still DOWN (no automatic opening)" gate_down_now
    wait_plant 588000
    check "train 3 confirmed: the gate is UP again within 15 s" gate_up_within 150
    check "two West windows confirmed (win_island=2,0)" eq "$(cfield win_island)" 2,0
    wait_plant 825000
    check "825 s, window of train 5 open (820 s): the controller wants the gate DOWN" target_down_now
    check "the gate is DOWN within 10 s, before train 5 arrives 30 s early at 850 s" gate_down_within 100
    wait_plant 856000
    check "train 5 confirmed: the gate is UP again within 15 s" gate_up_within 150
    check "three West windows confirmed (win_island=3,0)" eq "$(cfield win_island)" 3,0
    wait_plant 1000000 # the meter ends at 1000 s; finish_meter only allows 120 s after that
    finish_meter
    safety_checks part2 6
    cph=$(csv_get closure_ms_per_hour)
    echo "RESULT part2_closure_ms_per_hour=$cph permanent_ms_per_hour=$PERMANENT limit_pct=$LIMIT_PCT"
    [ -n "$cph" ] && echo "RESULT part2_closure_pct_of_permanent=$((cph * 100 / PERMANENT))"
    check "closure per hour in level 3 ($cph ms) is at most $LIMIT_PCT % of permanent closure" less_than "$((${cph:-999999999} * 100))" "$((PERMANENT * LIMIT_PCT + 1))"
    check "the level is still 3 at the end of part 2" eq "$(cfield level)" 3
    stall_report A
}

run_b() {
    TT=$BIN/scenarios/level3_outage.timetable.json
    boot level3_outage.json $TT
    start_meter 230000

    echo "=== part 3: cancelled West train (entry 7, window 50 .. 100 s)"
    west_fails 10000
    wait_plant 45000
    # The East train leaves the island at about 41.3 s (132 m at 25 m/s); with T_clear the gate is still
    # rising at 45 s (first run of B): check what the controller wants, and that the gate gets UP.
    check "45 s, East train cleared and no West window yet: the controller wants the gate UP" eq "$(cfield target)" UP
    check "the gate is UP before the window opens at 50 s" gate_up_within 50
    wait_plant 52000
    check "52 s, window 7 open: the gate goes DOWN within 10 s" gate_down_within 100
    wait_plant 102000
    check "102 s: past the window end, the gate is still DOWN (no automatic opening)" gate_down_now
    check "overdue notification to the guard (to=guard TRAIN_OVERDUE dir=west)" has_event "to=guard" "diag=TRAIN_OVERDUE" "dir=west"
    check "an unknown guard id is refused (exit 4)" sh -c "$BIN/lgctl guard-confirm --id X99 --dir west >/dev/null 2>&1; [ \$? -eq 4 ]"
    check "the refused confirmation changed nothing (gate still DOWN)" gate_down_now
    check "lgctl guard-confirm --id G01 --dir west (exit 0)" sh -c "$BIN/lgctl guard-confirm --id G01 --dir west >/dev/null"
    check "the gate is UP again within 15 s" gate_up_within 150
    check "one West window confirmed by the guard (win_guard=1,0)" eq "$(cfield win_guard)" 1,0

    echo "=== part 5: schedule information lost (lgctl schedule outage all)"
    check "lgctl schedule outage all (exit 0)" sh -c "$BIN/lgctl schedule outage all >/dev/null"
    t0=$(plant_ms)
    check "/dev/lg/schedule shows the outage" sched_has "dir=west info=0 outage=1"
    wait_plant $((t0 + 50000))
    check "50 s later: still level 3 (T_info_timeout is 60 s)" eq "$(cfield level)" 3
    wait_plant $((t0 + 65000))
    check "65 s after the outage: level 4" eq "$(cfield level)" 4
    check "the level change says DETECT_LOST_NO_INFO (reason 8)" has_event "kind=LEVEL" "from=3" "to=4" "reason=8"
    check "no schedule information any more (sched_ok=0,0)" eq "$(cfield sched_ok)" 0,0
    check "level 4: the gate is DOWN within 10 s" gate_down_within 100
    check "station told the crossing is in fail-safe (to=station NO_SCHEDULE)" has_event "to=station" "diag=NO_SCHEDULE"
    check "guard told (to=guard NO_SCHEDULE)" has_event "to=guard" "diag=NO_SCHEDULE"
    check "road sign: CROSSING CLOSED (to=road_sign NO_SCHEDULE)" has_event "to=road_sign" "diag=NO_SCHEDULE"
    echo "--- repair: information back, faults cleared, health reset, guard acknowledges"
    $BIN/lgctl schedule outage none
    $BIN/lgctl sensor west/a none
    $BIN/lgctl sensor west/b none
    sleep 3
    $BIN/lgctl health reset west/a
    $BIN/lgctl health reset west/b
    sleep 5
    check "information is back (sched_ok=1,1)" eq "$(cfield sched_ok)" 1,1
    check "level 4 holds without a guard" eq "$(cfield level)" 4
    check "lgctl guard-ack --id G01 (exit 0)" sh -c "$BIN/lgctl guard-ack --id G01 >/dev/null"
    check "level 4 is left within 45 s (T_stable 30 s)" level_below4_within 450
    finish_meter
    safety_checks partB 1
    stall_report B
}

case "$MODE" in
A | B) ;;
*)
    echo "usage: step12_level3.sh A|B"
    exit 2
    ;;
esac
echo "=== step12_level3 $MODE on $(uname -a 2>/dev/null)"
if [ "$MODE" = A ]; then run_a; else run_b; fi
echo "--- final state"
cat /dev/lg/controller
cat /dev/lg/schedule
cat /dev/lg/events
cleanup
echo "RESULT checks=$checks fails=$fails"
if [ $fails -eq 0 ]; then
    echo "RESULT step12_level3_$MODE PASS"
    exit 0
fi
echo "RESULT step12_level3_$MODE FAIL"
exit 1
