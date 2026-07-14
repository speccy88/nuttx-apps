/****************************************************************************
 * apps/system/p2berrybank/p2berrybank_main.c
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

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/boardctl.h>

#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/compiler.h>

#include <arch/board/p2_ec32mb_bank.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2BERRYBANK_SD_DEVICE       "/dev/mmcsd0"
#define P2BERRYBANK_SD_MOUNTPOINT   "/mnt/sd"
#define P2BERRYBANK_SCRIPT_PREFIX   P2BERRYBANK_SD_MOUNTPOINT "/"
#define P2BERRYBANK_MODULE_PATH     "/mnt/sd/berry-p2"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int berry_main(int argc, FAR char *argv[]);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool p2berrybank_safe_script_path(FAR const char *path)
{
  FAR const char *component;
  FAR const char *cursor;
  size_t component_length;
  size_t prefix_length = strlen(P2BERRYBANK_SCRIPT_PREFIX);

  if (path == NULL ||
      strncmp(path, P2BERRYBANK_SCRIPT_PREFIX, prefix_length) != 0 ||
      path[prefix_length] == '\0')
    {
      return false;
    }

  component = path + prefix_length;
  for (cursor = component; ; cursor++)
    {
      unsigned char ch = (unsigned char)*cursor;

      if (ch == '\\' || (ch != '\0' && (ch < 0x20 || ch == 0x7f)))
        {
          return false;
        }

      if (ch == '/' || ch == '\0')
        {
          component_length = (size_t)(cursor - component);
          if (component_length == 0 ||
              (component_length == 1 && component[0] == '.') ||
              (component_length == 2 && component[0] == '.' &&
               component[1] == '.'))
            {
              return false;
            }

          if (ch == '\0')
            {
              return true;
            }

          component = cursor + 1;
        }
    }
}

static bool p2berrybank_regular_file(FAR const char *path)
{
  struct stat file_stat;

  return stat(path, &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

static int p2berrybank_mount_sd(void)
{
  if (mkdir("/mnt", 0777) < 0 && errno != EEXIST)
    {
      int error = errno;

      fprintf(stderr, "P2BERRYBANK:SD=FAIL:MKDIR=/mnt:ERRNO=%d\n", error);
      return -error;
    }

  if (mkdir(P2BERRYBANK_SD_MOUNTPOINT, 0777) < 0 && errno != EEXIST)
    {
      int error = errno;

      fprintf(stderr, "P2BERRYBANK:SD=FAIL:MKDIR=%s:ERRNO=%d\n",
              P2BERRYBANK_SD_MOUNTPOINT, error);
      return -error;
    }

  if (mount(P2BERRYBANK_SD_DEVICE, P2BERRYBANK_SD_MOUNTPOINT,
            "vfat", 0, NULL) < 0)
    {
      int error = errno;

      fprintf(stderr,
              "P2BERRYBANK:SD=FAIL:DEVICE=%s:MOUNT=%s:ERRNO=%d:"
              "AUTOFORMAT=NO\n",
              P2BERRYBANK_SD_DEVICE, P2BERRYBANK_SD_MOUNTPOINT, error);
      return -error;
    }

  printf("P2BERRYBANK:SD=READY:DEVICE=%s:MOUNT=%s:AUTOFORMAT=NO\n",
         P2BERRYBANK_SD_DEVICE, P2BERRYBANK_SD_MOUNTPOINT);
  return 0;
}

static void p2berrybank_heap_marker(FAR const char *phase)
{
  struct mallinfo info = mallinfo();

  printf("P2BERRYBANK:HEAP:%s:ARENA=%lu:UORDBLKS=%lu:FORDBLKS=%lu:"
         "MXORDBLK=%lu\n",
         phase, (unsigned long)info.arena, (unsigned long)info.uordblks,
         (unsigned long)info.fordblks, (unsigned long)info.mxordblk);
}

static FAR const char *
p2berrybank_select_script(FAR const struct p2_bank_handoff_s *handoff,
                          bool sd_ready)
{
  FAR const char *fallback = CONFIG_SYSTEM_P2BERRYBANK_DEFAULT_SCRIPT;

  if (p2_bank_handoff_valid(handoff))
    {
      printf("P2BERRYBANK:HANDOFF=VALID:BANK_SIZE=%lu:BANK_CRC32=%08lx\n",
             (unsigned long)handoff->bank_size,
             (unsigned long)handoff->bank_crc32);

      if (handoff->script_path[0] != '\0')
        {
          if (!p2berrybank_safe_script_path(handoff->script_path))
            {
              fprintf(stderr,
                      "P2BERRYBANK:SCRIPT=REJECTED:REASON=UNSAFE_PATH\n");
              printf("P2BERRYBANK:SCRIPT=REPL:REASON=UNSAFE_HANDOFF_PATH\n");
              return NULL;
            }

          if (sd_ready && p2berrybank_regular_file(handoff->script_path))
            {
              printf("P2BERRYBANK:SCRIPT=SELECTED:PATH=%s\n",
                     handoff->script_path);
              return handoff->script_path;
            }

          fprintf(stderr, "P2BERRYBANK:SCRIPT=MISSING:PATH=%s\n",
                  handoff->script_path);
          printf("P2BERRYBANK:SCRIPT=REPL:REASON=HANDOFF_SCRIPT_UNAVAILABLE\n");
          return NULL;
        }

      printf("P2BERRYBANK:SCRIPT=REPL:REASON=HANDOFF_PATH_EMPTY\n");
      return NULL;
    }

  fprintf(stderr, "P2BERRYBANK:HANDOFF=INVALID\n");

  if (sd_ready && p2berrybank_safe_script_path(fallback) &&
      p2berrybank_regular_file(fallback))
    {
      printf("P2BERRYBANK:SCRIPT=DEFAULT:PATH=%s\n", fallback);
      return fallback;
    }

  printf("P2BERRYBANK:SCRIPT=REPL:REASON=INVALID_HANDOFF_NO_DEFAULT\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const struct p2_bank_handoff_s *fixed_handoff =
    (FAR const struct p2_bank_handoff_s *)
    (uintptr_t)P2_BANK_HANDOFF_ADDRESS;
  struct p2_bank_handoff_s handoff;
  FAR const char *script;
  FAR char *berry_argv[5];
  int berry_argc;
  int berry_status;
  int reset_status;
  bool sd_ready;

  (void)argc;
  (void)argv;

  /* Copy the warm-start handoff before validating it.  It lives above the
   * linked bank image and survives startup's .bss clear, but is not ordinary
   * application storage.
   */

  memcpy(&handoff, fixed_handoff, sizeof(handoff));

  printf("P2BERRYBANK:START:HUB_LIMIT=0x%08lx:HANDOFF=0x%08lx\n",
         (unsigned long)P2_BANK_HUB_IMAGE_LIMIT,
         (unsigned long)P2_BANK_HANDOFF_ADDRESS);

  sd_ready = p2berrybank_mount_sd() == 0;
  script = p2berrybank_select_script(&handoff, sd_ready);

  berry_argv[0] = (FAR char *)"berry";
  berry_argv[1] = (FAR char *)"-m";
  berry_argv[2] = (FAR char *)P2BERRYBANK_MODULE_PATH;
  berry_argc = 3;

  if (script != NULL)
    {
      berry_argv[berry_argc++] = (FAR char *)script;
    }
  else
    {
      printf("P2BERRYBANK:REPL=READY:EXIT=quit()\n");
    }

  berry_argv[berry_argc] = NULL;
  p2berrybank_heap_marker("BEFORE");
  berry_status = berry_main(berry_argc, berry_argv);
  p2berrybank_heap_marker("AFTER");

  printf("P2BERRYBANK:BERRY=RETURNED:STATUS=%d:RESET=FLASH_MANAGER\n",
         berry_status);
  fflush(stdout);
  fflush(stderr);

  reset_status = boardctl(BOARDIOC_RESET, 0);
  fprintf(stderr, "P2BERRYBANK:RESET=FAILED:STATUS=%d\n", reset_status);
  return EXIT_FAILURE;
}
