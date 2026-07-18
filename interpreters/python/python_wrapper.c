/****************************************************************************
 * apps/interpreters/python/python_wrapper.c
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
#include <sys/mount.h>
#include <sys/boardctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/drivers/ramdisk.h>

#ifndef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
#  include "romfs_cpython_modules.h"
#endif

#include "Python.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration settings */

#ifndef CONFIG_CPYTHON_ROMFS_RAMDEVNO
#  define CONFIG_CPYTHON_ROMFS_RAMDEVNO 1
#endif

#ifndef CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE
#  define CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE 64
#endif

#ifndef CONFIG_CPYTHON_ROMFS_MOUNTPOINT
#  define CONFIG_CPYTHON_ROMFS_MOUNTPOINT "/usr/local/lib"
#endif

#ifdef CONFIG_DISABLE_MOUNTPOINT
#  error "Mountpoint support is disabled"
#endif

#ifndef CONFIG_FS_ROMFS
#  error "ROMFS support not enabled"
#endif

#define NSECTORS(b)        \
  (((b) + CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE - 1) / \
   CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE)
#define STR_RAMDEVNO(m)    #m
#define MKMOUNT_DEVNAME(m) "/dev/ram" STR_RAMDEVNO(m)
#define MOUNT_DEVNAME      MKMOUNT_DEVNAME(CONFIG_CPYTHON_ROMFS_RAMDEVNO)

#ifdef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
int board_cpython_runtime_prepare(int fd);
int board_cpython_tmpfs_validate(void);
int board_cpython_romfs_image(FAR const uint8_t **image, FAR size_t *length);
int board_cpython_romdisk_register(int minor, FAR const uint8_t *image,
                                   uint32_t nsectors, uint16_t sectsize);
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The launcher serializes every call into this file.  The ROM disk and mount
 * outlive an individual CPython worker, so remember both successful stages
 * in resident Hub data.  Keeping the registration stage separately makes a
 * failed mount retryable without attempting to register the same minor
 * twice.
 */

static bool g_cpython_romdisk_registered;
static bool g_cpython_romfs_mounted;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: check_and_mount_romfs
 *
 * Description:
 *   Check if the ROMFS is already mounted, and if not, mount it.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   0 on success, 1 on failure
 *
 ****************************************************************************/

static int check_and_mount_romfs(void)
{
  int ret = OK;
#ifndef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
  struct boardioc_romdisk_s desc;
#endif
#ifdef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
  FAR const uint8_t *romfs_image;
  size_t romfs_length;
#endif

  if (g_cpython_romfs_mounted)
    {
      _info("Device is already mounted at %s\n",
            CONFIG_CPYTHON_ROMFS_MOUNTPOINT);
      return ret;
    }

  /* Create the RAM disk once.  If mounting fails below, retain this stage so
   * the next serialized invocation can retry only the mount.
   */

  if (!g_cpython_romdisk_registered)
    {
#ifdef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
      ret = board_cpython_romfs_image(&romfs_image, &romfs_length);
      if (ret < 0 || romfs_image == NULL || romfs_length == 0)
        {
          printf("ERROR: CPython external ROMFS is unavailable: %d\n", ret);
          return 1;
        }
#else
#  define romfs_image  romfs_cpython_modules_img
#  define romfs_length romfs_cpython_modules_img_len
#endif

#ifdef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
      ret = board_cpython_romdisk_register(
        CONFIG_CPYTHON_ROMFS_RAMDEVNO, romfs_image,
        NSECTORS(romfs_length),
        CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE);
#else
      desc.minor    = CONFIG_CPYTHON_ROMFS_RAMDEVNO;
      desc.nsectors = NSECTORS(romfs_length);
      desc.sectsize = CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE;
      desc.image    = (FAR uint8_t *)romfs_image;
      ret = boardctl(BOARDIOC_ROMDISK, (uintptr_t)&desc);
#endif

      if (ret < 0)
        {
          printf("ERROR: Failed to create RAM disk: %d\n", ret);
          return 1;
        }

      g_cpython_romdisk_registered = true;
#if defined(CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS) && \
    defined(CONFIG_ARCH_P2)
      printf("P2PY:ROMDISK:READY:MODE=BUFFERED:SECTOR=%u\n",
             CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE);
#endif
    }

  /* Mount the test file system */

  _info("Mounting ROMFS filesystem at target=%s with source=%s\n",
        CONFIG_CPYTHON_ROMFS_MOUNTPOINT, MOUNT_DEVNAME);

  ret = mount(MOUNT_DEVNAME, CONFIG_CPYTHON_ROMFS_MOUNTPOINT, "romfs",
              MS_RDONLY, NULL);
  if (ret < 0)
    {
      printf("ERROR: Mount failed: %s\n", strerror(errno));
      return 1;
    }

  g_cpython_romfs_mounted = true;
#ifdef CONFIG_ARCH_P2
  printf("P2PY:ROMFS:MOUNTED\n");
#endif
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: python_worker_main
 ****************************************************************************/

int python_worker_main(int argc, FAR char *argv[])
{
  int ret;

#ifdef CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS
  ret = board_cpython_runtime_prepare(STDIN_FILENO);
  if (ret < 0)
    {
      printf("ERROR: CPython external runtime preparation failed: %d\n",
             ret);
      return 1;
    }

  ret = board_cpython_tmpfs_validate();
  if (ret < 0)
    {
      printf("ERROR: CPython writable tmpfs is unavailable: %d\n", ret);
      return 1;
    }

#  ifdef CONFIG_ARCH_P2
  printf("P2PY:TMPFS:READY:PATH=%s:HEAP=%u\n",
         CONFIG_LIBC_TMPDIR, CONFIG_FS_HEAPSIZE);
#  endif
#endif

  ret = check_and_mount_romfs();
  if (ret != 0)
    {
      return ret;
    }

#ifdef CONFIG_ARCH_P2
  printf("P2PY:CPYTHON:EARLY:START\n");
#endif
  _pyruntime_early_init();
#ifdef CONFIG_ARCH_P2
  printf("P2PY:CPYTHON:EARLY:PASS\n");
#endif

  if (setenv("PYTHONHOME", "/usr/local", 1) < 0 ||
      setenv("PYTHON_BASIC_REPL", "1", 1) < 0 ||
      setenv("PYTHONPATH", CONFIG_INTERPRETERS_CPYTHON_PYTHONPATH, 1) < 0 ||
      setenv("HOME", CONFIG_LIBC_TMPDIR, 1) < 0 ||
      setenv("PYTHONNOUSERSITE", "1", 1) < 0)
    {
      printf("ERROR: CPython runtime environment setup failed: %s\n",
             strerror(errno));
      return 1;
    }

#ifdef CONFIG_ARCH_P2
  printf("P2PY:CPYTHON:RUN\n");
#endif
  return py_bytesmain(argc, argv);
}
