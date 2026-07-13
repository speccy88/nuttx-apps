/****************************************************************************
 * apps/testing/p2clock/p2clock_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t p2clock_counter(void)
{
  uint32_t value;

  __asm__ __volatile__("getct %0" : "=r" (value));
  return value;
}

static int p2clock_fail(FAR const char *stage, int error)
{
  if (error < 0)
    {
      error = -error;
    }

  if (error == 0)
    {
      error = EIO;
    }

  printf("P2CLOCK:FAIL:%s:ERRNO=%d\n", stage, error);
  fflush(stdout);
  return EXIT_FAILURE;
}

static int p2clock_emit_sample(uint32_t sequence)
{
  uint32_t counter = p2clock_counter();

  printf("P2CLOCK:SAMPLE:SEQ=%08" PRIX32 ":COUNTER=%08" PRIX32 "\n",
         sequence, counter);
  return fflush(stdout) == 0 ? 0 : -errno;
}

static int p2clock_read_byte(FAR uint8_t *value)
{
  ssize_t nread;

  do
    {
      nread = read(STDIN_FILENO, value, 1);
    }
  while (nread < 0 && errno == EINTR);

  if (nread == 1)
    {
      return OK;
    }

  return nread == 0 ? -EPIPE : -errno;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t sequence = 0;

  (void)argc;
  (void)argv;

  printf("P2CLOCK:READY:SYSCLK=%u:XTAL=%u:COUNTER_BITS=32\n",
         CONFIG_P2_SYSCLK_HZ, CONFIG_P2_XTAL_HZ);
  if (fflush(stdout) != 0)
    {
      return p2clock_fail("FLUSH", errno);
    }

  for (; ; )
    {
      uint8_t command;
      uint8_t terminator;
      int ret;

      ret = p2clock_read_byte(&command);
      if (ret < 0)
        {
          return p2clock_fail("READ", ret);
        }

      if (command == '\r' || command == '\n')
        {
          continue;
        }

      /* Consume the complete command frame before emitting a marker.  The
       * NuttX console echoes input, so this keeps the command byte and its
       * line ending separate from the machine-readable response.
       */

      ret = p2clock_read_byte(&terminator);
      if (ret < 0)
        {
          return p2clock_fail("FRAME", ret);
        }

      if (terminator != '\r' && terminator != '\n')
        {
          return p2clock_fail("FRAME", EPROTO);
        }

      if (command == 'S')
        {
          if (sequence == UINT32_MAX)
            {
              printf("P2CLOCK:FAIL:SEQUENCE_WRAP\n");
              fflush(stdout);
              return EXIT_FAILURE;
            }

          ret = p2clock_emit_sample(sequence);
          if (ret < 0)
            {
              return p2clock_fail("SAMPLE", ret);
            }

          sequence++;
          continue;
        }

      if (command == 'Q')
        {
          printf("P2CLOCK:DONE:SAMPLES=%08" PRIX32 "\n", sequence);
          if (fflush(stdout) != 0)
            {
              return p2clock_fail("FLUSH", errno);
            }

          return EXIT_SUCCESS;
        }

      printf("P2CLOCK:FAIL:COMMAND=%02X\n", command);
      fflush(stdout);
      return EXIT_FAILURE;
    }
}
