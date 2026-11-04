#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run as root:
#   run.sh [cpu] [iterations] [sleep_us] [busy_ms]
# cpu defaults to the first nohz_full CPU.
#
# Counts ticks (tick_nohz_handler expirations) on <cpu> and how many of
# them hit the userspace busy windows of nohz_user_tick. Each tick stop
# is classified by the context of its tick_stop event:
#   - irq exit: hardirq flag set; tick_irq_exit() runs before
#               HARDIRQ_OFFSET is dropped (the leftover tick stopping
#               itself, or any other interrupt exit)
#   - idle:     idle task (pid 0) outside hardirq, e.g. stop on idle entry
#   - task:     any other task outside hardirq, e.g. stop on return to
#               userspace

DIR=$(dirname "$(readlink -f "$0")")
NOHZ_LIST=$(cat /sys/devices/system/cpu/nohz_full)
CPU=${1:-${NOHZ_LIST%%[-,]*}}
ITERS=${2:-200}
SLEEP_US=${3:-100}
BUSY_MS=${4:-10}
T=/sys/kernel/tracing
OUT=/tmp/nohz_user_tick.trace

if [ -z "$CPU" ]; then
	echo "no nohz_full CPU, pass one explicitly" >&2
	exit 1
fi
[ -e $T/trace ] || mount -t tracefs nodev $T || exit 1

# 1 if $CPU is in the nohz_full list (e.g. "2-3,5")
NOHZ=$(printf '%s\n' "$NOHZ_LIST" | tr ',' '\n' | awk -F- -v c="$CPU" '
	{ lo = $1; hi = (NF > 1) ? $2 : $1; if (c >= lo && c <= hi) f = 1 }
	END { print f + 0 }')

echo "kernel:      $(uname -r)"
echo "cmdline:     $(cat /proc/cmdline)"
echo "nohz_full:   $NOHZ_LIST"
echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
echo "test:        cpu=$CPU ($([ "$NOHZ" = 1 ] && echo nohz_full || echo "not nohz_full")) iters=$ITERS sleep_us=$SLEEP_US busy_ms=$BUSY_MS"

# Only the test CPU's buffer is read and enlarged, which works for any CPU
# number and doesn't allocate memory on every CPU of a big machine
PCPU=$T/per_cpu/cpu$CPU
echo 0 > $T/tracing_on
echo 0 > $T/events/enable
echo > $T/trace
echo 1 > $T/options/irq-info
echo 8192 > $PCPU/buffer_size_kb
echo 1 > $T/events/timer/hrtimer_expire_entry/enable
echo 1 > $T/events/timer/tick_stop/enable
echo 1 > $T/tracing_on

"$DIR/nohz_user_tick" "$CPU" "$ITERS" "$SLEEP_US" "$BUSY_MS"

echo 0 > $T/tracing_on
cat $PCPU/trace > $OUT
echo 0 > $T/events/enable

awk -v nohz="$NOHZ" '
# Event lines look like:
#   <idle>-0       [002] d.h1.   105.123456: tick_stop: success=1 ...
# The task name may contain spaces, so locate the "[cpu]" field and take
# the pid from the field before it and the irq-info flags after it. The
# third flag is "h"/"H" in hardirq context.
function classify(   i, pid, flags) {
	for (i = 2; i <= NF; i++)
		if ($i ~ /^\[[0-9]+\]$/)
			break
	pid = $(i - 1)
	sub(/.*-/, "", pid)
	flags = $(i + 1)

	if (substr(flags, 3, 1) ~ /[hH]/)
		stops["irq"]++
	else if (pid == 0)
		stops["idle"]++
	else
		stops["task"]++
}
/tracing_mark_write: BUSY_START/	{ inwin = 1; windows++ }
/tracing_mark_write: BUSY_END/		{ inwin = 0 }
/hrtimer_expire_entry:.*function=tick_nohz_handler/ {
	ticks++
	if (inwin)
		user_ticks++
}
/tick_stop: success=1/			{ total_stops++; classify() }
/LOST [0-9]+ EVENTS/			{ lost = 1 }
END {
	printf "\nbusy windows:            %d\n", windows
	printf "ticks on cpu:            %d\n", ticks
	printf "ticks in user windows:   %d   %s\n", user_ticks,
	       nohz ? "<-- should be ~0 with a fix" :
		      "(not nohz_full: the tick runs while busy)"
	printf "tick stops:              %d\n", total_stops
	printf "  from irq exit:         %d\n", stops["irq"]
	printf "  from idle:             %d\n", stops["idle"]
	printf "  from task context:     %d\n", stops["task"]
	if (lost)
		print "WARNING: trace buffer overflowed, numbers are incomplete"
	printf "RESULT windows=%d ticks=%d user_ticks=%d idle_ticks=%d stops_irq=%d stops_idle=%d stops_task=%d lost=%d\n",
	       windows, ticks, user_ticks, ticks - user_ticks,
	       stops["irq"], stops["idle"], stops["task"], lost
}' $OUT

echo "raw trace:   $OUT"
