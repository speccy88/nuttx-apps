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

#if defined(CONFIG_ARCH_P2) && defined(CONFIG_STACK_COLORATION)
#  include <nuttx/arch.h>
#  include <nuttx/sched.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct python_worker_args_s
{
  int argc;
  FAR char **argv;
  int result;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CPython has one process-global runtime.  On P2 it also has one Hub overlay
 * execution slot.  This guard is intentionally acquired by the modest-stack
 * command task before the large worker stack is allocated.
 */

static mutex_t g_cpython_runtime_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int python_worker_main(int argc, FAR char *argv[]);

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

  return NULL;
}

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

  ret = pthread_create(&worker, &attr, python_worker, &args);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      printf("ERROR: CPython worker creation failed: %d\n", ret);
      return python_unlock(1);
    }

  ret = pthread_join(worker, NULL);
  if (ret != 0)
    {
      printf("ERROR: CPython worker join failed: %d\n", ret);
      return python_unlock(1);
    }

  return python_unlock(args.result);
}
