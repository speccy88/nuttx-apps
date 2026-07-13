/****************************************************************************
 * apps/testing/p2schedstress/p2schedstress_main.c
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
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/sched.h>

#include <arch/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2SCHED_PRIORITY_EVENTS        2000u
#define P2SCHED_RR_EVENTS              100000u
#define P2SCHED_SEMAPHORE_EVENTS       600000u
#define P2SCHED_PI_EVENTS              2000u
#define P2SCHED_CONDITION_EVENTS       100000u
#define P2SCHED_MQUEUE_EVENTS          100000u
#define P2SCHED_SIGNAL_EVENTS          100000u
#define P2SCHED_TIMER_EVENTS           10u
#define P2SCHED_PTHREAD_EVENTS         4u
#define P2SCHED_TASK_EVENTS            64u

#define P2SCHED_TOTAL_EVENTS \
  (P2SCHED_PRIORITY_EVENTS + P2SCHED_RR_EVENTS + \
   P2SCHED_SEMAPHORE_EVENTS + P2SCHED_PI_EVENTS + \
   P2SCHED_CONDITION_EVENTS + P2SCHED_MQUEUE_EVENTS + \
   P2SCHED_SIGNAL_EVENTS + P2SCHED_TIMER_EVENTS + \
   P2SCHED_PTHREAD_EVENTS + P2SCHED_TASK_EVENTS)

#define P2SCHED_RR_EVENTS_PER_THREAD   (P2SCHED_RR_EVENTS / 2u)
#define P2SCHED_SEM_EVENTS_PER_THREAD  (P2SCHED_SEMAPHORE_EVENTS / 2u)
#define P2SCHED_HIGH_PRIORITY \
  CONFIG_TESTING_P2SCHEDSTRESS_PRIORITY
#define P2SCHED_LOW_PRIORITY           (CONFIG_INIT_PRIORITY - 10)
#define P2SCHED_TASK_STATUS            37
#define P2SCHED_HEAP_BYTES             4096u
#define P2SCHED_HEAP_PATTERN           0xa5
#define P2SCHED_HEAP_CONCURRENCY_THREADS 2u
#define P2SCHED_HEAP_CONCURRENCY_ROUNDS  256u
#define P2SCHED_HEAP_CONCURRENCY_EVENTS \
  (P2SCHED_HEAP_CONCURRENCY_THREADS * P2SCHED_HEAP_CONCURRENCY_ROUNDS)
#define P2SCHED_HEAP_CONCURRENCY_BYTES 256u
#define P2SCHED_MQUEUE_NAME            "p2schedstress"
#define P2SCHED_TIMER_VALUE            0x25
#define P2SCHED_TIMER_NSEC             (CONFIG_USEC_PER_TICK * 1000L)
#define P2SCHED_PTHREAD_RESULT         ((FAR void *)(uintptr_t)0x12345678)

#if P2SCHED_TOTAL_EVENTS < 1000000u
#  error "P2 scheduler stress must account for at least one million events"
#endif

#if (P2SCHED_RR_EVENTS & 1u) != 0 || \
    (P2SCHED_SEMAPHORE_EVENTS & 1u) != 0
#  error "two-worker event targets must be even"
#endif

#if P2SCHED_HIGH_PRIORITY <= CONFIG_INIT_PRIORITY
#  error "high worker priority must exceed the init priority"
#endif

#if CONFIG_INIT_PRIORITY <= 10
#  error "init priority must leave room for a lower-priority worker"
#endif

#if CONFIG_USEC_PER_TICK >= 1000000
#  error "timer test requires a sub-second system tick"
#endif

#ifdef CONFIG_SMP
#  error "P2 scheduler stress is deliberately a flat-UP target"
#endif

#if CONFIG_RAM_SIZE > 524288
#  error "P2 scheduler stress must remain within the 512-KiB Hub image"
#endif

#if P2SCHED_HEAP_CONCURRENCY_EVENTS != 512u
#  error "concurrent heap test must complete exactly 512 allocations"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct p2sched_message_s
{
  uint32_t sequence;
  uint32_t inverse;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern uint8_t _sheap[];
extern uint8_t _eheap[];

/****************************************************************************
 * Private Data
 ****************************************************************************/

static sem_t g_priority_start;
static sem_t g_priority_done;
static volatile uint32_t g_priority_count;
static volatile uint32_t g_priority_post_returned;
static volatile int g_priority_error;

static volatile uint32_t g_rr_turn;
static volatile uint32_t g_rr_count[2];
static volatile bool g_rr_abort;
static volatile int g_rr_error;

static sem_t g_sem_baton[2];
static volatile uint32_t g_sem_count[2];
static volatile int g_sem_error;

static pthread_mutex_t g_pi_mutex;
static sem_t g_pi_low_go;
static sem_t g_pi_high_go;
static sem_t g_pi_locked;
static sem_t g_pi_attempting;
static sem_t g_pi_release;
static sem_t g_pi_low_done;
static sem_t g_pi_high_done;
static volatile uint32_t g_pi_count;
static volatile int g_pi_error;

static pthread_mutex_t g_condition_mutex;
static pthread_cond_t g_condition;
static sem_t g_condition_ready;
static volatile uint32_t g_condition_data;
static volatile uint32_t g_condition_count;
static volatile int g_condition_error;

static mqd_t g_mqueue;
static sem_t g_mqueue_ready;
static volatile uint32_t g_mqueue_receive_count;
static volatile uint32_t g_mqueue_send_count;
static volatile int g_mqueue_error;

static sem_t g_signal_ready;
static pthread_t g_signal_receiver;
static sigset_t g_signal_set;
static volatile uint32_t g_signal_receive_count;
static volatile uint32_t g_signal_send_count;
static volatile int g_signal_error;

static sem_t g_pthread_started;
static sem_t g_pthread_release;
static sem_t g_pthread_cancel_wait;

static volatile uint32_t g_task_generation;
static volatile uint32_t g_task_seen;

static pthread_barrier_t g_heap_concurrency_barrier;
static FAR uint8_t * volatile g_heap_concurrency_memory[2];
static volatile uint32_t g_heap_concurrency_count[2];
static volatile int g_heap_concurrency_error;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2sched_wait(FAR sem_t *sem)
{
  int ret;

  do
    {
      ret = sem_wait(sem);
    }
  while (ret < 0 && errno == EINTR);

  return ret;
}

static int p2sched_sem_init(FAR sem_t *sem, unsigned int value)
{
  if (sem_init(sem, 0, value) < 0)
    {
      return -errno;
    }

  if (sem_setprotocol(sem, SEM_PRIO_NONE) < 0)
    {
      int error = errno;

      sem_destroy(sem);
      return -error;
    }

  return OK;
}

static int p2sched_attr_init(FAR pthread_attr_t *attr, int priority,
                             int policy)
{
  struct sched_param param;
  int ret;

  ret = pthread_attr_init(attr);
  if (ret != 0)
    {
      return -ret;
    }

  ret = pthread_attr_setstacksize(
    attr, CONFIG_TESTING_P2SCHEDSTRESS_STACKSIZE);
  if (ret == 0)
    {
      ret = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setschedpolicy(attr, policy);
    }

  if (ret == 0)
    {
      param.sched_priority = priority;
      ret = pthread_attr_setschedparam(attr, &param);
    }

  if (ret != 0)
    {
      pthread_attr_destroy(attr);
      return -ret;
    }

  return OK;
}

static int p2sched_check_thread_policy(int expected_policy,
                                       int expected_priority)
{
  struct sched_param param;
  int policy;
  int ret;

  ret = pthread_getschedparam(pthread_self(), &policy, &param);
  if (ret != 0)
    {
      return -ret;
    }

  if (policy != expected_policy ||
      param.sched_priority != expected_priority)
    {
      return -EINVAL;
    }

  return OK;
}

static int p2sched_fail(FAR const char *stage, int error)
{
  printf("P2SCHED:FAIL:%s:CODE=%d\n", stage, error);
  return EXIT_FAILURE;
}

/****************************************************************************
 * Priority preemption
 ****************************************************************************/

static FAR void *p2sched_priority_worker(FAR void *arg)
{
  uint32_t index;
  int ret;

  UNUSED(arg);

  ret = p2sched_check_thread_policy(SCHED_FIFO, P2SCHED_HIGH_PRIORITY);
  if (ret < 0)
    {
      g_priority_error = ret;
    }

  for (index = 0; index < P2SCHED_PRIORITY_EVENTS; index++)
    {
      if (p2sched_wait(&g_priority_start) < 0)
        {
          g_priority_error = -errno;
          return NULL;
        }

      if (g_priority_post_returned != 0)
        {
          g_priority_error = -EAGAIN;
        }
      else
        {
          g_priority_count++;
        }

      if (sem_post(&g_priority_done) < 0)
        {
          g_priority_error = -errno;
          return NULL;
        }
    }

  return NULL;
}

static int p2sched_test_priority(FAR uint32_t *events)
{
  pthread_attr_t attr;
  pthread_t worker;
  uint32_t index;
  int ret;

  g_priority_count = 0;
  g_priority_post_returned = 0;
  g_priority_error = 0;

  ret = p2sched_sem_init(&g_priority_start, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2sched_sem_init(&g_priority_done, 0);
  if (ret < 0)
    {
      sem_destroy(&g_priority_start);
      return ret;
    }

  ret = p2sched_attr_init(&attr, P2SCHED_HIGH_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_sems;
    }

  ret = pthread_create(&worker, &attr, p2sched_priority_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_sems;
    }

  for (index = 0; index < P2SCHED_PRIORITY_EVENTS; index++)
    {
      g_priority_post_returned = 0;
      if (sem_post(&g_priority_start) < 0)
        {
          g_priority_error = -errno;
        }

      g_priority_post_returned = 1;
      if (p2sched_wait(&g_priority_done) < 0)
        {
          g_priority_error = -errno;
          break;
        }
    }

  ret = pthread_join(worker, NULL);
  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_priority_error != 0)
    {
      ret = g_priority_error;
    }
  else if (g_priority_count != P2SCHED_PRIORITY_EVENTS)
    {
      ret = -EIO;
    }
  else
    {
      *events = g_priority_count;
      ret = OK;
    }

out_with_sems:
  sem_destroy(&g_priority_done);
  sem_destroy(&g_priority_start);
  return ret;
}

/****************************************************************************
 * Round-robin baton
 ****************************************************************************/

static FAR void *p2sched_rr_worker(FAR void *arg)
{
  uintptr_t id = (uintptr_t)arg;
  uint32_t index;
  int ret;

  ret = p2sched_check_thread_policy(SCHED_RR, P2SCHED_HIGH_PRIORITY);
  if (ret < 0)
    {
      g_rr_error = ret;
      g_rr_abort = true;
      return NULL;
    }

  for (index = 0; index < P2SCHED_RR_EVENTS_PER_THREAD; index++)
    {
      while (g_rr_turn != id)
        {
          if (g_rr_abort)
            {
              return NULL;
            }

          sched_yield();
        }

      g_rr_count[id]++;
      g_rr_turn = id ^ 1u;

      /* A completed turn is not counted until this RR thread explicitly
       * yields the run queue to its equal-priority peer.
       */

      sched_yield();
    }

  return NULL;
}

static int p2sched_test_round_robin(FAR uint32_t *events)
{
  pthread_attr_t attr;
  pthread_t worker[2];
  bool first_created = false;
  int ret;

  g_rr_turn = 0;
  g_rr_count[0] = 0;
  g_rr_count[1] = 0;
  g_rr_abort = false;
  g_rr_error = 0;

  ret = p2sched_attr_init(&attr, P2SCHED_HIGH_PRIORITY, SCHED_RR);
  if (ret < 0)
    {
      return ret;
    }

  sched_lock();
  ret = pthread_create(&worker[0], &attr, p2sched_rr_worker,
                       (FAR void *)(uintptr_t)0);
  if (ret == 0)
    {
      first_created = true;
      ret = pthread_create(&worker[1], &attr, p2sched_rr_worker,
                           (FAR void *)(uintptr_t)1);
    }

  if (ret != 0)
    {
      g_rr_abort = true;
    }

  sched_unlock();
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      if (first_created)
        {
          pthread_join(worker[0], NULL);
        }

      return -ret;
    }

  ret = pthread_join(worker[0], NULL);
  if (ret == 0)
    {
      ret = pthread_join(worker[1], NULL);
    }

  if (ret != 0)
    {
      return -ret;
    }

  if (g_rr_error != 0)
    {
      return g_rr_error;
    }

  *events = g_rr_count[0] + g_rr_count[1];
  return *events == P2SCHED_RR_EVENTS ? OK : -EIO;
}

/****************************************************************************
 * Semaphore baton
 ****************************************************************************/

static FAR void *p2sched_sem_worker(FAR void *arg)
{
  uintptr_t id = (uintptr_t)arg;
  uint32_t index;

  for (index = 0; index < P2SCHED_SEM_EVENTS_PER_THREAD; index++)
    {
      if (p2sched_wait(&g_sem_baton[id]) < 0)
        {
          g_sem_error = -errno;
          return NULL;
        }

      g_sem_count[id]++;
      if (sem_post(&g_sem_baton[id ^ 1u]) < 0)
        {
          g_sem_error = -errno;
          return NULL;
        }
    }

  return NULL;
}

static int p2sched_test_semaphore(FAR uint32_t *events)
{
  pthread_attr_t attr;
  pthread_t worker[2];
  bool first_created = false;
  int ret;

  g_sem_count[0] = 0;
  g_sem_count[1] = 0;
  g_sem_error = 0;

  ret = p2sched_sem_init(&g_sem_baton[0], 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2sched_sem_init(&g_sem_baton[1], 0);
  if (ret < 0)
    {
      sem_destroy(&g_sem_baton[0]);
      return ret;
    }

  ret = p2sched_attr_init(&attr, P2SCHED_HIGH_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_sems;
    }

  sched_lock();
  ret = pthread_create(&worker[0], &attr, p2sched_sem_worker,
                       (FAR void *)(uintptr_t)0);
  if (ret == 0)
    {
      first_created = true;
      ret = pthread_create(&worker[1], &attr, p2sched_sem_worker,
                           (FAR void *)(uintptr_t)1);
    }

  if (ret == 0 && sem_post(&g_sem_baton[0]) < 0)
    {
      ret = errno;
    }

  if (ret != 0)
    {
      g_sem_error = -ret;
      sem_post(&g_sem_baton[0]);
      sem_post(&g_sem_baton[1]);
    }

  sched_unlock();
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      if (first_created)
        {
          pthread_cancel(worker[0]);
          pthread_join(worker[0], NULL);
        }

      ret = -ret;
      goto out_with_sems;
    }

  ret = pthread_join(worker[0], NULL);
  if (ret == 0)
    {
      ret = pthread_join(worker[1], NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_sem_error != 0)
    {
      ret = g_sem_error;
    }
  else
    {
      *events = g_sem_count[0] + g_sem_count[1];
      ret = *events == P2SCHED_SEMAPHORE_EVENTS ? OK : -EIO;
    }

out_with_sems:
  sem_destroy(&g_sem_baton[1]);
  sem_destroy(&g_sem_baton[0]);
  return ret;
}

/****************************************************************************
 * Priority-inheritance mutex transfer
 ****************************************************************************/

static FAR void *p2sched_pi_low_worker(FAR void *arg)
{
  struct sched_param param;
  uint32_t index;
  int policy;
  int ret;

  UNUSED(arg);

  ret = p2sched_check_thread_policy(SCHED_FIFO, P2SCHED_LOW_PRIORITY);
  if (ret < 0)
    {
      g_pi_error = ret;
    }

  for (index = 0; index < P2SCHED_PI_EVENTS; index++)
    {
      if (p2sched_wait(&g_pi_low_go) < 0 ||
          pthread_mutex_lock(&g_pi_mutex) != 0)
        {
          g_pi_error = -EIO;
          return NULL;
        }

      if (sem_post(&g_pi_locked) < 0 ||
          p2sched_wait(&g_pi_release) < 0)
        {
          g_pi_error = -EIO;
          return NULL;
        }

      ret = pthread_getschedparam(pthread_self(), &policy, &param);
      if (ret != 0 || param.sched_priority != P2SCHED_HIGH_PRIORITY)
        {
          g_pi_error = ret == 0 ? -EAGAIN : -ret;
        }

      ret = pthread_mutex_unlock(&g_pi_mutex);
      if (ret != 0)
        {
          g_pi_error = -ret;
          return NULL;
        }

      ret = pthread_getschedparam(pthread_self(), &policy, &param);
      if (ret != 0 || param.sched_priority != P2SCHED_LOW_PRIORITY)
        {
          g_pi_error = ret == 0 ? -EAGAIN : -ret;
        }

      if (sem_post(&g_pi_low_done) < 0)
        {
          g_pi_error = -errno;
          return NULL;
        }
    }

  return NULL;
}

static FAR void *p2sched_pi_high_worker(FAR void *arg)
{
  uint32_t index;
  int ret;

  UNUSED(arg);

  ret = p2sched_check_thread_policy(SCHED_FIFO, P2SCHED_HIGH_PRIORITY);
  if (ret < 0)
    {
      g_pi_error = ret;
    }

  for (index = 0; index < P2SCHED_PI_EVENTS; index++)
    {
      if (p2sched_wait(&g_pi_high_go) < 0 ||
          sem_post(&g_pi_attempting) < 0)
        {
          g_pi_error = -EIO;
          return NULL;
        }

      ret = pthread_mutex_lock(&g_pi_mutex);
      if (ret != 0)
        {
          g_pi_error = -ret;
          return NULL;
        }

      g_pi_count++;
      ret = pthread_mutex_unlock(&g_pi_mutex);
      if (ret != 0 || sem_post(&g_pi_high_done) < 0)
        {
          g_pi_error = ret != 0 ? -ret : -errno;
          return NULL;
        }
    }

  return NULL;
}

static int p2sched_pi_init_sems(void)
{
  FAR sem_t *sems[] =
  {
    &g_pi_low_go, &g_pi_high_go, &g_pi_locked, &g_pi_attempting,
    &g_pi_release, &g_pi_low_done, &g_pi_high_done
  };

  unsigned int index;
  int ret;

  for (index = 0; index < nitems(sems); index++)
    {
      ret = p2sched_sem_init(sems[index], 0);
      if (ret < 0)
        {
          while (index > 0)
            {
              index--;
              sem_destroy(sems[index]);
            }

          return ret;
        }
    }

  return OK;
}

static void p2sched_pi_destroy_sems(void)
{
  sem_destroy(&g_pi_high_done);
  sem_destroy(&g_pi_low_done);
  sem_destroy(&g_pi_release);
  sem_destroy(&g_pi_attempting);
  sem_destroy(&g_pi_locked);
  sem_destroy(&g_pi_high_go);
  sem_destroy(&g_pi_low_go);
}

static int p2sched_test_pi_mutex(FAR uint32_t *events)
{
  pthread_mutexattr_t mutex_attr;
  pthread_attr_t low_attr;
  pthread_attr_t high_attr;
  pthread_t low;
  pthread_t high;
  uint32_t index;
  int ret;

  g_pi_count = 0;
  g_pi_error = 0;

  ret = p2sched_pi_init_sems();
  if (ret < 0)
    {
      return ret;
    }

  ret = pthread_mutexattr_init(&mutex_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_sems;
    }

  ret = pthread_mutexattr_setprotocol(&mutex_attr, PTHREAD_PRIO_INHERIT);
  if (ret == 0)
    {
      ret = pthread_mutex_init(&g_pi_mutex, &mutex_attr);
    }

  pthread_mutexattr_destroy(&mutex_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_sems;
    }

  ret = p2sched_attr_init(&low_attr, P2SCHED_LOW_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_mutex;
    }

  ret = p2sched_attr_init(&high_attr, P2SCHED_HIGH_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      pthread_attr_destroy(&low_attr);
      goto out_with_mutex;
    }

  sched_lock();
  ret = pthread_create(&low, &low_attr, p2sched_pi_low_worker, NULL);
  if (ret == 0)
    {
      ret = pthread_create(&high, &high_attr, p2sched_pi_high_worker, NULL);
    }

  sched_unlock();
  pthread_attr_destroy(&high_attr);
  pthread_attr_destroy(&low_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_mutex;
    }

  for (index = 0; index < P2SCHED_PI_EVENTS; index++)
    {
      if (sem_post(&g_pi_low_go) < 0 ||
          p2sched_wait(&g_pi_locked) < 0 ||
          sem_post(&g_pi_high_go) < 0 ||
          p2sched_wait(&g_pi_attempting) < 0 ||
          sem_post(&g_pi_release) < 0 ||
          p2sched_wait(&g_pi_high_done) < 0 ||
          p2sched_wait(&g_pi_low_done) < 0)
        {
          g_pi_error = -EIO;
          break;
        }
    }

  ret = pthread_join(high, NULL);
  if (ret == 0)
    {
      ret = pthread_join(low, NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_pi_error != 0)
    {
      ret = g_pi_error;
    }
  else if (g_pi_count != P2SCHED_PI_EVENTS)
    {
      ret = -EIO;
    }
  else
    {
      *events = g_pi_count;
      ret = OK;
    }

out_with_mutex:
  pthread_mutex_destroy(&g_pi_mutex);
out_with_sems:
  p2sched_pi_destroy_sems();
  return ret;
}

/****************************************************************************
 * Condition-variable handoff
 ****************************************************************************/

static FAR void *p2sched_condition_waiter(FAR void *arg)
{
  uint32_t index;
  int ret;

  UNUSED(arg);

  ret = pthread_mutex_lock(&g_condition_mutex);
  if (ret != 0)
    {
      g_condition_error = -ret;
      return NULL;
    }

  for (index = 0; index < P2SCHED_CONDITION_EVENTS; index++)
    {
      if (sem_post(&g_condition_ready) < 0)
        {
          g_condition_error = -errno;
          break;
        }

      while (g_condition_data == 0)
        {
          ret = pthread_cond_wait(&g_condition, &g_condition_mutex);
          if (ret != 0)
            {
              g_condition_error = -ret;
              break;
            }
        }

      if (g_condition_error != 0)
        {
          break;
        }

      if (g_condition_data != index + 1u)
        {
          g_condition_error = -EILSEQ;
        }
      else
        {
          g_condition_count++;
        }

      g_condition_data = 0;
    }

  ret = pthread_mutex_unlock(&g_condition_mutex);
  if (ret != 0)
    {
      g_condition_error = -ret;
    }

  return NULL;
}

static FAR void *p2sched_condition_signaler(FAR void *arg)
{
  uint32_t index;
  int ret;

  UNUSED(arg);

  for (index = 0; index < P2SCHED_CONDITION_EVENTS; index++)
    {
      if (p2sched_wait(&g_condition_ready) < 0)
        {
          g_condition_error = -errno;
          return NULL;
        }

      ret = pthread_mutex_lock(&g_condition_mutex);
      if (ret != 0)
        {
          g_condition_error = -ret;
          return NULL;
        }

      if (g_condition_data != 0)
        {
          g_condition_error = -EAGAIN;
        }

      g_condition_data = index + 1u;
      ret = pthread_cond_signal(&g_condition);
      if (ret == 0)
        {
          ret = pthread_mutex_unlock(&g_condition_mutex);
        }

      if (ret != 0)
        {
          g_condition_error = -ret;
          return NULL;
        }
    }

  return NULL;
}

static int p2sched_test_condition(FAR uint32_t *events)
{
  pthread_attr_t waiter_attr;
  pthread_attr_t signaler_attr;
  pthread_t waiter;
  pthread_t signaler;
  int ret;

  g_condition_data = 0;
  g_condition_count = 0;
  g_condition_error = 0;

  ret = p2sched_sem_init(&g_condition_ready, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = pthread_mutex_init(&g_condition_mutex, NULL);
  if (ret == 0)
    {
      ret = pthread_cond_init(&g_condition, NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
      goto out_with_sem;
    }

  ret = p2sched_attr_init(&waiter_attr, P2SCHED_HIGH_PRIORITY,
                          SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_sync;
    }

  ret = p2sched_attr_init(&signaler_attr, P2SCHED_LOW_PRIORITY,
                          SCHED_FIFO);
  if (ret < 0)
    {
      pthread_attr_destroy(&waiter_attr);
      goto out_with_sync;
    }

  ret = pthread_create(&waiter, &waiter_attr,
                       p2sched_condition_waiter, NULL);
  if (ret == 0)
    {
      ret = pthread_create(&signaler, &signaler_attr,
                           p2sched_condition_signaler, NULL);
    }

  pthread_attr_destroy(&signaler_attr);
  pthread_attr_destroy(&waiter_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_sync;
    }

  ret = pthread_join(waiter, NULL);
  if (ret == 0)
    {
      ret = pthread_join(signaler, NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_condition_error != 0)
    {
      ret = g_condition_error;
    }
  else if (g_condition_count != P2SCHED_CONDITION_EVENTS)
    {
      ret = -EIO;
    }
  else
    {
      *events = g_condition_count;
      ret = OK;
    }

out_with_sync:
  pthread_cond_destroy(&g_condition);
  pthread_mutex_destroy(&g_condition_mutex);
out_with_sem:
  sem_destroy(&g_condition_ready);
  return ret;
}

/****************************************************************************
 * POSIX message-queue handoff
 ****************************************************************************/

static FAR void *p2sched_mqueue_receiver(FAR void *arg)
{
  struct p2sched_message_s message;
  unsigned int priority;
  uint32_t index;
  ssize_t size;

  UNUSED(arg);

  if (sem_post(&g_mqueue_ready) < 0)
    {
      g_mqueue_error = -errno;
      return NULL;
    }

  for (index = 0; index < P2SCHED_MQUEUE_EVENTS; index++)
    {
      do
        {
          size = mq_receive(g_mqueue, (FAR char *)&message,
                            sizeof(message), &priority);
        }
      while (size < 0 && errno == EINTR);

      if (size != (ssize_t)sizeof(message) || message.sequence != index ||
          message.inverse != ~index)
        {
          g_mqueue_error = -EILSEQ;
          return NULL;
        }

      g_mqueue_receive_count++;
    }

  return NULL;
}

static FAR void *p2sched_mqueue_sender(FAR void *arg)
{
  struct p2sched_message_s message;
  uint32_t index;
  int ret;

  UNUSED(arg);

  for (index = 0; index < P2SCHED_MQUEUE_EVENTS; index++)
    {
      message.sequence = index;
      message.inverse = ~index;

      do
        {
          ret = mq_send(g_mqueue, (FAR const char *)&message,
                        sizeof(message), 0);
        }
      while (ret < 0 && errno == EINTR);

      if (ret < 0)
        {
          g_mqueue_error = -errno;
          return NULL;
        }

      g_mqueue_send_count++;
    }

  return NULL;
}

static int p2sched_test_mqueue(FAR uint32_t *events)
{
  struct mq_attr mq_attr;
  pthread_attr_t receiver_attr;
  pthread_attr_t sender_attr;
  pthread_t receiver;
  pthread_t sender;
  int ret;

  g_mqueue_receive_count = 0;
  g_mqueue_send_count = 0;
  g_mqueue_error = 0;

  ret = p2sched_sem_init(&g_mqueue_ready, 0);
  if (ret < 0)
    {
      return ret;
    }

  mq_unlink(P2SCHED_MQUEUE_NAME);
  memset(&mq_attr, 0, sizeof(mq_attr));
  mq_attr.mq_maxmsg = 1;
  mq_attr.mq_msgsize = sizeof(struct p2sched_message_s);
  g_mqueue = mq_open(P2SCHED_MQUEUE_NAME, O_CREAT | O_RDWR, 0666,
                     &mq_attr);
  if (g_mqueue == (mqd_t)-1)
    {
      ret = -errno;
      goto out_with_sem;
    }

  ret = p2sched_attr_init(&receiver_attr, P2SCHED_HIGH_PRIORITY,
                          SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_queue;
    }

  ret = p2sched_attr_init(&sender_attr, P2SCHED_LOW_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      pthread_attr_destroy(&receiver_attr);
      goto out_with_queue;
    }

  ret = pthread_create(&receiver, &receiver_attr,
                       p2sched_mqueue_receiver, NULL);
  pthread_attr_destroy(&receiver_attr);
  if (ret == 0 && p2sched_wait(&g_mqueue_ready) < 0)
    {
      ret = errno;
    }

  if (ret == 0)
    {
      ret = pthread_create(&sender, &sender_attr,
                           p2sched_mqueue_sender, NULL);
    }

  pthread_attr_destroy(&sender_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_queue;
    }

  ret = pthread_join(receiver, NULL);
  if (ret == 0)
    {
      ret = pthread_join(sender, NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_mqueue_error != 0)
    {
      ret = g_mqueue_error;
    }
  else if (g_mqueue_receive_count != P2SCHED_MQUEUE_EVENTS ||
           g_mqueue_send_count != P2SCHED_MQUEUE_EVENTS)
    {
      ret = -EIO;
    }
  else
    {
      *events = g_mqueue_receive_count;
      ret = OK;
    }

out_with_queue:
  mq_close(g_mqueue);
  mq_unlink(P2SCHED_MQUEUE_NAME);
out_with_sem:
  sem_destroy(&g_mqueue_ready);
  return ret;
}

/****************************************************************************
 * Signal handoff
 ****************************************************************************/

static FAR void *p2sched_signal_receiver(FAR void *arg)
{
  siginfo_t info;
  uint32_t index;
  int signo;

  UNUSED(arg);

  if (sem_post(&g_signal_ready) < 0)
    {
      g_signal_error = -errno;
      return NULL;
    }

  for (index = 0; index < P2SCHED_SIGNAL_EVENTS; index++)
    {
      do
        {
          signo = sigwaitinfo(&g_signal_set, &info);
        }
      while (signo < 0 && errno == EINTR);

      if (signo != SIGUSR1 || info.si_signo != SIGUSR1)
        {
          g_signal_error = -EILSEQ;
          return NULL;
        }

      g_signal_receive_count++;
    }

  return NULL;
}

static FAR void *p2sched_signal_sender(FAR void *arg)
{
  uint32_t index;
  int ret;

  UNUSED(arg);

  for (index = 0; index < P2SCHED_SIGNAL_EVENTS; index++)
    {
      ret = pthread_kill(g_signal_receiver, SIGUSR1);
      if (ret != 0)
        {
          g_signal_error = -ret;
          return NULL;
        }

      g_signal_send_count++;
    }

  return NULL;
}

static int p2sched_test_signal(FAR uint32_t *events)
{
  pthread_attr_t receiver_attr;
  pthread_attr_t sender_attr;
  pthread_t sender;
  sigset_t old_set;
  int ret;

  g_signal_receive_count = 0;
  g_signal_send_count = 0;
  g_signal_error = 0;

  ret = p2sched_sem_init(&g_signal_ready, 0);
  if (ret < 0)
    {
      return ret;
    }

  sigemptyset(&g_signal_set);
  sigaddset(&g_signal_set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &g_signal_set, &old_set) < 0)
    {
      ret = -errno;
      goto out_with_sem;
    }

  ret = p2sched_attr_init(&receiver_attr, P2SCHED_HIGH_PRIORITY,
                          SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_mask;
    }

  ret = p2sched_attr_init(&sender_attr, P2SCHED_LOW_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      pthread_attr_destroy(&receiver_attr);
      goto out_with_mask;
    }

  ret = pthread_create(&g_signal_receiver, &receiver_attr,
                       p2sched_signal_receiver, NULL);
  pthread_attr_destroy(&receiver_attr);
  if (ret == 0 && p2sched_wait(&g_signal_ready) < 0)
    {
      ret = errno;
    }

  if (ret == 0)
    {
      ret = pthread_create(&sender, &sender_attr,
                           p2sched_signal_sender, NULL);
    }

  pthread_attr_destroy(&sender_attr);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_mask;
    }

  ret = pthread_join(g_signal_receiver, NULL);
  if (ret == 0)
    {
      ret = pthread_join(sender, NULL);
    }

  if (ret != 0)
    {
      ret = -ret;
    }
  else if (g_signal_error != 0)
    {
      ret = g_signal_error;
    }
  else if (g_signal_receive_count != P2SCHED_SIGNAL_EVENTS ||
           g_signal_send_count != P2SCHED_SIGNAL_EVENTS)
    {
      ret = -EIO;
    }
  else
    {
      *events = g_signal_receive_count;
      ret = OK;
    }

out_with_mask:
  sigprocmask(SIG_SETMASK, &old_set, NULL);
out_with_sem:
  sem_destroy(&g_signal_ready);
  return ret;
}

/****************************************************************************
 * POSIX timer wakeups
 ****************************************************************************/

static int p2sched_test_timer(FAR uint32_t *events)
{
  struct sigevent event;
  struct itimerspec value;
  siginfo_t info;
  sigset_t set;
  sigset_t old_set;
  timer_t timer;
  uint32_t count = 0;
  bool timer_created = false;
  int signo;
  int ret = OK;

  sigemptyset(&set);
  sigaddset(&set, SIGRTMIN);
  if (sigprocmask(SIG_BLOCK, &set, &old_set) < 0)
    {
      return -errno;
    }

  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGRTMIN;
  event.sigev_value.sival_int = P2SCHED_TIMER_VALUE;

  if (timer_create(CLOCK_REALTIME, &event, &timer) < 0)
    {
      ret = -errno;
      goto out;
    }

  timer_created = true;
  memset(&value, 0, sizeof(value));
  value.it_value.tv_nsec = P2SCHED_TIMER_NSEC;
  value.it_interval.tv_nsec = P2SCHED_TIMER_NSEC;
  if (timer_settime(timer, 0, &value, NULL) < 0)
    {
      ret = -errno;
      goto out;
    }

  while (count < P2SCHED_TIMER_EVENTS)
    {
      do
        {
          signo = sigwaitinfo(&set, &info);
        }
      while (signo < 0 && errno == EINTR);

      if (signo != SIGRTMIN || info.si_signo != SIGRTMIN ||
          info.si_value.sival_int != P2SCHED_TIMER_VALUE)
        {
          ret = -EILSEQ;
          goto out;
        }

      count++;
    }

  *events = count;

out:
  if (timer_created && timer_delete(timer) < 0 && ret == OK)
    {
      ret = -errno;
    }

  sigprocmask(SIG_SETMASK, &old_set, NULL);
  return ret;
}

/****************************************************************************
 * Pthread create, join, and cancellation
 ****************************************************************************/

static FAR void *p2sched_pthread_normal(FAR void *arg)
{
  UNUSED(arg);

  if (sem_post(&g_pthread_started) < 0 ||
      p2sched_wait(&g_pthread_release) < 0)
    {
      return NULL;
    }

  return P2SCHED_PTHREAD_RESULT;
}

static FAR void *p2sched_pthread_cancel(FAR void *arg)
{
  UNUSED(arg);

  if (sem_post(&g_pthread_started) < 0)
    {
      return NULL;
    }

  for (; ; )
    {
      p2sched_wait(&g_pthread_cancel_wait);
      pthread_testcancel();
    }

  return NULL;
}

static int p2sched_test_pthread(FAR uint32_t *events)
{
  pthread_attr_t attr;
  pthread_addr_t result;
  pthread_t worker;
  uint32_t count = 0;
  int ret;

  ret = p2sched_sem_init(&g_pthread_started, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2sched_sem_init(&g_pthread_release, 0);
  if (ret < 0)
    {
      goto out_with_started;
    }

  ret = p2sched_sem_init(&g_pthread_cancel_wait, 0);
  if (ret < 0)
    {
      goto out_with_release;
    }

  ret = p2sched_attr_init(&attr, P2SCHED_HIGH_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      goto out_with_cancel;
    }

  ret = pthread_create(&worker, &attr, p2sched_pthread_normal, NULL);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_attr;
    }

  if (p2sched_wait(&g_pthread_started) < 0)
    {
      ret = -errno;
      goto out_with_attr;
    }

  count++;
  if (sem_post(&g_pthread_release) < 0)
    {
      ret = -errno;
      goto out_with_attr;
    }

  ret = pthread_join(worker, &result);
  if (ret != 0 || result != P2SCHED_PTHREAD_RESULT)
    {
      ret = ret == 0 ? -EIO : -ret;
      goto out_with_attr;
    }

  count++;
  ret = pthread_create(&worker, &attr, p2sched_pthread_cancel, NULL);
  if (ret != 0)
    {
      ret = -ret;
      goto out_with_attr;
    }

  if (p2sched_wait(&g_pthread_started) < 0)
    {
      ret = -errno;
      goto out_with_attr;
    }

  count++;
  ret = pthread_cancel(worker);
  if (ret == 0)
    {
      ret = pthread_join(worker, &result);
    }

  if (ret != 0 || result != PTHREAD_CANCELED)
    {
      ret = ret == 0 ? -EIO : -ret;
      goto out_with_attr;
    }

  count++;
  *events = count;
  ret = count == P2SCHED_PTHREAD_EVENTS ? OK : -EIO;

out_with_attr:
  pthread_attr_destroy(&attr);
out_with_cancel:
  sem_destroy(&g_pthread_cancel_wait);
out_with_release:
  sem_destroy(&g_pthread_release);
out_with_started:
  sem_destroy(&g_pthread_started);
  return ret;
}

/****************************************************************************
 * Task recreation
 ****************************************************************************/

static int p2sched_recreated_task(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);

  g_task_seen = g_task_generation;
  return P2SCHED_TASK_STATUS;
}

static int p2sched_test_task_recreate(FAR uint32_t *events)
{
  uint32_t count = 0;
  uint32_t index;
  pid_t pid;
  int status;
  int ret;

  for (index = 0; index < P2SCHED_TASK_EVENTS; index++)
    {
      g_task_generation = index + 1u;
      g_task_seen = 0;
      pid = task_create("p2recreate", P2SCHED_HIGH_PRIORITY,
                        CONFIG_TESTING_P2SCHEDSTRESS_TASK_STACKSIZE,
                        p2sched_recreated_task, NULL);
      if (pid < 0)
        {
          return -errno;
        }

      ret = waitpid(pid, &status, 0);
      if (ret != pid || !WIFEXITED(status) ||
          WEXITSTATUS(status) != P2SCHED_TASK_STATUS ||
          g_task_seen != g_task_generation)
        {
          return -EIO;
        }

      count++;
    }

  *events = count;
  return count == P2SCHED_TASK_EVENTS ? OK : -EIO;
}

/****************************************************************************
 * Stack and heap checks
 ****************************************************************************/

static int p2sched_test_stack(void)
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
      return -EIO;
    }

  base = (uintptr_t)info.stack_base_ptr;
  top = base + info.adj_stack_size;
  sp = up_getsp();
  if (sp < base || sp >= top)
    {
      return -ERANGE;
    }

  tcb = nxsched_get_tcb(getpid());
  if (tcb == NULL)
    {
      return -ESRCH;
    }

  used = up_check_tcbstack(tcb, info.adj_stack_size);
  if (used == 0 || used > info.adj_stack_size)
    {
      return -EOVERFLOW;
    }

  printf("P2SCHED:STACK:PASS:CHECKS=3:SIZE=%zu:USED=%zu\n",
         info.adj_stack_size, used);
  return OK;
}

static int p2sched_test_heap(void)
{
  struct mallinfo before;
  struct mallinfo during;
  struct mallinfo after;
  FAR uint8_t *memory;
  uintptr_t heap_start;
  uintptr_t heap_end;
  uintptr_t address;
  size_t index;

  heap_start = (uintptr_t)_sheap;
  heap_end = (uintptr_t)_eheap;
  if (heap_start >= heap_end ||
      heap_end - heap_start < P2SCHED_HEAP_BYTES)
    {
      return -ERANGE;
    }

  before = mallinfo();
  memory = malloc(P2SCHED_HEAP_BYTES);
  if (memory == NULL)
    {
      return -ENOMEM;
    }

  address = (uintptr_t)memory;
  if (address < heap_start ||
      address > heap_end - P2SCHED_HEAP_BYTES)
    {
      free(memory);
      return -ERANGE;
    }

  memset(memory, P2SCHED_HEAP_PATTERN, P2SCHED_HEAP_BYTES);
  for (index = 0; index < P2SCHED_HEAP_BYTES; index++)
    {
      if (memory[index] != P2SCHED_HEAP_PATTERN)
        {
          free(memory);
          return -EILSEQ;
        }
    }

  during = mallinfo();
  if (during.uordblks < before.uordblks + (int)P2SCHED_HEAP_BYTES)
    {
      free(memory);
      return -EIO;
    }

  free(memory);
  after = mallinfo();
  if (after.uordblks > during.uordblks)
    {
      return -EIO;
    }

  printf("P2SCHED:HEAP:PASS:CHECKS=5:BEFORE=%d:DURING=%d:AFTER=%d\n",
         before.uordblks, during.uordblks, after.uordblks);
  return OK;
}

static uint8_t p2sched_heap_concurrency_pattern(uintptr_t id,
                                                uint32_t round,
                                                size_t index)
{
  return (uint8_t)(0x5a ^ (id << 6) ^ round ^ index);
}

static int p2sched_heap_concurrency_wait(void)
{
  int ret;

  ret = pthread_barrier_wait(&g_heap_concurrency_barrier);
  if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
    {
      return -ret;
    }

  return OK;
}

static FAR void *p2sched_heap_concurrency_worker(FAR void *arg)
{
  uintptr_t id = (uintptr_t)arg;
  uintptr_t other = id ^ 1u;
  FAR uint8_t *memory;
  FAR uint8_t *peer;
  uintptr_t address;
  uintptr_t peer_address;
  uint32_t round;
  size_t index;
  bool round_ok;
  int ret;

  for (round = 0; round < P2SCHED_HEAP_CONCURRENCY_ROUNDS; round++)
    {
      round_ok = true;
      memory = malloc(P2SCHED_HEAP_CONCURRENCY_BYTES);
      g_heap_concurrency_memory[id] = memory;
      if (memory == NULL)
        {
          g_heap_concurrency_error = -ENOMEM;
          round_ok = false;
        }
      else
        {
          for (index = 0; index < P2SCHED_HEAP_CONCURRENCY_BYTES; index++)
            {
              memory[index] = p2sched_heap_concurrency_pattern(id, round,
                                                               index);
            }
        }

      /* Neither worker can pass this barrier until both allocations and
       * fills are complete, so both buffers are live during verification.
       */

      ret = p2sched_heap_concurrency_wait();
      if (ret < 0)
        {
          g_heap_concurrency_error = ret;
          free(memory);
          return NULL;
        }

      peer = g_heap_concurrency_memory[other];
      if (memory == NULL || peer == NULL)
        {
          g_heap_concurrency_error = -ENOMEM;
          round_ok = false;
        }
      else
        {
          address = (uintptr_t)memory;
          peer_address = (uintptr_t)peer;
          if ((address < peer_address &&
               address + P2SCHED_HEAP_CONCURRENCY_BYTES > peer_address) ||
              (peer_address < address &&
               peer_address + P2SCHED_HEAP_CONCURRENCY_BYTES > address) ||
              address == peer_address)
            {
              g_heap_concurrency_error = -EFAULT;
              round_ok = false;
            }

          for (index = 0; index < P2SCHED_HEAP_CONCURRENCY_BYTES; index++)
            {
              if (memory[index] !=
                  p2sched_heap_concurrency_pattern(id, round, index))
                {
                  g_heap_concurrency_error = -EILSEQ;
                  round_ok = false;
                  break;
                }
            }
        }

      /* Both workers finish checking while both allocations remain live.
       * Only then may either worker free its buffer.
       */

      ret = p2sched_heap_concurrency_wait();
      if (ret < 0)
        {
          g_heap_concurrency_error = ret;
          free(memory);
          return NULL;
        }

      free(memory);
      g_heap_concurrency_memory[id] = NULL;
      if (round_ok)
        {
          g_heap_concurrency_count[id]++;
        }

      /* Complete both frees before either worker begins the next round. */

      ret = p2sched_heap_concurrency_wait();
      if (ret < 0)
        {
          g_heap_concurrency_error = ret;
          return NULL;
        }
    }

  return NULL;
}

static int p2sched_test_heap_concurrency(FAR uint32_t *events)
{
  pthread_attr_t attr;
  pthread_t worker[2];
  bool first_created = false;
  int ret;

  g_heap_concurrency_memory[0] = NULL;
  g_heap_concurrency_memory[1] = NULL;
  g_heap_concurrency_count[0] = 0;
  g_heap_concurrency_count[1] = 0;
  g_heap_concurrency_error = 0;

  ret = pthread_barrier_init(&g_heap_concurrency_barrier, NULL,
                             P2SCHED_HEAP_CONCURRENCY_THREADS);
  if (ret != 0)
    {
      return -ret;
    }

  ret = p2sched_attr_init(&attr, P2SCHED_HIGH_PRIORITY, SCHED_FIFO);
  if (ret < 0)
    {
      pthread_barrier_destroy(&g_heap_concurrency_barrier);
      return ret;
    }

  ret = pthread_create(&worker[0], &attr,
                       p2sched_heap_concurrency_worker,
                       (FAR void *)(uintptr_t)0);
  if (ret == 0)
    {
      first_created = true;
      ret = pthread_create(&worker[1], &attr,
                           p2sched_heap_concurrency_worker,
                           (FAR void *)(uintptr_t)1);
    }

  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      if (first_created)
        {
          pthread_cancel(worker[0]);
          pthread_join(worker[0], NULL);
        }

      pthread_barrier_destroy(&g_heap_concurrency_barrier);
      return -ret;
    }

  ret = pthread_join(worker[0], NULL);
  if (ret == 0)
    {
      ret = pthread_join(worker[1], NULL);
    }

  pthread_barrier_destroy(&g_heap_concurrency_barrier);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_heap_concurrency_error != 0)
    {
      return g_heap_concurrency_error;
    }

  if (g_heap_concurrency_memory[0] != NULL ||
      g_heap_concurrency_memory[1] != NULL)
    {
      return -EBUSY;
    }

  *events = g_heap_concurrency_count[0] + g_heap_concurrency_count[1];
  return *events == P2SCHED_HEAP_CONCURRENCY_EVENTS ? OK : -EIO;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t events;
  uint32_t total = 0;
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  printf("P2SCHED:BOOT\n");
  printf("P2SCHED:PROFILE:MODE=FLAT-UP:RAM=%u\n", CONFIG_RAM_SIZE);

  printf("P2SCHED:PRIORITY:START:TARGET=%u\n",
         P2SCHED_PRIORITY_EVENTS);
  ret = p2sched_test_priority(&events);
  if (ret < 0)
    {
      return p2sched_fail("PRIORITY", ret);
    }

  total += events;
  printf("P2SCHED:PRIORITY:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:ROUNDROBIN:START:TARGET=%u\n", P2SCHED_RR_EVENTS);
  ret = p2sched_test_round_robin(&events);
  if (ret < 0)
    {
      return p2sched_fail("ROUNDROBIN", ret);
    }

  total += events;
  printf("P2SCHED:ROUNDROBIN:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:SEMAPHORE:START:TARGET=%u\n",
         P2SCHED_SEMAPHORE_EVENTS);
  ret = p2sched_test_semaphore(&events);
  if (ret < 0)
    {
      return p2sched_fail("SEMAPHORE", ret);
    }

  total += events;
  printf("P2SCHED:SEMAPHORE:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:PI_MUTEX:START:TARGET=%u\n", P2SCHED_PI_EVENTS);
  ret = p2sched_test_pi_mutex(&events);
  if (ret < 0)
    {
      return p2sched_fail("PI_MUTEX", ret);
    }

  total += events;
  printf("P2SCHED:PI_MUTEX:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:CONDITION:START:TARGET=%u\n",
         P2SCHED_CONDITION_EVENTS);
  ret = p2sched_test_condition(&events);
  if (ret < 0)
    {
      return p2sched_fail("CONDITION", ret);
    }

  total += events;
  printf("P2SCHED:CONDITION:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:MQUEUE:START:TARGET=%u\n", P2SCHED_MQUEUE_EVENTS);
  ret = p2sched_test_mqueue(&events);
  if (ret < 0)
    {
      return p2sched_fail("MQUEUE", ret);
    }

  total += events;
  printf("P2SCHED:MQUEUE:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:SIGNAL:START:TARGET=%u\n", P2SCHED_SIGNAL_EVENTS);
  ret = p2sched_test_signal(&events);
  if (ret < 0)
    {
      return p2sched_fail("SIGNAL", ret);
    }

  total += events;
  printf("P2SCHED:SIGNAL:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:TIMER:START:TARGET=%u\n", P2SCHED_TIMER_EVENTS);
  ret = p2sched_test_timer(&events);
  if (ret < 0)
    {
      return p2sched_fail("TIMER", ret);
    }

  total += events;
  printf("P2SCHED:TIMER:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:PTHREAD:START:TARGET=%u\n", P2SCHED_PTHREAD_EVENTS);
  ret = p2sched_test_pthread(&events);
  if (ret < 0)
    {
      return p2sched_fail("PTHREAD", ret);
    }

  total += events;
  printf("P2SCHED:PTHREAD:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:TASK_RECREATE:START:TARGET=%u\n",
         P2SCHED_TASK_EVENTS);
  ret = p2sched_test_task_recreate(&events);
  if (ret < 0)
    {
      return p2sched_fail("TASK_RECREATE", ret);
    }

  total += events;
  printf("P2SCHED:TASK_RECREATE:PASS:COUNT=%" PRIu32 "\n", events);

  printf("P2SCHED:STACK:START\n");
  ret = p2sched_test_stack();
  if (ret < 0)
    {
      return p2sched_fail("STACK", ret);
    }

  printf("P2SCHED:HEAP:START\n");
  ret = p2sched_test_heap();
  if (ret < 0)
    {
      return p2sched_fail("HEAP", ret);
    }

  printf("P2SCHED:HEAP_CONCURRENCY:START:"
         "THREADS=2:ROUNDS=256:TARGET=512\n");
  ret = p2sched_test_heap_concurrency(&events);
  if (ret < 0)
    {
      return p2sched_fail("HEAP_CONCURRENCY", ret);
    }

  printf("P2SCHED:HEAP_CONCURRENCY:PASS:COUNT=512\n");

  if (total != P2SCHED_TOTAL_EVENTS || total < 1000000u)
    {
      return p2sched_fail("TOTAL", -EIO);
    }

  printf("P2SCHED:TOTAL:PASS:COUNT=%" PRIu32 "\n", total);
  printf("P2SCHED:PASS:COUNT=%" PRIu32 "\n", total);
  return EXIT_SUCCESS;
}
