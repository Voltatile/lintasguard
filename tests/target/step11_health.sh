#!/bin/sh
# Step 11 target test (README 9 step 11, level 1 early warning). Runs ON the QNX target:
#
#   ssh root@<vm-ip> sh -s < tests/target/step11_health.sh         (or make evidence TESTS="step11_health")
#
# Done when: DRIFT on a sensor or SLOW on the gate produces a WARNING and a maintenance notification
# BEFORE the component reaches FAILED, with traffic unaffected; the lead time is recorded as a metric.
#
#  part 1  baseline under lg-supervisor (lg-health installed): no warning at all, level 0, every verdict OK
#  part 2  DRIFT 1 unit/s on west/a at 50 s: WARNING (NOISE_RISING) and level 1; train 2 is activated and
#          its gate lowered in level 1 with the normal pre-warning (traffic unaffected); then FAILED: URGENT
#          ticket, METRIC lead_time_ms = t(FAILED) - t(WARNING); fault cleared at 100 s: OK, level 0
#  part 3  gate wear in steps between trains (SLOW 125, 150, 180 %): WARNING (GATE_SLOW) before FAILED,
#          level 4 with the gate down, lead time; repair: fault none, lgctl health reset gate, guard-ack
#  part 4  lg-health killed: the supervisor restarts it, level 2 for the restart, back to level 0
#
# Every part runs lg-meter and checks wrong-side 0 (never skipped). Prints one PASS:/FAIL: line per check,
# RESULT key=value lines, and exits non-zero when any check fails. Every check is ONE command. Needs
# everything in /tmp/lintasguard (make deploy IP=<vm-ip>). Plant time runs with the real clock; timings
# come from a VirtualBox VM: functional evidence only.

BIN=/tmp/lintasguard
CSV=/tmp/lg_s11_metrics.csv
SC=health_faults.json
T_STABLE_MS=30000 # LG_T_STABLE_MS in common/include/lg_config.h
PREWARNING_MS=5000 # LG_PREWARNING_MS
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
hverdict() { grep "^comp=$1 " /dev/lg/health 2>/dev/null | tr ' ' '\n' | sed -n 's/^verdict=//p'; }
plant_ms() { cat /dev/lg/plant 2>/dev/null | tr ' ' '\n' | sed -n 's/^t_ms=//p'; }
csv_get() { sed -n "s/^$1,//p" $CSV; }
pid_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$' | head -n 1 | tr -d ' '; }
killproc() { for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do kill -9 $p; done; }
cleanup() { # the supervisor first, so that it does not start anything again
    killproc lg-supervisor
    killproc lg-bridge
    killproc lg-health
    killproc lg-meter
    killproc lg-controller
    killproc lg-sensor
    killproc lg-plant
    killproc lg-gate
}
eq() { [ -n "$1" ] && [ "$1" = "$2" ]; }
at_least() { [ -n "$1" ] && [ -n "$2" ] && [ "$1" -ge "$2" ]; }
less_than() { [ -n "$1" ] && [ -n "$2" ] && [ "$1" -lt "$2" ]; }
# The first line of /dev/lg/events that matches all the given patterns (fixed strings).
ev_line() {
    l=$(cat /dev/lg/events 2>/dev/null)
    for p in "$@"; do
        l=$(echo "$l" | grep -F -- "$p")
    done
    echo "$l" | head -n 1
}
efield() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
level_trail() { # level transitions of controller pid $1 from slog2 (the event ring holds only 32)
    slog2info 2>/dev/null | grep -E "lg_controller\.$1[[:space:]]" |
        sed -n 's/.*level \([0-9]\) -> \([0-9]\).*/\1>\2/p' | tr '\n' ' '
}
trail_has() { echo "$1" | grep -q -- "$2"; }
wait_level0() { # boot release: level 0 and gate UP within 25 s
    i=0
    while [ $i -lt 250 ]; do
        [ "$(cfield level)" = "0" ] && [ "$(gfield state)" = "UP" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
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
fresh() { # fresh <scenario>: a new system under lg-supervisor, released to level 0
    cleanup
    rm -f $CSV /tmp/lg_s11_meter.out
    sleep 1.5
    $BIN/lg-supervisor -b $BIN -c $BIN/config/crossing.json -s $BIN/scenarios/$1 </dev/null >/tmp/lg_s11_sup.log 2>&1 &
    wait_level0
}
controller_pid() {
    c=""
    i=0
    while [ -z "$c" ] && [ $i -lt 20 ]; do
        c=$(pid_of lg-controller)
        [ -z "$c" ] && sleep 0.1
        i=$((i + 1))
    done
    echo "$c"
}
start_meter() { # start_meter <end_ms>
    $BIN/lg-meter -e "$1" -m $(($1 / 1000 + 60)) -o $CSV </dev/null >/tmp/lg_s11_meter.out 2>&1 &
    MPID=$!
}
finish_meter() {
    i=0
    while [ $i -lt 600 ] && kill -0 $MPID 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    wait $MPID 2>/dev/null
    METER_RC=$?
    echo "--- lg-meter output (exit $METER_RC):"
    cat /tmp/lg_s11_meter.out
}
safety_checks() { # safety_checks <part> <expected trains>
    for k in trains trains_without_warning closure_count closure_max_ms warning_avg_ms wrong_side_events \
        wrong_side_ticks meter_coverage_pct meter_max_gap_ms; do
        echo "RESULT $1_$k=$(csv_get $k)"
    done
    check "$1: wrong_side_events is 0" eq "$(csv_get wrong_side_events)" 0
    check "$1: wrong_side_ticks is 0" eq "$(csv_get wrong_side_ticks)" 0
    check "$1: lg-meter observed the whole run and saw no wrong side (exit 0)" [ "$METER_RC" = "0" ]
    check "$1: every train reached the island with the lights on" eq "$(csv_get trains_without_warning)" 0
    check "$1: trains seen by the meter: $2" eq "$(csv_get trains)" "$2"
}
all_ok() { # every component of /dev/lg/health has verdict OK
    for c in west/a west/b west/island east/a east/b east/island gate battery; do
        [ "$(hverdict $c)" = "OK" ] || return 1
    done
    return 0
}
sup_health_total() { sfield health total; }
gate_bad() { v=$(cfield comp_bad); [ -n "$v" ] && [ $((v & 0x40)) -ne 0 ]; }
new_pid() { [ -n "$1" ] && [ "$1" != "$2" ]; }
no_warning_events() { ! grep -q "severity=WARNING to=maintenance" /dev/lg/events; }
dump() {
    echo "--- /dev/lg/events:"
    cat /dev/lg/events
    echo "--- /dev/lg/controller:"
    cat /dev/lg/controller
    echo "--- /dev/lg/health:"
    cat /dev/lg/health
}

echo "=== Step 11 health test, scenario $SC, $(date)"
uname -a

# ---------------------------------------------------------------------------------------------------
echo "=== part 1: baseline, no fault (false alarms)"
check "part 1: fresh system released to level 0" fresh $SC
CPID=$(controller_pid)
check "part 1: lg-health runs under the supervisor" [ -n "$(pid_of lg-health)" ]
check "part 1: the controller was started with -H (health_armed 1 after the boot)" eq "$(cfield health_armed)" 1
start_meter 130000
MAXL=0
while :; do
    p=$(plant_ms)
    [ -z "$p" ] && break
    [ "$p" -ge 130000 ] && break
    lv=$(cfield level)
    [ -n "$lv" ] && [ "$lv" -gt "$MAXL" ] && MAXL=$lv
    sleep 0.2
done
finish_meter
dump
echo "RESULT part1_max_level=$MAXL"
echo "RESULT part1_warnings=$(cfield warnings)"
echo "RESULT part1_level_trail=$(level_trail "$CPID" | tr ' ' ',')"
check "part 1: no WARNING notification at all" no_warning_events
check "part 1: controller warnings counter is 0" eq "$(cfield warnings)" 0
check "part 1: no maintenance ticket" eq "$(cfield tickets)" 0
check "part 1: the level stayed 0 after the boot release" eq "$MAXL" 0
check "part 1: every component of /dev/lg/health is OK" all_ok
check "part 1: lg-health was never silent" eq "$(cfield health_silent)" 0
check "part 1: no learned noise baseline above base + 2 units (west/a)" less_than \
    "$(grep '^comp=west/a ' /dev/lg/health | tr ' ' '\n' | sed -n 's/^base_milli=//p')" 5001
safety_checks part1 3

# ---------------------------------------------------------------------------------------------------
echo "=== part 2: DRIFT 1 unit/s on west/a at 50 s"
check "part 2: fresh system released to level 0" fresh $SC
CPID=$(controller_pid)
start_meter 175000
INJ=""
CLR=""
LIGHTS_L1=""   # plant time the lights came on while in level 1
LOWER_L1=""    # plant time the gate left UP while in level 1
LV1_SEEN=0
prev_lights=1
prev_state=UP
while :; do
    p=$(plant_ms)
    [ -z "$p" ] && break
    [ "$p" -ge 175000 ] && break
    if [ -z "$INJ" ] && [ "$p" -ge 50000 ]; then
        INJ=$p
        check "part 2: inject lgctl sensor west/a fault drift 1 (exit 0)" $BIN/lgctl sensor west/a fault drift 1
        echo "RESULT part2_inject_plant_ms=$INJ"
    fi
    if [ -z "$CLR" ] && [ "$p" -ge 100000 ]; then
        CLR=$p
        check "part 2: clear lgctl sensor west/a fault none (exit 0)" $BIN/lgctl sensor west/a fault none
        echo "RESULT part2_clear_plant_ms=$CLR"
    fi
    lv=$(cfield level)
    li=$(gfield lights)
    st=$(gfield state)
    if [ "$lv" = "1" ]; then
        LV1_SEEN=1
        [ -z "$LIGHTS_L1" ] && [ "$li" = "1" ] && [ "$prev_lights" = "0" ] && LIGHTS_L1=$p
        [ -n "$LIGHTS_L1" ] && [ -z "$LOWER_L1" ] && [ "$st" != "UP" ] && [ "$prev_state" = "UP" ] && LOWER_L1=$p
    fi
    [ -n "$li" ] && prev_lights=$li
    [ -n "$st" ] && prev_state=$st
    sleep 0.1
done
END_LEVEL=$(cfield level)
finish_meter
dump
TRAIL=$(level_trail "$CPID")
WL=$(ev_line "severity=WARNING to=maintenance sensor=west/a")
FL=$(ev_line "severity=URGENT to=maintenance sensor=west/a")
ML=$(ev_line "kind=METRIC name=lead_time_ms component=west/a")
IL=$(ev_line "severity=INFO to=maintenance sensor=west/a")
TW=$(efield "$WL" t_ms)
TF=$(efield "$FL" t_ms)
LEAD=$(efield "$ML" value)
echo "RESULT part2_warning_line=$WL"
echo "RESULT part2_failed_line=$FL"
echo "RESULT part2_metric_line=$ML"
echo "RESULT part2_level_trail=$(echo "$TRAIL" | tr ' ' ',')"
echo "RESULT part2_lights_on_in_level1_plant_ms=$LIGHTS_L1"
echo "RESULT part2_gate_left_up_in_level1_plant_ms=$LOWER_L1"
echo "RESULT part2_lead_time_ms=$LEAD"
[ -n "$TW" ] && [ -n "$TF" ] && echo "RESULT part2_failed_minus_warning_ms=$((TF - TW))"
check "part 2: WARNING notification for west/a" [ -n "$WL" ]
check "part 2: its diagnosis is NOISE_RISING" eq "$(efield "$WL" diag)" NOISE_RISING
check "part 2: it carries a trend (trend_s at least 1)" at_least "$(efield "$WL" trend_s)" 1
check "part 2: level 1 was entered (0>1 in the level trail)" trail_has "$TRAIL" "0>1"
check "part 2: URGENT ticket for west/a (FAILED)" [ -n "$FL" ]
check "part 2: the warning came before the failure (seq)" less_than "$(efield "$WL" seq)" "$(efield "$FL" seq)"
check "part 2: the warning came before the failure (time)" less_than "$TW" "$TF"
check "part 2: level 2 followed level 1 (1>2 in the level trail)" trail_has "$TRAIL" "1>2"
check "part 2: METRIC lead_time_ms for west/a" [ -n "$ML" ]
check "part 2: lead_time_ms equals t(FAILED) - t(WARNING)" eq "$LEAD" "$((${TF:-0} - ${TW:-0}))"
check "part 2: lead time at least 5 s" at_least "$LEAD" 5000
check "part 2: a train was activated while in level 1" [ -n "$LIGHTS_L1" ]
check "part 2: its gate was lowered while in level 1" [ -n "$LOWER_L1" ]
check "part 2: with the normal pre-warning (lights on to gate moving at least 4.5 s, level 2 would be 0)" \
    at_least "$((${LOWER_L1:-0} - ${LIGHTS_L1:-0}))" 4500
check "part 2: INFO notification when west/a was OK again" [ -n "$IL" ]
check "part 2: back to level 0 by the end (fault cleared at 100 s)" eq "$END_LEVEL" 0
check "part 2: the healthy partner west/b got no notification" [ -z "$(ev_line 'to=maintenance sensor=west/b')" ]
check "part 2: the controller was not restarted" eq "$(pid_of lg-controller)" "$CPID"
safety_checks part2 4

# ---------------------------------------------------------------------------------------------------
echo "=== part 3: gate wear (SLOW 125 % at 50 s, 150 % at 95 s, 180 % at 135 s)"
check "part 3: fresh system released to level 0" fresh $SC
CPID=$(controller_pid)
start_meter 218000
S1=""
S2=""
S3=""
REPAIR=""
ACK=""
L1_PLANT=""
while :; do
    p=$(plant_ms)
    [ -z "$p" ] && break
    [ "$p" -ge 218000 ] && break
    if [ -z "$S1" ] && [ "$p" -ge 50000 ]; then
        S1=$p
        check "part 3: lgctl gate fault slow 125 (exit 0)" $BIN/lgctl gate fault slow 125
    fi
    if [ -z "$S2" ] && [ "$p" -ge 95000 ]; then
        S2=$p
        check "part 3: lgctl gate fault slow 150 (exit 0)" $BIN/lgctl gate fault slow 150
    fi
    if [ -z "$S3" ] && [ "$p" -ge 135000 ]; then
        S3=$p
        check "part 3: lgctl gate fault slow 180 (exit 0)" $BIN/lgctl gate fault slow 180
    fi
    [ -z "$L1_PLANT" ] && [ "$(cfield level)" = "1" ] && L1_PLANT=$p
    if [ -z "$REPAIR" ] && [ "$(cfield level)" = "4" ] && [ -n "$S3" ] && [ "$(gfield state)" = "DOWN" ]; then
        REPAIR=$p
        echo "RESULT part3_level4_plant_ms=$p"
        echo "--- /dev/lg/health at the failure:"
        cat /dev/lg/health
        check "part 3: the gate is held DOWN in level 4" eq "$(gfield state)" DOWN
        # Review M1: a restarted lg-health has not looked at the gate yet; that is no repair.
        check "part 3: lgctl kill health while the gate is FAILED (exit 0)" $BIN/lgctl kill health
        check "part 3: lg-health restarted (total 1)" wait_eq sup_health_total "" 1 30
        sleep 3
        check "part 3: the gate is still FAILED for the controller after the restart (comp_bad has bit 0x40)" gate_bad
        check "part 3: still level 4 after the lg-health restart" eq "$(cfield level)" 4
        echo "RESULT part3_health_unassessed=$(cfield health_unassessed)"
        check "part 3: repair: lgctl gate fault none (exit 0)" $BIN/lgctl gate fault none
        check "part 3: repair: lgctl health reset gate (exit 0)" $BIN/lgctl health reset gate
        sleep 1
        check "part 3: the gate verdict is OK after the reset" eq "$(hverdict gate)" OK
        check "part 3: still level 4 after the repair (no release without a guard)" eq "$(cfield level)" 4
        check "part 3: lgctl guard-ack --id G01 (exit 0)" $BIN/lgctl guard-ack --id G01
        ACK=$(plant_ms)
        echo "RESULT part3_guard_ack_plant_ms=$ACK"
    fi
    sleep 0.1
done
END_LEVEL=$(cfield level)
END_GATE=$(gfield state)
finish_meter
dump
TRAIL=$(level_trail "$CPID")
WL=$(ev_line "severity=WARNING to=maintenance" "component=gate")
FL=$(ev_line "severity=URGENT to=maintenance" "component=gate")
ML=$(ev_line "kind=METRIC name=lead_time_ms component=gate")
TW=$(efield "$WL" t_ms)
TF=$(efield "$FL" t_ms)
LEAD=$(efield "$ML" value)
echo "RESULT part3_warning_line=$WL"
echo "RESULT part3_failed_line=$FL"
echo "RESULT part3_metric_line=$ML"
echo "RESULT part3_level_trail=$(echo "$TRAIL" | tr ' ' ',')"
echo "RESULT part3_lead_time_ms=$LEAD"
echo "RESULT part3_steps_plant_ms=$S1,$S2,$S3"
check "part 3: WARNING notification for the gate" [ -n "$WL" ]
check "part 3: its diagnosis is GATE_SLOW" eq "$(efield "$WL" diag)" GATE_SLOW
echo "RESULT part3_level1_plant_ms=$L1_PLANT"
check "part 3: level 1 came with the 125 % step (before the 150 % step at 95 s)" less_than "$L1_PLANT" "${S2:-95000}"
check "part 3: level 1 was entered (0>1)" trail_has "$TRAIL" "0>1"
check "part 3: URGENT ticket for the gate (FAILED)" [ -n "$FL" ]
check "part 3: the warning came before the failure (seq)" less_than "$(efield "$WL" seq)" "$(efield "$FL" seq)"
check "part 3: level 4 followed (1>4)" trail_has "$TRAIL" "1>4"
check "part 3: METRIC lead_time_ms for the gate" [ -n "$ML" ]
check "part 3: lead_time_ms equals t(FAILED) - t(WARNING)" eq "$LEAD" "$((${TF:-0} - ${TW:-0}))"
check "part 3: lead time at least 10 s" at_least "$LEAD" 10000
check "part 3: the guard acknowledgement released level 4 (4>0 after the failure)" trail_has "$TRAIL" "4>0 .*4>0"
check "part 3: level 0 at the end" eq "$END_LEVEL" 0
check "part 3: gate UP at the end" eq "$END_GATE" UP
check "part 3: the controller was not restarted" eq "$(pid_of lg-controller)" "$CPID"
safety_checks part3 5

# ---------------------------------------------------------------------------------------------------
echo "=== part 4: lg-health killed"
check "part 4: fresh system released to level 0" fresh test_idle.json
CPID=$(controller_pid)
start_meter 60000
HP=$(pid_of lg-health)
check "part 4: lgctl kill health (exit 0)" $BIN/lgctl kill health
check "part 4: the supervisor restarted lg-health within 3 s (total 1)" wait_eq sup_health_total "" 1 30
check "part 4: the controller goes to level 2 for the restart" wait_eq cfield level 2 30
NHP=$(pid_of lg-health)
check "part 4: it is a new process" new_pid "$NHP" "$HP"
# Watch the gate on every 100 ms poll until level 0 is back (at most T_stable + 15 s).
GATE_LEFT_UP=0
BACK0=0
i=0
while [ $i -lt 450 ]; do
    [ "$(gfield state)" != "UP" ] && GATE_LEFT_UP=1
    if [ "$(cfield level)" = "0" ]; then
        BACK0=1
        break
    fi
    sleep 0.1
    i=$((i + 1))
done
check "part 4: back to level 0 within T_stable + 15 s" eq "$BACK0" 1
check "part 4: the gate stayed UP on every poll during the restart (no train in this scenario)" eq "$GATE_LEFT_UP" 0
check "part 4: the gate watchdog did not trip" eq "$(gfield watchdog_trips)" 0
check "part 4: health verdicts flow again (health_silent 0)" eq "$(cfield health_silent)" 0
finish_meter
echo "RESULT part4_level_trail=$(level_trail "$CPID" | tr ' ' ',')"
# test_idle.json has no train before 3000 s: these two hold trivially here, they are kept because every
# part checks wrong-side (CLAUDE.md rule 3); parts 1 to 3 check them with trains.
check "part 4: wrong_side_events is 0" eq "$(csv_get wrong_side_events)" 0
check "part 4: wrong_side_ticks is 0" eq "$(csv_get wrong_side_ticks)" 0
check "part 4: lg-meter observed the whole run (exit 0)" [ "$METER_RC" = "0" ]

cleanup
echo "RESULT checks=$checks fails=$fails"
if [ $fails -ne 0 ]; then
    echo "FAIL: step11_health ($fails of $checks checks failed)"
    exit 1
fi
echo "PASS: step11_health ($checks checks)"
exit 0
