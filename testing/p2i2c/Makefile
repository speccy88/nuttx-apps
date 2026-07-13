############################################################################
# apps/testing/p2i2c/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME = $(CONFIG_TESTING_P2I2C_PROGNAME)
PRIORITY = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_DEFAULT_TASK_STACKSIZE)
MODULE = $(CONFIG_TESTING_P2I2C)

MAINSRC = p2i2c_main.c

include $(APPDIR)/Application.mk
