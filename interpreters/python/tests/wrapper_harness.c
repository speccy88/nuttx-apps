/****************************************************************************
 * apps/interpreters/python/tests/wrapper_harness.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/boardctl.h>
#include <sys/mount.h>

#include <nuttx/drivers/ramdisk.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

extern int python_worker_main(int argc, char *argv[]);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_romfs_image[100];
static int g_prepare_calls;
static int g_image_calls;
static int g_boardctl_calls;
static int g_mount_calls;
static int g_early_init_calls;
static int g_python_calls;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_cpython_runtime_prepare(int fd)
{
  assert(fd == 0);
  g_prepare_calls++;
  return 0;
}

int board_cpython_romfs_image(const uint8_t **image, size_t *length)
{
  g_image_calls++;
  *image = g_romfs_image;
  *length = sizeof(g_romfs_image);
  return 0;
}

int boardctl(int command, uintptr_t arg)
{
  const struct boardioc_romdisk_s *desc =
    (const struct boardioc_romdisk_s *)arg;

  assert(command == BOARDIOC_ROMDISK);
  assert(desc->minor == 1);
  assert(desc->nsectors == 2);
  assert(desc->sectsize == 64);
  assert(desc->image == g_romfs_image);
  g_boardctl_calls++;
  return 0;
}

int mount(const char *source, const char *target, const char *filesystem,
          unsigned long flags, const void *data)
{
  assert(strcmp(source, "/dev/ram1") == 0);
  assert(strcmp(target, "/usr/local/lib") == 0);
  assert(strcmp(filesystem, "romfs") == 0);
  assert(flags == MS_RDONLY);
  assert(data == NULL);

  g_mount_calls++;
  if (g_mount_calls == 1)
    {
      errno = EIO;
      return -1;
    }

  return 0;
}

void _pyruntime_early_init(void)
{
  g_early_init_calls++;
}

int py_bytesmain(int argc, char *argv[])
{
  assert(argc == 3);
  assert(strcmp(argv[0], "python") == 0);
  assert(strcmp(argv[2], "pass") == 0);
  g_python_calls++;
  return 10 + g_python_calls;
}

int main(void)
{
  char *argv[] =
  {
    "python", "-c", "pass", NULL
  };

  int ret;

  /* A mount failure retains only the successful RAM-disk registration. */

  ret = python_worker_main(3, argv);
  assert(ret == 1);
  assert(g_prepare_calls == 1);
  assert(g_image_calls == 1);
  assert(g_boardctl_calls == 1);
  assert(g_mount_calls == 1);
  assert(g_early_init_calls == 0);
  assert(g_python_calls == 0);

  /* Retry the failed stage without duplicate device registration. */

  ret = python_worker_main(3, argv);
  assert(ret == 11);
  assert(g_prepare_calls == 2);
  assert(g_image_calls == 1);
  assert(g_boardctl_calls == 1);
  assert(g_mount_calls == 2);
  assert(g_early_init_calls == 1);
  assert(g_python_calls == 1);

  /* A mounted ROMFS persists and needs no procfs lookup or mount call. */

  ret = python_worker_main(3, argv);
  assert(ret == 12);
  assert(g_prepare_calls == 3);
  assert(g_image_calls == 1);
  assert(g_boardctl_calls == 1);
  assert(g_mount_calls == 2);
  assert(g_early_init_calls == 2);
  assert(g_python_calls == 2);

  puts("PASS: persistent CPython ROMFS mount state");
  return 0;
}
