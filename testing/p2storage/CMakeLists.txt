# ##############################################################################
# apps/testing/p2storage/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_TESTING_P2STORAGE)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2STORAGE_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_TESTING_P2STORAGE_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2STORAGE}
    SRCS
    p2storage_main.c)
endif()
