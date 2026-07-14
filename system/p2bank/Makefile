############################################################################
# apps/system/p2bank/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = p2bank berry
PRIORITY  = $(CONFIG_SYSTEM_P2BANK_PRIORITY) $(CONFIG_SYSTEM_P2BANK_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_P2BANK_STACKSIZE) $(CONFIG_SYSTEM_P2BANK_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_P2BANK)

CSRCS   = p2bank.c
MAINSRC = p2bank_main.c berry_main.c

include $(APPDIR)/Application.mk
