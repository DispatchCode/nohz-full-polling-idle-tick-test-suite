# nohz_full polling idle tick test suite

Tests for stopping the scheduler tick on `nohz_full` CPUs booted with
`idle=poll`.

On a `nohz_full` CPU the tick is normally kept stopped while a single task
runs. With `idle=poll`, the idle loop restarts the tick before polling, and
nothing stops it again on idle exit, so it fires at least once in userspace
after every wakeup. The patches under test:

- stop the tick when a `nohz_full` CPU enters polling idle (other CPUs keep
  restarting it as before);
- make the nohz_full kick set `TIF_NEED_RESCHED` when it is handled in idle,
  so that `cpu_idle_poll()` returns and the tick is re-evaluated. Without
  this, a timer enqueued while polling with the tick stopped is not
  programmed until something else wakes the CPU up;
- re-evaluate the tick when a stopped tick expires in polling idle, so that
  timers that were already queued get programmed in turn;
- stop the tick on return to userspace when it was left running in the
  kernel.

The suite checks that the leftover tick is gone, and that nothing that needs
the tick (timers, tick dependencies, CPU hotplug) breaks.

## Layout

| Path | Content |
|------|---------|
| `run-all.sh` | Runs every test and prints PASS/FAIL per check |
| `nohz_user_tick_test/` | Sleep/busy workload and trace analysis (T1, T4, T5) |
| `nohz_poll_timer_test/` | Kernel module arming timers on a polling CPU (T2, T5) |
| `nohz_dep_test/` | POSIX CPU timer and scheduler dependency checks (T3) |
| `Makefile` | Builds the binaries and the module |

## Requirements

- Bare metal or a guest, with at least one `nohz_full` CPU and one
  housekeeping CPU. T5 needs a second `nohz_full` CPU that nothing else uses.
- Kernel configuration: `CONFIG_NO_HZ_FULL`, `CONFIG_HIGH_RES_TIMERS`,
  `CONFIG_FTRACE` with trace events, `CONFIG_MODULES` with
  `CONFIG_MODULE_UNLOAD`, and `CONFIG_HOTPLUG_CPU` for T5. Recommended:
  `CONFIG_PROVE_LOCKING` and `CONFIG_DEBUG_PREEMPT`, whose warnings T6
  catches.
- `taskset`, `insmod`, `rmmod` and `awk`, and tracefs (mounted on
  `/sys/kernel/tracing` if it isn't).

## Building

On the machine running the kernel under test:

```sh
make
```

This builds the module against the running kernel
(`/lib/modules/$(uname -r)/build`). For a kernel tree that isn't installed,
e.g. booted with vng, point to it:

```sh
make KDIR=/path/to/linux
```

The module only loads on the kernel it was built against: rebuild it after
building each kernel. The userspace binaries don't depend on the kernel.

## Running

Boot the kernel under test with `nohz_full` and `idle=poll`, for example:

```
nohz_full=2-3 isolcpus=domain,managed_irq,2-3 idle=poll
```

On bare metal, add these to the kernel command line. With
[virtme-ng](https://github.com/arighi/virtme-ng), boot the kernel from its
source tree (without `-r`):

```sh
cd /path/to/linux
vng --user root -p 4 -a "nohz_full=2-3 isolcpus=domain,managed_irq,2-3 idle=poll"
```

Then, as root:

```sh
/path/to/suite/run-all.sh [cpu] [housekeeping_cpu] [hotplug_cpu]
```

The CPUs are taken from `/sys/devices/system/cpu/nohz_full` and
`/sys/devices/system/cpu/online` unless given:

| Argument | Default | Meaning |
|----------|---------|---------|
| `cpu` | first `nohz_full` CPU | CPU under test |
| `housekeeping_cpu` | first online CPU that is not `nohz_full` | used for T4, and to load the module and start helper tasks |
| `hotplug_cpu` | second `nohz_full` CPU | cycled offline/online in T5 (skipped if there is none) |

To test a module built elsewhere, set `KO=/path/to/nohz_poll_timer_test.ko`.

The whole run takes about 30 seconds. The script exits with status 0 only
if no check fails. The full output of every test goes to
`/tmp/nohz_suite.log`.

Run it a second time without `idle=poll` as a control: every check should
pass on any kernel, and T4 is skipped.

## Tests

| Test | What it checks | Pass criterion |
|------|----------------|----------------|
| **T1** user tick after idle | A task alternates 100us sleeps with 10ms of userspace work, 200 times. Ticks traced inside the work windows are ticks that interrupted userspace | at most 2% of the windows see a tick |
| **T2** timer remote enqueue | A `timer_list` timer is armed from the housekeeping CPU on the polling CPU | none lost, at most 50ms late |
| **T2** timer local enqueue | The timer is armed from a hardirq on the polling CPU itself, which sends a self-kick | none lost, at most 50ms late |
| **T2** timer local enqueue, 1ms | Same, with a timer due within a jiffy | none lost, at most 50ms late |
| **T2** two queued timers | Two timers armed together from the housekeeping CPU, at +10ms and +40ms. With the tick stopped only the first expiry is programmed: the second needs the tick to be re-evaluated after the first fires | none lost, at most 50ms late |
| **T3** POSIX CPU timer dependency | Sleep/work cycles with `ITIMER_PROF` armed. CPU timers are only checked from the tick, so the tick must restart whenever the task runs | at least 80% of the expected `SIGPROF` |
| **T3** scheduler dependency | Two CPU-bound tasks woken together on the idle CPU must get the tick back and share the CPU | each gets 35-65% of the CPU, with at least 5 preemptions |
| **T4** housekeeping CPU unchanged | CPUs that are not `nohz_full` keep ticking while polling (needs `idle=poll`) | at least 2 ticks per 50ms sleep |
| **T5** hotplug | 5 offline/online cycles of a `nohz_full` CPU, then T1 and T2 on it | as T1 and T2 |
| **T6** kernel warnings | No `WARNING`, `BUG`, lockdep report, hung task or RCU stall during the run | no new lines in `dmesg` |

## Expected results

With `idle=poll`:

| Kernel | T1 | T2 local | T2 two queued timers | Everything else |
|--------|----|----------|----------------------|-----------------|
| Unpatched | FAIL: a tick in almost every window (the bug) | PASS: the tick runs while polling | PASS | PASS |
| Tick stopped in polling idle, without the kick change | PASS | FAIL: timers lost or seconds late | FAIL | PASS |
| Tick stopped and kick change, without re-evaluation on stopped tick expiry | PASS | PASS | FAIL: second timer lost or seconds late | PASS |
| Full series | PASS | PASS | PASS | PASS |

## Reading the output

Each line shows the test, its result and the measured values:

```
T1 user tick after idle   PASS  user_ticks=0/200 windows (max 4), idle_ticks=0, stops irq/idle/task=0/0/0
```

- `user_ticks`: ticks that hit the userspace work windows. It must be about 0
  on `nohz_full` CPUs (T1, T5). In T4 the CPU is not `nohz_full`, so the tick
  runs while it is busy (about one tick per millisecond of work at HZ=1000):
  T4 only checks the ticks while polling (`idle_ticks`).
- `idle_ticks`: ticks outside the windows, mostly while idle.
- `stops irq/idle/task`: where the tick was stopped, from the context of each
  `tick_stop` event. `irq` means IRQ exit (the leftover tick stopping itself),
  `idle` the idle task, `task` any other task (on return to userspace).
- `avg_late_us`/`max_late_us` in T2: lateness against the programmed expiry,
  in jiffy steps. One jiffy is normal (1000us at HZ=1000): the timer wheel
  rounds the expiry up. The bug shows up as lost timers or seconds of delay.

## Notes

- In a guest the vCPUs are usually not pinned on the host, so timings are
  noisier than on bare metal. The pass criteria leave room for that.
- T5 takes a CPU offline and back online. Pass a `hotplug_cpu` that nothing
  else uses; with a single `nohz_full` CPU it is skipped.
- The tracing state is changed: events are enabled during the tests and the
  trace buffer of the CPU under test is enlarged to 8MB.
