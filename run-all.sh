#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Test suite for stopping the tick in polling idle on nohz_full CPUs.
#
# Boot (bare metal or a guest) with at least one nohz_full CPU and idle=poll,
# e.g. nohz_full=2-3 isolcpus=domain,managed_irq,2-3 idle=poll, then run as
# root:
#   [KO=path/to/module.ko] run-all.sh [cpu] [housekeeping_cpu] [hotplug_cpu]
#
# cpu:              nohz_full CPU under test (default: first nohz_full CPU)
# housekeeping_cpu: online CPU that is not nohz_full, used for T4 and to
#                   start the module and helper tasks (default: first one)
# hotplug_cpu:      nohz_full CPU nothing else uses, cycled offline/online
#                   (default: second nohz_full CPU, T5 is skipped if none)
#
# T1 fails on unpatched kernels: that is the bug being fixed.
# The full log goes to /tmp/nohz_suite.log.

DIR=$(dirname "$(readlink -f "$0")")
KO=${KO:-$DIR/nohz_poll_timer_test/nohz_poll_timer_test.ko}
USER_TICK=$DIR/nohz_user_tick_test/run.sh
DEP=$DIR/nohz_dep_test/nohz_dep_test
LOG=/tmp/nohz_suite.log

# Max lateness accepted for a timer_list timer, against its programmed
# expiry. One jiffy is normal (the timer wheel rounds the expiry up); a
# lost kick shows up as lost timers or seconds of delay. The margin covers
# low HZ values and vCPUs preempted by the host.
MAX_LATE_US=50000

PASS=0
FAIL=0
SKIP=0

# --- Helpers -----------------------------------------------------------------

report()
{
	printf '%-36s %-4s  %s\n' "$1" "$2" "$3"
	case $2 in
	PASS) PASS=$((PASS + 1)) ;;
	FAIL) FAIL=$((FAIL + 1)) ;;
	*)    SKIP=$((SKIP + 1)) ;;
	esac
}

# judge <name> <detail> <test...>: PASS if the test command succeeds
judge()
{
	name=$1
	detail=$2
	shift 2
	if "$@"; then
		report "$name" PASS "$detail"
	else
		report "$name" FAIL "$detail"
	fi
}

# val <key> <line>: value of "key=<number>" in line
val()
{
	printf '%s\n' "$2" | sed -n "s/.*[ ]$1=\([-0-9][0-9]*\).*/\1/p"
}

# expand_cpulist <list like 2-3,5>: one CPU per line
expand_cpulist()
{
	printf '%s\n' "$1" | tr ',' '\n' | awk -F- 'NF {
		hi = (NF > 1) ? $2 : $1
		for (c = $1; c <= hi; c++)
			print c
	}'
}

# in_cpulist <cpu> <list like 2-3,5>
in_cpulist()
{
	expand_cpulist "$2" | grep -qx "$1"
}

# in_range <value> <min> <max>
in_range()
{
	[ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}

# run_logged <name> <command...>: run a command, append its output to the
# log and set RES to its RESULT line (empty on failure)
run_logged()
{
	name=$1
	shift
	echo "### $name: $*" >> $LOG
	RES=$("$@" 2>&1 | tee -a $LOG | grep '^RESULT')
}

# --- Checks ------------------------------------------------------------------

# check_user_tick <name> <cpu> <iters>: no tick in the userspace windows
check_user_tick()
{
	run_logged "$1" "$USER_TICK" "$2" "$3" 100 10
	if [ -z "$RES" ]; then
		report "$1" FAIL "no result, see $LOG"
		return
	fi
	win=$(val windows "$RES")
	user=$(val user_ticks "$RES")
	max=$((win / 50))
	[ $max -lt 2 ] && max=2
	detail="user_ticks=$user/$win windows (max $max), idle_ticks=$(val idle_ticks "$RES"), stops irq/idle/task=$(val stops_irq "$RES")/$(val stops_idle "$RES")/$(val stops_task "$RES")"
	[ "$(val lost "$RES")" = 1 ] && detail="$detail, TRACE OVERFLOW"
	judge "$1" "$detail" [ "$user" -le $max ]
}

# check_timer <name> <cpu> <mode> <delay_ms> <iters>: timers armed on a
# polling CPU fire on time
check_timer()
{
	before=$(dmesg | wc -l)
	echo "### $1: insmod cpu=$2 mode=$3 delay_ms=$4 iters=$5" >> $LOG
	if ! taskset -c "$HK" insmod "$KO" cpu="$2" mode="$3" delay_ms="$4" iters="$5" >> $LOG 2>&1; then
		report "$1" FAIL "insmod failed (module built for another kernel?), see $LOG"
		return
	fi
	rmmod nohz_poll_timer_test
	new=$(dmesg | tail -n +$((before + 1)) | grep nohz_poll_timer_test)
	echo "$new" >> $LOG
	line=$(printf '%s\n' "$new" | grep 'nohz_poll_timer_test: cpu=' | tail -1)
	lost=$(val lost "$line")
	late=$(val max_late_us "$line")
	if [ -z "$lost" ]; then
		report "$1" FAIL "no result, see $LOG"
		return
	fi
	detail="fired=$(val fired "$line") lost=$lost avg_late_us=$(val avg_late_us "$line") max_late_us=$late"
	if printf '%s\n' "$new" | grep -q 'did not happen in idle'; then
		detail="$detail, some enqueues NOT in idle"
	fi
	ok=0
	[ "$lost" -eq 0 ] && [ "$late" -le $MAX_LATE_US ] && ok=1
	judge "$1" "$detail" [ $ok = 1 ]
}

# check_prof <name>: the POSIX CPU timer dependency restarts the tick
check_prof()
{
	run_logged "$1" "$DEP" prof "$CPU" 100 100 20 10
	sig=$(val signals "$RES")
	exp=$(val expected "$RES")
	if [ -z "$exp" ] || [ "$exp" -le 0 ]; then
		report "$1" FAIL "no result, see $LOG"
		return
	fi
	judge "$1" "SIGPROF=$sig expected=$exp" [ $((sig * 100)) -ge $((exp * 80)) ]
}

# check_share <name>: the scheduler dependency restarts the tick
check_share()
{
	run_logged "$1" taskset -c "$HK" "$DEP" share "$CPU" 200 2000
	s0=$(val share0 "$RES")
	s1=$(val share1 "$RES")
	n0=$(val nivcsw0 "$RES")
	n1=$(val nivcsw1 "$RES")
	if [ -z "$s0" ] || [ -z "$s1" ]; then
		report "$1" FAIL "no result, see $LOG"
		return
	fi
	ok=0
	in_range "$s0" 350 650 && in_range "$s1" 350 650 &&
		[ "$n0" -ge 5 ] && [ "$n1" -ge 5 ] && ok=1
	judge "$1" "shares=$s0/$s1 permille, preemptions=$n0/$n1" [ $ok = 1 ]
}

# check_housekeeping <name>: CPUs that are not nohz_full keep ticking while
# polling (50ms sleeps)
check_housekeeping()
{
	if [ $POLL = 0 ]; then
		report "$1" SKIP "needs idle=poll"
		return
	fi
	run_logged "$1" "$USER_TICK" "$HK" 40 50000 5
	if [ -z "$RES" ]; then
		report "$1" FAIL "no result, see $LOG"
		return
	fi
	win=$(val windows "$RES")
	idle=$(val idle_ticks "$RES")
	judge "$1" "idle_ticks=$idle over $win sleeps of 50ms" [ "$idle" -ge $((win * 2)) ]
}

# check_hotplug: cycle a spare nohz_full CPU offline/online, then rerun the
# tick and timer checks on it
check_hotplug()
{
	name="T5 hotplug 5 offline/online cycles"
	online=/sys/devices/system/cpu/cpu$HP/online
	if [ -z "$HP" ]; then
		report "$name" SKIP "no spare nohz_full CPU"
		return
	fi
	if ! in_cpulist "$HP" "$NOHZ" || [ "$HP" = "$CPU" ] || [ ! -w "$online" ]; then
		report "$name" SKIP "CPU $HP not a hotpluggable spare nohz_full CPU"
		return
	fi
	ok=1
	for i in 1 2 3 4 5; do
		echo 0 > $online 2>> $LOG || ok=0
		echo 1 > $online 2>> $LOG || ok=0
	done
	if [ $ok = 0 ] || [ "$(cat $online)" != 1 ]; then
		report "$name" FAIL "CPU $HP, see $LOG"
		echo 1 > $online 2> /dev/null
		return
	fi
	report "$name" PASS "CPU $HP"
	check_user_tick "T5 user tick after hotplug" "$HP" 50
	check_timer "T5 timer local after hotplug" "$HP" local 10 10
	check_timer "T5 timer remote after hotplug" "$HP" remote 10 10
}

# check_warnings <name>: no kernel warnings since the suite started
check_warnings()
{
	warn=$(dmesg | tail -n +$((DMESG_START + 1)) |
	       grep -E 'WARNING:|BUG:|lockdep|INFO: task|rcu.*stall')
	if [ -z "$warn" ]; then
		report "$1" PASS "none"
	else
		report "$1" FAIL "see below"
		printf '%s\n' "$warn" | head -20
	fi
}

# --- Setup -------------------------------------------------------------------

NOHZ=$(cat /sys/devices/system/cpu/nohz_full)
ONLINE=$(cat /sys/devices/system/cpu/online)
POLL=0
grep -qw "idle=poll" /proc/cmdline && POLL=1

CPU=${1:-$(expand_cpulist "$NOHZ" | sed -n 1p)}
HK=${2:-$(expand_cpulist "$ONLINE" | while read -r c; do
	in_cpulist "$c" "$NOHZ" || { echo "$c"; break; }
done)}
HP=${3:-$(expand_cpulist "$NOHZ" | grep -vx "$CPU" | sed -n 1p)}

echo "kernel:      $(uname -r)"
echo "cmdline:     $(cat /proc/cmdline)"
echo "nohz_full:   $NOHZ"
echo "cpuidle:     $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null)"
echo "cpus:        test=$CPU housekeeping=$HK hotplug=$HP"
echo "module:      $KO"
echo

if [ "$(id -u)" != 0 ]; then
	echo "must run as root" >&2
	exit 1
fi
if [ -z "$CPU" ] || ! in_cpulist "$CPU" "$NOHZ"; then
	echo "CPU '$CPU' is not nohz_full (nohz_full='$NOHZ')" >&2
	exit 1
fi
if [ -z "$HK" ] || in_cpulist "$HK" "$NOHZ" || ! in_cpulist "$HK" "$ONLINE"; then
	echo "no usable housekeeping CPU ('$HK'): it must be online and not nohz_full" >&2
	exit 1
fi
for f in "$USER_TICK" "$DEP" "$KO"; do
	if [ ! -e "$f" ]; then
		echo "missing $f, run make in $DIR first" >&2
		exit 1
	fi
done
if [ $POLL = 0 ]; then
	echo "NOTE: idle=poll is not set: the polling idle path is not exercised,"
	echo "      every test should pass on any kernel (control run)."
	echo
fi

: > $LOG
DMESG_START=$(dmesg | wc -l)

# --- Tests -------------------------------------------------------------------

# T1: no leftover tick in userspace after an idle trip
check_user_tick "T1 user tick after idle" "$CPU" 200

# T2: timers enqueued while polling with the tick stopped
check_timer "T2 timer remote enqueue" "$CPU" remote 10 20
check_timer "T2 timer local enqueue" "$CPU" local 10 20
check_timer "T2 timer local enqueue, 1ms" "$CPU" local 1 20

# T3: tick dependencies still restart the tick after idle
check_prof "T3 POSIX CPU timer dependency"
check_share "T3 scheduler dependency"

# T4: CPUs that are not nohz_full keep ticking while polling
check_housekeeping "T4 housekeeping CPU unchanged"

# T5: CPU hotplug of a nohz_full CPU
check_hotplug

# T6: no kernel warnings during the run
check_warnings "T6 kernel warnings"

echo
echo "passed=$PASS failed=$FAIL skipped=$SKIP   (log: $LOG)"
[ $FAIL -eq 0 ]
