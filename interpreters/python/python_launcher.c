/****************************************************************************
 * apps/interpreters/python/python_launcher.c
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
#include <nuttx/mutex.h>

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
#  include <nuttx/clock.h>
#  include <nuttx/semaphore.h>
#endif

#if defined(CONFIG_ARCH_P2) && defined(CONFIG_STACK_COLORATION)
#  include <nuttx/arch.h>
#  include <nuttx/sched.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
#  include <arch/overlay.h>
#  ifdef CONFIG_P2_EC32MB_PSRAM_UNIFIED
#    include <arch/board/p2_ec32mb_psram.h>
#  endif
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct python_worker_args_s
{
  int argc;
  FAR char **argv;
  int result;
#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
  sem_t completed;
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CPython has one process-global runtime.  On P2 it also has one Hub overlay
 * execution slot.  This guard is intentionally acquired by the modest-stack
 * command task before the large worker stack is allocated.
 */

static mutex_t g_cpython_runtime_lock = NXMUTEX_INITIALIZER;

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
/* The container uploader temporarily owns stdin/stdout as a binary channel.
 * Do not let the resident monitor print until the worker has restored the
 * console to line mode.  There is only one worker while the runtime lock is
 * held, so one process-global semaphore exactly represents that handoff.
 */

static sem_t g_python_overlay_telemetry_ready;
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int python_worker_main(int argc, FAR char *argv[]);

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
void python_overlay_telemetry_start(void)
{
  (void)nxsem_post(&g_python_overlay_telemetry_ready);
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR void *python_worker(FAR void *arg)
{
  FAR struct python_worker_args_s *args = arg;

  args->result = python_worker_main(args->argc, args->argv);

#ifdef CONFIG_ARCH_P2
  /* A test-body marker alone is insufficient evidence of a clean CPython
   * shutdown: py_bytesmain() can still report a finalization failure after
   * the script has printed it.  Expose the actual worker result so P2 HIL
   * can require an exact zero exit before accepting stack telemetry.
   */

  printf("P2PY:WORKER:EXIT:CODE=%d\n", args->result);
#endif

#if defined(CONFIG_ARCH_P2) && defined(CONFIG_STACK_COLORATION)
  {
    FAR struct tcb_s *tcb = nxsched_self();
    size_t size = tcb->adj_stack_size;
    size_t used = up_check_tcbstack(tcb, size);

    if (used > size)
      {
        printf("ERROR: CPython worker stack telemetry is invalid\n");
      }
    else
      {
        printf("P2PY:WORKER:STACK:FREE=%zu:SIZE=%zu\n", size - used, size);
      }
  }
#endif

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
  /* Unblock the launcher even when runtime preparation failed before the
   * wrapper reached its normal console-safe handoff.  A second post after a
   * successful handoff is harmless and remains private to this invocation.
   */

  python_overlay_telemetry_start();
  (void)nxsem_post(&args->completed);
#endif

  return NULL;
}

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
void python_overlay_report(FAR const char *stage)
{
  struct p2_overlay_stats_s stats;
#ifdef CONFIG_P2_EC32MB_PSRAM_UNIFIED
  struct p2_psram_cache_stats_s cache;
#endif
  unsigned int flags;
  int ret;

  ret = p2_overlay_get_stats(&stats);
  if (ret < 0)
    {
      printf("P2PY:OVL:%s:ERROR=%d\n", stage, ret);
      return;
    }

  flags = (stats.ready ? 1 : 0) | (stats.transition ? 2 : 0);
  printf("P2PY:OVL:%s:E=%016llX:X=%016llX:D=%016llX:A=%016llX:"
         "L=%016llX:B=%016llX:DEP=%08lX:MAX=%08lX:G=%08lX:"
         "LG=%08lX:LB=%08lX:REQ=%08lX:STUB=%08lX:F=%02X:ERR=%d\n",
         stage,
         (unsigned long long)stats.entry_count,
         (unsigned long long)stats.exit_count,
         (unsigned long long)stats.direct_count,
         (unsigned long long)stats.load_attempt_count,
         (unsigned long long)stats.load_count,
         (unsigned long long)stats.load_bytes,
         (unsigned long)stats.current_depth,
         (unsigned long)stats.maximum_depth,
         (unsigned long)stats.loaded_group,
         (unsigned long)stats.loading_group,
         (unsigned long)stats.loading_bytes,
         (unsigned long)stats.last_requested_group,
         (unsigned long)stats.last_stub_index,
         flags,
         stats.last_error);

#ifdef CONFIG_P2_EC32MB_PSRAM_UNIFIED
  ret = p2_psram_get_cache_stats(&cache);
  if (ret < 0)
    {
      printf("P2PY:XMEM:%s:ERROR=%d\n", stage, ret);
      return;
    }

  printf("P2PY:XMEM:%s:H=%016llX:M=%016llX:F=%016llX:W=%016llX:"
         "B=%016llX\n",
         stage,
         (unsigned long long)cache.hits,
         (unsigned long long)cache.misses,
         (unsigned long long)cache.fills,
         (unsigned long long)cache.writes,
         (unsigned long long)cache.bypasses);
#endif
}

/* Emit a deterministic bounded Space-Saving snapshot in resident-table order.
 * SAMPLE is called by the 60-second launcher timer and FINAL records the
 * terminal state.  C is the estimated count and E its maximum overcount, so
 * C-E is a lower bound.  The evidence-bound host decoder performs ranking;
 * sorting here would spend scarce Hub code and worker-stack space on a purely
 * diagnostic presentation step.
 */

static void python_overlay_report_hot(FAR const char *stage)
{
  struct p2_overlay_hot_snapshot_s snapshot;
  uint32_t index;
  int ret;

  ret = p2_overlay_get_hot_snapshot(&snapshot);
  if (ret < 0)
    {
      printf("P2PY:HOT:%s:ERROR=%d\n", stage, ret);
      return;
    }

  if (snapshot.capacity != P2_OVERLAY_HOT_CAPACITY ||
      snapshot.used > snapshot.capacity)
    {
      printf("P2PY:HOT:%s:ERROR=%d\n", stage, -EOVERFLOW);
      return;
    }

  printf("P2PY:HOT:%s:N=%02lX:T=%016llX\n", stage,
         (unsigned long)snapshot.used,
         (unsigned long long)snapshot.total_count);
  for (index = 0; index < snapshot.used; index++)
    {
      FAR const struct p2_overlay_hot_entry_s *entry =
        &snapshot.entries[index];

      printf("P2PY:HOT:%s:R=%02lX:CG=%08lX:CO=%08lX:TG=%08lX:"
             "TS=%08lX:C=%016llX:E=%016llX\n",
             stage,
             (unsigned long)index,
             (unsigned long)entry->caller_group,
             (unsigned long)entry->caller_offset,
             (unsigned long)entry->target_group,
             (unsigned long)entry->target_stub,
             (unsigned long long)entry->count,
             (unsigned long long)entry->error);
    }
}
#endif

static int python_unlock(int result)
{
  int ret;

  ret = nxmutex_unlock(&g_cpython_runtime_lock);
  if (ret < 0)
    {
      printf("ERROR: CPython runtime unlock failed: %d\n", ret);
      return 1;
    }

  return result;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: python_launcher_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct python_worker_args_s args;
  pthread_attr_t attr;
  pthread_t worker;
  int ret;

  ret = nxmutex_trylock(&g_cpython_runtime_lock);
  if (ret < 0)
    {
      if (ret == -EAGAIN || ret == -EBUSY)
        {
          printf("P2PY:RUNTIME:BUSY:CODE=%d\n", EBUSY);
        }
      else
        {
          printf("ERROR: CPython runtime lock failed: %d\n", ret);
        }

      return 1;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      printf("ERROR: CPython worker attributes failed: %d\n", ret);
      return python_unlock(1);
    }

  ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
  if (ret != 0)
    {
      printf("ERROR: CPython worker scheduling failed: %d\n", ret);
      pthread_attr_destroy(&attr);
      return python_unlock(1);
    }

  ret = pthread_attr_setstacksize(
    &attr, CONFIG_INTERPRETERS_CPYTHON_STACKSIZE);
  if (ret != 0)
    {
      printf("ERROR: CPython worker stack size failed: %d\n", ret);
      pthread_attr_destroy(&attr);
      return python_unlock(1);
    }

  args.argc = argc;
  args.argv = argv;
  args.result = 1;
#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
  ret = nxsem_init(&args.completed, 0, 0);
  if (ret < 0)
    {
      printf("ERROR: CPython telemetry semaphore failed: %d\n", ret);
      pthread_attr_destroy(&attr);
      return python_unlock(1);
    }

  ret = nxsem_init(&g_python_overlay_telemetry_ready, 0, 0);
  if (ret < 0)
    {
      printf("ERROR: CPython telemetry-ready semaphore failed: %d\n", ret);
      (void)nxsem_destroy(&args.completed);
      pthread_attr_destroy(&attr);
      return python_unlock(1);
    }
#endif

  ret = pthread_create(&worker, &attr, python_worker, &args);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      printf("ERROR: CPython worker creation failed: %d\n", ret);
#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
      (void)nxsem_destroy(&args.completed);
      (void)nxsem_destroy(&g_python_overlay_telemetry_ready);
#endif
      return python_unlock(1);
    }

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
  ret = nxsem_wait_uninterruptible(&g_python_overlay_telemetry_ready);
  if (ret < 0)
    {
      printf("ERROR: CPython telemetry-ready wait failed: %d\n", ret);
    }

  python_overlay_report("LAUNCH");
  for (; ; )
    {
      ret = nxsem_tickwait_uninterruptible(
        &args.completed,
        MSEC2TICK(
          CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY_INTERVAL_MS));
      if (ret == 0)
        {
          break;
        }

      if (ret != -ETIMEDOUT)
        {
          printf("ERROR: CPython telemetry wait failed: %d\n", ret);
          break;
        }

      python_overlay_report("SAMPLE");
      python_overlay_report_hot("SAMPLE");
    }
#endif

  ret = pthread_join(worker, NULL);
  if (ret != 0)
    {
      printf("ERROR: CPython worker join failed: %d\n", ret);
#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
      (void)nxsem_destroy(&args.completed);
      (void)nxsem_destroy(&g_python_overlay_telemetry_ready);
#endif
      return python_unlock(1);
    }

#ifdef CONFIG_INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY
  python_overlay_report("FINAL");
  python_overlay_report_hot("FINAL");
  (void)nxsem_destroy(&args.completed);
  (void)nxsem_destroy(&g_python_overlay_telemetry_ready);
#endif

  return python_unlock(args.result);
}
