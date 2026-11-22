#!/bin/sh
# Static check (CLAUDE.md rule 4): lg-bridge is reachable from the network, so it must have no way to
# command the gate. It may only read devices; it must not open the gate or the controller by name,
# send messages, or build gate commands. Run by `make test`. Prints PASS/FAIL, exits non-zero on FAIL.
dir=${1:-target/lg-bridge}
if [ ! -d "$dir" ] || [ -z "$(ls "$dir"/*.c 2>/dev/null)" ]; then
    echo "PASS: check_bridge_no_gate (no bridge sources yet)"
    exit 0
fi
bad=$(grep -nE 'LG_NAME_GATE|LG_NAME_CONTROLLER|"lg/gate"|"lg/controller"|LG_MSG_GATE_CMD|LG_GATE_CMD_|lg_gate_cmd_msg_t|name_open|MsgSend|ConnectAttach|devctl|O_WRONLY|O_RDWR' "$dir"/*.c)
if [ -n "$bad" ]; then
    echo "$bad"
    echo "FAIL: check_bridge_no_gate (lg-bridge must not command the gate or send messages)"
    exit 1
fi
echo "PASS: check_bridge_no_gate"
exit 0
