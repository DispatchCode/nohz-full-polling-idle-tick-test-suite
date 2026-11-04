# SPDX-License-Identifier: GPL-2.0
# Builds the userspace tests and the module. By default the module is built
# against the running kernel; for a kernel tree that isn't installed (e.g.
# booted with vng) use: make KDIR=/path/to/built/kernel/tree
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C nohz_user_tick_test
	$(MAKE) -C nohz_dep_test
	$(MAKE) -C nohz_poll_timer_test KDIR=$(KDIR)

clean:
	$(MAKE) -C nohz_user_tick_test clean
	$(MAKE) -C nohz_dep_test clean
	$(MAKE) -C nohz_poll_timer_test KDIR=$(KDIR) clean
