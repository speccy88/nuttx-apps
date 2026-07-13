# ##############################################################################
# apps/testing/p2psram/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_TESTING_P2PSRAM)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2PSRAM_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_TESTING_P2PSRAM_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2PSRAM}
    SRCS
    p2psram_main.c)
endif()
