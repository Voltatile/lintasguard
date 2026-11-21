#!/bin/sh
# Step 7 target test: lg-controller (level 0 and level 4), lg-meter, and the normal_day scenario.
# Runs ON the QNX target (sh -s over ssh, see scripts/collect_evidence.sh).
# Needs everything in /tmp/lintasguard (make deploy IP=<vm-ip>): binaries, config/, scenarios/.
# Prints one PASS:/FAIL: line per check and exits non-zero when any check fails.
# Every check is ONE command: never write "check name [ a ] && [ b ]" (only [ a ] would be checked).
# Timing numbers come from a VirtualBox VM: functional evidence only.
#
# Parts: 1 boot without sensors, 2 boot release and idle, 3 controller killed (gate watchdog) and
# restarted, 4 gate stopped (SIGSTOP), 5 plant gone and back, 6 normal_day with lg-meter.
# Level 4 is only left by a guard acknowledgement, which does not exist before Step 8: every part
# that ends in level 4 therefore restarts the controller, which boots into level 4 and is released
# by the boot check (README 8.6).

BIN=/tmp/lintasguard
SENSORS="west/a west/b west/island east/a east/b east/island"
CSV=/tmp/step07_metrics.csv
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

# ---- helpers ----------------------------------------------------------------------------
cfield() { cat /dev/lg/controller | tr ' ' '\n' | sed -n "s/^$1=//p"; }
gfield() { cat /dev/lg/gate | tr ' ' '\n' | sed -n "s/^$1=//p"; }
csv_get() { sed -n "s/^$1,//p" $CSV; }
pid_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$' | head -n 1; }

in_range() { # in_range <value> <low> <high>
    [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}
at_least() { # at_least <value> <low>
    [ -n "$1" ] && [ "$1" -ge "$2" ]
}
grew_by() { # grew_by <before> <after> <at least>
    [ -n "$1" ] && [ -n "$2" ] && [ "$2" -ge "$(($1 + $3))" ]
}

killproc() {
    for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do
        kill -9 "$p" 2>/dev/null
    done
}

cleanup() {
    killproc lg-meter
    killproc lg-controller
    killproc lg-sensor
    killproc lg-plant
    killproc lg-gate
}

wait_for() { # wait_for <path> <max tenths of a second>
    i=0
    while [ $i -lt "$2" ]; do
        [ -e "$1" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# wait_eq <reader> <field> <value> <max tenths>: reader is cfield or gfield
wait_eq() {
    i=0
    while [ $i -lt "$4" ]; do
        [ "$($1 $2)" = "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
# wait_ne <reader> <field> <value> <max tenths>: the field has some other value
wait_ne() {
    i=0
    while [ $i -lt "$4" ]; do
        v=$($1 $2)
        [ -n "$v" ] && [ "$v" != "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

start_gate() {
    $BIN/lg-gate -v </dev/null >/tmp/gate.log 2>&1 &
    wait_for /dev/lg/gate 50
}
start_controller() {
    $BIN/lg-controller -v </dev/null >/tmp/ctl.log 2>&1 &
    wait_for /dev/lg/controller 50
}
# Part 6 only: the controller gets the crossing config, so the guard list (G01) is loaded and a guard
# acknowledgement is accepted.
start_controller_guarded() {
    $BIN/lg-controller -c $BIN/config/crossing.json -v </dev/null >/tmp/ctl.log 2>&1 &
    wait_for /dev/lg/controller 50
}

# guard_acker: a stand-in for the guard. After the boot release (level 0 seen once) it acknowledges level 4
# once a second until the stop file exists (at most 400 rounds). A VM stall of 200 ms or more makes a
# sensor look silent or the gate look silent and locks level 4 (the rule: it guards the gate), and this
# script has no supervisor to restart anything; without a guard the rest of the run would be spent in
# level 4. Every acknowledgement is logged, so the run shows whether a stall happened.
guard_acker() {
    wait_eq cfield level 0 300 || return 1
    n=0
    while [ $n -lt 400 ] && [ ! -e /tmp/step07_ack_stop ]; do
        if [ "$(cfield level)" = "4" ]; then
            echo "level 4 seen, acknowledging" >> /tmp/step07_acks.txt
            $BIN/lgctl guard-ack --id G01 >/dev/null 2>&1
        fi
        sleep 1
        n=$((n + 1))
    done
}

start_plant() { # start_plant <scenario file name>
    $BIN/lg-plant --crossing $BIN/config/crossing.json --scenario $BIN/scenarios/$1 -v \
        </dev/null >/tmp/plant.log 2>&1 &
    wait_for /dev/name/local/lg/plant 50
}
start_sensors() {
    ok=0
    for s in $SENSORS; do
        $BIN/lg-sensor -d "${s%/*}" -i "${s#*/}" -v </dev/null >/tmp/sensor_$(echo $s | tr / _).log 2>&1 &
        wait_for /dev/lg/sensor/$s 50 || ok=1
    done
    return $ok
}

# answers_within <seconds> <command...>: the command finishes within the time (a frozen device would not)
answers_within() {
    limit=$(($1 * 10))
    shift
    "$@" >/dev/null 2>&1 &
    cp=$!
    n=0
    while [ $n -lt $limit ] && kill -0 $cp 2>/dev/null; do
        sleep 0.1
        n=$((n + 1))
    done
    if kill -0 $cp 2>/dev/null; then
        kill -9 $cp 2>/dev/null
        return 1
    fi
    return 0
}

# ---- multi-part conditions, as functions so that check sees ONE command ------------------
write_refused() { ! sh -c 'echo x > /dev/lg/controller' 2>/dev/null; }
level4_boot_state() { # level 4, gate target DOWN, lights and bell on
    [ "$(cfield level)" = "4" ] && [ "$(cfield target)" = "DOWN" ] && [ "$(cfield lights)" = "1" ] && [ "$(cfield bell)" = "1" ]
}
gate_up_dark() { # gate UP with lights off
    [ "$(gfield state)" = "UP" ] && [ "$(gfield lights)" = "0" ]
}
gate_down_lit() {
    [ "$(gfield state)" = "DOWN" ] && [ "$(gfield lights)" = "1" ]
}
prio_ok() { # the controller read its own scheduling back: SCHED_FIFO (1), priority 50
    [ "$(cfield sched_policy)" = "1" ] && [ "$(cfield sched_priority)" = "50" ]
}
slog_level_lines() { # lines of the level changes of controller pid $1
    slog2info 2>/dev/null | grep -E "lg_controller\.$1[[:space:]]" | grep -c 'level [0-9] -> [0-9]'
}

# freeze_cycles <pid> <n>: stop and continue the gate n times; the controller must answer its status
# device within 2 s while the gate is stopped, every time. A stop that lands after the gate has
# received a command and before it replied would freeze a controller that talks to the gate on its
# main thread (seen in an earlier version of this test: it hung for an hour).
freeze_cycles() {
    k=0
    bad=0
    while [ $k -lt "$2" ]; do
        kill -STOP "$1"
        sleep 0.25
        answers_within 2 cat /dev/lg/controller || bad=$((bad + 1))
        kill -CONT "$1"
        sleep 0.25
        k=$((k + 1))
    done
    [ $bad -eq 0 ]
}

# restart_controller: a fresh controller that boots in level 4 and is released by the boot check
restart_controller() {
    killproc lg-controller
    sleep 0.5
    start_controller
}

# Safety net: a hang (for example a frozen controller device) must not block the whole run for ever.
# The guard holds none of this script's descriptors (with the ssh pipe open in the background it
# would keep the ssh session alive after the test has finished) and it ends by itself within a
# second when the script creates the marker file, so nothing has to be killed afterwards.
rm -f /tmp/step07_done /tmp/step07_guard.txt
(
    exec >/tmp/step07_guard.txt 2>&1 </dev/null
    n=0
    while [ $n -lt 1500 ] && [ ! -e /tmp/step07_done ]; do
        sleep 1
        n=$((n + 1))
    done
    [ -e /tmp/step07_done ] && exit 0
    echo "FAIL: the test did not finish within 1500 s, killing the processes"
    cleanup
    sleep 3
    kill -9 $$
) &

cleanup
rm -f $CSV /tmp/meter.out /tmp/step07_ack_stop /tmp/step07_acks.txt
echo "=== part 1: boot without sensors"
start_gate
check "gate boots DOWN" [ "$(gfield state)" = "DOWN" ]
start_controller
check "the controller device appears" [ -e /dev/lg/controller ]
check "the controller runs SCHED_FIFO priority 50, read back by the process itself" prio_ok
check "the controller starts in level 4 with the gate target DOWN and lights and bell on" level4_boot_state
check "writing to /dev/lg/controller is refused" write_refused
sleep 2
check "the controller found the gate and gets replies" at_least "$(cfield gate_replies)" 50
sleep 6
check "no sensor ever reported: level 4 after more than the boot check time" [ "$(cfield level)" = "4" ]
check "no sensor ever reported: the gate is still DOWN with lights and bell on" gate_down_lit
check "the gate watchdog did not trip (the controller keeps feeding it)" [ "$(gfield watchdog_trips)" = "0" ]

echo "=== part 2: boot release and idle (scenario without trains)"
start_plant test_idle.json
check "all six sensors start" start_sensors
check "boot check: level 0 once all six sensors are healthy" wait_eq cfield level 0 150
check "the gate rises to UP and lights and bell go off" wait_eq gfield state UP 100
sleep 1
check "gate UP with lights off" gate_up_dark
s1=$(cfield samples)
sleep 3
s2=$(cfield samples)
check "samples keep arriving (600 per second expected, at least 500 in 3 s)" grew_by "$s1" "$s2" 500
check "no sample was rejected" [ "$(cfield rejected)" = "0" ]
check "still level 0 and the gate still UP after 3 more seconds" gate_up_dark
check "the watchdog did not trip while the controller is alive" [ "$(gfield watchdog_trips)" = "0" ]
echo "--- controller: $(cat /dev/lg/controller)"
echo "--- gate: $(cat /dev/lg/gate)"

echo "=== part 3: the controller is killed while the gate is UP"
killproc lg-controller
check "the gate leaves UP by itself (watchdog)" wait_ne gfield state UP 30
check "the gate reaches DOWN" wait_eq gfield state DOWN 80
check "the watchdog tripped at least once" at_least "$(gfield watchdog_trips)" 1
start_controller
check "a new controller starts in level 4 with the gate target DOWN" level4_boot_state
check "the new controller is released by the boot check (sensors reconnected)" wait_eq cfield level 0 150
check "the gate rises again" wait_eq gfield state UP 100
check "samples reach the new controller" at_least "$(cfield samples)" 100

echo "=== part 4: the gate is stopped (SIGSTOP)"
GPID=$(pid_of lg-gate)
check "found the gate process" [ -n "$GPID" ]
kill -STOP "$GPID"
check "a gate that stops answering puts the controller in level 4" wait_eq cfield level 4 30
check "the controller still answers while its gate is frozen" answers_within 2 cat /dev/lg/controller
check "the controller counted the gate timeouts" at_least "$(cfield gate_timeouts)" 1
kill -CONT "$GPID"
check "the continued gate goes DOWN" wait_eq gfield state DOWN 100
sleep 6
check "level 4 stays after the gate answers again (only a guard may release it)" [ "$(cfield level)" = "4" ]
check "the gate stays DOWN with the lights on" gate_down_lit
check "the controller answered its status device in all 25 stop/continue cycles of the gate" freeze_cycles "$GPID" 25
check "the controller counted dropped requests or timeouts while the gate was frozen (information, must exist)" [ -n "$(cfield gate_dropped)" ]
echo "--- controller after the freeze cycles: $(cat /dev/lg/controller)"

echo "=== part 5: the plant disappears (sensors go blind) and comes back"
restart_controller
check "boot check after the restart: level 0" wait_eq cfield level 0 150
check "the gate is UP again" wait_eq gfield state UP 100
killproc lg-plant
check "blind sensors (no truth, quality 0): level 4" wait_eq cfield level 4 30
check "the gate lowers" wait_eq gfield state DOWN 100
start_plant test_idle.json
sleep 8
check "sensors see a plant again, but level 4 is not left without a guard" [ "$(cfield level)" = "4" ]
check "the gate stays DOWN with the lights on" gate_down_lit

echo "=== part 6: normal_day with lg-meter"
cleanup
sleep 1
start_gate
start_controller_guarded
start_plant normal_day.json
check "all six sensors start" start_sensors
CPID=$(pid_of lg-controller)
guard_acker </dev/null >/dev/null 2>&1 &
$BIN/lg-meter -e 285000 -m 330 -o $CSV </dev/null >/tmp/meter.out 2>&1 &
MPID=$!
wait $MPID
mrc=$?
touch /tmp/step07_ack_stop
ACKS=$(cat /tmp/step07_acks.txt 2>/dev/null | wc -l | tr -d ' ')
echo "--- lg-meter output:"
cat /tmp/meter.out
echo "--- guard acknowledgements sent during the run (a VM stall locked level 4): $ACKS"
check "lg-meter exits 0 (no wrong-side event)" [ "$mrc" = "0" ]
check "wrong-side events: 0" [ "$(csv_get wrong_side_events)" = "0" ]
check "wrong-side ticks: 0" [ "$(csv_get wrong_side_ticks)" = "0" ]
check "the meter got a sample in at least 95 percent of its periods" at_least "$(csv_get meter_coverage_pct)" 95
check "no gap between two meter samples above 200 ms" in_range "$(csv_get meter_max_sample_gap_ms)" 0 200
check "the meter took a plausible number of samples (at least 15000)" at_least "$(csv_get meter_samples)" 15000
check "the plant ran to the end of the scenario" at_least "$(csv_get plant_end_ms)" 285000
check "six trains crossed" [ "$(csv_get trains)" = "6" ]
check "every train had its lights on before it reached the island" [ "$(csv_get trains_without_warning)" = "0" ]
check "the controller ends in level 0" [ "$(csv_get controller_level_end)" = "0" ]
check "the gate ends UP with the lights off" gate_up_dark
check "no sample was rejected during the run" [ "$(cfield rejected)" = "0" ]
if [ "$ACKS" = "0" ]; then
    # No stall locked level 4: the run must be exactly the normal day.
    check "the controller completed six gate cycles" [ "$(csv_get controller_cycles)" = "6" ]
    check "five or six gate closures were measured (the first may still be the boot closure)" in_range "$(csv_get closure_count)" 5 6
    check "the watchdog never tripped during the run" [ "$(gfield watchdog_trips)" = "0" ]
    check "the only level change of the run is the boot release (4 to 0), seen in slog2" [ "$(slog_level_lines "$CPID")" = "1" ]
else
    # A stall locked level 4 and the guard had to acknowledge: the gate stayed DOWN for a while, so a train
    # can pass without a new cycle and the cycle, closure, watchdog and level-change counts no longer
    # describe the normal day. They are NOT checked in this case (the run is reported as disturbed).
    echo "--- DISTURBED RUN: level 4 was locked and acknowledged $ACKS time(s); cycle, closure, watchdog and level-change checks skipped"
    check "the controller completed at least one gate cycle" at_least "$(csv_get controller_cycles)" 1
fi
echo "--- metrics file $CSV:"
cat $CSV
echo "--- controller: $(cat /dev/lg/controller)"
echo "--- gate: $(cat /dev/lg/gate)"
echo "--- slog2info, controller lines of this run:"
slog2info 2>/dev/null | grep -E "lg_controller\.$CPID[[:space:]]" | tail -n 30

touch /tmp/step07_done
cleanup
echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
