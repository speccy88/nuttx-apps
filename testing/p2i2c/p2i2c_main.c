/****************************************************************************
 * apps/testing/p2i2c/p2i2c_main.c
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

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2I2C_BUS_PATH          "/dev/i2c0"
#define P2I2C_PRESSURE_PATH     "/dev/press0"
#define P2I2C_BMP180_ADDRESS    0x77
#define P2I2C_BMP180_ID_REG     0xd0
#define P2I2C_BMP180_ID         0x55
#define P2I2C_FREQUENCY         I2C_SPEED_STANDARD
#define P2I2C_READING_COUNT     32
#define P2I2C_PRESSURE_MIN_PA   30000
#define P2I2C_PRESSURE_MAX_PA   120000
#define P2I2C_FNV_OFFSET        UINT32_C(2166136261)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2i2c_fail(FAR const char *stage, int error)
{
  if (error < 0)
    {
      error = -error;
    }

  if (error == 0)
    {
      error = EIO;
    }

  printf("P2I2C:FAIL:%s:%d\n", stage, error);
  return EXIT_FAILURE;
}

static noinline_function uint32_t
p2i2c_fnv1a(FAR const uint8_t *buffer, size_t length, uint32_t hash)
{
  size_t index;

  for (index = 0; index < length; index++)
    {
      uint32_t value = hash ^ buffer[index];
      uint32_t times3;
      uint32_t times25;
      uint32_t times403;

      /* 0x01000193 = 2^24 + 403.  The optimizer barriers keep this exact
       * addition chain from being folded back into a multiply helper.
       */

      times3 = value << 1;
      __asm__ __volatile__("" : "+r" (times3));
      times3 += value;
      __asm__ __volatile__("" : "+r" (times3));
      times25 = (times3 << 3) + value;
      __asm__ __volatile__("" : "+r" (times25));
      times403 = (times25 << 4) + times3;
      __asm__ __volatile__("" : "+r" (times403));
      hash = (value << 24) + times403;
    }

  return hash;
}

static uint32_t p2i2c_hash_pressure(uint32_t hash, uint32_t pressure)
{
  uint8_t bytes[sizeof(pressure)];

  bytes[0] = pressure;
  bytes[1] = pressure >> 8;
  bytes[2] = pressure >> 16;
  bytes[3] = pressure >> 24;
  return p2i2c_fnv1a(bytes, sizeof(bytes), hash);
}

static int p2i2c_read_id(int fd, FAR uint8_t *id)
{
  struct i2c_transfer_s transfer;
  struct i2c_msg_s messages[2];
  uint8_t reg = P2I2C_BMP180_ID_REG;
  int ret;

  messages[0].frequency = P2I2C_FREQUENCY;
  messages[0].addr = P2I2C_BMP180_ADDRESS;
  messages[0].flags = I2C_M_NOSTOP;
  messages[0].buffer = &reg;
  messages[0].length = 1;

  messages[1].frequency = P2I2C_FREQUENCY;
  messages[1].addr = P2I2C_BMP180_ADDRESS;
  messages[1].flags = I2C_M_READ;
  messages[1].buffer = id;
  messages[1].length = 1;

  transfer.msgv = messages;
  transfer.msgc = 2;
  ret = ioctl(fd, I2CIOC_TRANSFER,
              (unsigned long)((uintptr_t)&transfer));
  if (ret < 0)
    {
      return -errno;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t minimum = UINT32_MAX;
  uint32_t maximum = 0;
  uint32_t hash = P2I2C_FNV_OFFSET;
  uint8_t id = 0;
  unsigned int index;
  int pressure_fd;
  int i2c_fd;
  int ret;

  (void)argc;
  (void)argv;

  printf("P2I2C:START:BUS=/dev/i2c0:SDA=24:SCL=25:"
         "ADDR=0x77:FREQ=100000\n");

  i2c_fd = open(P2I2C_BUS_PATH, O_RDONLY);
  if (i2c_fd < 0)
    {
      return p2i2c_fail("i2c-open", errno);
    }

  ret = p2i2c_read_id(i2c_fd, &id);
  if (ret < 0)
    {
      close(i2c_fd);
      return p2i2c_fail("id-transfer", ret);
    }

  if (close(i2c_fd) < 0)
    {
      return p2i2c_fail("i2c-close", errno);
    }

  if (id != P2I2C_BMP180_ID)
    {
      return p2i2c_fail("id-value", ENODEV);
    }

  printf("P2I2C:ID=0x55:REGISTER=0xD0:"
         "TRANSFER=WRITE_RESTART_READ\n");

  pressure_fd = open(P2I2C_PRESSURE_PATH, O_RDONLY);
  if (pressure_fd < 0)
    {
      return p2i2c_fail("pressure-open", errno);
    }

  for (index = 0; index < P2I2C_READING_COUNT; index++)
    {
      uint32_t pressure;
      ssize_t nread;

      nread = read(pressure_fd, &pressure, sizeof(pressure));
      if (nread < 0)
        {
          ret = -errno;
          close(pressure_fd);
          return p2i2c_fail("pressure-read", ret);
        }

      if (nread != sizeof(pressure))
        {
          close(pressure_fd);
          return p2i2c_fail("pressure-size", EIO);
        }

      if (pressure < P2I2C_PRESSURE_MIN_PA ||
          pressure > P2I2C_PRESSURE_MAX_PA)
        {
          close(pressure_fd);
          return p2i2c_fail("pressure-range", ERANGE);
        }

      if (pressure < minimum)
        {
          minimum = pressure;
        }

      if (pressure > maximum)
        {
          maximum = pressure;
        }

      hash = p2i2c_hash_pressure(hash, pressure);
    }

  if (close(pressure_fd) < 0)
    {
      return p2i2c_fail("pressure-close", errno);
    }

  printf("P2I2C:READINGS=32:MIN=%" PRIu32 ":MAX=%" PRIu32
         ":FNV1A=%08" PRIX32 "\n", minimum, maximum, hash);
  printf("P2I2C:PASS\n");
  return EXIT_SUCCESS;
}
