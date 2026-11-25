#!/bin/sh
# Self-test of check_schedule_no_gate.sh: it must fail on each forbidden call planted in a copy of a source,
# and on a missing source directory, and pass on a clean file that does what lg-schedule does. Run by
# `make test`.
chk=tests/static/check_schedule_no_gate.sh
tmp=build/host/schedule_nogate_selftest
fails=0

expect() { # expect <0|1> <name> <file content or "-" for no directory>
    rm -rf "$tmp"
    if [ "$3" != "-" ]; then
        mkdir -p "$tmp"
        printf '%s\n' "$3" >"$tmp/main.c"
    fi
    sh "$chk" "$tmp" /dev/null >/dev/null 2>&1
    rc=$?
    [ "$rc" -ne 0 ] && rc=1
    if [ "$rc" != "$1" ]; then
        echo "FAIL: self-test $2 (exit $rc, expected $1)"
        fails=$((fails + 1))
    fi
}

expect 0 "clean source" 'int f(void) { coid = name_open(LG_NAME_CONTROLLER, 0); r.hdr.type = LG_MSG_SCHED_REPLY;
ack.hdr.type = LG_MSG_ACK; c = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0); if (dcmd != DCMD_LG_SCHED_OUTAGE) {}
f = fopen(path, "rb"); return MsgSend(coid, &r, sizeof r, &ack, sizeof ack); }'
expect 1 "no source directory" -
for bad in 'name_open(LG_NAME_GATE, 0);' 'name_open("lg/gate", 0);' 'open(LG_DEV_GATE, O_RDONLY);' \
    'open("/dev/lg/gate", O_RDONLY);' 'msg.hdr.type = LG_MSG_GATE_CMD;' 'm.hdr.type = LG_MSG_GUARD_ACK;' \
    'm.hdr.type = LG_MSG_HEALTH_VERDICT;' 'devctl(fd, DCMD_LG_GATE_FAULT, &r, sizeof r, 0);' \
    'devctl(fd, DCMD_LG_SENSOR_FAULT, &r, sizeof r, 0);' 'devctl(fd, DCMD_LG_HEALTH_RESET, &r, sizeof r, 0);' \
    'name_open(LG_NAME_BRIDGE, 0);' 'ConnectAttach(0, pid, chid, 0, 0);' 'ioctl(fd, 1);' \
    'open("/dev/lg/sensor/west/a", O_WRONLY);' 'fopen(path, "w");' 'system("lgctl kill gate");' \
    'spawnl(P_NOWAIT, "/x", "x", 0);' 'execv("/x", argv);' 'kill(pid, SIGKILL);' \
    'lg_gate_model_command(&m, LG_GATE_CMD_RAISE, now);'; do
    expect 1 "$bad" "$bad"
done
rm -rf "$tmp"
if [ $fails -ne 0 ]; then
    echo "FAIL: test_check_schedule_no_gate ($fails)"
    exit 1
fi
echo "PASS: test_check_schedule_no_gate"
exit 0
