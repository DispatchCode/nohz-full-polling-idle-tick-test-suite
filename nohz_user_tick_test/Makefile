# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -O2 -Wall

nohz_user_tick: nohz_user_tick.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nohz_user_tick
