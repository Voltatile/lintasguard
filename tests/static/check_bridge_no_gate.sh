#!/bin/sh
# Static check (CLAUDE.md rule 4): lg-bridge is reachable from the network, so it must have no way to
# command the gate or disturb the crossing. It may only read devices and (Step 12) forward a station
# departure to lg-schedule; it must not open the gate or the controller by name, send any other message,
# use devctl/ioctl, open a device for writing, run other programs or signal other processes. Run by
# `make test`. Prints PASS/FAIL, exits non-zero on FAIL.
#
#   check_bridge_no_gate.sh [bridge dir] [makefile]
#
# Checked: every .c and .h in the bridge directory, every core/*.c that the Makefile links into
# lg-bridge, and the code-carrying headers of target/include that the bridge includes. lg_heartbeat.h is
# the one shared helper that sends: only a name_open of the supervisor and heartbeat pulses are allowed
# there. Review finding M4 (Step 10): the first version grepped only target/lg-bridge/*.c.
dir=${1:-target/lg-bridge}
mk=${2:-Makefile}

if [ ! -d "$dir" ] || [ -z "$(ls "$dir"/*.c 2>/dev/null)" ]; then
    echo "FAIL: check_bridge_no_gate (no lg-bridge sources in $dir: moved or renamed?)"
    exit 1
fi

PAT='LG_NAME_GATE|LG_NAME_CONTROLLER|"lg/gate"|"lg/controller"|LG_MSG_GATE_CMD|LG_GATE_CMD_|lg_gate_cmd_msg_t'
PAT="$PAT"'|LG_MSG_GUARD_ACK|lg_guard_ack_t|LG_MSG_SCHED_REPLY|LG_DEV_SCHEDULE|DCMD_LG_SCHED_OUTAGE'
PAT="$PAT"'|lg_gate_model_command|lg_gate_model_set_fault|MsgDeliver|ConnectAttach|devctl|DCMD_|ioctl'
PAT="$PAT"'|O_WRONLY|O_RDWR|fopen|freopen|(^|[^a-z_])system *\(|popen|spawn[lvp]|posix_spawn|(^|[^a-z_])exec[lv]'
PAT="$PAT"'|(^|[^a-z_])kill *\(|SignalKill|ThreadDestroy'

files=$(ls "$dir"/*.c "$dir"/*.h 2>/dev/null)
core=$(sed -n 's#^\$(BUILD)/target/lg-bridge:##p' "$mk" 2>/dev/null | tr ' ' '\n' | grep -E '^core/.*\.c$')
files="$files $core"
for h in $(sed -n 's/^#include "\(.*\)"/\1/p' "$dir"/*.c 2>/dev/null | sort -u); do
    if [ -f "target/include/$h" ] && [ "$h" != "lg_heartbeat.h" ]; then
        files="$files target/include/$h"
    fi
done

bad=$(grep -nE "$PAT" $files 2>/dev/null)
if [ -n "$bad" ]; then
    echo "$bad"
    echo "FAIL: check_bridge_no_gate (lg-bridge must not command the gate, send messages or run programs)"
    exit 1
fi

# Step 12: the one message lg-bridge may send is a station departure to lg-schedule (which can only move a
# level 3 window earlier or add one). So: name_open only of lg/schedule, only that message type, and every
# MsgSend sends the departure structure "dep".
other=$(grep -nE 'name_open' $files 2>/dev/null | grep -vE 'name_open\(LG_NAME_SCHEDULE, 0\)')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_bridge_no_gate (lg-bridge may only open lg/schedule by name)"
    exit 1
fi
other=$(grep -nE 'hdr\.type *=[^=]' $files 2>/dev/null | grep -v 'LG_MSG_STATION_DEPARTURE')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_bridge_no_gate (lg-bridge may only build LG_MSG_STATION_DEPARTURE)"
    exit 1
fi
other=$(grep -nE 'MsgSend' $files 2>/dev/null | grep -vE 'MsgSend\(coid, &dep, sizeof dep, &ack, sizeof ack\)')
if [ -n "$other" ]; then
    echo "$other"
    echo "FAIL: check_bridge_no_gate (lg-bridge may only send the station departure)"
    exit 1
fi

# The heartbeat helper: the only name it may open is the supervisor's, the only send a pulse.
hb=target/include/lg_heartbeat.h
if grep -q '#include "lg_heartbeat.h"' "$dir"/*.c 2>/dev/null; then
    other=$(grep -nE 'name_open|MsgSend|ConnectAttach|devctl|ioctl' "$hb" | grep -vE '^[0-9]+: *(/\*|\*)' |
        grep -vE 'name_open\(LG_NAME_SUPERVISOR, 0\)|MsgSendPulse\(')
    if [ -n "$other" ]; then
        echo "$other"
        echo "FAIL: check_bridge_no_gate ($hb may only open the supervisor and send heartbeat pulses)"
        exit 1
    fi
fi
echo "PASS: check_bridge_no_gate ($(echo $files | wc -w | tr -d ' ') files)"
exit 0
