#!/bin/sh
# Step 6 target test: two properties of QNX channels and pulses that the sensor design relies on.
# Runs ON the target (sh -s over ssh). Needs /tmp/lintasguard/lg-chan-demo (make deploy).
# Observations on this VM only; not a comparison with any other system.
# Every check is ONE command.

BIN=/tmp/lintasguard
fails=0
checks=0

check() {
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

prio_of() { # prio_of <label text>: the priority printed for a case of the pulse test
    grep "pulse: $1" /tmp/chan_pulse.txt | sed -n 's/.*priority after the pulse: \([0-9]*\).*/\1/p'
}
inherit_runs_at_creation_priority() {
    start=$(sed -n 's/^pulse: the process started at priority \([0-9]*\).*/\1/p' /tmp/chan_pulse.txt)
    got=$(prio_of "SIGEV_PULSE_PRIO_INHERIT, default channel")
    [ -n "$start" ] && [ -n "$got" ] && [ "$got" = "$start" ] && [ "$got" != "45" ]
}
explicit_runs_at_45() { [ "$(prio_of "explicit priority, default channel")" = "45" ]; }
explicit_fixed_runs_at_45() { [ "$(prio_of "explicit priority, fixed-priority channel")" = "45" ]; }

run_private() { # run_private <0|1> <output file>
    rm -f "$2" "$2.client"
    $BIN/lg-chan-demo private-server "$1" </dev/null >"$2" 2>&1 &
    SP=$!
    pid=""
    i=0
    while [ -z "$pid" ] && [ $i -lt 30 ]; do
        pid=$(sed -n 's/^private: server pid=\([0-9]*\) chid=\([0-9]*\).*/\1/p' "$2")
        [ -z "$pid" ] && sleep 0.1
        i=$((i + 1))
    done
    [ -n "$pid" ] || { kill -9 $SP 2>/dev/null; return 1; }
    chid=$(sed -n 's/^private: server pid=\([0-9]*\) chid=\([0-9]*\).*/\2/p' "$2")
    $BIN/lg-chan-demo private-client "$pid" "$chid" </dev/null >"$2.client" 2>&1
    sleep 3
    kill -9 $SP 2>/dev/null
    cat "$2.client" >> "$2"
    return 0
}
has() { grep -q "$2" "$1"; }
lacks() { ! grep -q "$2" "$1"; }

echo "=== step06_channels: $(date)"
echo "=== $(uname -a)"

echo "--- pulse priority:"
$BIN/lg-chan-demo pulse-prio </dev/null | tee /tmp/chan_pulse.txt
check "SIGEV_PULSE_PRIO_INHERIT on a default channel runs the receiver at the priority the process started with, not at 45" \
    inherit_runs_at_creation_priority
check "an explicit pulse priority runs the receiver at 45 (default channel)" explicit_runs_at_45
check "an explicit pulse priority runs the receiver at 45 (fixed-priority channel)" explicit_fixed_runs_at_45

echo "--- an ordinary channel (not private):"
check "the private-channel experiment runs (ordinary channel)" run_private 0 /tmp/chan_open.txt
cat /tmp/chan_open.txt
check "ordinary channel: another process can attach" has /tmp/chan_open.txt "another process attaches: ok"
check "ordinary channel: the pulse of the other process reaches the owner as a tick" has /tmp/chan_open.txt "received a pulse with code 2"

echo "--- a private channel (_NTO_CHF_PRIVATE):"
check "the private-channel experiment runs (private channel)" run_private 1 /tmp/chan_priv.txt
cat /tmp/chan_priv.txt
check "private channel: the owner can attach to its own channel" has /tmp/chan_priv.txt "own attach: ok"
check "private channel: another process cannot attach" has /tmp/chan_priv.txt "another process attaches: Operation not permitted"
check "private channel: no pulse of another process arrives" lacks /tmp/chan_priv.txt "received a pulse with code 2"

echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
