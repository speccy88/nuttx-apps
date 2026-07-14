# ##############################################################################
# apps/system/p2recv/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_SYSTEM_P2RECV)
  nuttx_add_application(
    NAME
    ${CONFIG_SYSTEM_P2RECV_PROGNAME}
    PRIORITY
    ${CONFIG_SYSTEM_P2RECV_PRIORITY}
    STACKSIZE
    ${CONFIG_SYSTEM_P2RECV_STACKSIZE}
    MODULE
    ${CONFIG_SYSTEM_P2RECV}
    SRCS
    p2recv_main.c)
endif()
