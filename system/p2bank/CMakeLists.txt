# ##############################################################################
# apps/system/p2bank/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_SYSTEM_P2BANK)
  nuttx_add_application(
    NAME p2bank
    PRIORITY ${CONFIG_SYSTEM_P2BANK_PRIORITY}
    STACKSIZE ${CONFIG_SYSTEM_P2BANK_STACKSIZE}
    SRCS p2bank.c p2bank_main.c)

  nuttx_add_application(
    NAME berry
    PRIORITY ${CONFIG_SYSTEM_P2BANK_PRIORITY}
    STACKSIZE ${CONFIG_SYSTEM_P2BANK_STACKSIZE}
    SRCS p2bank.c berry_main.c)
endif()
