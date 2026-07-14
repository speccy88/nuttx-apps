/****************************************************************************
 * apps/system/p2bank/berry_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "p2bank.h"

int main(int argc, FAR char *argv[])
{
  return p2bank_launch(true, argc, argv);
}
