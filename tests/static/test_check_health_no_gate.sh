#!/bin/sh
# Self-test of check_health_no_gate.sh: it must fail on each forbidden call planted in a copy of a source,
# and on a missing source directory, and pass on a clean file that does what lg-health does. Run by
# `make test`.
chk=tests/static/check_health_no_gate.sh
tmp=build/host/health_nogate_selftest
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

expect 0 "clean source" 'int f(void) { int fd = open("/dev/lg/gate", O_RDONLY); devctl(fd, DCMD_LG_GATE_STATS, 0, 0, 0);
msg.hdr.type = LG_MSG_HEALTH_VERDICT; f = fopen(path, "rb"); return MsgSend(coid, &msg, sizeof msg, 0, 0); }'
expect 1 "no source directory" -
for bad in 'name_open(LG_NAME_GATE, 0);' 'devctl(fd, DCMD_LG_GATE_FAULT, &r, sizeof r, 0);' \
    'devctl(fd, DCMD_LG_GATE_BATTERY, &r, sizeof r, 0);' 'devctl(fd, DCMD_LG_SENSOR_FAULT, &r, sizeof r, 0);' \
    'devctl(fd, DCMD_SOMETHING_ELSE, 0, 0, 0);' 'msg.hdr.type = LG_MSG_GATE_CMD;' 'm.hdr.type = LG_MSG_GUARD_ACK;' \
    'ioctl(fd, 1);' 'open("/dev/lg/gate", O_RDWR);' 'open("/dev/lg/sensor/west/a", O_WRONLY);' \
    'fopen("/dev/lg/gate", "r+");' 'fopen(path, "w");' 'system("lgctl kill gate");' 'spawnl(P_NOWAIT, "/x", "x", 0);' 'execv("/x", argv);' 'kill(pid, SIGKILL);' \
    'lg_gate_model_command(&m, LG_GATE_CMD_RAISE, now);' 'ConnectAttach(0, pid, chid, 0, 0);'; do
    expect 1 "$bad" "$bad"
done
rm -rf "$tmp"
if [ $fails -ne 0 ]; then
    echo "FAIL: test_check_health_no_gate ($fails)"
    exit 1
fi
echo "PASS: test_check_health_no_gate"
exit 0
