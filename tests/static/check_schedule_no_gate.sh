#!/bin/sh
# Static check (CLAUDE.md rule 4): only lg-controller may command the gate. lg-schedule receives station
# departures and sends schedule information to the controller; it must have no way to command the gate or
# change a device:
#  - no gate name or path, no gate command type or helper, no fault, battery or health devctl, no guard message;
#  - devctl only its own DCMD_LG_SCHED_OUTAGE (served, not sent);
#  - the only message types it builds are LG_MSG_SCHED_REPLY and LG_MSG_ACK;
#  - name_open only of lg/controller; ConnectAttach only to its own channel (pid 0, the tick timer);
#  - no descriptor opened for writing, no other programs, no signals.
# Run by `make test`. Prints PASS/FAIL, exits non-zero on FAIL.
#
#   check_schedule_no_gate.sh [schedule dir] [makefile]
#
# Checked: every .c and .h in the lg-schedule directory and every core/*.c the Makefile links into it.
dir=${1:-target/lg-schedule}
mk=${2:-Makefile}

if [ ! -d "$dir" ] || [ -z "$(ls "$dir"/*.c 2>/dev/null)" ]; then
    echo "FAIL: check_schedule_no_gate (no lg-schedule sources in $dir: moved or renamed?)"
    exit 1
fi

files=$(ls "$dir"/*.c "$dir"/*.h 2>/dev/null)
core=$(sed -n 's#^\$(BUILD)/target/lg-schedule:##p' "$mk" 2>/dev/null | tr ' ' '\n' | grep -E '^core/.*\.c$')
files="$files $core"
# The code-carrying target headers it includes. lg_heartbeat.h is checked separately below; lg_devctl.h only
# defines numbers and structures (the uses are checked in the sources).
for h in $(sed -n 's/^#include "\(.*\)"/\1/p' "$dir"/*.c 2>/dev/null | sort -u); do
    if [ -f "target/include/$h" ] && [ "$h" != "lg_heartbeat.h" ] && [ "$h" != "lg_devctl.h" ]; then
        files="$files target/include/$h"
    fi
done

PAT='LG_NAME_GATE|"lg/gate"|LG_DEV_GATE|/dev/lg/gate|LG_MSG_GATE_CMD|LG_GATE_CMD_|lg_gate_cmd_msg_t|lg_gate_model_'
PAT="$PAT"'|LG_MSG_GUARD_ACK|lg_guard_ack_t|ioctl'
PAT="$PAT"'|O_WRONLY|O_RDWR|freopen|(^|[^a-z_])system *\(|popen|spawn[lvp]|posix_spawn|(^|[^a-z_])exec[lv]'
PAT="$PAT"'|(^|[^a-z_])kill *\(|SignalKill|ThreadDestroy|MsgDeliver'

bad=$(grep -nE "$PAT" $files 2>/dev/null)
if [ -n "$bad" ]; then
    echo "$bad"
    echo "FAIL: check_schedule_no_gate (lg-schedule must not command the gate or change a device)"
    exit 1
fi
other=$(grep -nE 'fopen' $files 2>/dev/null | grep -vE 'fopen\([^,]*, *"rb?"\)')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_schedule_no_gate (fopen only with mode r or rb)"
    exit 1
fi
other=$(grep -noE 'DCMD_[A-Z_]+' $files 2>/dev/null | grep -vE ':DCMD_LG_SCHED_OUTAGE$')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_schedule_no_gate (only its own DCMD_LG_SCHED_OUTAGE)"
    exit 1
fi
other=$(grep -nE 'hdr\.type *=[^=]' $files 2>/dev/null | grep -vE 'LG_MSG_SCHED_REPLY|LG_MSG_ACK')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_schedule_no_gate (lg-schedule may only send LG_MSG_SCHED_REPLY and LG_MSG_ACK)"
    exit 1
fi
other=$(grep -nE 'name_open *\(' $files 2>/dev/null | grep -vE 'name_open\(LG_NAME_CONTROLLER, 0\)')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_schedule_no_gate (name_open only of lg/controller)"
    exit 1
fi
other=$(grep -nE 'ConnectAttach *\(' $files 2>/dev/null | grep -vE 'ConnectAttach\(0, 0, ')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_schedule_no_gate (ConnectAttach only to its own channel)"
    exit 1
fi
hb=target/include/lg_heartbeat.h
if grep -q '#include "lg_heartbeat.h"' "$dir"/*.c 2>/dev/null; then
    other=$(grep -nE 'name_open|MsgSend|ConnectAttach|devctl|ioctl' "$hb" | grep -vE '^[0-9]+: *(/\*|\*)' |
        grep -vE 'name_open\(LG_NAME_SUPERVISOR, 0\)|MsgSendPulse\(')
    if [ -n "$other" ]; then
        echo "$other"
        echo "FAIL: check_schedule_no_gate ($hb may only open the supervisor and send heartbeat pulses)"
        exit 1
    fi
fi
echo "PASS: check_schedule_no_gate ($(echo $files | wc -w | tr -d ' ') files)"
exit 0
