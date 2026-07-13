############################################################################
# apps/testing/p2clock/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_TESTING_P2CLOCK_PROGNAME)
PRIORITY = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_DEFAULT_TASK_STACKSIZE)
MODULE = $(CONFIG_TESTING_P2CLOCK)

MAINSRC = p2clock_main.c

include $(APPDIR)/Application.mk
