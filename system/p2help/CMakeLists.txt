# ##############################################################################
# apps/system/p2help/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_SYSTEM_P2HELP)
  nuttx_add_application(
    NAME
    ${CONFIG_SYSTEM_P2HELP_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_SYSTEM_P2HELP}
    SRCS
    p2help_main.c)
endif()
