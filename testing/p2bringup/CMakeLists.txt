# ##############################################################################
# apps/testing/p2bringup/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_TESTING_P2BRINGUP)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2BRINGUP_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2BRINGUP}
    SRCS
    p2bringup_main.c)
endif()
