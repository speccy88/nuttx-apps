/****************************************************************************
 * apps/testing/p2psram/p2psram_main.c
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

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/sched.h>

#include <arch/board/p2_ec32mb_psram.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2PSRAM_BUFFER_SIZE          (32 * 1024)
#define P2PSRAM_PROGRESS_SIZE        (4 * 1024 * 1024)
#define P2PSRAM_BOUNDARY_SIZE        (8 * 1024 * 1024)
#define P2PSRAM_ADDRESS_LINE_COUNT   23
#define P2PSRAM_FNV_OFFSET           UINT32_C(2166136261)
#define P2PSRAM_FNV_PRIME            UINT32_C(16777619)
#define P2PSRAM_MAX_REQUEST          (64 * 1024)
#define P2PSRAM_QPI_CLOCK_HZ         5000000
#define P2PSRAM_CE_LIMIT_CYCLES      1440
#define P2PSRAM_TICK_USEC            10000
#define P2PSRAM_TIMEOUT_TICKS        500
#define P2PSRAM_CANCEL_GRACE_TICKS   100
#define P2PSRAM_RANDOM_COUNT         1024
#define P2PSRAM_TIMEOUT_DEADLINE     1
#define P2PSRAM_TIMEOUT_MIN_USEC     24576

#if CONFIG_P2_EC32MB_PSRAM_MAX_REQUEST != P2PSRAM_MAX_REQUEST
#  error "P2 PSRAM HIL requires the exact 64-KiB request profile"
#endif

#if CONFIG_P2_EC32MB_PSRAM_TIMEOUT_TICKS != P2PSRAM_TIMEOUT_TICKS
#  error "P2 PSRAM HIL requires the exact default timeout"
#endif

#if CONFIG_P2_EC32MB_PSRAM_CANCEL_GRACE_TICKS != \
    P2PSRAM_CANCEL_GRACE_TICKS
#  error "P2 PSRAM HIL requires the exact cancellation grace period"
#endif

#if CONFIG_TESTING_P2PSRAM_RANDOM_COUNT != P2PSRAM_RANDOM_COUNT
#  error "P2 PSRAM HIL requires exactly 1024 random transfers"
#endif

#if USEC_PER_TICK != P2PSRAM_TICK_USEC
#  error "P2 PSRAM HIL timeout proof requires 10-ms scheduler ticks"
#endif

#if P2PSRAM_TIMEOUT_MIN_USEC <= P2PSRAM_TICK_USEC
#  error "P2 PSRAM timeout stimulus is not provably longer than one tick"
#endif

#if P2PSRAM_FNV_PRIME != UINT32_C(0x01000193)
#  error "P2 PSRAM FNV shift/add decomposition drifted"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct p2psram_pattern_state_s
{
  uint32_t sequence;
  uint32_t address;
  uint8_t address_byte;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_p2psram_buffer[P2PSRAM_BUFFER_SIZE];
static volatile bool g_p2psram_workload_run;
static volatile uint32_t g_p2psram_workload_count;
static sem_t g_p2psram_workload_done;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2psram_fail(FAR const char *stage, int error)
{
  if (error < 0)
    {
      error = -error;
    }

  if (error == 0)
    {
      error = EIO;
    }

  printf("P2PSRAM:FAIL:%s:%d\n", stage, error);
  return EXIT_FAILURE;
}

static int p2psram_parse_sequence(FAR const char *text,
                                  FAR uint32_t *sequence)
{
  uint32_t value = 0;
  unsigned int index;

  if (text == NULL || strlen(text) != 8)
    {
      return -EINVAL;
    }

  for (index = 0; index < 8; index++)
    {
      unsigned int digit;

      if (text[index] >= '0' && text[index] <= '9')
        {
          digit = text[index] - '0';
        }
      else if (text[index] >= 'A' && text[index] <= 'F')
        {
          digit = text[index] - 'A' + 10;
        }
      else
        {
          return -EINVAL;
        }

      value = value << 4 | digit;
    }

  *sequence = value;
  return 0;
}

static noinline_function void p2psram_pattern_init(
  FAR struct p2psram_pattern_state_s *state,
  uint32_t sequence, uint32_t address)
{
  state->sequence = sequence;
  state->address = address;
  state->address_byte = (uint8_t)(address * 37u +
                                  (address >> 8) * 17u +
                                  (address >> 16) +
                                  (address >> 24) * 0x5bu);
}

static inline_function uint8_t p2psram_pattern_next(
  FAR struct p2psram_pattern_state_s *state)
{
  uint8_t sequence_byte;
  uint8_t value;

  sequence_byte = (uint8_t)(state->sequence >>
                            ((state->address & 3u) * 8u));
  value = sequence_byte + state->address_byte;
  state->address++;
  state->address_byte += 37u;
  if ((state->address & UINT32_C(0xff)) == 0)
    {
      state->address_byte += 17u;
    }

  if ((state->address & UINT32_C(0xffff)) == 0)
    {
      state->address_byte++;
    }

  if ((state->address & UINT32_C(0xffffff)) == 0)
    {
      state->address_byte += 0x5bu;
    }

  return value;
}

static void p2psram_fill(uint32_t sequence, uint32_t address,
                         FAR uint8_t *buffer, size_t length)
{
  struct p2psram_pattern_state_s state;
  size_t index;

  p2psram_pattern_init(&state, sequence, address);
  for (index = 0; index < length; index++)
    {
      buffer[index] = p2psram_pattern_next(&state);
    }
}

static noinline_function uint32_t
p2psram_hash(FAR const uint8_t *buffer, size_t length, uint32_t hash)
{
  size_t index;

  for (index = 0; index < length; index++)
    {
      uint32_t value = hash ^ buffer[index];
      uint32_t times3;
      uint32_t times25;
      uint32_t times403;

      /* 0x01000193 = 2^24 + 403.  Optimizer barriers keep this exact
       * addition chain from being folded back into the P2 toolchain's
       * generic multiplication helper.  The barriers emit no instructions.
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

static int p2psram_io(int fd, bool write_data, uint32_t address,
                      FAR uint8_t *buffer, size_t length)
{
  size_t done = 0;

  if (lseek(fd, address, SEEK_SET) != (off_t)address)
    {
      return -errno;
    }

  while (done < length)
    {
      ssize_t ret;

      if (write_data)
        {
          ret = write(fd, &buffer[done], length - done);
        }
      else
        {
          ret = read(fd, &buffer[done], length - done);
        }

      if (ret < 0)
        {
          return -errno;
        }

      if (ret == 0)
        {
          return -EIO;
        }

      done += ret;
    }

  return 0;
}

static int p2psram_write_u32(int fd, uint32_t address, uint32_t value)
{
  uint8_t bytes[4];

  bytes[0] = value;
  bytes[1] = value >> 8;
  bytes[2] = value >> 16;
  bytes[3] = value >> 24;
  return p2psram_io(fd, true, address, bytes, sizeof(bytes));
}

static int p2psram_read_u32(int fd, uint32_t address,
                            FAR uint32_t *value)
{
  uint8_t bytes[4];
  int ret;

  ret = p2psram_io(fd, false, address, bytes, sizeof(bytes));
  if (ret >= 0)
    {
      *value = (uint32_t)bytes[0] |
               (uint32_t)bytes[1] << 8 |
               (uint32_t)bytes[2] << 16 |
               (uint32_t)bytes[3] << 24;
    }

  return ret;
}

static int p2psram_walking(int fd)
{
  unsigned int bit;

  for (bit = 0; bit < 32; bit++)
    {
      uint32_t expected = UINT32_C(1) << bit;
      uint32_t actual = 0;
      int ret;

      ret = p2psram_write_u32(fd, 0, expected);
      if (ret < 0)
        {
          return ret;
        }

      ret = p2psram_read_u32(fd, 0, &actual);
      if (ret < 0 || actual != expected)
        {
          printf("P2PSRAM:DIAG:WALKING:BIT=%u:PHASE=ONE:"
                 "EXPECTED=%08" PRIX32 ":ACTUAL=%08" PRIX32
                 ":RESULT=%d\n",
                 bit, expected, actual, ret);
          return ret < 0 ? ret : -EILSEQ;
        }

      expected = ~expected;
      ret = p2psram_write_u32(fd, 0, expected);
      if (ret < 0)
        {
          return ret;
        }

      ret = p2psram_read_u32(fd, 0, &actual);
      if (ret < 0 || actual != expected)
        {
          printf("P2PSRAM:DIAG:WALKING:BIT=%u:PHASE=ZERO:"
                 "EXPECTED=%08" PRIX32 ":ACTUAL=%08" PRIX32
                 ":RESULT=%d\n",
                 bit, expected, actual, ret);
          return ret < 0 ? ret : -EILSEQ;
        }
    }

  return 0;
}

static int p2psram_address_lines(int fd, uint32_t sequence)
{
  uint32_t base_expected = UINT32_C(0x5a000000) ^ sequence;
  uint32_t base_actual;
  unsigned int bit;
  int ret;

  ret = p2psram_write_u32(fd, 0, base_expected);
  if (ret < 0)
    {
      return ret;
    }

  for (bit = 2; bit <= 24; bit++)
    {
      uint32_t address = UINT32_C(1) << bit;
      uint32_t value = UINT32_C(0xa5000000) ^ sequence ^ address;
      ret = p2psram_write_u32(fd, address, value);

      if (ret < 0)
        {
          return ret;
        }
    }

  ret = p2psram_read_u32(fd, 0, &base_actual);
  if (ret < 0 || base_actual != base_expected)
    {
      return ret < 0 ? ret : -EILSEQ;
    }

  for (bit = 2; bit <= 24; bit++)
    {
      uint32_t address = UINT32_C(1) << bit;
      uint32_t expected = UINT32_C(0xa5000000) ^ sequence ^ address;
      uint32_t actual;
      ret = p2psram_read_u32(fd, address, &actual);

      if (ret < 0 || actual != expected)
        {
          return ret < 0 ? ret : -EILSEQ;
        }
    }

  ret = p2psram_read_u32(fd, 0, &base_actual);
  if (ret < 0 || base_actual != base_expected)
    {
      return ret < 0 ? ret : -EILSEQ;
    }

  return 0;
}

static int p2psram_boundaries(int fd, uint32_t sequence)
{
  static const uint32_t boundaries[] =
  {
    0,
    P2PSRAM_BOUNDARY_SIZE,
    P2PSRAM_BOUNDARY_SIZE * 2,
    P2PSRAM_BOUNDARY_SIZE * 3,
    P2_PSRAM_SIZE_BYTES,
  };

  uint8_t expected[16];
  unsigned int index;

  for (index = 0; index < sizeof(boundaries) / sizeof(boundaries[0]);
       index++)
    {
      uint32_t address = boundaries[index] == 0 ? 0 :
        (boundaries[index] == P2_PSRAM_SIZE_BYTES ?
         P2_PSRAM_SIZE_BYTES - sizeof(expected) : boundaries[index] - 8);
      int ret;

      p2psram_fill(sequence ^ index, address, expected, sizeof(expected));
      memcpy(g_p2psram_buffer, expected, sizeof(expected));
      ret = p2psram_io(fd, true, address, g_p2psram_buffer,
                       sizeof(expected));
      if (ret < 0)
        {
          return ret;
        }

      memset(g_p2psram_buffer, 0, sizeof(expected));
      ret = p2psram_io(fd, false, address, g_p2psram_buffer,
                       sizeof(expected));
      if (ret < 0 || memcmp(g_p2psram_buffer, expected,
                            sizeof(expected)) != 0)
        {
          return ret < 0 ? ret : -EILSEQ;
        }
    }

  return 0;
}

static uint32_t p2psram_random_next(uint32_t state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static int p2psram_random(int fd, uint32_t sequence)
{
  uint32_t state = sequence == 0 ? UINT32_C(0x7135a9c3) : sequence;
  unsigned int iteration;

  for (iteration = 0; iteration < CONFIG_TESTING_P2PSRAM_RANDOM_COUNT;
       iteration++)
    {
      uint32_t address;
      uint32_t expected;
      uint32_t actual;
      int ret;

      state = p2psram_random_next(state);
      address = state % (P2_PSRAM_SIZE_BYTES - 4);
      expected = p2psram_random_next(state ^ iteration);
      ret = p2psram_write_u32(fd, address, expected);
      if (ret < 0)
        {
          return ret;
        }

      ret = p2psram_read_u32(fd, address, &actual);
      if (ret < 0 || actual != expected)
        {
          return ret < 0 ? ret : -EILSEQ;
        }
    }

  return 0;
}

static uint32_t p2psram_bytes_per_second(uint32_t bytes, clock_t ticks)
{
  uint64_t rate;

  if (ticks <= 0)
    {
      ticks = 1;
    }

  rate = (uint64_t)bytes * TICK_PER_SEC / (uint32_t)ticks;
  return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

static int p2psram_full_coverage(int fd, uint32_t sequence,
                                 FAR uint32_t *write_rate,
                                 FAR uint32_t *read_rate,
                                 FAR uint32_t *checksum)
{
  uint32_t address;
  uint32_t write_hash = P2PSRAM_FNV_OFFSET;
  uint32_t read_hash = P2PSRAM_FNV_OFFSET;
  clock_t write_ticks = 0;
  clock_t read_ticks = 0;
  int ret;

  for (address = 0; address < P2_PSRAM_SIZE_BYTES;
       address += sizeof(g_p2psram_buffer))
    {
      clock_t start;

      p2psram_fill(sequence, address, g_p2psram_buffer,
                   sizeof(g_p2psram_buffer));
      write_hash = p2psram_hash(g_p2psram_buffer,
                                sizeof(g_p2psram_buffer), write_hash);
      start = clock_systime_ticks();
      ret = p2psram_io(fd, true, address, g_p2psram_buffer,
                       sizeof(g_p2psram_buffer));
      write_ticks += clock_systime_ticks() - start;
      if (ret < 0)
        {
          return ret;
        }

      if ((address + sizeof(g_p2psram_buffer)) %
          P2PSRAM_PROGRESS_SIZE == 0)
        {
          printf("P2PSRAM:PROGRESS:SEQUENCE=%08" PRIX32
                 ":WRITE=%" PRIu32 "\n", sequence,
                 address + (uint32_t)sizeof(g_p2psram_buffer));
        }
    }

  for (address = 0; address < P2_PSRAM_SIZE_BYTES;
       address += sizeof(g_p2psram_buffer))
    {
      struct p2psram_pattern_state_s state;
      clock_t start;
      size_t index;

      memset(g_p2psram_buffer, 0, sizeof(g_p2psram_buffer));
      start = clock_systime_ticks();
      ret = p2psram_io(fd, false, address, g_p2psram_buffer,
                       sizeof(g_p2psram_buffer));
      read_ticks += clock_systime_ticks() - start;
      if (ret < 0)
        {
          return ret;
        }

      p2psram_pattern_init(&state, sequence, address);
      for (index = 0; index < sizeof(g_p2psram_buffer); index++)
        {
          if (g_p2psram_buffer[index] !=
              p2psram_pattern_next(&state))
            {
              return -EILSEQ;
            }
        }

      read_hash = p2psram_hash(g_p2psram_buffer,
                               sizeof(g_p2psram_buffer), read_hash);
      if ((address + sizeof(g_p2psram_buffer)) %
          P2PSRAM_PROGRESS_SIZE == 0)
        {
          printf("P2PSRAM:PROGRESS:SEQUENCE=%08" PRIX32
                 ":READ=%" PRIu32 "\n", sequence,
                 address + (uint32_t)sizeof(g_p2psram_buffer));
        }
    }

  if (read_hash != write_hash)
    {
      return -EILSEQ;
    }

  *write_rate = p2psram_bytes_per_second(P2_PSRAM_SIZE_BYTES,
                                          write_ticks);
  *read_rate = p2psram_bytes_per_second(P2_PSRAM_SIZE_BYTES,
                                         read_ticks);
  *checksum = read_hash;
  return 0;
}

static int p2psram_workload_task(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);

  while (g_p2psram_workload_run)
    {
      g_p2psram_workload_count++;
      if ((g_p2psram_workload_count & 255u) == 0)
        {
          sched_yield();
        }
    }

  sem_post(&g_p2psram_workload_done);
  return EXIT_SUCCESS;
}

static int p2psram_workload_start(FAR pid_t *pid)
{
  g_p2psram_workload_count = 0;
  g_p2psram_workload_run = true;
  if (sem_init(&g_p2psram_workload_done, 0, 0) < 0)
    {
      g_p2psram_workload_run = false;
      return -errno;
    }

  *pid = task_create("p2psram-work", SCHED_PRIORITY_DEFAULT,
                     CONFIG_TESTING_P2PSRAM_WORKER_STACKSIZE,
                     p2psram_workload_task, NULL);
  if (*pid < 0)
    {
      g_p2psram_workload_run = false;
      sem_destroy(&g_p2psram_workload_done);
      return -errno;
    }

  return 0;
}

static int p2psram_workload_stop(void)
{
  int semret;

  g_p2psram_workload_run = false;
  do
    {
      semret = sem_wait(&g_p2psram_workload_done);
    }
  while (semret < 0 && errno == EINTR);

  sem_destroy(&g_p2psram_workload_done);
  if (semret < 0)
    {
      return -errno;
    }

  return 0;
}

static int p2psram_concurrent(uint32_t sequence, FAR uint32_t *work,
                              FAR uint32_t *elapsed_ticks,
                              FAR uint32_t *available_permille)
{
  uint32_t service_work;
  uint32_t service_before;
  uint32_t baseline_work;
  uint32_t baseline_before;
  clock_t start;
  clock_t elapsed;
  pid_t pid;
  ssize_t ret;
  int status;

  p2psram_fill(sequence ^ UINT32_C(0xc011c011), 0,
               g_p2psram_buffer, sizeof(g_p2psram_buffer));
  status = p2psram_workload_start(&pid);
  if (status < 0)
    {
      return status;
    }

  service_before = g_p2psram_workload_count;
  start = clock_systime_ticks();
  ret = p2_psram_transfer(P2_PSRAM_OPERATION_WRITE, 0,
                          g_p2psram_buffer,
                          sizeof(g_p2psram_buffer), 0);
  elapsed = clock_systime_ticks() - start;
  service_work = g_p2psram_workload_count - service_before;
  status = p2psram_workload_stop();
  if (status < 0)
    {
      return status;
    }

  if (ret != sizeof(g_p2psram_buffer) || service_work == 0 || elapsed <= 0)
    {
      return ret < 0 ? (int)ret : -EIO;
    }

  /* Run the same yielding workload for the same wall-clock duration with
   * no PSRAM request.  The ratio is an explicit CPU-availability measure;
   * its complement is the NuttX CPU occupancy imposed by the service call.
   */

  status = p2psram_workload_start(&pid);
  if (status < 0)
    {
      return status;
    }

  baseline_before = g_p2psram_workload_count;
  usleep(TICK2USEC(elapsed));
  baseline_work = g_p2psram_workload_count - baseline_before;
  status = p2psram_workload_stop();
  if (status < 0 || baseline_work == 0)
    {
      return status < 0 ? status : -EIO;
    }

  *work = service_work;
  *elapsed_ticks = elapsed;
  *available_permille =
    (uint32_t)((uint64_t)service_work * 1000u / baseline_work);
  if (*available_permille > 1000)
    {
      *available_permille = 1000;
    }

  if (*available_permille == 0)
    {
      return -EAGAIN;
    }

  return 0;
}

static int p2psram_timeout_recovery(uint32_t sequence)
{
  uint32_t expected = sequence ^ UINT32_C(0x5a5aa5a5);
  uint32_t actual;
  ssize_t ret;

  ret = p2_psram_transfer(P2_PSRAM_OPERATION_READ, 0,
                          g_p2psram_buffer,
                          sizeof(g_p2psram_buffer),
                          P2PSRAM_TIMEOUT_DEADLINE);
  if (ret != -ETIMEDOUT)
    {
      return ret < 0 ? (int)ret : -ETIME;
    }

  memcpy(g_p2psram_buffer, &expected, sizeof(expected));
  ret = p2_psram_transfer(P2_PSRAM_OPERATION_WRITE,
                          P2_PSRAM_SIZE_BYTES - 4,
                          g_p2psram_buffer, sizeof(expected), 0);
  if (ret != sizeof(expected))
    {
      return ret < 0 ? (int)ret : -EIO;
    }

  memset(g_p2psram_buffer, 0, sizeof(expected));
  ret = p2_psram_transfer(P2_PSRAM_OPERATION_READ,
                          P2_PSRAM_SIZE_BYTES - 4,
                          g_p2psram_buffer, sizeof(expected), 0);
  memcpy(&actual, g_p2psram_buffer, sizeof(actual));
  if (ret != sizeof(expected) || actual != expected)
    {
      return ret < 0 ? (int)ret : -EILSEQ;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct p2_psram_geometry_s geometry;
  uint32_t sequence;
  uint32_t write_rate;
  uint32_t read_rate;
  uint32_t checksum;
  uint32_t concurrent_work;
  uint32_t concurrent_ticks;
  uint32_t available_permille;
  int fd;
  int ret;

  if (argc != 2 || p2psram_parse_sequence(argv[1], &sequence) < 0)
    {
      return p2psram_fail("ARGUMENT", EINVAL);
    }

  printf("P2PSRAM:BEGIN:SEQUENCE=%08" PRIX32 "\n", sequence);
  ret = p2_psram_get_geometry(&geometry);
  if (ret < 0)
    {
      return p2psram_fail("GEOMETRY", ret);
    }

  if (geometry.size_bytes != P2_PSRAM_SIZE_BYTES ||
      geometry.chip_count != 4 || geometry.chip_size_bytes != 8388608 ||
      geometry.natural_word_bytes != 4 ||
      geometry.max_request_bytes != P2PSRAM_MAX_REQUEST ||
      geometry.qpi_clock_hz != P2PSRAM_QPI_CLOCK_HZ ||
      geometry.ce_low_limit_cycles != P2PSRAM_CE_LIMIT_CYCLES ||
      geometry.service_cog >= 8)
    {
      return p2psram_fail("GEOMETRY", EPROTO);
    }

  printf("P2PSRAM:GEOMETRY:SIZE=%" PRIu32 ":CHIPS=%" PRIu32
         ":CHIP_SIZE=%" PRIu32 ":WORD=%" PRIu32 ":MAX_REQUEST=%" PRIu32
         ":COG=%" PRIu32 "\n",
         geometry.size_bytes, geometry.chip_count,
         geometry.chip_size_bytes, geometry.natural_word_bytes,
         geometry.max_request_bytes, geometry.service_cog);
  printf("P2PSRAM:PROFILE:MAX_REQUEST=%d:QPI_HZ=%d:TICK_USEC=%d"
         ":TIMEOUT_TICKS=%d:CANCEL_GRACE_TICKS=%d\n",
         P2PSRAM_MAX_REQUEST, P2PSRAM_QPI_CLOCK_HZ, P2PSRAM_TICK_USEC,
         P2PSRAM_TIMEOUT_TICKS, P2PSRAM_CANCEL_GRACE_TICKS);

  fd = open(P2_PSRAM_DEVICE_PATH, O_RDWR);
  if (fd < 0)
    {
      return p2psram_fail("OPEN", errno);
    }

  ret = p2psram_walking(fd);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("WALKING", ret);
    }

  printf("P2PSRAM:WALKING:PASS:BITS=32\n");
  ret = p2psram_address_lines(fd, sequence);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("ADDRESS", ret);
    }

  printf("P2PSRAM:ADDRESS:PASS:LINES=%d\n", P2PSRAM_ADDRESS_LINE_COUNT);
  ret = p2psram_boundaries(fd, sequence);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("BOUNDARY", ret);
    }

  printf("P2PSRAM:BOUNDARY:PASS:COUNT=5\n");
  ret = p2psram_random(fd, sequence);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("RANDOM", ret);
    }

  printf("P2PSRAM:RANDOM:PASS:COUNT=%d\n",
         CONFIG_TESTING_P2PSRAM_RANDOM_COUNT);
  ret = p2psram_full_coverage(fd, sequence, &write_rate, &read_rate,
                              &checksum);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("FULL", ret);
    }

  printf("P2PSRAM:FULL:PASS:BYTES=%" PRIu32 ":FNV1A=%08" PRIX32 "\n",
         P2_PSRAM_SIZE_BYTES, checksum);
  printf("P2PSRAM:THROUGHPUT:WRITE_BPS=%" PRIu32
         ":READ_BPS=%" PRIu32 "\n", write_rate, read_rate);

  ret = p2psram_concurrent(sequence, &concurrent_work, &concurrent_ticks,
                            &available_permille);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("CONCURRENT", ret);
    }

  printf("P2PSRAM:CONCURRENT:PASS:WORK=%" PRIu32
         ":ELAPSED_TICKS=%" PRIu32 ":CPU_AVAILABLE_PERMILLE=%" PRIu32
         ":CPU_OCCUPANCY_PERMILLE=%" PRIu32 "\n",
         concurrent_work, concurrent_ticks, available_permille,
         1000u - available_permille);
  ret = p2psram_timeout_recovery(sequence);
  if (ret < 0)
    {
      close(fd);
      return p2psram_fail("TIMEOUT", ret);
    }

  printf("P2PSRAM:TIMEOUT:PASS:RESULT=%d:BYTES=%d:DEADLINE_TICKS=%d"
         ":MIN_WIRE_USEC=%d:TICK_USEC=%d\n",
         ETIMEDOUT, P2PSRAM_BUFFER_SIZE, P2PSRAM_TIMEOUT_DEADLINE,
         P2PSRAM_TIMEOUT_MIN_USEC, P2PSRAM_TICK_USEC);
  printf("P2PSRAM:RECOVERY:PASS\n");
  close(fd);

  ret = p2_psram_get_geometry(&geometry);
  if (ret < 0 || geometry.max_ce_low_cycles == 0 ||
      geometry.max_ce_low_cycles > geometry.ce_low_limit_cycles)
    {
      return p2psram_fail("CE-TIMING", ret < 0 ? ret : EOVERFLOW);
    }

  printf("P2PSRAM:CE_TIMING:PASS:MAX_CYCLES=%" PRIu32
         ":LIMIT_CYCLES=%" PRIu32 "\n",
         geometry.max_ce_low_cycles, geometry.ce_low_limit_cycles);
  printf("P2PSRAM:PASS:SEQUENCE=%08" PRIX32 "\n", sequence);
  return EXIT_SUCCESS;
}
