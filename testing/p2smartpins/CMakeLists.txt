# ##############################################################################
# apps/testing/p2smartpins/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_TESTING_P2SMARTPINS)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2SMARTPINS_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2SMARTPINS}
    SRCS
    p2smartpins_main.c)
endif()
