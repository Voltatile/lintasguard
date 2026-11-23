#!/bin/sh
# Step 10 target test: lg-bridge under the supervisor (priority, port, status device, restart, restart
# budget, behaviour while the controller is stopped). The network side (NDJSON stream, rejected
# commands, slow clients) is checked from the host by tests/host/bridge_check.py.
# Runs ON the QNX target (sh -s over ssh, see scripts/collect_evidence.sh).
# Needs everything in /tmp/lintasguard (make deploy IP=<vm-ip>): binaries, config/, scenarios/.
# Prints one PASS:/FAIL: line per check and exits non-zero when any check fails.
# Every check is ONE command: never write "check name [ a ] && [ b ]" (only [ a ] would be checked).
# Timing numbers come from a VirtualBox VM: functional evidence only.
#
# Parts: 1 boot (priority 10 on every thread, port 5555, /dev/lg/bridge read only), 2 kill the bridge
# once (restart, level 1 until T_stable, gate untouched), 3 kill the bridge until its restart budget is
# spent (DEAD bridge = level 1 only, gate untouched), 4 controller stopped (SIGSTOP): the bridge keeps
# beating and is not restarted.

BIN=/tmp/lintasguard
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

# ---- helpers (same as step08_supervisor.sh) ---------------------------------------------
cfield() { cat /dev/lg/controller | tr ' ' '\n' | sed -n "s/^$1=//p"; }
gfield() { cat /dev/lg/gate | tr ' ' '\n' | sed -n "s/^$1=//p"; }
bfield() { cat /dev/lg/bridge | tr ' ' '\n' | sed -n "s/^$1=//p"; }
sfield() { grep "^proc=$1 " /dev/lg/supervisor | tr ' ' '\n' | sed -n "s/^$2=//p"; } # sfield <name> <field>
pid_of() { pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$' | head -n 1; }
exit_code_of() { "$@" >/dev/null 2>&1; echo $?; }

at_least() { # at_least <value> <low>
    [ -n "$1" ] && [ "$1" -ge "$2" ]
}
greater() { # greater <value> <than>
    [ -n "$1" ] && [ -n "$2" ] && [ "$1" -gt "$2" ]
}

killproc() {
    for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do
        kill -9 "$p" 2>/dev/null
    done
}

cleanup() { # the supervisor first, so that it does not start anything again
    killproc lg-supervisor
    killproc lg-bridge
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
wait_eq() { # wait_eq <reader> <field> <value> <max tenths>: reader is cfield, gfield or bfield
    i=0
    while [ $i -lt "$4" ]; do
        [ "$($1 $2)" = "$3" ] && return 0
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

start_sup() { # start_sup <scenario file name>
    $BIN/lg-supervisor -b $BIN -c $BIN/config/crossing.json -s $BIN/scenarios/$1 -v \
        </dev/null >/tmp/sup.log 2>&1 &
    wait_for /dev/lg/supervisor 50
}
# A fresh system: everything started by the supervisor, boot check passed, gate UP.
fresh() { # fresh <scenario file name>
    cleanup
    sleep 1.5
    start_sup "$1"
    wait_sup gate state RUNNING 100 || return 1
    wait_eq cfield level 0 250 || return 1
    wait_eq gfield state UP 100 || return 1
    wait_sup bridge state RUNNING 50 || return 1
    sleep 1
    return 0
}
gate_up_dark() { [ "$(gfield state)" = "UP" ] && [ "$(gfield lights)" = "0" ]; }
# The gate is UP and dark, and the level is $1, every half second for $2 seconds.
level_and_gate_hold() { # level_and_gate_hold <level> <seconds>
    n=0
    while [ $n -lt $(($2 * 2)) ]; do
        [ "$(cfield level)" = "$1" ] || return 1
        gate_up_dark || return 1
        sleep 0.5
        n=$((n + 1))
    done
    return 0
}
# Every thread of lg-bridge runs at priority 10 (FIFO for the bridge's own threads, RR for the shared
# heartbeat connector of lg_heartbeat.h); at least the main, status, reader and connector threads.
bridge_threads_prio10() {
    p=$(pid_of lg-bridge)
    [ -n "$p" ] || return 1
    all=$(pidin -p "$p" -F '%p' 2>/dev/null | grep -E '^ *[0-9]+[a-z]$' | wc -l | tr -d ' ')
    ten=$(pidin -p "$p" -F '%p' 2>/dev/null | grep -cE '^ *10[fr]$')
    [ "$all" -ge 4 ] && [ "$all" = "$ten" ]
}
port_listening() { netstat -an 2>/dev/null | grep -E '[.:]5555[[:space:]]' | grep -q LISTEN; }
bridge_write_refused() { ! sh -c 'echo x > /dev/lg/bridge' 2>/dev/null; }
# The level event 0 -> 1 with reason 5 (process restarting) is in the controller event ring.
level1_event() { grep -q 'kind=LEVEL severity=WARNING from=0 to=1 reason=5' /dev/lg/events; }
# Kill the running bridge and wait for its replacement (another pid) to be RUNNING.
kill_and_wait_new() {
    old=$(pid_of lg-bridge)
    [ "$(exit_code_of $BIN/lgctl kill bridge)" = "0" ] || return 1
    i=0
    while [ $i -lt 50 ]; do
        n=$(pid_of lg-bridge)
        [ -n "$n" ] && [ "$n" != "$old" ] && [ "$(sfield bridge state)" = "RUNNING" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# ---------------------------------------------------------------------------------------------
echo "=== part 1: boot with the bridge"
check "fresh system (test_idle), bridge RUNNING" fresh test_idle.json
check "every thread of lg-bridge runs at priority 10" bridge_threads_prio10
check "TCP port 5555 is listening" port_listening
check "/dev/lg/bridge reports port=5555" [ "$(bfield port)" = "5555" ]
check "/dev/lg/bridge refuses writes" bridge_write_refused
S1=$(bfield snapshots)
H1=$(sfield bridge hb_rx)
sleep 2
check "the device reader keeps taking snapshots (2 s: at least 10 more)" at_least "$(bfield snapshots)" $((S1 + 10))
check "the supervisor keeps receiving bridge heartbeats" greater "$(sfield bridge hb_rx)" "$H1"
check "the gate is UP and dark" gate_up_dark
echo "--- /dev/lg/bridge:"
cat /dev/lg/bridge

echo "=== part 2: kill the bridge once"
check "lgctl kill bridge: a new bridge is RUNNING within 5 s" kill_and_wait_new
check "the supervisor counts one restart" [ "$(sfield bridge total)" = "1" ]
check "the controller goes to level 1 (bridge restarting = early warning) within 3 s" wait_eq cfield level 1 30
check "the event ring holds the level change 0 -> 1, reason 5 (process restarting)" level1_event
check "level 1 holds and the gate stays UP and dark for 10 s" level_and_gate_hold 1 10
check "level 0 again after T_stable (within 45 s)" wait_eq cfield level 0 450
check "the gate is still UP and dark" gate_up_dark
check "the new bridge listens on 5555" port_listening
echo "--- /dev/lg/events:"
cat /dev/lg/events

echo "=== part 3: kill the bridge until the restart budget is spent"
check "fresh system (test_idle), bridge RUNNING" fresh test_idle.json
check "kill 1: replaced" kill_and_wait_new
check "kill 2: replaced" kill_and_wait_new
check "kill 3: replaced" kill_and_wait_new
check "kill 4 accepted by lgctl" [ "$(exit_code_of $BIN/lgctl kill bridge)" = "0" ]
check "budget spent (3 per 10 s): the bridge is DEAD within 5 s" wait_sup bridge state DEAD 50
check "no lg-bridge process is left" [ -z "$(pid_of lg-bridge)" ]
check "a dead bridge raises the level to 1 only" wait_eq cfield level 1 30
check "level 1 holds and the gate stays UP and dark for 10 s" level_and_gate_hold 1 10
echo "--- /dev/lg/supervisor:"
cat /dev/lg/supervisor

echo "=== part 4: the controller is stopped (SIGSTOP), the bridge keeps going"
check "fresh system (test_idle), bridge RUNNING" fresh test_idle.json
BPID=$(pid_of lg-bridge)
H1=$(sfield bridge hb_rx)
CPID=$(pid_of lg-controller)
kill -STOP "$CPID"
check "the supervisor kills the stopped controller (why=hang)" wait_sup controller why hang 30
check "a new controller is RUNNING within 5 s" wait_sup controller state RUNNING 50
sleep 2
check "the bridge kept its pid (not restarted)" [ "$(pid_of lg-bridge)" = "$BPID" ]
check "the bridge restart count is 0" [ "$(sfield bridge total)" = "0" ]
check "the supervisor kept receiving bridge heartbeats" greater "$(sfield bridge hb_rx)" "$H1"
S1=$(bfield snapshots)
sleep 2
check "snapshots continue with the new controller (2 s: at least 10 more)" at_least "$(bfield snapshots)" $((S1 + 10))
echo "--- /dev/lg/bridge:"
cat /dev/lg/bridge

cleanup
echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
