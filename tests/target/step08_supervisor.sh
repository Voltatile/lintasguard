#!/bin/sh
# Step 8 target test: lg-supervisor (spawn order, heartbeats, restart, budget), PROC_STATUS to the
# controller, lgctl kill / guard-ack / status, plant epoch.
# Runs ON the QNX target (sh -s over ssh, see scripts/collect_evidence.sh).
# Needs everything in /tmp/lintasguard (make deploy IP=<vm-ip>): binaries, config/, scenarios/.
# Prints one PASS:/FAIL: line per check and exits non-zero when any check fails.
# Every check is ONE command: never write "check name [ a ] && [ b ]" (only [ a ] would be checked).
# Timing numbers come from a VirtualBox VM: functional evidence only.
#
# Parts: 1 boot through the supervisor, 2 kill a sensor (level 2 until T_stable), 3 hang and crash
# faults, 4 restart budget, 5 kill the controller, 6 kill the gate (level 4, guard-ack), 7 kill an
# island sensor, 8 kill the plant (epoch), 9 kill the supervisor, 10 supervisor and controller
# killed together (gate watchdog), 11 controller stopped (SIGSTOP), 12 normal_day with lg-meter and
# kills during the run.
# Level 4 is left only by a guard acknowledgement (or a controller restart, which boots in level 4
# and is released by the boot check); the guard id G01 comes from config/crossing.json.

BIN=/tmp/lintasguard
CSV=/tmp/step08_metrics.csv
SENSOR_NAMES="west/a west/b west/island east/a east/b east/island"
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
sfield() { grep "^proc=$1 " /dev/lg/supervisor | tr ' ' '\n' | sed -n "s/^$2=//p"; } # sfield <name> <field>
csv_get() { sed -n "s/^$1,//p" $CSV; }
plant_ms() { cat /dev/lg/plant | tr ' ' '\n' | sed -n 's/^t_ms=//p'; }
pid_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$' | head -n 1; }
count_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -cE '^ *[0-9]+$'; }
now_s() { date +%s; }

in_range() { # in_range <value> <low> <high>
    [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}
at_least() { # at_least <value> <low>
    [ -n "$1" ] && [ "$1" -ge "$2" ]
}
exit_code_of() { "$@" >/dev/null 2>&1; echo $?; }

killproc() {
    for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do
        kill -9 "$p" 2>/dev/null
    done
}

cleanup() { # the supervisor first, so that it does not start anything again
    killproc lg-supervisor
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
wait_eq() { # wait_eq <reader> <field> <value> <max tenths>: reader is cfield or gfield
    i=0
    while [ $i -lt "$4" ]; do
        [ "$($1 $2)" = "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_ne() { # wait_ne <reader> <field> <value> <max tenths>: the field has some other value
    i=0
    while [ $i -lt "$4" ]; do
        v=$($1 $2)
        [ -n "$v" ] && [ "$v" != "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_sup() { # wait_sup <proc name> <field> <value> <max tenths>
    i=0
    while [ $i -lt "$4" ]; do
        [ "$(sfield "$1" "$2")" = "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_sup_ge() { # wait_sup_ge <proc name> <field> <min> <max tenths>
    i=0
    while [ $i -lt "$4" ]; do
        v=$(sfield "$1" "$2")
        [ -n "$v" ] && [ "$v" -ge "$3" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
wait_plant_ms() { # wait_plant_ms <plant time ms> <max tenths>
    i=0
    while [ $i -lt "$2" ]; do
        v=$(plant_ms)
        [ -n "$v" ] && [ "$v" -ge "$1" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}
level_holds() { # level_holds <level> <seconds>: the level is that value every half second
    n=0
    while [ $n -lt $(($2 * 2)) ]; do
        [ "$(cfield level)" = "$1" ] || return 1
        sleep 0.5
        n=$((n + 1))
    done
    return 0
}

start_sup() { # start_sup <scenario file name>
    $BIN/lg-supervisor -b $BIN -c $BIN/config/crossing.json -s $BIN/scenarios/$1 -v \
        </dev/null >/tmp/sup.log 2>&1 &
    wait_for /dev/lg/supervisor 50
}
all_running() { [ "$(grep -c 'state=RUNNING' /dev/lg/supervisor)" = "9" ]; }
# A fresh system: everything started by the supervisor, boot check passed, gate UP.
fresh() { # fresh <scenario file name>
    cleanup
    sleep 1.5
    start_sup "$1"
    wait_sup gate state RUNNING 100 || return 1
    wait_eq cfield level 0 250 || return 1
    wait_eq gfield state UP 100 || return 1
    sleep 1
    return 0
}
gate_up_dark() { [ "$(gfield state)" = "UP" ] && [ "$(gfield lights)" = "0" ]; }
gate_down_lit() { [ "$(gfield state)" = "DOWN" ] && [ "$(gfield lights)" = "1" ]; }
no_dead() { ! grep -q 'state=DEAD' /dev/lg/supervisor; }
# The transitions of the level, in order, of controller pid $1 (for example "4>0 0>2 2>0 ")
level_trail() {
    slog2info 2>/dev/null | grep "lg_controller\.$1" | sed -n 's/.*level \([0-9]\) -> \([0-9]\).*/\1>\2/p' | tr '\n' ' '
}
started_names() { # names in the order the supervisor $1 started them (first start of each)
    slog2info 2>/dev/null | grep "lg_supervisor\.$1" | sed -n 's/.* started \([^ ]*\) as pid.*/\1/p'
}
boot_order_ok() { # the gate first, the controller last, the plant before the controller
    list=$(started_names "$1")
    [ "$(echo "$list" | sed -n 1p)" = "gate" ] && [ "$(echo "$list" | tail -n 1)" = "controller" ] &&
        [ "$(echo "$list" | grep -c '^sensor/')" = "6" ] && [ "$(echo "$list" | wc -l | tr -d ' ')" = "9" ]
}
all_sensor_totals_zero() {
    for s in $SENSOR_NAMES; do
        [ "$(sfield sensor/$s total)" = "0" ] || return 1
    done
    return 0
}
all_hb_ok() {
    for n in controller gate plant sensor/west/a sensor/west/b sensor/west/island sensor/east/a sensor/east/b sensor/east/island; do
        v=$(sfield $n hb_rx)
        [ -n "$v" ] && [ "$v" -ge 20 ] || return 1
    done
    return 0
}
# Level 0 within 45 s of the last acknowledgement. A sensor that looks silent for a moment (a VM stall of
# 200 ms or more) raises the required level and voids the pending acknowledgement, which is the rule: the
# guard then acknowledges again. At most 3 acknowledgements in all; how many were needed is printed.
reack_until_level0() {
    tries=0
    while [ $tries -lt 3 ]; do
        if wait_eq cfield level 0 450; then
            echo "--- level 0 reached, acknowledgements after the first: $tries"
            return 0
        fi
        $BIN/lgctl guard-ack --id G01 >/dev/null 2>&1
        tries=$((tries + 1))
    done
    wait_eq cfield level 0 450
}
ack_until_level0() { [ "$(exit_code_of $BIN/lgctl guard-ack --id G01)" = "0" ] && reack_until_level0; }
write_refused() { ! sh -c 'echo x > /dev/lg/supervisor' 2>/dev/null; }
sup_prio_logged() { slog2info 2>/dev/null | grep "lg_supervisor\.$1" | grep -q 'running SCHED_FIFO priority 55'; }

# Safety net: a hang must not block the whole run for ever (see step07_controller.sh).
rm -f /tmp/step08_done /tmp/step08_guard.txt
(
    exec >/tmp/step08_guard.txt 2>&1 </dev/null
    n=0
    while [ $n -lt 2400 ] && [ ! -e /tmp/step08_done ]; do
        sleep 1
        n=$((n + 1))
    done
    [ -e /tmp/step08_done ] && exit 0
    echo "FAIL: the test did not finish within 2400 s, killing the processes"
    cleanup
    sleep 3
    kill -9 $$
) &

cleanup
rm -f $CSV /tmp/meter.out
sleep 1

echo "=== part 1: boot through the supervisor (scenario without trains)"
start_sup test_idle.json
SPID=$(pid_of lg-supervisor)
check "the supervisor device appears and a supervisor runs" [ -e /dev/lg/supervisor ] && [ -n "$SPID" ]
check "the controller, started last, reaches RUNNING within 15 s" wait_sup controller state RUNNING 150
check "every row of the table is RUNNING" all_running
check "start order: the gate first, the controller last, six sensors, nine in all" boot_order_ok "$SPID"
check "the supervisor runs SCHED_FIFO 55 (logged by the process itself)" sup_prio_logged "$SPID"
check "boot check: level 0 once the sensors are healthy" wait_eq cfield level 0 200
check "the gate rises to UP with the lights off" wait_eq gfield state UP 100
sleep 2
check "gate UP with lights off" gate_up_dark
check "no process was restarted during boot (all sensor rows total 0)" all_sensor_totals_zero
check "gate, plant and controller rows have total 0" [ "$(sfield gate total)$(sfield plant total)$(sfield controller total)" = "000" ]
check "the controller got the supervisor's table (proc_status_rx at least 10)" at_least "$(cfield proc_status_rx)" 10
check "the controller treats the supervisor as armed and alive" [ "$(cfield sup_armed)$(cfield sup_silent)" = "10" ]
check "no process is reported down to the controller (proc_bad 0x0)" [ "$(cfield proc_bad)" = "0x0" ]
check "the supervisor's link to the controller is up" [ "$(grep -o 'linked=[01]' /dev/lg/supervisor)" = "linked=1" ]
check "heartbeats arrive from all nine processes (hb_rx at least 20 each)" all_hb_ok
check "no heartbeat was ignored in a normal run" [ "$(sfield gate hb_ignored)$(sfield controller hb_ignored)" = "00" ]
check "writing to /dev/lg/supervisor is refused" write_refused
check "the gate watchdog did not trip (the controller keeps feeding it)" [ "$(gfield watchdog_trips)" = "0" ]
check "lgctl status prints both devices (exit 0)" [ "$(exit_code_of $BIN/lgctl status)" = "0" ]
check "lgctl kill with a bad name is a usage error (exit 2)" [ "$(exit_code_of $BIN/lgctl kill toaster)" = "2" ]
check "lgctl kill sensor with a bad sensor is a usage error (exit 2)" [ "$(exit_code_of $BIN/lgctl kill sensor north/a)" = "2" ]
echo "--- supervisor table:"
cat /dev/lg/supervisor

echo "=== part 2: a sensor is killed (lgctl kill sensor west/a)"
CPID=$(pid_of lg-controller)
OLD=$(sfield sensor/west/a pid)
T0=$(now_s)
check "lgctl kill sensor west/a (exit 0)" [ "$(exit_code_of $BIN/lgctl kill sensor west/a)" = "0" ]
check "the supervisor notices the exit and counts one restart in the window" wait_sup sensor/west/a restarts 1/3 50
check "the new sensor is RUNNING again within 5 s" wait_sup sensor/west/a state RUNNING 50
check "the new sensor has another pid than the killed one" [ "$(sfield sensor/west/a pid)" != "$OLD" ]
check "the restart is recorded: why=exit, total 1" [ "$(sfield sensor/west/a why)$(sfield sensor/west/a total)" = "exit1" ]
echo "--- west/a recovery_ms (detection of the exit to the first heartbeat of the new process): $(sfield sensor/west/a recovery_ms)"
check "recovery within the start grace (recovery_ms between 1 and 1500)" in_range "$(sfield sensor/west/a recovery_ms)" 1 1500
check "the controller drops to level 2 (the redundant sensor still works)" wait_eq cfield level 2 20
check "level 2 holds while the new sensor is stable but T_stable has not passed (20 s)" level_holds 2 20
check "the gate stays UP with the lights off during the restart" gate_up_dark
check "the watchdog did not trip" [ "$(gfield watchdog_trips)" = "0" ]
check "level 0 again after T_stable (30 s)" wait_eq cfield level 0 300
T1=$(now_s)
echo "--- seconds from the kill to level 0: $((T1 - T0))"
check "level 0 came after T_stable and not much later (28 to 50 s after the kill)" in_range "$((T1 - T0))" 28 50
check "the level trail is boot release, down to 2, back to 0 (and nothing else)" [ "$(level_trail "$CPID")" = "4>0 0>2 2>0 " ]
check "the controller sees nothing down again (proc_bad 0x0)" [ "$(cfield proc_bad)" = "0x0" ]
check "the other sensors were not touched" [ "$(sfield sensor/west/b total)$(sfield sensor/east/a total)$(sfield gate total)" = "000" ]

echo "=== part 3: hang and crash faults are repaired by a restart"
check "lgctl sensor west/b fault hang (exit 0)" [ "$(exit_code_of $BIN/lgctl sensor west/b fault hang)" = "0" ]
check "the missing heartbeat is noticed and the sensor killed (why=hang, total 1)" wait_sup sensor/west/b why hang 30
check "the sensor is started again and RUNNING within 5 s" wait_sup sensor/west/b state RUNNING 50
check "the restart cleared the fault (stats say fault=NONE)" [ "$($BIN/lgctl sensor west/b stats | tr ' ' '\n' | sed -n 's/^fault=//p')" = "NONE" ]
check "the controller dropped to level 2 meanwhile" wait_eq cfield level 2 20
# The sensor aborts inside the devctl call, so lgctl may report an error: only the restart counts.
$BIN/lgctl sensor east/a fault crash >/dev/null 2>&1
check "a crashed sensor is started again (total 1, why=exit)" wait_sup sensor/east/a why exit 30
check "and is RUNNING within 5 s" wait_sup sensor/east/a state RUNNING 50
check "its samples reach the controller again (stats: samples_sent grows)" [ "$($BIN/lgctl sensor east/a stats | tr ' ' '\n' | sed -n 's/^samples_sent=//p')" -ge 1 ]
check "no process is DEAD" no_dead

echo "=== part 4: restart budget (3 restarts in 10 s), then DEAD"
check "fresh system" fresh test_idle.json
k=0
while [ $k -lt 4 ]; do
    wait_sup sensor/east/b state RUNNING 30
    $BIN/lgctl kill sensor east/b >/dev/null 2>&1
    sleep 0.4 # the supervisor must have seen this exit before the next kill reads the table
    k=$((k + 1))
done
check "after the fourth kill within 10 s the sensor is DEAD" wait_sup sensor/east/b state DEAD 30
check "it was restarted 3 times and has no process now" [ "$(sfield sensor/east/b total)$(sfield sensor/east/b pid)" = "30" ]
check "the controller is told: east/b (process id 7) is down (proc_bad 0x80)" wait_eq cfield proc_bad 0x80 40
check "one lost sensor of a redundant pair is level 2, not 4" wait_eq cfield level 2 30
sleep 12 # longer than the 10 s window: a DEAD sensor must stay DEAD
check "still DEAD with no pid after the window" [ "$(sfield sensor/east/b state)$(sfield sensor/east/b pid)" = "DEAD0" ]
check "lgctl kill on a DEAD sensor reports that nothing runs (exit 1)" [ "$(exit_code_of $BIN/lgctl kill sensor east/b)" = "1" ]
check "the other eight processes are RUNNING" [ "$(grep -c 'state=RUNNING' /dev/lg/supervisor)" = "8" ]
check "the gate stays UP and the watchdog did not trip" gate_up_dark

echo "=== part 5: the controller is killed (lgctl kill controller)"
check "fresh system" fresh test_idle.json
OLDC=$(sfield controller pid)
WT0=$(gfield watchdog_trips)
check "lgctl kill controller (exit 0)" [ "$(exit_code_of $BIN/lgctl kill controller)" = "0" ]
check "the gate leaves UP within 3 s" wait_ne gfield state UP 30
check "the gate reaches DOWN" wait_eq gfield state DOWN 80
echo "--- watchdog trips before: $WT0, after: $(gfield watchdog_trips) (the new controller may command the gate before the watchdog trips)"
check "the supervisor started a new controller (total 1)" wait_sup controller total 1 50
check "the controller exit is recorded as why=exit" [ "$(sfield controller why)" = "exit" ]
check "the new controller has another pid" [ "$(sfield controller pid)" != "$OLDC" ]
check "the new controller is RUNNING" wait_sup controller state RUNNING 50
check "the new controller boots in level 4" wait_eq cfield level 4 20
check "the new controller learns the supervisor's table within 3 s (sup_armed 1)" wait_eq cfield sup_armed 1 30
check "the supervisor started it with -r (restart: no boot release), level 4 holds for 15 s although everything is healthy" level_holds 4 15
check "the gate is DOWN with the lights on" gate_down_lit
check "nothing is reported down to the new controller (proc_bad 0x0)" wait_eq cfield proc_bad 0x0 50
check "guard G01 acknowledges and level 0 follows after T_stable (re-acknowledgement allowed)" ack_until_level0
check "the gate rises again" wait_eq gfield state UP 100
check "the gate is UP with lights off" gate_up_dark

echo "=== part 6: the gate is killed (level 4, guard-ack)"
check "fresh system" fresh test_idle.json
check "lgctl kill gate (exit 0)" [ "$(exit_code_of $BIN/lgctl kill gate)" = "0" ]
check "the supervisor restarts the gate (total 1) and it is RUNNING" wait_sup gate total 1 30
check "the new gate boots DOWN with the lights on" wait_eq gfield state DOWN 30
check "the controller goes to level 4" wait_eq cfield level 4 30
sleep 8
check "level 4 stays after the gate is back (no release without a guard)" [ "$(cfield level)" = "4" ]
check "the gate is DOWN with the lights on" gate_down_lit
check "an unknown guard id is refused (exit 4)" [ "$(exit_code_of $BIN/lgctl guard-ack --id G99)" = "4" ]
check "the refusal is counted and level 4 stays" [ "$(cfield guard_refused)$(cfield level)" = "14" ]
check "a malformed guard id is rejected (exit 3)" [ "$(exit_code_of $BIN/lgctl guard-ack --id ABCDEFGHIJ)" = "3" ]
check "guard G01 acknowledges (exit 0)" [ "$(exit_code_of $BIN/lgctl guard-ack --id G01)" = "0" ]
T0=$(now_s)
check "the controller counted the acknowledgement" [ "$(cfield guard_acks)" = "1" ]
check "level 4 is left after T_stable (30 s), not at once" level_holds 4 20
check "then it goes to level 0 (re-acknowledgement allowed)" reack_until_level0
T1=$(now_s)
echo "--- seconds from the acknowledgement to level 0: $((T1 - T0))"
check "the gate rises" wait_eq gfield state UP 100
check "gate UP with lights off" gate_up_dark

echo "=== part 7: an island sensor is killed (no island, no timetable: level 4)"
check "fresh system" fresh test_idle.json
check "lgctl kill sensor west/island (exit 0)" [ "$(exit_code_of $BIN/lgctl kill sensor west/island)" = "0" ]
check "the controller goes to level 4" wait_eq cfield level 4 30
check "the gate lowers" wait_eq gfield state DOWN 100
check "the sensor is RUNNING again" wait_sup sensor/west/island state RUNNING 50
sleep 6
check "level 4 stays after the sensor is back" [ "$(cfield level)" = "4" ]
check "guard G01 acknowledges and level 0 follows after T_stable (re-acknowledgement allowed)" ack_until_level0
check "the gate rises" wait_eq gfield state UP 100

echo "=== part 8: the plant is killed (it continues the scenario at the same epoch)"
check "fresh system" fresh test_idle.json
PT1=$(plant_ms)
T0=$(now_s)
check "plant time is running (more than 5 s)" at_least "$PT1" 5000
check "lgctl kill plant (exit 0)" [ "$(exit_code_of $BIN/lgctl kill plant)" = "0" ]
check "the supervisor restarts the plant (total 1) and it is RUNNING" wait_sup plant total 1 30
check "the plant device is back" wait_for /dev/lg/plant 30
sleep 3
PT3=$(plant_ms)
T3=$(now_s)
echo "--- plant time before the kill: $PT1 ms, 3 s after the restart: $PT3 ms, wall seconds between: $((T3 - T0))"
check "the scenario was continued, not replayed from 0 (plant time kept up with the wall clock)" at_least "$PT3" "$((PT1 + (T3 - T0 - 2) * 1000))"
check "blind sensors (no truth while the plant was away): level 4" wait_eq cfield level 4 50
check "the gate is DOWN with the lights on" wait_eq gfield state DOWN 100
sleep 5
check "guard G01 acknowledges and level 0 follows after T_stable (re-acknowledgement allowed)" ack_until_level0

echo "=== part 9: the supervisor is killed (the children keep running)"
check "fresh system" fresh test_idle.json
SPID=$(pid_of lg-supervisor)
kill -9 "$SPID"
sleep 1
check "the controller still runs" [ -n "$(pid_of lg-controller)" ]
check "all six sensors still run" [ "$(count_of lg-sensor)" = "6" ]
check "the gate still runs" [ -n "$(pid_of lg-gate)" ]
check "the plant still runs" [ -n "$(pid_of lg-plant)" ]
check "the controller notices the silent supervisor: level 4 within 8 s" wait_eq cfield level 4 80
check "sup_silent is 1" [ "$(cfield sup_silent)" = "1" ]
check "the gate is DOWN with the lights on" wait_eq gfield state DOWN 100
V=$(pid_of lg-sensor)
kill -9 "$V"
sleep 2
check "nobody restarts a killed sensor now (five sensors left)" [ "$(count_of lg-sensor)" = "5" ]

echo "=== part 10: supervisor and controller are killed together (the gate watchdog is the last line)"
check "fresh system" fresh test_idle.json
WT0=$(gfield watchdog_trips)
killproc lg-supervisor
killproc lg-controller
check "the gate leaves UP within 3 s" wait_ne gfield state UP 30
check "the gate reaches DOWN" wait_eq gfield state DOWN 80
check "the gate watchdog tripped (trips grew)" [ "$(gfield watchdog_trips)" -gt "$WT0" ]
sleep 3
check "nobody started a controller again" [ -z "$(pid_of lg-controller)" ]
check "the gate stays DOWN" gate_down_lit

echo "=== part 11: the controller is stopped (SIGSTOP), not killed"
check "fresh system" fresh test_idle.json
STOPPED=$(pid_of lg-controller)
kill -STOP "$STOPPED"
check "no heartbeat from the stopped controller: the supervisor kills it (why=hang)" wait_sup controller why hang 30
check "a new controller is RUNNING within 5 s" wait_sup controller state RUNNING 50
check "the new controller has another pid" [ "$(pid_of lg-controller)" != "$STOPPED" ]
check "the gate left UP (watchdog or boot lock of the new controller)" wait_eq gfield state DOWN 80
sleep 3
echo "--- table after the stop (information: did sensors that were waiting on the controller get restarted too):"
cat /dev/lg/supervisor
check "no process was given up (none DEAD)" no_dead
check "the new controller is in level 4 (restart: guard needed)" wait_eq cfield level 4 30
sleep 6
check "guard G01 acknowledges and level 0 follows after T_stable (re-acknowledgement allowed)" ack_until_level0

echo "=== part 12: normal_day with lg-meter, three kills during the run"
cleanup
sleep 1.5
start_sup normal_day.json
check "the controller, started last, reaches RUNNING within 15 s" wait_sup controller state RUNNING 150
check "all nine rows are RUNNING" all_running
$BIN/lg-meter -e 285000 -m 330 -o $CSV </dev/null >/tmp/meter.out 2>&1 &
MPID=$!
check "the plant reaches 108 s" wait_plant_ms 108000 1500
# west/a dies 2 s before train 3 (west, strike at 110 s): west/b must carry the detection
check "kill west/a before train 3" [ "$(exit_code_of $BIN/lgctl kill sensor west/a)" = "0" ]
check "the plant reaches 158 s" wait_plant_ms 158000 600
check "kill east/b before train 4 (east, strike at 160 s)" [ "$(exit_code_of $BIN/lgctl kill sensor east/b)" = "0" ]
check "the plant reaches 205 s" wait_plant_ms 205000 600
check "kill the controller 5 s before train 5 (west, strike at 210 s)" [ "$(exit_code_of $BIN/lgctl kill controller)" = "0" ]
check "the new controller is RUNNING and in level 4 (restart: no boot release)" wait_eq cfield level 4 50
sleep 8
check "guard G01 acknowledges and level 0 follows after T_stable (re-acknowledgement allowed)" ack_until_level0
wait $MPID
mrc=$?
echo "--- lg-meter output:"
cat /tmp/meter.out
check "lg-meter exits 0 (no wrong-side event)" [ "$mrc" = "0" ]
check "wrong-side events: 0" [ "$(csv_get wrong_side_events)" = "0" ]
check "wrong-side ticks: 0" [ "$(csv_get wrong_side_ticks)" = "0" ]
check "the meter got a sample in at least 95 percent of its periods" at_least "$(csv_get meter_coverage_pct)" 95
check "no gap between two meter samples above 200 ms" in_range "$(csv_get meter_max_sample_gap_ms)" 0 200
check "the plant ran to the end of the scenario" at_least "$(csv_get plant_end_ms)" 285000
check "six trains crossed" [ "$(csv_get trains)" = "6" ]
check "every train had its lights on before it reached the island" [ "$(csv_get trains_without_warning)" = "0" ]
# The meter reads the controller device once, through a descriptor that died with the first controller:
# its controller_* fields (level_end 99, cycles, late ticks) are void in this run. Read it live instead.
check "the controller ends in level 0 (read live: the meter lost its descriptor when the controller was restarted)" [ "$(cfield level)" = "0" ]
check "west/a, east/b and the controller were restarted once each" [ "$(sfield sensor/west/a total)$(sfield sensor/east/b total)$(sfield controller total)" = "111" ]
check "nothing else was restarted and nothing is DEAD" [ "$(sfield gate total)$(sfield plant total)$(sfield sensor/west/b total)$(sfield sensor/west/island total)$(sfield sensor/east/a total)$(sfield sensor/east/island total)" = "000000" ]
check "no process is DEAD" no_dead
check "the gate ends UP with the lights off" gate_up_dark
echo "--- metrics file $CSV:"
cat $CSV
echo "--- controller: $(cat /dev/lg/controller)"
echo "--- gate: $(cat /dev/lg/gate)"
echo "--- supervisor:"
cat /dev/lg/supervisor
echo "--- slog2info, supervisor lines of this run:"
slog2info 2>/dev/null | grep "lg_supervisor\.$(pid_of lg-supervisor)" | tail -n 40

touch /tmp/step08_done
cleanup
echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
