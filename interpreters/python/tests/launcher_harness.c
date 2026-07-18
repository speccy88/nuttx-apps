/****************************************************************************
 * apps/interpreters/python/tests/launcher_harness.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/sched.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_WORKER_STACK_SIZE 65536

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct launch_call_s
{
  int argc;
  char **argv;
  int result;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

extern int python_builtin_main(int argc, char *argv[]);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
static bool g_block_worker;
static bool g_worker_entered;
static int g_worker_result;
static int g_create_error;
static int g_create_attempts;
static int g_worker_calls;
static int g_seen_argc;
static char **g_seen_argv;
static size_t g_seen_stack_size;
static struct tcb_s g_worker_tcb =
{
  TEST_WORKER_STACK_SIZE
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                        void *(*entry)(void *), void *arg)
{
  size_t stack_size;
  int inherit;
  int error;

  error = pthread_attr_getstacksize(attr, &stack_size);
  assert(error == 0);
  error = pthread_attr_getinheritsched(attr, &inherit);
  assert(error == 0);
  assert(inherit == PTHREAD_INHERIT_SCHED);

  pthread_mutex_lock(&g_gate);
  g_create_attempts++;
  g_seen_stack_size = stack_size;
  error = g_create_error;
  g_create_error = 0;
  pthread_mutex_unlock(&g_gate);

  if (error != 0)
    {
      return error;
    }

  return pthread_create(thread, attr, entry, arg);
}

struct tcb_s *nxsched_self(void)
{
  return &g_worker_tcb;
}

size_t up_check_tcbstack(struct tcb_s *tcb, size_t check_size)
{
  assert(tcb == &g_worker_tcb);
  assert(check_size == TEST_WORKER_STACK_SIZE);
  return 8192;
}

int python_worker_main(int argc, char *argv[])
{
  int result;

  pthread_mutex_lock(&g_gate);
  g_worker_calls++;
  g_seen_argc = argc;
  g_seen_argv = argv;
  g_worker_entered = true;
  pthread_cond_broadcast(&g_changed);

  while (g_block_worker)
    {
      pthread_cond_wait(&g_changed, &g_gate);
    }

  result = g_worker_result;
  pthread_mutex_unlock(&g_gate);
  return result;
}

static void *launch_thread(void *arg)
{
  struct launch_call_s *call = arg;

  call->result = python_builtin_main(call->argc, call->argv);
  return NULL;
}

static void wait_for_worker(void)
{
  pthread_mutex_lock(&g_gate);
  while (!g_worker_entered)
    {
      pthread_cond_wait(&g_changed, &g_gate);
    }

  pthread_mutex_unlock(&g_gate);
}

static void set_worker(int result, bool block)
{
  pthread_mutex_lock(&g_gate);
  g_worker_result = result;
  g_block_worker = block;
  g_worker_entered = false;
  pthread_mutex_unlock(&g_gate);
}

int main(void)
{
  char *first_argv[] =
  {
    "python", "-c", "first", NULL
  };

  char *busy_argv[] =
  {
    "python", "-c", "busy", NULL
  };

  char *repeat_argv[] =
  {
    "python", "-c", "repeat", NULL
  };

  struct launch_call_s first =
  {
    3, first_argv, -1
  };

  pthread_t launcher;
  int ret;

  set_worker(37, true);
  ret = pthread_create(&launcher, NULL, launch_thread, &first);
  assert(ret == 0);
  wait_for_worker();

  /* The second command must fail before attempting another worker create. */

  ret = python_builtin_main(3, busy_argv);
  assert(ret == 1);

  pthread_mutex_lock(&g_gate);
  assert(g_create_attempts == 1);
  assert(g_worker_calls == 1);
  assert(g_seen_stack_size == TEST_WORKER_STACK_SIZE);
  assert(g_seen_argc == 3);
  assert(g_seen_argv == first_argv);
  assert(strcmp(g_seen_argv[2], "first") == 0);
  g_block_worker = false;
  pthread_cond_broadcast(&g_changed);
  pthread_mutex_unlock(&g_gate);

  ret = pthread_join(launcher, NULL);
  assert(ret == 0);
  assert(first.result == 37);

  /* A clean join releases the guard and preserves the next exit status. */

  set_worker(19, false);
  ret = python_builtin_main(3, repeat_argv);
  assert(ret == 19);
  assert(g_create_attempts == 2);
  assert(g_worker_calls == 2);

  /* A failed allocation/create also releases the guard for a retry. */

  pthread_mutex_lock(&g_gate);
  g_create_error = EAGAIN;
  pthread_mutex_unlock(&g_gate);
  ret = python_builtin_main(3, repeat_argv);
  assert(ret == 1);
  assert(g_create_attempts == 3);
  assert(g_worker_calls == 2);

  set_worker(23, false);
  ret = python_builtin_main(3, repeat_argv);
  assert(ret == 23);
  assert(g_create_attempts == 4);
  assert(g_worker_calls == 3);

  puts("PASS: guarded CPython launcher");
  return 0;
}
