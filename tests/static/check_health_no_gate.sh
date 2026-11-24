#!/bin/sh
# Static check (CLAUDE.md rule 4): only lg-controller may command the gate. lg-health reads the devices and
# sends verdicts to the controller; it must have no way to command the gate or change a device:
#  - no gate name, no gate command type or helper, no gate fault or battery injection;
#  - devctl only with DCMD_LG_SENSOR_STATS and DCMD_LG_GATE_STATS (and its own DCMD_LG_HEALTH_RESET);
#  - the only message type it builds is LG_MSG_HEALTH_VERDICT;
#  - no descriptor opened for writing, no other programs, no signals.
# Run by `make test`. Prints PASS/FAIL, exits non-zero on FAIL.
#
#   check_health_no_gate.sh [health dir] [makefile]
#
# Checked: every .c and .h in the lg-health directory and every core/*.c the Makefile links into it.
dir=${1:-target/lg-health}
mk=${2:-Makefile}

if [ ! -d "$dir" ] || [ -z "$(ls "$dir"/*.c 2>/dev/null)" ]; then
    echo "FAIL: check_health_no_gate (no lg-health sources in $dir: moved or renamed?)"
    exit 1
fi

files=$(ls "$dir"/*.c "$dir"/*.h 2>/dev/null)
core=$(sed -n 's#^\$(BUILD)/target/lg-health:##p' "$mk" 2>/dev/null | tr ' ' '\n' | grep -E '^core/.*\.c$')
files="$files $core"
# The code-carrying target headers it includes (review L9, as in the bridge check). lg_heartbeat.h is the
# shared helper that opens the supervisor and sends heartbeat pulses: checked separately below.
for h in $(sed -n 's/^#include "\(.*\)"/\1/p' "$dir"/*.c 2>/dev/null | sort -u); do
    # lg_devctl.h only defines the devctl numbers and structures (no code): the uses are checked in the sources.
    if [ -f "target/include/$h" ] && [ "$h" != "lg_heartbeat.h" ] && [ "$h" != "lg_devctl.h" ]; then
        files="$files target/include/$h"
    fi
done

PAT='LG_NAME_GATE|"lg/gate"|LG_MSG_GATE_CMD|LG_GATE_CMD_|lg_gate_cmd_msg_t|lg_gate_model_command'
PAT="$PAT"'|lg_gate_model_set_fault|DCMD_LG_GATE_FAULT|DCMD_LG_GATE_BATTERY|DCMD_LG_SENSOR_FAULT|ioctl'
PAT="$PAT"'|O_WRONLY|O_RDWR|freopen|(^|[^a-z_])system *\(|popen|spawn[lvp]|posix_spawn|(^|[^a-z_])exec[lv]'
PAT="$PAT"'|(^|[^a-z_])kill *\(|SignalKill|ThreadDestroy|MsgDeliver|ConnectAttach'

bad=$(grep -nE "$PAT" $files 2>/dev/null)
if [ -n "$bad" ]; then
    echo "$bad"
    echo "FAIL: check_health_no_gate (lg-health must not command the gate or change a device)"
    exit 1
fi
# fopen only for reading (lg_read_file reads the thresholds with "rb").
other=$(grep -nE 'fopen' $files 2>/dev/null | grep -vE 'fopen\([^,]*, *"rb?"\)')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_health_no_gate (fopen only with mode r or rb)"
    exit 1
fi
# Every DCMD_ named is one of the allowed ones.
other=$(grep -noE 'DCMD_[A-Z_]+' $files 2>/dev/null | grep -vE ':(DCMD_LG_SENSOR_STATS|DCMD_LG_GATE_STATS|DCMD_LG_HEALTH_RESET)$')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_health_no_gate (only the STATS devctls may be used)"
    exit 1
fi
# Every message type it fills in is a health verdict.
other=$(grep -nE 'hdr\.type *=[^=]' $files 2>/dev/null | grep -v 'LG_MSG_HEALTH_VERDICT')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_health_no_gate (lg-health may only send LG_MSG_HEALTH_VERDICT)"
    exit 1
fi
hb=target/include/lg_heartbeat.h
if grep -q '#include "lg_heartbeat.h"' "$dir"/*.c 2>/dev/null; then
    other=$(grep -nE 'name_open|MsgSend|ConnectAttach|devctl|ioctl' "$hb" | grep -vE '^[0-9]+: *(/\*|\*)' |
        grep -vE 'name_open\(LG_NAME_SUPERVISOR, 0\)|MsgSendPulse\(')
    if [ -n "$other" ]; then
        echo "$other"
        echo "FAIL: check_health_no_gate ($hb may only open the supervisor and send heartbeat pulses)"
        exit 1
    fi
fi
echo "PASS: check_health_no_gate ($(echo $files | wc -w | tr -d ' ') files)"
exit 0
