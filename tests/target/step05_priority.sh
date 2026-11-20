#!/bin/sh
# Step 5 target test: server priority while serving a lower-priority client (QNX 8.0).
# Runs ON the target (sh -s over ssh). Needs /tmp/lintasguard/lg-prio-demo (make deploy).
# Observation on this VM only; not a comparison with any other system.

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

# Starts the server, waits until it printed its pid, runs the client, then stops the server.
run_case() { # run_case <fixed 0|1> <output file>
    rm -f "$2"
    $BIN/lg-prio-demo server "$1" </dev/null >"$2" 2>&1 &
    SPID=$!
    pid=""
    i=0
    while [ -z "$pid" ] && [ $i -lt 30 ]; do
        pid=$(sed -n 's/^server pid=\([0-9]*\) chid=\([0-9]*\).*/\1/p' "$2")
        [ -z "$pid" ] && sleep 0.1
        i=$((i + 1))
    done
    if [ -z "$pid" ]; then
        echo "the server did not start"
        kill -9 $SPID 2>/dev/null
        return 1
    fi
    chid=$(sed -n 's/^server pid=\([0-9]*\) chid=\([0-9]*\).*/\2/p' "$2")
    $BIN/lg-prio-demo client "$pid" "$chid" </dev/null
    sleep 1
    kill -9 $SPID 2>/dev/null
    return 0
}

echo "=== step05_priority: $(date)"
echo "=== $(uname -a)"

echo "--- default channel (flags 0):"
run_case 0 /tmp/prio_default.txt
cat /tmp/prio_default.txt
check "default channel: the server thread drops to the client priority (10) while serving" \
    grep -q 'msg 0: prio while serving=10' /tmp/prio_default.txt

echo "--- channel with _NTO_CHF_FIXED_PRIORITY:"
run_case 1 /tmp/prio_fixed.txt
cat /tmp/prio_fixed.txt
check "fixed-priority channel: the server thread stays at 48 while serving" \
    grep -q 'msg 0: prio while serving=48' /tmp/prio_fixed.txt
check "fixed-priority channel: still 48 before the next receive" \
    grep -q 'prio before the next receive=48' /tmp/prio_fixed.txt

echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
