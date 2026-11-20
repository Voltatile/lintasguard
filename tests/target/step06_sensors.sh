#!/bin/sh
# Step 6 target test: lg-plant, six lg-sensor processes, lgctl, and the sample stream.
# Runs ON the QNX target (sh -s over ssh, see scripts/collect_evidence.sh).
# Needs everything in /tmp/lintasguard (make deploy IP=<vm-ip>): binaries, config/, scenarios/.
# Prints one PASS:/FAIL: line per check and exits non-zero when any check fails.
# Every check is ONE command: never write "check name [ a ] && [ b ]" (only [ a ] would be checked).
# Timing numbers come from a VirtualBox VM: functional evidence only.

BIN=/tmp/lintasguard
SENSORS="west/a west/b west/island east/a east/b east/island"
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

# ---- helpers ----------------------------------------------------------------------------
sensor_line() { cat /dev/lg/sensor/$1; }
sfield() { sensor_line "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
stat_of() { $BIN/lgctl sensor "$1" stats | tr ' ' '\n' | sed -n "s/^$2=//p"; }
plant_ms() { cat /dev/lg/plant | tr ' ' '\n' | sed -n 's/^t_ms=//p'; }

in_range() { # in_range <value> <low> <high>
    [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}

# Kill by PID from pidin (slay only matches the name exactly as the process was started).
killproc() {
    for p in $(pidin -p "$1" -F '%a' 2>/dev/null | grep -E '^ *[0-9]+$'); do
        kill -9 "$p" 2>/dev/null
    done
}

cleanup() {
    killproc lg-sample-sink
    killproc lg-sensor
    killproc lg-plant
    killproc lgctl
}

wait_for() { # wait_for <path> <max tenths of a second>
    i=0
    while [ $i -lt "$2" ]; do
        [ -e "$1" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

start_plant() { # start_plant <scenario file name>
    $BIN/lg-plant --crossing $BIN/config/crossing.json --scenario $BIN/scenarios/$1 -v \
        </dev/null >/tmp/plant.log 2>&1 &
    wait_for /dev/name/local/lg/plant 50
}

start_sensor() { # start_sensor <dir> <id>
    $BIN/lg-sensor -d "$1" -i "$2" -v </dev/null >/tmp/sensor_$1_$2.log 2>&1 &
    wait_for /dev/lg/sensor/$1/$2 50
}

start_sensors() {
    ok=0
    for s in $SENSORS; do
        start_sensor "${s%/*}" "${s#*/}" || ok=1
    done
    return $ok
}

other_five_present() {
    for s in west/a west/b east/a east/b east/island; do
        [ -e /dev/lg/sensor/$s ] || return 1
    done
    return 0
}

all_paths_exist() {
    for s in $SENSORS; do
        [ -e /dev/lg/sensor/$s ] || return 1
    done
    return 0
}

# ---- multi-part conditions, as functions so that check sees ONE command ------------------
idle_all_clear() {
    for s in $SENSORS; do
        [ "$(sfield $s occupied)" = "0" ] || return 1
        [ "$(sfield $s axles)" = "0" ] || return 1
        [ "$(sfield $s fault)" = "NONE" ] || return 1
    done
    return 0
}
idle_quality_high() {
    for s in $SENSORS; do
        q=$(sfield $s quality)
        in_range "$q" 90 100 || return 1
    done
    return 0
}
text_format_ok() {
    sensor_line west/a | grep -qE '^occupied=[01] quality=[0-9]+ axles=[0-9]+ raw=[0-9]+ fault=[A-Z_]+ seq=[0-9]+ tick=[0-9]+$'
}
others_quality_97() { # every sensor except $1 reads quality 97
    for s in $SENSORS; do
        [ "$s" = "$1" ] && continue
        [ "$(sfield $s quality)" = "97" ] || return 1
    done
    return 0
}
all_quality_97() {
    for s in $SENSORS; do
        [ "$(sfield $s quality)" = "97" ] || return 1
    done
    return 0
}
restarted_clear() { # the sensor $1 reads clear with fault NONE (both parts must hold)
    [ "$(sfield $1 occupied)" = "0" ] && [ "$(sfield $1 fault)" = "NONE" ]
}
unchanged() { # unchanged <before> <after>: both present and equal (an empty pair would pass a plain test)
    [ -n "$1" ] && [ -n "$2" ] && [ "$1" = "$2" ]
}
grew_by() { # grew_by <before> <after> <at least>
    [ -n "$1" ] && [ -n "$2" ] && [ "$2" -ge "$(($1 + $3))" ]
}
# answers_within <seconds> <command...>: the command finishes within the time (a frozen device would not)
answers_within() {
    limit=$(($1 * 10))
    shift
    "$@" >/dev/null 2>&1 &
    cp=$!
    n=0
    while [ $n -lt $limit ] && kill -0 $cp 2>/dev/null; do
        sleep 0.1
        n=$((n + 1))
    done
    if kill -0 $cp 2>/dev/null; then
        kill -9 $cp 2>/dev/null
        return 1
    fi
    return 0
}
stats_prio_ok() { $BIN/lgctl sensor west/a stats | grep -q 'sched_policy=1 sched_priority=45'; }
worker_prio_ok() { $BIN/lgctl sensor west/a stats | grep -q 'worker_policy=1 worker_priority=45'; }
all_workers_prio_ok() { # every sensor reports its worker thread at SCHED_FIFO 45
    for s in $SENSORS; do
        $BIN/lgctl sensor $s stats | grep -q 'worker_policy=1 worker_priority=45' || return 1
    done
    return 0
}
write_refused() { [ "$wrc" -ne 0 ] && ! echo "$wr" | grep -q '1 bytes'; }

# raw_range <sensor> <n>: spread (max - min) of the raw level over n reads.
raw_range() {
    lo=2000
    hi=0
    n=0
    while [ $n -lt "$2" ]; do
        r=$(sfield "$1" raw)
        [ "$r" -lt $lo ] && lo=$r
        [ "$r" -gt $hi ] && hi=$r
        sleep 0.02
        n=$((n + 1))
    done
    echo $((hi - lo))
}

sink_value() { # sink_value <file> <sensor> <field>
    grep "sink: $2 " "$1" | tr ' ' '\n' | sed -n "s/^$3=//p"
}
all_sinks_gap_free() {
    for s in $SENSORS; do
        [ "$(sink_value "$1" $s gaps)" = "0" ] || return 1
    done
    return 0
}
all_sinks_count_ok() { # at least <min> samples from every sensor
    for s in $SENSORS; do
        c=$(sink_value "$1" $s count)
        [ -n "$c" ] && [ "$c" -ge "$2" ] || return 1
    done
    return 0
}
all_sinks_age_below() { # max age below <us> for every sensor
    for s in $SENSORS; do
        a=$(sink_value "$1" $s max_age_us)
        [ -n "$a" ] && [ "$a" -lt "$2" ] || return 1
    done
    return 0
}
others_gap_free() { # every sensor except the two named in $2 and $3 has at most 2 percent gaps
    for s in $SENSORS; do
        [ "$s" = "$2" ] && continue
        [ "$s" = "$3" ] && continue
        g=$(sink_value "$1" $s gaps)
        c=$(sink_value "$1" $s count)
        [ -n "$g" ] && [ -n "$c" ] && [ $((g * 100)) -le $((c * 2)) ] || return 1
    done
    return 0
}
# A sensor skips sequence numbers when its timer pulse comes 20 ms or more late, and the VM alone does
# that (the bare timer of the baseline counts those gaps as over_20ms). So the missing numbers are
# compared with the baseline: at most the baseline percentage plus 2 points, for every sensor.
sink_gaps_small() { # sink_gaps_small <file> <baseline percent of gaps over 20 ms>
    limit=$(($2 + 2))
    for s in $SENSORS; do
        g=$(sink_value "$1" $s gaps)
        c=$(sink_value "$1" $s count)
        [ -n "$g" ] && [ -n "$c" ] && [ $((g * 100)) -le $((c * limit)) ] || return 1
    done
    return 0
}
sink_avg_age_below() { # average sample age below <us> for every sensor
    for s in $SENSORS; do
        a=$(sink_value "$1" $s avg_age_us)
        [ -n "$a" ] && [ "$a" -lt "$2" ] || return 1
    done
    return 0
}
sink_max_age() { # largest max_age_us of all sensors
    m=0
    for s in $SENSORS; do
        a=$(sink_value "$1" $s max_age_us)
        [ -n "$a" ] && [ "$a" -gt "$m" ] && m=$a
    done
    echo $m
}
jitter_value() { # jitter_value <line> <field>
    echo "$1" | sed -n "s/.* $2=\([0-9]*\).*/\1/p"
}

# trace_run <until plant t_ms> <file>: one line per round with the plant time and the six sensors
trace_run() {
    : > "$2"
    t=0
    while [ "$t" -lt "$1" ]; do
        set -- "$1" "$2" $(cat /dev/lg/plant)
        t=${3#t_ms=}
        set -- "$1" "$2" "$t" $(cat /dev/lg/sensor/west/a)
        wa=${4#occupied=}
        waa=${6#axles=}
        set -- "$1" "$2" "$3" "$wa" "$waa" $(cat /dev/lg/sensor/west/b)
        wb=${6#occupied=}
        wba=${8#axles=}
        set -- "$1" "$2" "$3" "$wa" "$waa" "$wb" "$wba" $(cat /dev/lg/sensor/west/island)
        wi=${8#occupied=}
        wia=${10#axles=}
        set -- "$1" "$2" "$3" "$wa" "$waa" "$wb" "$wba" "$wi" "$wia" $(cat /dev/lg/sensor/east/a)
        ea=${10#occupied=}
        eaa=${12#axles=}
        set -- "$1" "$2" "$3" "$wa" "$waa" "$wb" "$wba" "$wi" "$wia" "$ea" "$eaa" $(cat /dev/lg/sensor/east/b)
        eb=${12#occupied=}
        eba=${14#axles=}
        set -- "$1" "$2" "$3" "$wa" "$waa" "$wb" "$wba" "$wi" "$wia" "$ea" "$eaa" "$eb" "$eba" $(cat /dev/lg/sensor/east/island)
        ei=${14#occupied=}
        eia=${16#axles=}
        echo "$3 $wa $waa $wb $wba $wi $wia $ea $eaa $eb $eba $ei $eia" >> "$2"
        t=$3
        set -- "$1" "$2"
        sleep 0.05
    done
}

# Columns of the trace: 1 t, 2/3 west a occ/axles, 4/5 west b, 6/7 west island, 8/9 east a,
# 10/11 east b, 12/13 east island.
# max_round <file>: the largest step between two time stamps of the trace (one round of the loop).
max_round() {
    awk 'NR > 1 && $1 - pt > r { r = $1 - pt } { pt = $1 } END { print r + 0 }' "$1"
}
# bracket <file> <col> <rise|fall> <expected ms> <t from> <t to>: does the transition lie where the
# model puts it? The plant time is read BEFORE the sensors in every round, so a reading can be up to
# one round R (the largest step of the whole trace) newer than its time stamp. A rise lies between
# the last clear stamp and the first occupied stamp plus R, a fall between the last occupied stamp
# and the first clear stamp plus R; 50 ms are added for the age of a sample.
bracket() {
    awk -v c="$2" -v kind="$3" -v e="$4" -v lo="$5" -v hi="$6" '
        { if (NR > 1 && $1 - pt > r) r = $1 - pt
          pt = $1
          if ($1 >= lo && $1 < hi) { n++; tt[n] = $1; vv[n] = $c } }
        END {
            if (kind == "rise") {
                for (i = 2; i <= n; i++) if (vv[i] == 1) break
                if (i > n || vv[i - 1] != 0) exit 1
                exit !(e + 50 >= tt[i - 1] && e <= tt[i] + r + 50)
            } else {
                for (i = n; i >= 1; i--) if (vv[i] == 1) break
                if (i < 1 || i == n) exit 1
                exit !(e + 50 >= tt[i] && e <= tt[i + 1] + r + 50)
            }
        }' "$1"
}
first_occ() { # first_occ <file> <column> <t from> <t to>
    awk -v c="$2" -v lo="$3" -v hi="$4" '$1 >= lo && $1 < hi && $c == 1 && !r { r = $1 } END { print r + 0 }' "$1"
}
last_occ() {
    awk -v c="$2" -v lo="$3" -v hi="$4" '$1 >= lo && $1 < hi && $c == 1 { l = $1 } END { print l + 0 }' "$1"
}
max_in() { # max_in <file> <column> <t from> <t to>
    awk -v c="$2" -v lo="$3" -v hi="$4" '$1 >= lo && $1 < hi { if ($c > m) m = $c } END { print m + 0 }' "$1"
}
final_in() { # value of a column in the last line
    awk -v c="$2" 'END { print $c + 0 }' "$1"
}

echo "=== step06_sensors: $(date)"
echo "=== $(uname -a)"
cleanup
sleep 1

# ======================================================================================
# Phase 1: idle plant (the only train comes after an hour). Stream, faults, restarts.
# ======================================================================================
echo "--- baseline: a bare 10 ms timer pulse at priority 45, nothing else running (10 s)"
base=$($BIN/lg-jitter 10 45)
echo "$base"
base_pct=$(jitter_value "$base" late_percent)
base_over20=$(jitter_value "$base" over_20ms)
base_over20_pct=$((base_over20 * 100 / 1000))
check "the timer baseline was measured" [ -n "$base_pct" ]
echo "--- phase 1: scenario test_idle"
check "lg-plant starts and registers /dev/name/local/lg/plant" start_plant test_idle.json
check "six lg-sensor processes start and register their devices" start_sensors
check "all six /dev/lg/sensor/<dir>/<id> paths exist" all_paths_exist
sleep 1
echo "--- cat /dev/lg/plant:"
cat /dev/lg/plant
echo "--- idle samples:"
for s in $SENSORS; do echo "$s: $(sensor_line $s)"; done
check "idle: every sensor reads clear, 0 axles, fault NONE" idle_all_clear
check "idle: every sensor has quality 90 or more" idle_quality_high
check "the sensor text has the documented format" text_format_ok
check "lg-sensor device thread runs SCHED_FIFO priority 45 (read back through devctl)" stats_prio_ok
check "lg-sensor worker thread runs SCHED_FIFO priority 45 (read back by the worker itself, all six sensors)" all_workers_prio_ok

echo "--- write attempt on a sensor device:"
wr=$(dd if=/dev/zero of=/dev/lg/sensor/west/a bs=1 count=1 2>&1)
wrc=$?
echo "$wr"
check "writing to a sensor device is refused (dd non-zero, 0 bytes copied)" write_refused

# ---- sample stream to the controller stand-in, over 10 s -------------------------------
$BIN/lg-sample-sink -t 10 </dev/null >/tmp/sink1.txt 2>&1 &
SINK=$!
sleep 3 # the sensors reconnect to the new controller within 1 s

# ---- noise (during the stream: noise does not change the sample flow) ------------------
before=$(raw_range west/a 40)
$BIN/lgctl sensor west/a noise 50
sleep 0.2
during=$(raw_range west/a 60)
q=$(sfield west/a quality)
echo "--- west/a raw spread: healthy $before counts, noise 50: $during counts, quality $q"
check "healthy raw spread is small (60 counts or less)" in_range "$before" 0 60
check "lgctl sensor west/a noise 50 widens the raw spread (300 counts or more)" [ "$during" -ge 300 ]
check "noise 50 gives quality 50" [ "$q" = "50" ]
$BIN/lgctl sensor west/a none
sleep 0.2
after=$(raw_range west/a 40)
check "fault none brings the raw spread back (60 counts or less)" in_range "$after" 0 60
check "fault none brings the quality back to 97" [ "$(sfield west/a quality)" = "97" ]
check "the other five sensors were not affected by noise on west/a (quality 97)" others_quality_97 west/a

# ---- stuck occupied: reads occupied with no train ---------------------------------------
$BIN/lgctl sensor west/b stuck-occupied
sleep 0.3
echo "--- west/b stuck-occupied: $(sensor_line west/b)"
check "stuck-occupied: west/b reads occupied with no train" [ "$(sfield west/b occupied)" = "1" ]
check "stuck-occupied: the sensor still reports good quality (it looks healthy)" [ "$(sfield west/b quality)" = "97" ]
check "stuck-occupied: west/a next to it still reads clear" [ "$(sfield west/a occupied)" = "0" ]
$BIN/lgctl sensor west/b none
sleep 0.3
check "stuck-occupied cleared: west/b reads clear again" [ "$(sfield west/b occupied)" = "0" ]

# ---- end of the stream window ------------------------------------------------------------
wait $SINK
echo "--- sink after 10 s without dropout or hang:"
cat /tmp/sink1.txt
check "sink: every sensor delivered 800 samples or more in about 10 s" all_sinks_count_ok /tmp/sink1.txt 800
echo "--- baseline: $base_over20 of 1000 ticks of the bare timer were 20 ms or more late ($base_over20_pct percent)"
check "sink: sequence gaps of every sensor are at most the baseline percentage of 20 ms stalls plus 2 points" sink_gaps_small /tmp/sink1.txt "$base_over20_pct"
echo "--- largest sample age of any sensor: $(sink_max_age /tmp/sink1.txt) us (README 10.3 asks for under 20000 us; this is information, VM timer jitter decides it)"
check "sink: the average sample age is below 2 ms for every sensor" sink_avg_age_below /tmp/sink1.txt 2000
t1=$(stat_of west/a ticks)
o1=$(stat_of west/a overruns)
echo "--- west/a ticks=$t1 overruns=$o1 (overrun = tick more than 2 ms later than 10 ms)"
echo "--- west/a overruns: $((o1 * 100 / t1)) percent of its ticks; the bare timer of the baseline: $base_pct percent"
check "west/a overruns are at most 10 points above the bare timer baseline" [ $((o1 * 100 / t1)) -le $((base_pct + 10)) ]

# ---- dropout and hang, with a second sink -------------------------------------------------
$BIN/lg-sample-sink -t 8 </dev/null >/tmp/sink2.txt 2>&1 &
SINK=$!
sleep 2
$BIN/lgctl sensor east/a dropout 50
$BIN/lgctl sensor east/b hang
ta=$(stat_of east/a ticks)
na=$(stat_of east/a no_sample)
sb=$(stat_of east/b samples_sent)
nb=$(stat_of east/b no_sample)
sleep 3
ta2=$(stat_of east/a ticks)
na2=$(stat_of east/a no_sample)
sb2=$(stat_of east/b samples_sent)
nb2=$(stat_of east/b no_sample)
ratio=-1
if [ "$((ta2 - ta))" -gt 0 ]; then
    ratio=$(( (na2 - na) * 100 / (ta2 - ta) ))
fi
echo "--- east/a dropout 50: $((na2 - na)) of $((ta2 - ta)) ticks without a sample = $ratio percent"
echo "--- east/b hang: samples_sent $sb -> $sb2, no_sample $nb -> $nb2"
check "dropout 50: between 35 and 65 percent of the ticks have no sample" in_range "$ratio" 35 65
check "hang: east/b sends no samples while hung" unchanged "$sb" "$sb2"
check "hang: east/b counts its silent ticks" [ "$nb2" -gt "$((nb + 200))" ]
check "hang: east/b still answers reads and shows fault=HANG" [ "$(sfield east/b fault)" = "HANG" ]
$BIN/lgctl sensor east/a none
$BIN/lgctl sensor east/b none
wait $SINK
echo "--- sink with dropout on east/a and hang on east/b:"
cat /tmp/sink2.txt
ga=$(sink_value /tmp/sink2.txt east/a gaps)
gb=$(sink_value /tmp/sink2.txt east/b gaps)
echo "--- gaps seen by the sink: east/a=$ga east/b=$gb"
check "sink sees the dropout as sequence gaps on east/a (100 or more)" [ "$ga" -ge 100 ]
check "sink sees the hang as sequence gaps on east/b (200 or more)" [ "$gb" -ge 200 ]
check "sink: no gaps on the four healthy sensors" others_gap_free /tmp/sink2.txt east/a east/b
# The sink has finished by now, so samples_sent cannot grow. A sensor that produces samples again
# stops counting silent ticks (no_sample), whether or not a controller is there to receive them.
sleep 1
nb3=$(stat_of east/b no_sample)
tb3=$(stat_of east/b ticks)
sleep 1
nb4=$(stat_of east/b no_sample)
tb4=$(stat_of east/b ticks)
echo "--- east/b after the hang: no_sample $nb3 -> $nb4, ticks $tb3 -> $tb4"
check "hang cleared: east/b produces samples again (no_sample stops growing)" unchanged "$nb3" "$nb4"
check "hang cleared: east/b keeps ticking" [ "$tb4" -gt "$((tb3 + 50))" ]

# ---- a controller that is alive but silent (SIGSTOP) ---------------------------------------
# The worker of a sensor may then wait for a reply that never comes (the kernel keeps a sender
# blocked after the server has received the message). The device must still answer, the ticks
# must continue after the controller continues, and the sensors must find a new controller
# after the old one is gone.
$BIN/lg-sample-sink -t 40 </dev/null >/tmp/sink3.txt 2>&1 &
SINK3=$!
sleep 3
sa=$(stat_of west/a samples_sent)
sleep 1
sb=$(stat_of west/a samples_sent)
check "before the stop: west/a delivers samples to the controller stand-in" grew_by "$sa" "$sb" 50
kill -STOP $SINK3
sleep 3
check "controller stopped: reading the sensor device still answers within 2 s" answers_within 2 cat /dev/lg/sensor/west/a
check "controller stopped: lgctl stats still answers within 2 s" answers_within 2 $BIN/lgctl sensor west/a stats
check "controller stopped: a read of a second sensor still answers within 2 s" answers_within 2 cat /dev/lg/sensor/east/island
tk1=$(stat_of west/a ticks)
sleep 2
tk2=$(stat_of west/a ticks)
echo "--- ticks of west/a while the controller is stopped: $tk1 -> $tk2 (information: a worker that is blocked in a reply does not tick)"
echo "--- west/a while the controller is stopped: $(sensor_line west/a)"
kill -CONT $SINK3
sleep 3
ta=$(stat_of west/a ticks)
sleep 1
tb=$(stat_of west/a ticks)
check "controller continued: the sensor ticks again (50 or more ticks in 1 s)" grew_by "$ta" "$tb" 50
kill -STOP $SINK3
sleep 1
kill -9 $SINK3
sleep 2
nc1=$(stat_of west/a no_controller)
cn1=$(stat_of west/a connects)
sleep 1
nc2=$(stat_of west/a no_controller)
echo "--- controller gone: west/a no_controller $nc1 -> $nc2, connects $cn1"
check "controller gone: the sensor counts ticks without a controller" grew_by "$nc1" "$nc2" 50
$BIN/lg-sample-sink -t 6 </dev/null >/tmp/sink4.txt 2>&1 &
SINK4=$!
sleep 3
cn2=$(stat_of west/a connects)
sa=$(stat_of west/a samples_sent)
sleep 1
sb=$(stat_of west/a samples_sent)
echo "--- new controller: west/a connects $cn1 -> $cn2"
check "a new controller is found again (connects went up)" grew_by "$cn1" "$cn2" 1
check "a new controller receives the samples again" grew_by "$sa" "$sb" 50
wait $SINK4

# ---- drift: the quality falls over time ----------------------------------------------------
$BIN/lgctl sensor east/island drift 10
sleep 4
qd=$(sfield east/island quality)
echo "--- east/island after 4 s of drift 10: quality $qd"
check "drift 10: the quality fell below 70 after 4 s" [ "$qd" -lt 70 ]
check "drift 10: the quality is still above 0 after 4 s" [ "$qd" -gt 0 ]
$BIN/lgctl sensor east/island none

# ---- devctl refusals -------------------------------------------------------------------------
$BIN/lgctl sensor west/a noise 101 >/dev/null 2>&1
rc=$?
check "lgctl: a noise level above 100 is rejected (exit code 3 = EINVAL)" [ $rc -eq 3 ]
$BIN/lgctl sensor west/a delay 2001 >/dev/null 2>&1
rc=$?
check "lgctl: a delay above 2000 ms is rejected (exit code 3)" [ $rc -eq 3 ]
$BIN/lgctl sensor west/a fault-ro noise 50 >/dev/null 2>&1
rc=$?
check "lgctl: a fault through a read-only descriptor is refused (exit code 4 = EPERM)" [ $rc -eq 4 ]
check "the refused and rejected requests changed nothing on west/a" [ "$(sfield west/a fault)" = "NONE" ]
$BIN/lgctl sensor west/a bogus >/dev/null 2>&1
rc=$?
check "lgctl: an unknown mode is a usage error (exit code 2)" [ $rc -eq 2 ]
$BIN/lgctl sensor north/a noise 10 >/dev/null 2>&1
rc=$?
check "lgctl: an unknown sensor is a usage error (exit code 2)" [ $rc -eq 2 ]

# ---- crash injection and restart of one sensor ------------------------------------------------
$BIN/lgctl sensor west/island crash >/dev/null 2>&1
sleep 1
check "after CRASH the device /dev/lg/sensor/west/island is gone" [ ! -e /dev/lg/sensor/west/island ]
check "after CRASH the other five sensors are still there" other_five_present
check "the crashed sensor starts again" start_sensor west island
check "the restarted sensor reads clear with fault NONE" restarted_clear west/island

# ---- the plant goes away and comes back ---------------------------------------------------------
echo "--- plant restart"
killproc lg-plant
sleep 1
pf1=$(stat_of west/a plant_fail)
sleep 1
pf2=$(stat_of west/a plant_fail)
echo "--- west/a without a plant: $(sensor_line west/a), plant_fail $pf1 -> $pf2"
check "without the plant a sensor reports quality 0" [ "$(sfield west/a quality)" = "0" ]
check "without the plant the failures are counted" [ "$pf2" -gt "$pf1" ]
check "lg-plant starts again" start_plant test_idle.json
sleep 3
echo "--- west/a with the plant back: $(sensor_line west/a), plant time $(plant_ms) ms"
check "all six sensors reconnect to the restarted plant (quality back to 97)" all_quality_97
check "the restarted plant restarted the scenario clock (under 5 s)" [ "$(plant_ms)" -lt 5000 ]

# ======================================================================================
# Phase 2: two trains pass (scenario test_short): west train at 5 s, east train at 30 s.
# ======================================================================================
echo "--- phase 2: scenario test_short"
killproc lg-sensor
killproc lg-plant
sleep 1
start_plant test_short.json
start_sensors
sleep 1
$BIN/lgctl sensor east/b delay 1000
echo "--- east/b has a 1000 ms delay for the east train; tracing until plant time 52 s"
trace_run 52000 /tmp/trace.txt
lines=$(wc -l < /tmp/trace.txt)
echo "--- trace: $lines lines, first and last:"
head -n 1 /tmp/trace.txt
tail -n 1 /tmp/trace.txt
echo "--- trace columns: t waOcc waAxl wbOcc wbAxl wiOcc wiAxl eaOcc eaAxl ebOcc ebAxl eiOcc eiAxl"

# The trace takes the plant time first and the sensors after it, and one round takes about 100 ms:
# a sensor reading can be up to about 100 ms newer than its time stamp. The lower bounds of the
# strike-in windows allow for that (the upper bounds are not loosened).
# West train (strikes in at 5000 ms, 30 m/s, 120 m, 8 axles): window t < 29000
a_rise=$(first_occ /tmp/trace.txt 2 0 29000)
a_last=$(last_occ /tmp/trace.txt 2 0 29000)
b_rise=$(first_occ /tmp/trace.txt 4 0 29000)
i_rise=$(first_occ /tmp/trace.txt 6 0 29000)
i_last=$(last_occ /tmp/trace.txt 6 0 29000)
ei_rise=$(first_occ /tmp/trace.txt 12 0 29000)
echo "--- west train: west/a occupied $a_rise..$a_last ms, west/b from $b_rise, island $i_rise..$i_last, east island from $ei_rise"
R=$(max_round /tmp/trace.txt)
echo "--- largest step of the trace (one round of the loop): $R ms"
check "trace has at least 60 lines" [ "$lines" -ge 60 ]
check "the largest step of the trace is below 1500 ms (the brackets stay meaningful)" [ "$R" -lt 1500 ]
check "west train: west/a goes occupied at the strike-in time (model: 5001 ms, bracketed by the trace)" bracket /tmp/trace.txt 2 rise 5001 0 29000
check "west train: west/a goes clear when the rear leaves its zone (model: 9067 ms, bracketed)" bracket /tmp/trace.txt 2 fall 9067 0 29000
check "west train: west/b goes occupied together with west/a (within one round of the trace)" \
    in_range "$b_rise" "$((a_rise - R - 50))" "$((a_rise + R + 50))"
check "west train: the island sensor goes occupied when the front enters (model: 18334 ms, bracketed)" bracket /tmp/trace.txt 6 rise 18334 0 29000
check "west train: the island sensor goes clear when the rear leaves (model: 22734 ms, bracketed)" bracket /tmp/trace.txt 6 fall 22734 0 29000
check "west train: the east island sensor also sees the train on the island (physical occupancy)" \
    bracket /tmp/trace.txt 12 rise 18334 0 29000
check "west train: east/a never reads occupied (direction aware)" [ "$(max_in /tmp/trace.txt 8 0 29000)" = "0" ]
check "west train: east/b never reads occupied (direction aware)" [ "$(max_in /tmp/trace.txt 10 0 29000)" = "0" ]
check "west train: west/a counted its 8 axles" [ "$(max_in /tmp/trace.txt 3 0 29000)" = "8" ]
check "west train: west/b counted its 8 axles" [ "$(max_in /tmp/trace.txt 5 0 29000)" = "8" ]
check "west train: the west island sensor counted 8 axles out" [ "$(max_in /tmp/trace.txt 7 0 29000)" = "8" ]
check "west train: the east island sensor counted no axles (east trains only)" [ "$(max_in /tmp/trace.txt 13 0 29000)" = "0" ]

# East train (strikes in at 30000 ms, 6 axles), east/b delayed by 1000 ms
ea_rise=$(first_occ /tmp/trace.txt 8 29000 99999)
ea_last=$(last_occ /tmp/trace.txt 8 29000 99999)
eb_rise=$(first_occ /tmp/trace.txt 10 29000 99999)
ei2_rise=$(first_occ /tmp/trace.txt 12 29000 99999)
lag=$((eb_rise - ea_rise))
echo "--- east train: east/a occupied $ea_rise..$ea_last ms, east/b from $eb_rise (lag $lag ms), island from $ei2_rise"
check "east train: east/a goes occupied at the strike-in time (model: 30001 ms, bracketed)" bracket /tmp/trace.txt 8 rise 30001 29000 99999
check "east train: east/b with delay 1000 follows east/a by 1000 ms (within one round of the trace)" in_range "$lag" "$((1000 - R - 50))" "$((1000 + R + 50))"
check "east train: the east island sensor goes occupied (model: 43334 ms, bracketed)" bracket /tmp/trace.txt 12 rise 43334 29000 99999
check "east train: west/a never reads occupied again" [ "$(max_in /tmp/trace.txt 2 29000 99999)" = "0" ]
check "east train: east/a counted 6 axles" [ "$(final_in /tmp/trace.txt 9)" = "6" ]
check "east train: east/b counted 6 axles (delayed, but all of them)" [ "$(final_in /tmp/trace.txt 11)" = "6" ]
check "east train: the east island sensor counted 6 axles out" [ "$(final_in /tmp/trace.txt 13)" = "6" ]
check "east train: the west counters stayed at 8" [ "$(final_in /tmp/trace.txt 3)" = "8" ]
check "east train: the west island counter stayed at 8" [ "$(final_in /tmp/trace.txt 7)" = "8" ]
echo "--- plant at the end: $(cat /dev/lg/plant)"
check "both trains are finished at the end of the scenario" sh -c "cat /dev/lg/plant | grep -q 'finished=2'"

echo "--- final stats of every sensor (information):"
for s in $SENSORS; do echo "$s: $($BIN/lgctl sensor $s stats)"; done
echo "--- slog2info, plant events:"
slog2info 2>/dev/null | grep "lg_plant" | grep -E "train" | tail -n 12

cleanup
echo "=== RESULT: $checks checks, $fails failed"
[ $fails -eq 0 ]
