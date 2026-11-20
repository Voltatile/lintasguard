#!/bin/sh
# Step 5 target test: lg-gate resource manager, command channel, watchdog, devctl.
# Runs ON the QNX target (sh -s over ssh, see scripts/collect_evidence.sh).
# Needs /tmp/lintasguard/lg-gate and /tmp/lintasguard/lg-gate-test (make deploy IP=<vm-ip>).
# Prints one PASS:/FAIL: line per check and exits non-zero when any check fails.
# Every check is ONE command: never write "check name [ a ] && [ b ]" (only [ a ] would be checked).
# Timing numbers come from a VirtualBox VM: functional evidence only.

BIN=/tmp/lintasguard
GATE=/dev/lg/gate
NAME=/dev/name/local/lg/gate
GPID=
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

state() { sed -n 's/.*state=\([A-Z]*\).*/\1/p' $GATE; }
field() { tr ' ' '\n' < $GATE | sed -n "s/^$1=//p"; }

wait_state() { # wait_state <STATE> <max tenths of a second>
    want=$1
    max=$2
    i=0
    while [ $i -lt "$max" ]; do
        [ "$(state)" = "$want" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# Kill by PID from pidin (slay only matches the name exactly as the process was started).
killproc() {
    for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do
        kill -9 "$p" 2>/dev/null
    done
}

cleanup() {
    killproc lg-gate-test
    killproc lg-gate
}

start_gate() {
    killproc lg-gate
    sleep 1
    $BIN/lg-gate -v </dev/null >/tmp/gate.log 2>&1 &
    GPID=$!
    i=0
    while [ $i -lt 30 ]; do
        [ -e $NAME ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

in_range() { # in_range <value> <low> <high>
    [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}

# ---- conditions with several parts, as functions so that check sees ONE command ----------
boot_ok() {
    [ "$(field position)" = "90" ] && [ "$(field lights)" = "1" ] &&
        [ "$(field bell)" = "1" ] && [ "$(field target)" = "DOWN" ]
}
lights_bell_on() { [ "$(field lights)" = "1" ] && [ "$(field bell)" = "1" ]; }
held_up_3s() {
    sleep 3
    [ "$(field watchdog_trips)" = "0" ] && [ "$(state)" = "UP" ]
}
no_repeat_trips() { sleep 2; [ "$(field watchdog_trips)" = "1" ]; }
log_has_watchdog() { slog2info 2>/dev/null | grep "lg_gate\.$GPID" | grep -qi watchdog; }
pidin_sees_gate() { pidin -p lg-gate 2>/dev/null | grep -q lg-gate; }
gate_gone() { ! pidin -p lg-gate 2>/dev/null | grep -q lg-gate; }
name_gone() { [ ! -e $NAME ]; }
stats_prio_ok() { echo "$stats" | grep -q 'sched_policy=1 sched_priority=48'; }
write_refused() { [ "$wrc" -ne 0 ] && ! echo "$wr" | grep -q '1 bytes'; }
out_has_rejected() { echo "$1" | grep -q rejected; }

echo "=== step05_gate: $(date)"
echo "=== $(uname -a)"
cleanup

# ---- start and boot state ---------------------------------------------------------------
check "lg-gate starts and registers name $NAME" start_gate
check "pidin lists lg-gate while it runs (so the crash check below can fail)" pidin_sees_gate
check "boot state is DOWN (gate starts down)" [ "$(state)" = "DOWN" ]
check "boot: position 90, lights on, bell on, target DOWN" boot_ok
echo "--- cat $GATE:"
cat $GATE

# ---- raise with a live controller (heartbeats) ------------------------------------------
$BIN/lg-gate-test cmd raise 30000 </dev/null >/tmp/holder.log 2>&1 &
HPID=$!
check "raise command accepted, gate reaches UP" wait_state UP 80
echo "--- cat $GATE (UP):"
cat $GATE
tm=$(field travel_ms)
check "raise travel time within 10 percent of nominal 4000 ms (got $tm ms)" in_range "$tm" 3600 4400
check "UP is held while heartbeats keep coming (3 s, no watchdog trip)" held_up_3s

# ---- the serving thread keeps its real-time priority -----------------------------------
stats=$($BIN/lg-gate-test stats)
echo "--- $stats"
check "lg-gate runs SCHED_FIFO priority 48, read back by the process itself" stats_prio_ok

# ---- no write path ----------------------------------------------------------------------
echo "--- write attempt:"
wr=$(dd if=/dev/zero of=$GATE bs=1 count=1 2>&1)
wrc=$?
echo "$wr"
check "writing to $GATE is refused (dd exits non-zero, 0 bytes copied)" write_refused
sleep 0.3
check "gate still UP after the refused write" [ "$(state)" = "UP" ]

# ---- kill the client: the watchdog lowers the gate ---------------------------------------
echo "--- kill -9 the client (pid $HPID)"
kill -9 $HPID
i=0
s=UP
while [ $i -lt 15 ]; do
    s=$(state)
    [ "$s" != "UP" ] && break
    sleep 0.02
    i=$((i + 1))
done
echo "--- the gate had left UP after $i polls of the state file: state=$s"
check "gate leaves UP after the client dies (sanity poll, not a timed bound)" [ "$s" != "UP" ]
check "watchdog trip counted once" [ "$(field watchdog_trips)" = "1" ]
check "gate reaches DOWN by itself" wait_state DOWN 80
echo "--- cat $GATE (after watchdog):"
cat $GATE
# The gate measures the silence itself, on its own CLOCK_MONOTONIC, when it trips.
silence=$(slog2info 2>/dev/null | grep "lg_gate\.$GPID" | grep "no controller command" | head -n 1 |
    sed -n 's/.*heartbeat for \([0-9]*\) ms.*/\1/p')
echo "--- silence measured by lg-gate at the trip: $silence ms (limit 100 ms, tick 10 ms)"
check "watchdog tripped after 101 to 160 ms of silence (measured by the gate)" in_range "$silence" 101 160
check "no repeated trips once DOWN (2 s)" no_repeat_trips
check "lights and bell are on after the watchdog" lights_bell_on

# ---- raise without keepalive is undone by the watchdog -----------------------------------
echo "--- raise once with no heartbeats"
$BIN/lg-gate-test cmd raise </dev/null
sleep 1
check "raise without keepalive: watchdog lowers it again (trip 2)" [ "$(field watchdog_trips)" = "2" ]
check "gate returns to DOWN" wait_state DOWN 80

# ---- explicit lower ------------------------------------------------------------------------
$BIN/lg-gate-test cmd raise 30000 </dev/null >/tmp/holder.log 2>&1 &
HPID=$!
check "gate UP again before the explicit lower" wait_state UP 80
$BIN/lg-gate-test cmd lower </dev/null
check "explicit lower command reaches DOWN" wait_state DOWN 80
kill -9 $HPID 2>/dev/null
sleep 0.5

# ---- fault injection through devctl --------------------------------------------------------
$BIN/lg-gate-test fault slow 150 </dev/null
check "devctl SLOW 150 percent accepted" [ "$(field fault)" = "SLOW" ]
$BIN/lg-gate-test cmd raise 30000 </dev/null >/tmp/holder.log 2>&1 &
HPID=$!
check "slow gate still reaches UP" wait_state UP 150
tm=$(field travel_ms)
check "slow raise takes about 1.5 x nominal (got $tm ms, expect 5400 to 6600)" in_range "$tm" 5400 6600
$BIN/lg-gate-test cmd lower </dev/null
check "gate back DOWN after the slow cycle" wait_state DOWN 150
kill -9 $HPID 2>/dev/null
$BIN/lg-gate-test fault none </dev/null
check "fault cleared" [ "$(field fault)" = "NONE" ]
$BIN/lg-gate-test fault slow 50 </dev/null >/dev/null 2>&1
rc=$?
check "devctl rejects a factor below 100 percent (exit code 3 = EINVAL)" [ $rc -eq 3 ]
$BIN/lg-gate-test fault-ro slow 150 </dev/null >/dev/null 2>&1
rc=$?
check "devctl fault through a read-only descriptor is refused (exit code 4 = EPERM)" [ $rc -eq 4 ]
check "the refused read-only fault changed nothing" [ "$(field fault)" = "NONE" ]

# ---- malformed messages are rejected ---------------------------------------------------------
before=$(field rejected)
out1=$($BIN/lg-gate-test raw 0x1002 2>&1)
out2=$($BIN/lg-gate-test raw 0x2000 2>&1)
echo "--- $out1"
echo "--- $out2"
check "GATE_CMD with an invalid command value is rejected" out_has_rejected "$out1"
check "a message of an unknown type is rejected" out_has_rejected "$out2"
check "rejected counter went up by one (invalid command only)" \
    [ "$(field rejected)" = "$((before + 1))" ]

# ---- logging --------------------------------------------------------------------------------
echo "--- slog2info, watchdog lines of this gate (the buffer set lg-gate is listed as lg_gate.<pid>):"
slog2info 2>/dev/null | grep "lg_gate\.$GPID" | grep -i watchdog
echo "--- slog2info, last lines of this gate:"
slog2info 2>/dev/null | grep "lg_gate\.$GPID" | tail -n 8
check "slog2 holds a watchdog entry of this gate process (pid $GPID)" log_has_watchdog

# ---- crash injection and restart --------------------------------------------------------------
echo "--- crash injection"
$BIN/lg-gate-test fault crash </dev/null >/dev/null 2>&1
sleep 1
check "after CRASH the lg-gate process is gone" gate_gone
check "after CRASH the name $NAME is gone" name_gone
check "lg-gate restarts" start_gate
check "restarted gate comes up DOWN" [ "$(state)" = "DOWN" ]

cleanup
echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
