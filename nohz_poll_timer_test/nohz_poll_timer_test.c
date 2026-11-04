// SPDX-License-Identifier: GPL-2.0
/*
 * Check that a timer_list timer enqueued on a nohz_full CPU sitting in
 * polling idle (idle=poll) still fires on time.
 *
 * If the tick is stopped while polling, the new timer is only programmed
 * once the idle loop reevaluates the tick, and cpu_idle_poll() only
 * returns on need_resched(). The enqueue kicks the target CPU:
 *
 * mode=remote: add_timer_on() from the CPU loading the module. On x86 the
 *              kick reaches a polling CPU through
 *              call_function_single_prep_ipi(), which sets NEED_RESCHED.
 * mode=local:  a pinned hard hrtimer fires on the target CPU while it
 *              polls and calls add_timer_on() for that same CPU from
 *              hardirq, which takes the local self-kick path.
 *
 * Usage (as root, from a housekeeping CPU):
 *   insmod nohz_poll_timer_test.ko cpu=<nohz_full cpu> mode=local [iters=20]
 *          [delay_ms=10] [settle_ms=50] [timeout_ms=2000]
 *   dmesg | tail
 *   rmmod nohz_poll_timer_test
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/timer.h>

static int cpu = -1;
module_param(cpu, int, 0444);
MODULE_PARM_DESC(cpu, "Target nohz_full CPU (required)");

static char *mode = "local";
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "local (enqueue from hardirq on target) or remote");

static int iters = 20;
module_param(iters, int, 0444);

static int delay_ms = 10;
module_param(delay_ms, int, 0444);
MODULE_PARM_DESC(delay_ms, "Timer expiry, relative to enqueue");

static int settle_ms = 50;
module_param(settle_ms, int, 0444);
MODULE_PARM_DESC(settle_ms, "Time for the target to go back to polling idle");

static int timeout_ms = 2000;
module_param(timeout_ms, int, 0444);
MODULE_PARM_DESC(timeout_ms, "Extra wait before declaring the timer lost");

static struct timer_list test_timer;
static struct hrtimer arm_hrtimer;
static DECLARE_COMPLETION(fired);
static long late_jiffies;
static bool armed_in_idle;

/*
 * Lateness is measured against the programmed expiry, so that a delay
 * while arming (e.g. the vCPU preempted by the host) doesn't count.
 */
static void test_timer_fn(struct timer_list *t)
{
	late_jiffies = (long)(jiffies - t->expires);
	complete(&fired);
}

static void arm_test_timer(void)
{
	test_timer.expires = jiffies + msecs_to_jiffies(delay_ms);
	add_timer_on(&test_timer, cpu);
}

/* Hardirq on the target CPU, normally interrupting polling idle */
static enum hrtimer_restart arm_hrtimer_fn(struct hrtimer *h)
{
	armed_in_idle = is_idle_task(current);
	arm_test_timer();
	return HRTIMER_NORESTART;
}

static void start_arm_hrtimer(void *unused)
{
	hrtimer_start(&arm_hrtimer, ms_to_ktime(settle_ms),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static int __init nohz_poll_timer_test_init(void)
{
	bool local = !strcmp(mode, "local");
	s64 late_us, max_late_us = 0, sum_late_us = 0;
	int i, ok = 0, lost = 0, not_idle = 0;
	unsigned long wait;

	if (!local && strcmp(mode, "remote"))
		return -EINVAL;
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	if (!tick_nohz_full_cpu(cpu))
		pr_warn("nohz_poll_timer_test: CPU %d is not nohz_full\n", cpu);
	if (!local && cpu == raw_smp_processor_id())
		pr_warn("nohz_poll_timer_test: loading from the target CPU, use taskset\n");

	timer_setup(&test_timer, test_timer_fn, TIMER_PINNED);
	hrtimer_setup(&arm_hrtimer, arm_hrtimer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);

	wait = msecs_to_jiffies(settle_ms + delay_ms + timeout_ms);

	for (i = 0; i < iters; i++) {
		reinit_completion(&fired);

		if (local) {
			armed_in_idle = false;
			smp_call_function_single(cpu, start_arm_hrtimer, NULL, 1);
		} else {
			msleep(settle_ms);
			arm_test_timer();
		}

		if (!wait_for_completion_timeout(&fired, wait)) {
			hrtimer_cancel(&arm_hrtimer);
			timer_delete_sync(&test_timer);
			lost++;
			pr_info("nohz_poll_timer_test: iter %d: LOST (not fired after %d ms)\n",
				i, delay_ms + timeout_ms);
			continue;
		}

		if (local && !armed_in_idle)
			not_idle++;

		late_us = (s64)late_jiffies * (USEC_PER_SEC / HZ);
		max_late_us = max(max_late_us, late_us);
		sum_late_us += late_us;
		ok++;
	}

	pr_info("nohz_poll_timer_test: cpu=%d mode=%s iters=%d delay_ms=%d: fired=%d lost=%d avg_late_us=%lld max_late_us=%lld\n",
		cpu, mode, iters, delay_ms, ok, lost,
		ok ? sum_late_us / ok : 0, max_late_us);
	if (not_idle)
		pr_info("nohz_poll_timer_test: %d enqueues did not happen in idle, results less meaningful\n",
			not_idle);

	return 0;
}

static void __exit nohz_poll_timer_test_exit(void)
{
	hrtimer_cancel(&arm_hrtimer);
	timer_delete_sync(&test_timer);
}

module_init(nohz_poll_timer_test_init);
module_exit(nohz_poll_timer_test_exit);
MODULE_DESCRIPTION("Timer enqueue on a nohz_full CPU in polling idle");
MODULE_LICENSE("GPL");
