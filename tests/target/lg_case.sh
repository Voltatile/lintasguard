#!/bin/sh
# One fault-campaign case (README 9 step 9, 11.1). Runs ON the QNX target:
#
#   ssh root@<vm-ip> sh -s -- <name> <sensor> <fault> <inject_ms> <clear_ms> <end_ms> \
#       <expect_trains> <expect_level> <expect_diag> <scenario file> < tests/target/lg_case.sh
#
# host/campaign_runner/run.py builds these arguments from campaigns/*.json.
#   sensor       west/a ... east/island
#   fault        an lgctl sensor fault mode (stuck-occupied, stuck-clear, hang, crash, ...) or none
#   inject_ms    plant time of the injection; clear_ms plant time of "fault none", 0 = never
#   end_ms       plant time at which the case ends (lg-meter -e)
#   expect_diag  diagnosis of the URGENT ticket for the sensor, alternatives separated by |, NONE = no
#                ticket at all
# Needs everything in /tmp/lintasguard (make deploy IP=<vm-ip>). A fresh system under lg-supervisor,
# lg-meter watching the island truth and the gate. Prints one PASS:/FAIL: line per check, RESULT
# key=value lines for the report, and exits non-zero when any check fails. Every check is ONE command.
# The wrong-side checks are in every case and are never skipped. Plant time runs with the real clock;
# timings come from a VirtualBox VM: functional evidence only.

NAME=$1
SENSOR=$2
FAULT=$3
INJECT_MS=$4
CLEAR_MS=$5
END_MS=$6
EXPECT_TRAINS=$7
EXPECT_LEVEL=$8
EXPECT_DIAG=$9
SCENARIO=${10}

BIN=/tmp/lintasguard
CSV=/tmp/lg_case_metrics.csv
T_STABLE_MS=30000 # LG_T_STABLE_MS in common/include/lg_config.h
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
plant_ms() { cat /dev/lg/plant 2>/dev/null | tr ' ' '\n' | sed -n 's/^t_ms=//p'; }
csv_get() { sed -n "s/^$1,//p" $CSV; }
pid_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$' | head -n 1 | tr -d ' '; }
killproc() { for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do kill -9 $p; done; }
cleanup() { # the supervisor first, so that it does not start anything again
    killproc lg-supervisor
    killproc lg-bridge
    killproc lg-meter
    killproc lg-controller
    killproc lg-sensor
    killproc lg-plant
    killproc lg-gate
}
eq() { [ -n "$1" ] && [ "$1" = "$2" ]; }
at_least() { [ -n "$1" ] && [ -n "$2" ] && [ "$1" -ge "$2" ]; }
wait_level0() { # boot release: level 0 and gate UP within 25 s
    i=0
    while [ $i -lt 250 ]; do
        [ "$(cfield level)" = "0" ] && [ "$(gfield state)" = "UP" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
# The transitions of the level of controller pid $1, in order (for example "4>0 0>2 2>0 "), from its
# slog2 lines. The pid must be followed by blanks: a plain prefix also matched other controllers whose
# pid starts with the same digits (seen in the first campaign trial). slog2 is used and not the event
# ring because a run with many tickets pushes the level events out of the 32-entry ring.
level_trail() {
    slog2info 2>/dev/null | grep -E "lg_controller\.$1[[:space:]]" |
        sed -n 's/.*level \([0-9]\) -> \([0-9]\).*/\1>\2/p' | tr '\n' ' '
}
ticket_ok() { # the URGENT ticket for the sensor carries one of the expected diagnoses
    grep "severity=URGENT to=maintenance sensor=$SENSOR " /dev/lg/events | grep -qE "diag=($EXPECT_DIAG) "
}
lgctl_ok() { $BIN/lgctl sensor "$SENSOR" fault "$1" >/dev/null 2>&1; }

echo "=== case $NAME: sensor $SENSOR fault $FAULT inject $INJECT_MS clear $CLEAR_MS end $END_MS scenario $SCENARIO"
echo "RESULT case=$NAME"
cleanup
rm -f $CSV /tmp/lg_case_meter.out
sleep 1.5

$BIN/lg-supervisor -b $BIN -c $BIN/config/crossing.json -s $BIN/scenarios/$SCENARIO \
    </dev/null >/tmp/lg_case_sup.log 2>&1 &
check "boot: level 0 and gate UP within 25 s" wait_level0
# pidin on a busy VM sometimes returns nothing: try for up to 2 s (seen once: an empty pid emptied the
# level trail and failed the pid check of a controller that had run the whole case).
CPID=""
i=0
while [ -z "$CPID" ] && [ $i -lt 20 ]; do
    CPID=$(pid_of lg-controller)
    [ -z "$CPID" ] && sleep 0.1
    i=$((i + 1))
done
echo "RESULT controller_pid=$CPID"
BOOT_MS=$(plant_ms)
echo "RESULT boot_plant_ms=$BOOT_MS"
if [ "$FAULT" != "none" ]; then
    check "the boot release came before the injection time" at_least "$INJECT_MS" "${BOOT_MS:-999999}"
fi

$BIN/lg-meter -e "$END_MS" -m $((END_MS / 1000 + 60)) -o $CSV </dev/null >/tmp/lg_case_meter.out 2>&1 &
MPID=$!

# Watch the level every 100 ms from the boot release to the end; inject and clear on the way.
UP_MS=""
BACK_MS=""
MAX_LEVEL=0
INJECTED=0
INJ_AT=""
CLEARED=0
CLR_AT=""
[ "$FAULT" = "none" ] && INJECTED=1
while :; do
    p=$(plant_ms)
    [ -z "$p" ] && break
    [ "$p" -ge "$END_MS" ] && break
    if [ $INJECTED -eq 0 ] && [ "$p" -ge "$INJECT_MS" ]; then
        INJECTED=1
        INJ_AT=$p
        if [ "$FAULT" = "crash" ]; then
            # The sensor aborts inside the devctl call, so lgctl may report an error: the restart
            # check below (supervisor total 1) is what counts.
            lgctl_ok crash
            echo "RESULT inject_lgctl_rc=$?"
        else
            check "inject: lgctl sensor $SENSOR fault $FAULT (exit 0)" lgctl_ok "$FAULT"
        fi
        echo "RESULT inject_plant_ms=$INJ_AT"
    fi
    if [ "$CLEAR_MS" -gt 0 ] && [ $CLEARED -eq 0 ] && [ "$p" -ge "$CLEAR_MS" ]; then
        CLEARED=1
        CLR_AT=$p
        check "clear: lgctl sensor $SENSOR fault none (exit 0)" lgctl_ok none
        echo "RESULT clear_plant_ms=$CLR_AT"
    fi
    lv=$(cfield level)
    if [ -n "$lv" ]; then
        [ "$lv" -gt "$MAX_LEVEL" ] && MAX_LEVEL=$lv
        if [ "$lv" -gt 0 ] && [ -z "$UP_MS" ]; then
            UP_MS=$p
        fi
        if [ "$lv" = "0" ] && [ -n "$UP_MS" ] && [ -z "$BACK_MS" ]; then
            BACK_MS=$p
        fi
    fi
    sleep 0.1
done

# The end state, read at once: the next train of the scenario may strike in soon after end_ms.
END_PLANT=$(plant_ms)
END_LEVEL=$(cfield level)
END_FAILED=$(cfield sensor_failed)
END_GATE="$(gfield state)$(gfield lights)"
END_WD=$(gfield watchdog_trips)
END_CPID=""
i=0
while [ -z "$END_CPID" ] && [ $i -lt 20 ]; do
    END_CPID=$(pid_of lg-controller)
    [ -z "$END_CPID" ] && sleep 0.1
    i=$((i + 1))
done
echo "RESULT end_plant_ms=$END_PLANT"

# lg-meter ends by itself at END_MS.
i=0
while [ $i -lt 300 ] && kill -0 $MPID 2>/dev/null; do
    sleep 0.1
    i=$((i + 1))
done
wait $MPID 2>/dev/null
METER_RC=$?

TRAIL=$(level_trail "$CPID")
REF=${INJ_AT:-$INJECT_MS}
[ -n "$CLR_AT" ] && REF_CLR=$CLR_AT || REF_CLR=$REF
REC_INJECT=""
REC_CLEAR=""
if [ -n "$BACK_MS" ]; then
    REC_INJECT=$((BACK_MS - REF))
    REC_CLEAR=$((BACK_MS - REF_CLR))
fi
SUP_REC=$(sfield "sensor/$SENSOR" recovery_ms)
SUP_TOTAL=$(sfield "sensor/$SENSOR" total)

echo "--- lg-meter output (exit $METER_RC):"
cat /tmp/lg_case_meter.out
echo "--- /dev/lg/events:"
cat /dev/lg/events
echo "--- /dev/lg/controller:"
cat /dev/lg/controller
echo "--- sensors:"
for s in west/a west/b west/island east/a east/b east/island; do echo "$s: $(cat /dev/lg/sensor/$s 2>/dev/null)"; done
echo "--- supervisor row of sensor/$SENSOR:"
grep "^proc=sensor/$SENSOR " /dev/lg/supervisor

echo "RESULT level_trail=$(echo "$TRAIL" | tr ' ' ',')"
echo "RESULT max_level=$MAX_LEVEL"
echo "RESULT level_up_plant_ms=$UP_MS"
echo "RESULT level0_plant_ms=$BACK_MS"
echo "RESULT recovery_inject_ms=$REC_INJECT"
echo "RESULT recovery_clear_ms=$REC_CLEAR"
echo "RESULT supervisor_recovery_ms=$SUP_REC"
echo "RESULT supervisor_restarts=$SUP_TOTAL"
echo "RESULT tickets=$(cfield tickets)"
echo "RESULT events=$(cfield events)"
echo "RESULT sensor_failed=$END_FAILED"
echo "RESULT unmet=$(cfield unmet)"
echo "RESULT late_ticks=$(cfield late_ticks)"
echo "RESULT ticks=$(cfield ticks)"
for k in trains trains_without_warning closure_count closure_max_ms closure_ms_per_hour warning_avg_ms \
    wrong_side_events wrong_side_ticks meter_coverage_pct meter_max_gap_ms controller_cycles; do
    echo "RESULT $k=$(csv_get $k)"
done

# Safety first: these hold in every case.
check "wrong_side_events is 0" eq "$(csv_get wrong_side_events)" 0
check "wrong_side_ticks is 0" eq "$(csv_get wrong_side_ticks)" 0
check "lg-meter observed the whole run and saw no wrong side (exit 0)" [ "$METER_RC" = "0" ]
check "every train reached the island with the lights on (trains_without_warning 0)" eq "$(csv_get trains_without_warning)" 0
check "trains seen by the meter: $EXPECT_TRAINS" eq "$(csv_get trains)" "$EXPECT_TRAINS"
check "every train had a gate closure (closure_count at least $EXPECT_TRAINS)" at_least "$(csv_get closure_count)" "$EXPECT_TRAINS"
check "the controller was not restarted (same pid at the end)" eq "$END_CPID" "$CPID"
# The fault was seen and handled at the expected level.
check "highest level after the boot release is $EXPECT_LEVEL (never 3 or 4)" eq "$MAX_LEVEL" "$EXPECT_LEVEL"
if [ "$EXPECT_DIAG" = "NONE" ]; then
    check "no maintenance ticket (tickets 0)" eq "$(cfield tickets)" 0
    check "the level trail is only the boot release (4>0)" eq "$TRAIL" "4>0 "
else
    check "an URGENT ticket for $SENSOR with diag $EXPECT_DIAG" ticket_ok
    check "exactly one ticket (tickets 1)" eq "$(cfield tickets)" 1
    check "the level trail is boot release, down to 2, back to 0" eq "$TRAIL" "4>0 0>2 2>0 "
    check "level 2 came after the injection (watched from the boot release)" at_least "$UP_MS" "$REF"
    check "level 0 again before the end (recovery recorded)" [ -n "$BACK_MS" ]
    check "back to level 0 not before T_stable after the fault ended" at_least "$REC_CLEAR" $T_STABLE_MS
fi
case "$FAULT" in
crash | hang) check "the supervisor restarted the sensor once (total 1)" eq "$SUP_TOTAL" 1 ;;
*) check "the sensor process was not restarted (total 0)" eq "$SUP_TOTAL" 0 ;;
esac
# The end state (read right after end_ms).
check "end: level 0" eq "$END_LEVEL" 0
check "end: no sensor FAILED (sensor_failed 0x0)" eq "$END_FAILED" 0x0
check "end: gate UP with the lights off" eq "$END_GATE" UP0
check "the gate watchdog never tripped" eq "$END_WD" 0

cleanup
echo "RESULT checks=$checks failed=$fails"
if [ $fails -eq 0 ]; then
    echo "PASS: case $NAME ($checks checks)"
    exit 0
fi
echo "FAIL: case $NAME ($fails of $checks checks failed)"
exit 1
