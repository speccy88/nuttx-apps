# ##############################################################################
# apps/examples/p2lvgl/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_EXAMPLES_P2LVGL)
  nuttx_add_application(
    NAME
    p2lvgl
    PRIORITY
    ${CONFIG_EXAMPLES_P2LVGL_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_P2LVGL_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_P2LVGL}
    DEPENDS
    lvgl
    SRCS
    p2lvgl.c)
endif()
