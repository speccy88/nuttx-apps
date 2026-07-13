# ##############################################################################
# apps/testing/p2clock/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_TESTING_P2CLOCK)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2CLOCK_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2CLOCK}
    SRCS
    p2clock_main.c)
endif()
