#!/bin/sh
# Self-test of check_bridge_no_gate.sh: it must fail on each forbidden call planted in a copy of a bridge
# source, and on a missing source directory, and pass on a clean file. Run by `make test`.
chk=tests/static/check_bridge_no_gate.sh
tmp=build/host/nogate_selftest
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

expect 0 "clean source" 'int f(void) { return open("/dev/lg/gate", O_RDONLY); }'
expect 1 "no source directory" -
for bad in 'name_open(LG_NAME_GATE, 0);' 'MsgSend(coid, &m, sizeof m, 0, 0);' 'devctl(fd, 1, 0, 0, 0);' \
    'ioctl(fd, 1);' 'open("/dev/lg/gate", O_RDWR);' 'fopen("/dev/lg/gate", "r+");' 'system("lgctl kill gate");' \
    'popen("lgctl", "r");' 'spawnl(P_NOWAIT, "/x", "x", 0);' 'execv("/x", argv);' 'kill(pid, SIGKILL);' \
    'SignalKill(0, pid, 0, SIGKILL, 0, 0);' 'lg_gate_model_command(&m, LG_GATE_CMD_RAISE, now);'; do
    expect 1 "$bad" "$bad"
done
rm -rf "$tmp"
if [ $fails -ne 0 ]; then
    echo "FAIL: test_check_bridge_no_gate ($fails)"
    exit 1
fi
echo "PASS: test_check_bridge_no_gate"
exit 0
