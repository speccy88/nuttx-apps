# ##############################################################################
# apps/examples/p2touchcal/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_EXAMPLES_P2TOUCHCAL)
  nuttx_add_application(
    NAME
    p2touchcal
    PRIORITY
    ${CONFIG_EXAMPLES_P2TOUCHCAL_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_P2TOUCHCAL_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_P2TOUCHCAL}
    SRCS
    p2touchcal.c)
endif()
