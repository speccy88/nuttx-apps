/****************************************************************************
 * apps/system/p2bank/p2bank.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/p2_ec32mb_bank.h>
#include <arch/board/p2_ec32mb_psram.h>

#include "p2bank.h"
#include "p2bank_logic.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2bank_error(FAR const char *stage, int error)
{
  fprintf(stderr, "P2BANK:ERROR:STAGE=%s:CODE=%d\n", stage, error);
  return EXIT_FAILURE;
}

static uint32_t p2bank_crc_part(uint32_t state, FAR const uint8_t *buffer,
                                size_t length)
{
  size_t offset;

  for (offset = 0; offset < length; offset++)
    {
      state = p2_bank_crc32_byte(state, buffer[offset]);
    }

  return state;
}

static int p2bank_read_file(int descriptor, FAR uint8_t *buffer,
                            size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t nread = read(descriptor, buffer + offset, length - offset);

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (nread == 0)
        {
          return -ENODATA;
        }

      offset += nread;
    }

  return 0;
}

static int p2bank_stage_file(int descriptor, uint32_t image_size,
                             FAR uint8_t *buffer, FAR uint32_t *crc_out)
{
  uint32_t offset = 0;
  uint32_t state = UINT32_MAX;

  while (offset < image_size)
    {
      size_t chunk = image_size - offset;
      ssize_t transferred;
      int ret;

      if (chunk > CONFIG_SYSTEM_P2BANK_BUFSIZE)
        {
          chunk = CONFIG_SYSTEM_P2BANK_BUFSIZE;
        }

      ret = p2bank_read_file(descriptor, buffer, chunk);
      if (ret < 0)
        {
          return ret;
        }

      state = p2bank_crc_part(state, buffer, chunk);
      transferred = p2_psram_transfer(P2_PSRAM_OPERATION_WRITE,
                                      P2_BANK_PSRAM_STAGE_ADDRESS + offset,
                                      buffer, chunk, 0);
      if (transferred < 0)
        {
          return (int)transferred;
        }

      if ((size_t)transferred != chunk)
        {
          return -EIO;
        }

      offset += chunk;
    }

  /* Reject a file which grew after fstat(), rather than silently launching a
   * prefix.  A shrinking file is caught by p2bank_read_file().
   */

  for (; ; )
    {
      ssize_t nread = read(descriptor, buffer, 1);

      if (nread < 0 && errno == EINTR)
        {
          continue;
        }

      if (nread < 0)
        {
          return -errno;
        }

      if (nread != 0)
        {
          return -EFBIG;
        }

      break;
    }

  *crc_out = state ^ UINT32_MAX;
  return 0;
}

static int p2bank_verify_psram(uint32_t image_size, uint32_t expected_crc,
                               FAR uint8_t *buffer)
{
  uint32_t offset = 0;
  uint32_t state = UINT32_MAX;

  while (offset < image_size)
    {
      size_t chunk = image_size - offset;
      ssize_t transferred;

      if (chunk > CONFIG_SYSTEM_P2BANK_BUFSIZE)
        {
          chunk = CONFIG_SYSTEM_P2BANK_BUFSIZE;
        }

      transferred = p2_psram_transfer(P2_PSRAM_OPERATION_READ,
                                      P2_BANK_PSRAM_STAGE_ADDRESS + offset,
                                      buffer, chunk, 0);
      if (transferred < 0)
        {
          return (int)transferred;
        }

      if ((size_t)transferred != chunk)
        {
          return -EIO;
        }

      state = p2bank_crc_part(state, buffer, chunk);
      offset += chunk;
    }

  return (state ^ UINT32_MAX) == expected_crc ? 0 : -EBADMSG;
}

static int p2bank_write_handoff(uint32_t image_size, uint32_t image_crc,
                                FAR const char *script)
{
  FAR struct p2_bank_handoff_s *destination =
    (FAR struct p2_bank_handoff_s *)(uintptr_t)P2_BANK_HANDOFF_ADDRESS;
  struct p2_bank_handoff_s handoff;
  size_t script_length = strlen(script);

  memset(&handoff, 0, sizeof(handoff));
  handoff.magic = P2_BANK_HANDOFF_MAGIC;
  handoff.version = P2_BANK_HANDOFF_VERSION;
  handoff.header_size = sizeof(handoff);
  handoff.bank_size = image_size;
  handoff.bank_crc32 = image_crc;
  memcpy(handoff.script_path, script, script_length + 1);
  handoff.handoff_crc32 = p2_bank_handoff_crc32(&handoff);

  memcpy(destination, &handoff, sizeof(handoff));
  __asm__ __volatile__("" : : : "memory");
  return p2_bank_handoff_valid(destination) ? 0 : -EIO;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int p2bank_launch(bool berry_alias, int argc, FAR char *argv[])
{
  FAR const char *bank_path;
  FAR const char *script_path;
  struct stat status;
  FAR uint8_t *buffer = NULL;
  uint32_t image_size;
  uint32_t image_crc;
  int descriptor = -1;
  bool psram_reserved = false;
  int ret;

  if ((berry_alias && argc > 2) || (!berry_alias && argc > 3))
    {
      fprintf(stderr, berry_alias ? "Usage: berry [script-path]\n" :
              "Usage: p2bank [bank-file [script-path]]\n");
      return p2bank_error("ARGS", -EINVAL);
    }

  bank_path = berry_alias || argc < 2 ? CONFIG_SYSTEM_P2BANK_DEFAULT_BANK :
                                       argv[1];
  script_path = berry_alias ? (argc == 2 ? argv[1] : "") :
                              (argc == 3 ? argv[2] : "");

  if (!p2bank_path_safe(bank_path, "/mnt/flash/", 255))
    {
      return p2bank_error("BANK_PATH", -EINVAL);
    }

  if (script_path[0] != '\0' &&
      !p2bank_path_safe(script_path, "/mnt/sd/",
                        P2_BANK_SCRIPT_PATH_MAX - 1u))
    {
      return p2bank_error("SCRIPT_PATH", -EINVAL);
    }

  descriptor = open(bank_path, O_RDONLY);
  if (descriptor < 0)
    {
      return p2bank_error("OPEN", -errno);
    }

  if (fstat(descriptor, &status) < 0)
    {
      ret = p2bank_error("STAT", -errno);
      goto out;
    }

  if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
      (uint64_t)status.st_size > P2_BANK_HUB_IMAGE_LIMIT ||
      ((uint64_t)status.st_size &
       (P2_PSRAM_NATURAL_WORD_BYTES - 1u)) != 0)
    {
      ret = p2bank_error("SIZE", -EFBIG);
      goto out;
    }

  image_size = (uint32_t)status.st_size;
  buffer = malloc(CONFIG_SYSTEM_P2BANK_BUFSIZE);
  if (buffer == NULL)
    {
      ret = p2bank_error("ALLOC", -ENOMEM);
      goto out;
    }

  ret = p2_psram_bank_reserve();
  if (ret < 0)
    {
      ret = p2bank_error("RESERVE", ret);
      goto out;
    }

  psram_reserved = true;

  printf("P2BANK:START:BANK=%s:SCRIPT=%s:BYTES=%" PRIu32
         ":PSRAM=0x%08" PRIX32 "\n",
         bank_path, script_path[0] == '\0' ? "REPL" : script_path,
         image_size, P2_BANK_PSRAM_STAGE_ADDRESS);

  ret = p2bank_stage_file(descriptor, image_size, buffer, &image_crc);
  if (ret < 0)
    {
      ret = p2bank_error("STAGE", ret);
      goto out;
    }

  close(descriptor);
  descriptor = -1;
  printf("P2BANK:STAGED:BYTES=%" PRIu32 ":CRC32=%08" PRIX32 "\n",
         image_size, image_crc);

  ret = p2bank_verify_psram(image_size, image_crc, buffer);
  if (ret < 0)
    {
      ret = p2bank_error("VERIFY", ret);
      goto out;
    }

  printf("P2BANK:VERIFIED:BYTES=%" PRIu32 ":CRC32=%08" PRIX32 "\n",
         image_size, image_crc);
  ret = p2bank_write_handoff(image_size, image_crc, script_path);
  if (ret < 0)
    {
      ret = p2bank_error("HANDOFF", ret);
      goto out;
    }

  printf("P2BANK:SWITCHING:HUB_END=0x%08" PRIX32
         ":HANDOFF=0x%08" PRIX32 "\n",
         P2_BANK_HUB_IMAGE_LIMIT, P2_BANK_HANDOFF_ADDRESS);
  fflush(stdout);

  ret = p2_psram_boot_bank(P2_BANK_PSRAM_STAGE_ADDRESS, image_size);
  ret = p2bank_error("SWITCH", ret < 0 ? ret : -EIO);

out:
  if (descriptor >= 0)
    {
      close(descriptor);
    }

  if (psram_reserved)
    {
      int release_ret = p2_psram_bank_release();

      if (release_ret < 0)
        {
          fprintf(stderr, "P2BANK:ERROR:STAGE=RELEASE:CODE=%d\n",
                  release_ret);
          ret = EXIT_FAILURE;
        }
    }

  free(buffer);
  return ret;
}
