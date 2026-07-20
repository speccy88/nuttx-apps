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
#include <wchar.h>

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
static int g_tmpfs_validate_calls;
static int g_image_calls;
static int g_register_calls;
static int g_mount_calls;
static int g_early_init_calls;
static int g_no_site_calls;
static int g_fixed_path_calls;
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

int board_cpython_tmpfs_validate(void)
{
  g_tmpfs_validate_calls++;
  return 0;
}

int board_cpython_romfs_image(const uint8_t **image, size_t *length)
{
  g_image_calls++;
  *image = g_romfs_image;
  *length = sizeof(g_romfs_image);
  return 0;
}

int board_cpython_romdisk_register(int minor, const uint8_t *image,
                                   uint32_t nsectors, uint16_t sectsize)
{
  assert(minor == 1);
  assert(nsectors == 1);
  assert(sectsize == 512);
  assert(image == g_romfs_image);
  g_register_calls++;
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

void py_p2_set_default_no_site(void)
{
  g_no_site_calls++;
}

void py_p2_set_fixed_path_config(const wchar_t *writable_path)
{
  assert(wcscmp(writable_path, L"/tmp") == 0);
  g_fixed_path_calls++;
}

int py_bytesmain(int argc, char *argv[])
{
  assert(argc == 3);
  assert(strcmp(argv[0], "python") == 0);
  assert(strcmp(argv[2], "pass") == 0);
  assert(g_no_site_calls == g_python_calls + 1);
  assert(g_fixed_path_calls == g_python_calls + 1);
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
  assert(g_tmpfs_validate_calls == 1);
  assert(g_image_calls == 1);
  assert(g_register_calls == 1);
  assert(g_mount_calls == 1);
  assert(g_early_init_calls == 0);
  assert(g_no_site_calls == 0);
  assert(g_fixed_path_calls == 0);
  assert(g_python_calls == 0);

  /* Retry the failed stage without duplicate device registration. */

  ret = python_worker_main(3, argv);
  assert(ret == 11);
  assert(g_prepare_calls == 2);
  assert(g_tmpfs_validate_calls == 2);
  assert(g_image_calls == 1);
  assert(g_register_calls == 1);
  assert(g_mount_calls == 2);
  assert(g_early_init_calls == 1);
  assert(g_no_site_calls == 1);
  assert(g_fixed_path_calls == 1);
  assert(g_python_calls == 1);

  /* A mounted ROMFS persists and needs no procfs lookup or mount call. */

  ret = python_worker_main(3, argv);
  assert(ret == 12);
  assert(g_prepare_calls == 3);
  assert(g_tmpfs_validate_calls == 3);
  assert(g_image_calls == 1);
  assert(g_register_calls == 1);
  assert(g_mount_calls == 2);
  assert(g_early_init_calls == 2);
  assert(g_no_site_calls == 2);
  assert(g_fixed_path_calls == 2);
  assert(g_python_calls == 2);

  puts("PASS: persistent CPython ROMFS mount state");
  return 0;
}
