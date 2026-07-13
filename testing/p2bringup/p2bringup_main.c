/****************************************************************************
 * apps/testing/p2bringup/p2bringup_main.c
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
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

#include <arch/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2BRINGUP_DATA_MAGIC       UINT32_C(0x5aa59669)
#define P2BRINGUP_HEAP_TEST_SIZE   64
#define P2BRINGUP_TIMER_STATUS     17
#define P2BRINGUP_RECREATE_STATUS  18
#define P2BRINGUP_SEM_STATUS       19

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern uint8_t _sheap[];
extern uint8_t _eheap[];

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile uint32_t g_data_magic = P2BRINGUP_DATA_MAGIC;
static volatile uint32_t g_bss_value;
static volatile uint32_t g_timer_generation;
static volatile uint32_t g_timer_started;
static volatile uint32_t g_timer_ok;
static volatile uint32_t g_sem_waiting;
static volatile uint32_t g_sem_woke;
static volatile uint32_t g_sem_post_returned;
static volatile uint32_t g_sem_preempted;

static sem_t g_done_sem;
static sem_t g_gate_sem;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2bringup_fail(FAR const char *stage)
{
  printf("P2NUTTX:FAIL:%s\n", stage);
  return EXIT_FAILURE;
}

static int p2bringup_sem_wait(FAR sem_t *sem)
{
  int ret;

  do
    {
      ret = sem_wait(sem);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

static int p2bringup_wait_task(pid_t pid, int expected)
{
  int status;
  int ret;

  ret = waitpid(pid, &status, 0);
  if (ret != pid || !WIFEXITED(status) || WEXITSTATUS(status) != expected)
    {
      return ERROR;
    }

  return OK;
}

static int p2bringup_timer_task(int argc, FAR char *argv[])
{
  clock_t start;
  clock_t elapsed;
  clock_t expected;
  uint32_t generation;
  int status;
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  generation = g_timer_generation;
  g_timer_started = generation;
  start = clock_systime_ticks();
  ret = usleep(CONFIG_TESTING_P2BRINGUP_SLEEP_MSEC * USEC_PER_MSEC);
  elapsed = clock_systime_ticks() - start;
  expected = MSEC2TICK(CONFIG_TESTING_P2BRINGUP_SLEEP_MSEC);

  if (ret == 0 && elapsed >= expected &&
      elapsed <= expected + SEC2TICK(1))
    {
      g_timer_ok = generation;
    }

  status = generation == 1 ? P2BRINGUP_TIMER_STATUS :
                             P2BRINGUP_RECREATE_STATUS;
  if (sem_post(&g_done_sem) < 0)
    {
      return EXIT_FAILURE;
    }

  return status;
}

static int p2bringup_run_timer_task(uint32_t generation, int expected_status)
{
  pid_t pid;

  g_timer_generation = generation;
  g_timer_started = 0;
  g_timer_ok = 0;

  pid = task_create("p2timer", CONFIG_TESTING_P2BRINGUP_PRIORITY,
                    CONFIG_TESTING_P2BRINGUP_STACKSIZE,
                    p2bringup_timer_task, NULL);
  if (pid < 0 || g_timer_started != generation)
    {
      return ERROR;
    }

  if (p2bringup_sem_wait(&g_done_sem) < 0 ||
      p2bringup_wait_task(pid, expected_status) < 0 ||
      g_timer_ok != generation)
    {
      return ERROR;
    }

  return OK;
}

static int p2bringup_sem_task(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);

  g_sem_waiting = 1;
  if (p2bringup_sem_wait(&g_gate_sem) < 0)
    {
      return EXIT_FAILURE;
    }

  g_sem_woke = 1;
  if (g_sem_post_returned == 0)
    {
      g_sem_preempted = 1;
    }

  return P2BRINGUP_SEM_STATUS;
}

static int p2bringup_test_semaphore(void)
{
  pid_t pid;

  g_sem_waiting = 0;
  g_sem_woke = 0;
  g_sem_post_returned = 0;
  g_sem_preempted = 0;

  pid = task_create("p2sem", CONFIG_TESTING_P2BRINGUP_PRIORITY,
                    CONFIG_TESTING_P2BRINGUP_STACKSIZE,
                    p2bringup_sem_task, NULL);
  if (pid < 0 || g_sem_waiting != 1)
    {
      return ERROR;
    }

  if (sem_post(&g_gate_sem) < 0)
    {
      return ERROR;
    }

  g_sem_post_returned = 1;
  if (p2bringup_wait_task(pid, P2BRINGUP_SEM_STATUS) < 0 ||
      g_sem_woke != 1 || g_sem_preempted != 1)
    {
      return ERROR;
    }

  return OK;
}

static int p2bringup_test_heap(void)
{
  FAR uint8_t *memory;
  uintptr_t start;
  uintptr_t end;
  uintptr_t address;
  size_t index;

  start = (uintptr_t)_sheap;
  end = (uintptr_t)_eheap;
  if (start >= end || end - start < P2BRINGUP_HEAP_TEST_SIZE)
    {
      return ERROR;
    }

  memory = malloc(P2BRINGUP_HEAP_TEST_SIZE);
  if (memory == NULL)
    {
      return ERROR;
    }

  address = (uintptr_t)memory;
  if (address < start || address > end - P2BRINGUP_HEAP_TEST_SIZE)
    {
      free(memory);
      return ERROR;
    }

  memset(memory, 0xa5, P2BRINGUP_HEAP_TEST_SIZE);
  for (index = 0; index < P2BRINGUP_HEAP_TEST_SIZE; index++)
    {
      if (memory[index] != 0xa5)
        {
          free(memory);
          return ERROR;
        }
    }

  free(memory);
  return OK;
}

static int p2bringup_test_stack(void)
{
  FAR struct tcb_s *tcb;
  struct stackinfo_s info;
  uintptr_t base;
  uintptr_t top;
  uintptr_t sp;
  size_t used;

  if (nxsched_get_stackinfo(0, &info) < 0 ||
      info.stack_base_ptr == NULL || info.adj_stack_size == 0)
    {
      return ERROR;
    }

  base = (uintptr_t)info.stack_base_ptr;
  top = base + info.adj_stack_size;
  sp = up_getsp();
  if (sp < base || sp >= top)
    {
      return ERROR;
    }

  tcb = nxsched_get_tcb(getpid());
  if (tcb == NULL)
    {
      return ERROR;
    }

  used = up_check_tcbstack(tcb, info.adj_stack_size);
  if (used == 0 || used > info.adj_stack_size)
    {
      return ERROR;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);

  printf("P2NUTTX:BOOT\n");

  if (g_data_magic != P2BRINGUP_DATA_MAGIC)
    {
      return p2bringup_fail("DATA");
    }

  printf("P2NUTTX:DATA=OK\n");

  if (g_bss_value != 0)
    {
      return p2bringup_fail("BSS");
    }

  printf("P2NUTTX:BSS=OK\n");

  if (p2bringup_test_heap() < 0)
    {
      return p2bringup_fail("HEAP");
    }

  printf("P2NUTTX:HEAP=OK\n");

  if (sem_init(&g_done_sem, 0, 0) < 0 ||
      sem_init(&g_gate_sem, 0, 0) < 0)
    {
      return p2bringup_fail("SEMINIT");
    }

  if (p2bringup_run_timer_task(1, P2BRINGUP_TIMER_STATUS) < 0)
    {
      return p2bringup_fail("TICK");
    }

  printf("P2NUTTX:TICK=OK\n");

  if (p2bringup_run_timer_task(2, P2BRINGUP_RECREATE_STATUS) < 0)
    {
      return p2bringup_fail("TASKS");
    }

  printf("P2NUTTX:TASKS=OK\n");

  if (p2bringup_test_semaphore() < 0)
    {
      return p2bringup_fail("SEMAPHORE");
    }

  printf("P2NUTTX:SEMAPHORE=OK\n");

  if (p2bringup_test_stack() < 0)
    {
      return p2bringup_fail("STACKS");
    }

  printf("P2NUTTX:STACKS=OK\n");
  printf("P2NUTTX:PASS\n");

  sem_destroy(&g_gate_sem);
  sem_destroy(&g_done_sem);
  return EXIT_SUCCESS;
}
