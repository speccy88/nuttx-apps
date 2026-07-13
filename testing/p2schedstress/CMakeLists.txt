# ##############################################################################
# apps/testing/p2schedstress/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_TESTING_P2SCHEDSTRESS)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2SCHEDSTRESS_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2SCHEDSTRESS}
    SRCS
    p2schedstress_main.c)
endif()
