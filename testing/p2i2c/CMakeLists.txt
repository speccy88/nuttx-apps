# ##############################################################################
# apps/testing/p2i2c/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_TESTING_P2I2C)
  nuttx_add_application(
    NAME
    ${CONFIG_TESTING_P2I2C_PROGNAME}
    PRIORITY
    SCHED_PRIORITY_DEFAULT
    STACKSIZE
    ${CONFIG_DEFAULT_TASK_STACKSIZE}
    MODULE
    ${CONFIG_TESTING_P2I2C}
    SRCS
    p2i2c_main.c)
endif()
