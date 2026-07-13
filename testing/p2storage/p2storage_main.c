/****************************************************************************
 * apps/testing/p2storage/p2storage_main.c
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

#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/fs/ioctl.h>

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
#  include "fsutils/mkfatfs.h"
#  include "fsutils/mksmartfs.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2STORAGE_DESTRUCTIVE_MAGIC "P2STORAGE-I-ACCEPT-DATA-LOSS-V1"
#define P2STORAGE_RECORD_SIZE       CONFIG_TESTING_P2STORAGE_RECORD_SIZE
#define P2STORAGE_RECORD_DATA_SIZE  252
#define P2STORAGE_INTERRUPT_SIZE    128
#define P2STORAGE_FULL_CHUNK_SIZE   4096
#define P2STORAGE_FULL_FSYNC_SIZE   (64 * 1024)
#define P2STORAGE_FULL_PROGRESS_SIZE (1024 * 1024)
#define P2STORAGE_STREAM_SIZE       CONFIG_TESTING_P2STORAGE_STREAM_SIZE

#define P2STORAGE_FLASH_FILE        "p2record.bin"
#define P2STORAGE_SD_FILE           "p2record.bin"
#define P2STORAGE_RENAME_DIR        "p2dir"
#define P2STORAGE_RENAME_SOURCE     "p2dir/source.tmp"
#define P2STORAGE_RENAME_DEST       "p2dir/renamed.bin"
#define P2STORAGE_FLASH_FULL_FILE   "p2full.bin"
#define P2STORAGE_INTERRUPT_FILE    "p2interrupt.tmp"
#define P2STORAGE_SCRATCH_FILE      "p2scrtch.bin"

#if CONFIG_TESTING_P2STORAGE_RECORD_SIZE != 256
#  error "The strict P2 storage protocol requires 256-byte records"
#endif

#if CONFIG_TESTING_P2STORAGE_STREAM_SIZE != 1048576
#  error "The strict P2 storage protocol requires a one-MiB stream"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum p2storage_medium_e
{
  P2STORAGE_MEDIUM_FLASH = 0,
  P2STORAGE_MEDIUM_SD
};

struct p2storage_medium_s
{
  FAR const char *name;
  FAR const char *devpath;
  FAR const char *mountpoint;
  FAR const char *fstype;
  FAR const char *record_name;
  uint8_t tag;
  uint8_t seed;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct p2storage_medium_s g_flash =
{
  .name        = "FLASH",
  .devpath     = CONFIG_TESTING_P2STORAGE_FLASH_DEVPATH,
  .mountpoint  = CONFIG_TESTING_P2STORAGE_FLASH_MOUNTPOINT,
  .fstype      = "smartfs",
  .record_name = P2STORAGE_FLASH_FILE,
  .tag         = 'F',
  .seed        = 0x46
};

static const struct p2storage_medium_s g_sd =
{
  .name        = "SD",
  .devpath     = CONFIG_TESTING_P2STORAGE_SD_DEVPATH,
  .mountpoint  = CONFIG_TESTING_P2STORAGE_SD_MOUNTPOINT,
  .fstype      = "vfat",
  .record_name = P2STORAGE_SD_FILE,
  .tag         = 'S',
  .seed        = 0x53
};

static uint8_t g_io_buffer[P2STORAGE_FULL_CHUNK_SIZE];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int p2storage_fail(FAR const char *stage, int error)
{
  if (error < 0)
    {
      error = -error;
    }

  if (error == 0)
    {
      error = EIO;
    }

  printf("P2STORAGE:FAIL:%s:%d\n", stage, error);
  return EXIT_FAILURE;
}

static int p2storage_errno(void)
{
  return errno > 0 ? -errno : -EIO;
}

static uint32_t p2storage_fnv1a(FAR const uint8_t *data, size_t length)
{
  uint32_t hash = UINT32_C(2166136261);
  size_t i;

  for (i = 0; i < length; i++)
    {
      hash ^= data[i];
      hash *= UINT32_C(16777619);
    }

  return hash;
}

static uint32_t p2storage_fnv1a_part(FAR const uint8_t *data,
                                     size_t length, uint32_t hash)
{
  size_t i;

  for (i = 0; i < length; i++)
    {
      hash ^= data[i];
      hash *= UINT32_C(16777619);
    }

  return hash;
}

static uint8_t p2storage_stream_byte(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence,
  uint32_t offset)
{
  uint8_t sequence_byte = (uint8_t)(sequence >> ((offset & 3u) * 8u));

  return (uint8_t)(medium->seed + (offset & 0xffu) * 37u +
                   sequence_byte);
}

static void p2storage_fill_stream(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence,
  uint32_t offset, FAR uint8_t *buffer, size_t length)
{
  size_t i;

  for (i = 0; i < length; i++)
    {
      buffer[i] = p2storage_stream_byte(medium, sequence,
                                        offset + (uint32_t)i);
    }
}

static void p2storage_put_u32le(FAR uint8_t *dest, uint32_t value)
{
  dest[0] = (uint8_t)value;
  dest[1] = (uint8_t)(value >> 8);
  dest[2] = (uint8_t)(value >> 16);
  dest[3] = (uint8_t)(value >> 24);
}

static uint32_t p2storage_get_u32le(FAR const uint8_t *source)
{
  return (uint32_t)source[0] |
         (uint32_t)source[1] << 8 |
         (uint32_t)source[2] << 16 |
         (uint32_t)source[3] << 24;
}

static uint32_t p2storage_make_record(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence,
  FAR uint8_t record[P2STORAGE_RECORD_SIZE])
{
  static const uint8_t prefix[7] =
  {
    'P', '2', 'S', 'T', 'R', 'G', '1'
  };

  uint32_t checksum;
  size_t i;

  memcpy(record, prefix, sizeof(prefix));
  record[7] = medium->tag;
  p2storage_put_u32le(&record[8], sequence);

  for (i = 12; i < P2STORAGE_RECORD_DATA_SIZE; i++)
    {
      uint8_t sequence_byte =
        (uint8_t)(sequence >> ((i & 3u) * 8u));

      record[i] = (uint8_t)(medium->seed + i * 37u + sequence_byte);
    }

  checksum = p2storage_fnv1a(record, P2STORAGE_RECORD_DATA_SIZE);
  p2storage_put_u32le(&record[P2STORAGE_RECORD_DATA_SIZE], checksum);
  return checksum;
}

static int p2storage_parse_sequence(FAR const char *text,
                                    FAR uint32_t *sequence)
{
  uint32_t value = 0;
  unsigned int i;

  if (text == NULL || strlen(text) != 8)
    {
      return -EINVAL;
    }

  for (i = 0; i < 8; i++)
    {
      unsigned int digit;

      if (text[i] >= '0' && text[i] <= '9')
        {
          digit = (unsigned int)(text[i] - '0');
        }
      else if (text[i] >= 'A' && text[i] <= 'F')
        {
          digit = (unsigned int)(text[i] - 'A') + 10;
        }
      else
        {
          return -EINVAL;
        }

      value = (value << 4) | digit;
    }

  *sequence = value;
  return 0;
}

static int p2storage_make_path(FAR char *path, size_t path_size,
                               FAR const struct p2storage_medium_s *medium,
                               FAR const char *name)
{
  int length;

  length = snprintf(path, path_size, "%s/%s", medium->mountpoint, name);
  if (length < 0 || (size_t)length >= path_size)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

static int p2storage_make_mountpoint(
  FAR const struct p2storage_medium_s *medium)
{
  char parent[PATH_MAX];
  FAR char *slash;
  size_t length;

  length = strlen(medium->mountpoint);
  if (length == 0 || length >= sizeof(parent))
    {
      return -ENAMETOOLONG;
    }

  memcpy(parent, medium->mountpoint, length + 1);
  slash = strrchr(parent, '/');
  if (slash != NULL && slash != parent)
    {
      *slash = '\0';
      if (mkdir(parent, 0777) < 0 && errno != EEXIST)
        {
          return p2storage_errno();
        }
    }

  if (mkdir(medium->mountpoint, 0777) < 0 && errno != EEXIST)
    {
      return p2storage_errno();
    }

  return 0;
}

static int p2storage_mount(FAR const struct p2storage_medium_s *medium)
{
  int ret;

  ret = p2storage_make_mountpoint(medium);
  if (ret < 0)
    {
      return ret;
    }

  if (mount(medium->devpath, medium->mountpoint, medium->fstype,
            0, NULL) < 0)
    {
      return p2storage_errno();
    }

  return 0;
}

static int p2storage_unmount(FAR const struct p2storage_medium_s *medium)
{
  return umount(medium->mountpoint) < 0 ? p2storage_errno() : 0;
}

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
static int p2storage_ensure_unmounted(
  FAR const struct p2storage_medium_s *medium)
{
  if (umount(medium->mountpoint) == 0)
    {
      return 0;
    }

  if (errno == EINVAL || errno == ENOENT)
    {
      return 0;
    }

  return p2storage_errno();
}

static int p2storage_write_all(int fd, FAR const uint8_t *buffer,
                               size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t nwritten = write(fd, &buffer[offset], length - offset);

      if (nwritten < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return p2storage_errno();
        }

      if (nwritten == 0)
        {
          return -EIO;
        }

      offset += (size_t)nwritten;
    }

  return 0;
}
#endif

static int p2storage_read_exact(int fd, FAR uint8_t *buffer, size_t length)
{
  size_t offset = 0;
  uint8_t extra;
  ssize_t nread;

  while (offset < length)
    {
      nread = read(fd, &buffer[offset], length - offset);

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return p2storage_errno();
        }

      if (nread == 0)
        {
          return -EIO;
        }

      offset += (size_t)nread;
    }

  do
    {
      nread = read(fd, &extra, 1);
    }
  while (nread < 0 && errno == EINTR);

  if (nread < 0)
    {
      return p2storage_errno();
    }

  if (nread != 0)
    {
      return -EFBIG;
    }

  return 0;
}

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
static int p2storage_write_record(
  FAR const struct p2storage_medium_s *medium, FAR const char *name,
  uint32_t sequence, FAR uint32_t *checksum)
{
  uint8_t record[P2STORAGE_RECORD_SIZE];
  char path[PATH_MAX];
  int fd;
  int ret;

  ret = p2storage_make_path(path, sizeof(path), medium, name);
  if (ret < 0)
    {
      return ret;
    }

  *checksum = p2storage_make_record(medium, sequence, record);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  ret = p2storage_write_all(fd, record, sizeof(record));
  if (ret == 0 && fsync(fd) < 0)
    {
      ret = p2storage_errno();
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  return ret;
}

static int p2storage_write_stream(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence,
  FAR uint32_t *checksum)
{
  char path[PATH_MAX];
  uint32_t offset;
  uint32_t hash = UINT32_C(2166136261);
  int fd;
  int ret;

  ret = p2storage_make_path(path, sizeof(path), medium,
                            medium->record_name);
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  for (offset = 0; offset < P2STORAGE_STREAM_SIZE;
       offset += sizeof(g_io_buffer))
    {
      p2storage_fill_stream(medium, sequence, offset, g_io_buffer,
                            sizeof(g_io_buffer));
      hash = p2storage_fnv1a_part(g_io_buffer, sizeof(g_io_buffer), hash);
      ret = p2storage_write_all(fd, g_io_buffer, sizeof(g_io_buffer));
      if (ret < 0)
        {
          break;
        }
    }

  if (ret == 0 && fsync(fd) < 0)
    {
      ret = p2storage_errno();
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret == 0)
    {
      *checksum = hash;
    }

  return ret;
}
#endif

static int p2storage_verify_record(
  FAR const struct p2storage_medium_s *medium, FAR const char *name,
  uint32_t sequence, FAR uint32_t *checksum)
{
  uint8_t expected[P2STORAGE_RECORD_SIZE];
  uint8_t actual[P2STORAGE_RECORD_SIZE];
  uint32_t actual_checksum;
  char path[PATH_MAX];
  int fd;
  int ret;

  ret = p2storage_make_path(path, sizeof(path), medium, name);
  if (ret < 0)
    {
      return ret;
    }

  *checksum = p2storage_make_record(medium, sequence, expected);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  ret = p2storage_read_exact(fd, actual, sizeof(actual));
  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret < 0)
    {
      return ret;
    }

  actual_checksum = p2storage_fnv1a(actual, P2STORAGE_RECORD_DATA_SIZE);
  if (actual_checksum !=
      p2storage_get_u32le(&actual[P2STORAGE_RECORD_DATA_SIZE]) ||
      memcmp(actual, expected, sizeof(actual)) != 0)
    {
      return -EIO;
    }

  return 0;
}

static int p2storage_verify_stream(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence,
  FAR uint32_t *checksum)
{
  struct stat file_stat;
  char path[PATH_MAX];
  uint32_t offset;
  uint32_t hash = UINT32_C(2166136261);
  int fd;
  int ret = 0;

  ret = p2storage_make_path(path, sizeof(path), medium,
                            medium->record_name);
  if (ret < 0)
    {
      return ret;
    }

  if (stat(path, &file_stat) < 0)
    {
      return p2storage_errno();
    }

  if (file_stat.st_size != P2STORAGE_STREAM_SIZE)
    {
      return -EFBIG;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  for (offset = 0; offset < P2STORAGE_STREAM_SIZE;
       offset += sizeof(g_io_buffer))
    {
      size_t i;
      size_t received = 0;

      while (received < sizeof(g_io_buffer))
        {
          ssize_t nread = read(fd, &g_io_buffer[received],
                               sizeof(g_io_buffer) - received);

          if (nread < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              ret = p2storage_errno();
              goto out;
            }

          if (nread == 0)
            {
              ret = -EIO;
              goto out;
            }

          received += (size_t)nread;
        }

      for (i = 0; i < sizeof(g_io_buffer); i++)
        {
          if (g_io_buffer[i] !=
              p2storage_stream_byte(medium, sequence,
                                    offset + (uint32_t)i))
            {
              ret = -EIO;
              goto out;
            }
        }

      hash = p2storage_fnv1a_part(g_io_buffer, sizeof(g_io_buffer), hash);
    }

out:
  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret == 0)
    {
      *checksum = hash;
    }

  return ret;
}

static int p2storage_probe_medium(
  FAR const struct p2storage_medium_s *medium)
{
  struct geometry geometry;
  int fd;

  fd = open(medium->devpath, O_RDONLY);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  memset(&geometry, 0, sizeof(geometry));
  if (ioctl(fd, BIOC_GEOMETRY,
            (unsigned long)((uintptr_t)&geometry)) < 0)
    {
      int ret = p2storage_errno();
      close(fd);
      return ret;
    }

  if (close(fd) < 0)
    {
      return p2storage_errno();
    }

  if (!geometry.geo_available || !geometry.geo_writeenabled ||
      geometry.geo_nsectors <= 0 || geometry.geo_sectorsize <= 0)
    {
      return -ENODEV;
    }

  printf("P2STORAGE:PROBE:%s:DEV=%s:AVAILABLE=1:WRITE=1:"
         "SECTORS=%" PRIuMAX ":SECTORSIZE=%" PRIuMAX ":PASS\n",
         medium->name, medium->devpath,
         (uintmax_t)geometry.geo_nsectors,
         (uintmax_t)geometry.geo_sectorsize);
  return 0;
}

static int p2storage_probe(void)
{
  int ret;

  ret = p2storage_probe_medium(&g_flash);
  if (ret < 0)
    {
      return ret;
    }

  return p2storage_probe_medium(&g_sd);
}

static int p2storage_verify_persistence(
  FAR const struct p2storage_medium_s *medium, uint32_t sequence)
{
  bool premounted = false;
  uint32_t checksum;
  int ret;
  int unmount_ret;

#ifdef CONFIG_TESTING_P2STORAGE_FLASH_PREMOUNTED
  premounted = medium == &g_flash;
#endif

  if (!premounted)
    {
      ret = p2storage_mount(medium);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = p2storage_verify_stream(medium, sequence, &checksum);
  unmount_ret = premounted ? 0 : p2storage_unmount(medium);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  if (ret < 0)
    {
      return ret;
    }

  printf("P2STORAGE:%s:PERSISTENCE:SEQUENCE=%08" PRIX32
         ":BYTES=%u:FNV1A=%08" PRIX32 ":PASS\n",
         medium->name, sequence, P2STORAGE_STREAM_SIZE, checksum);
  return 0;
}

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
static int p2storage_auth(FAR const char *magic)
{
  return magic != NULL &&
         strcmp(magic, P2STORAGE_DESTRUCTIVE_MAGIC) == 0 ? 0 : -EACCES;
}

static int p2storage_format(FAR const struct p2storage_medium_s *medium)
{
  int ret;

  ret = p2storage_make_mountpoint(medium);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_ensure_unmounted(medium);
  if (ret < 0)
    {
      return ret;
    }

  if (medium == &g_flash)
    {
#ifdef CONFIG_SMARTFS_MULTI_ROOT_DIRS
      if (mksmartfs(medium->devpath, 0, 1) < 0)
#else
      if (mksmartfs(medium->devpath, 0) < 0)
#endif
        {
          return p2storage_errno();
        }
    }
  else
    {
      struct fat_format_s format = FAT_FORMAT_INITIALIZER;
      static const uint8_t label[11] =
      {
        'P', '2', 'S', 'T', 'O', 'R', 'A', 'G', 'E', ' ', ' '
      };

      memcpy(format.ff_volumelabel, label, sizeof(label));
      format.ff_fattype = 32;
      format.ff_volumeid = UINT32_C(0x50325344);
      if (mkfatfs(medium->devpath, &format) < 0)
        {
          return p2storage_errno();
        }
    }

  ret = p2storage_mount(medium);
  if (ret == 0)
    {
      ret = p2storage_unmount(medium);
    }

  if (ret == 0)
    {
      printf("P2STORAGE:%s:FORMAT:PASS\n", medium->name);
    }

  return ret;
}

static int p2storage_roundtrip(
  FAR const struct p2storage_medium_s *medium, FAR const char *name,
  uint32_t sequence, FAR uint32_t *checksum)
{
  int ret;
  int unmount_ret;

  ret = p2storage_mount(medium);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_write_record(medium, name, sequence, checksum);
  if (ret == 0)
    {
      ret = p2storage_verify_record(medium, name, sequence, checksum);
    }

  unmount_ret = p2storage_unmount(medium);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  return ret;
}

static int p2storage_write(FAR const struct p2storage_medium_s *medium,
                           uint32_t sequence)
{
  uint32_t checksum;
  int ret;
  int unmount_ret;

  ret = p2storage_mount(medium);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_write_stream(medium, sequence, &checksum);
  if (ret == 0)
    {
      ret = p2storage_verify_stream(medium, sequence, &checksum);
    }

  unmount_ret = p2storage_unmount(medium);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  if (ret < 0)
    {
      return ret;
    }

  printf("P2STORAGE:%s:WRITE:SEQUENCE=%08" PRIX32
         ":BYTES=%u:FNV1A=%08" PRIX32 ":PASS\n",
         medium->name, sequence, P2STORAGE_STREAM_SIZE, checksum);
  printf("P2STORAGE:READY:RESET=%s:SEQUENCE=%08" PRIX32 "\n",
         medium->name, sequence);
  return 0;
}

static int p2storage_sd_rename_delete(uint32_t sequence, bool markers,
                                      FAR uint32_t *checksum)
{
  char directory[PATH_MAX];
  char source[PATH_MAX];
  char destination[PATH_MAX];
  int ret;
  int unmount_ret;

  ret = p2storage_make_path(directory, sizeof(directory), &g_sd,
                            P2STORAGE_RENAME_DIR);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_make_path(source, sizeof(source), &g_sd,
                            P2STORAGE_RENAME_SOURCE);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_make_path(destination, sizeof(destination), &g_sd,
                            P2STORAGE_RENAME_DEST);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_mount(&g_sd);
  if (ret < 0)
    {
      return ret;
    }

  if (unlink(source) < 0 && errno != ENOENT)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (unlink(destination) < 0 && errno != ENOENT)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (rmdir(directory) < 0 && errno != ENOENT)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (mkdir(directory, 0777) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (markers)
    {
      printf("P2STORAGE:SD:MKDIR:SEQUENCE=%08" PRIX32 ":PASS\n",
             sequence);
    }

  ret = p2storage_write_record(&g_sd, P2STORAGE_RENAME_SOURCE,
                               sequence, checksum);
  if (ret < 0)
    {
      goto out;
    }

  if (rename(source, destination) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  ret = p2storage_verify_record(&g_sd, P2STORAGE_RENAME_DEST,
                                sequence, checksum);
  if (ret < 0)
    {
      goto out;
    }

  if (access(source, F_OK) == 0 || errno != ENOENT)
    {
      ret = -EEXIST;
      goto out;
    }

  if (markers)
    {
      printf("P2STORAGE:SD:RENAME:SEQUENCE=%08" PRIX32 ":PASS\n",
             sequence);
    }

  if (unlink(destination) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (access(destination, F_OK) == 0 || errno != ENOENT)
    {
      ret = -EEXIST;
      goto out;
    }

  if (rmdir(directory) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (markers)
    {
      printf("P2STORAGE:SD:DELETE:SEQUENCE=%08" PRIX32 ":PASS\n",
             sequence);
    }

out:
  unmount_ret = p2storage_unmount(&g_sd);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  return ret;
}

static int p2storage_flash_cycle(uint32_t base_sequence)
{
  uint32_t checksum;
  unsigned int i;
  int ret;

  for (i = 0; i < CONFIG_TESTING_P2STORAGE_FLASH_CYCLE_COUNT; i++)
    {
      uint32_t sequence = base_sequence + i;

      ret = p2storage_roundtrip(&g_flash, P2STORAGE_SCRATCH_FILE,
                                sequence, &checksum);
      if (ret < 0)
        {
          return ret;
        }

      printf("P2STORAGE:FLASH:CYCLE:ITERATION=%u:SEQUENCE=%08" PRIX32
             ":FNV1A=%08" PRIX32 ":PASS\n",
             i + 1, sequence, checksum);
    }

  printf("P2STORAGE:FLASH:CYCLE:COUNT=%u:PASS\n",
         CONFIG_TESTING_P2STORAGE_FLASH_CYCLE_COUNT);
  return 0;
}

static int p2storage_sd_stress(uint32_t base_sequence)
{
  uint32_t checksum;
  unsigned int i;
  int ret;

  for (i = 0; i < CONFIG_TESTING_P2STORAGE_SD_STRESS_COUNT; i++)
    {
      uint32_t sequence = base_sequence + i;

      ret = p2storage_sd_rename_delete(sequence, false, &checksum);
      if (ret < 0)
        {
          return ret;
        }

      printf("P2STORAGE:SD:STRESS:ITERATION=%u:SEQUENCE=%08" PRIX32
             ":FNV1A=%08" PRIX32 ":PASS\n",
             i + 1, sequence, checksum);
    }

  printf("P2STORAGE:SD:STRESS:COUNT=%u:PASS\n",
         CONFIG_TESTING_P2STORAGE_SD_STRESS_COUNT);
  return 0;
}

static int p2storage_alternate(uint32_t base_sequence)
{
  uint32_t checksum;
  unsigned int i;
  int ret;

  for (i = 0; i < CONFIG_TESTING_P2STORAGE_BUS_ALTERNATE_COUNT; i++)
    {
      uint32_t sequence = base_sequence + i;

      ret = p2storage_roundtrip(&g_flash, P2STORAGE_SCRATCH_FILE,
                                sequence, &checksum);
      if (ret < 0)
        {
          return ret;
        }

      ret = p2storage_roundtrip(&g_sd, P2STORAGE_SCRATCH_FILE,
                                sequence, &checksum);
      if (ret < 0)
        {
          return ret;
        }

      printf("P2STORAGE:BUS:ITERATION=%u:FLASH=PASS:SD=PASS\n", i + 1);
    }

  printf("P2STORAGE:BUS:ALTERNATE:COUNT=%u:PASS\n",
         CONFIG_TESTING_P2STORAGE_BUS_ALTERNATE_COUNT);
  return 0;
}

static void p2storage_fill_full_buffer(uint32_t base_sequence,
                                       uint32_t chunk)
{
  unsigned int i;

  for (i = 0; i < P2STORAGE_FULL_CHUNK_SIZE / P2STORAGE_RECORD_SIZE; i++)
    {
      p2storage_make_record(&g_flash,
                            base_sequence + chunk *
                            (P2STORAGE_FULL_CHUNK_SIZE /
                             P2STORAGE_RECORD_SIZE) + i,
                            &g_io_buffer[i * P2STORAGE_RECORD_SIZE]);
    }
}

static int p2storage_flash_full(uint32_t sequence)
{
  uint8_t expected[P2STORAGE_RECORD_SIZE];
  uint8_t actual[P2STORAGE_RECORD_SIZE];
  struct stat file_stat;
  char path[PATH_MAX];
  uintmax_t total = 0;
  uintmax_t next_progress = P2STORAGE_FULL_PROGRESS_SIZE;
  size_t unsynced = 0;
  uint32_t chunk = 0;
  uint32_t checksum;
  bool full = false;
  int fd = -1;
  int ret;
  int unmount_ret;

  ret = p2storage_make_path(path, sizeof(path), &g_flash,
                            P2STORAGE_FLASH_FULL_FILE);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_mount(&g_flash);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_verify_stream(&g_flash, sequence, &checksum);
  if (ret < 0)
    {
      goto out;
    }

  if (unlink(path) < 0 && errno != ENOENT)
    {
      ret = p2storage_errno();
      goto out;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  while (total < CONFIG_TESTING_P2STORAGE_FLASH_FULL_MAX_BYTES)
    {
      ssize_t nwritten;

      p2storage_fill_full_buffer(sequence, chunk);
      nwritten = write(fd, g_io_buffer, sizeof(g_io_buffer));
      if (nwritten < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == ENOSPC)
            {
              full = true;
              break;
            }

          ret = p2storage_errno();
          goto out;
        }

      if (nwritten == 0)
        {
          ret = -EIO;
          goto out;
        }

      chunk++;
      total += (uintmax_t)nwritten;
      unsynced += (size_t)nwritten;

      while (total >= next_progress)
        {
          printf("P2STORAGE:FLASH:FULL:PROGRESS:SEQUENCE=%08" PRIX32
                 ":BYTES=%" PRIuMAX "\n",
                 sequence, next_progress);
          next_progress += P2STORAGE_FULL_PROGRESS_SIZE;
        }

      if (unsynced >= P2STORAGE_FULL_FSYNC_SIZE ||
          (size_t)nwritten != sizeof(g_io_buffer))
        {
          if (fsync(fd) < 0)
            {
              if (errno == ENOSPC)
                {
                  full = true;
                  break;
                }

              ret = p2storage_errno();
              goto out;
            }

          unsynced = 0;
        }
    }

  if (!full && unsynced > 0 && fsync(fd) < 0)
    {
      if (errno == ENOSPC)
        {
          full = true;
        }
      else
        {
          ret = p2storage_errno();
          goto out;
        }
    }

  if (!full || total == 0)
    {
      ret = -EFBIG;
      goto out;
    }

  if (close(fd) < 0 && errno != ENOSPC)
    {
      fd = -1;
      ret = p2storage_errno();
      goto out;
    }

  fd = -1;
  if (stat(path, &file_stat) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (file_stat.st_size <= 0)
    {
      ret = -EIO;
      goto out;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  p2storage_make_record(&g_flash, sequence, expected);
  if (read(fd, actual, sizeof(actual)) != sizeof(actual) ||
      memcmp(actual, expected, sizeof(actual)) != 0)
    {
      ret = -EIO;
      goto out;
    }

  if (close(fd) < 0)
    {
      fd = -1;
      ret = p2storage_errno();
      goto out;
    }

  fd = -1;
  if (unlink(path) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  ret = p2storage_verify_stream(&g_flash, sequence, &checksum);

  if (ret < 0)
    {
      goto out;
    }

  ret = 0;

out:
  if (fd >= 0)
    {
      close(fd);
    }

  if (ret < 0)
    {
      unlink(path);
    }

  unmount_ret = p2storage_unmount(&g_flash);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  if (ret == 0)
    {
      printf("P2STORAGE:FLASH:FULL:SEQUENCE=%08" PRIX32
             ":BYTES=%" PRIuMAX ":ENOSPC=1:PASS\n",
             sequence, total);
    }

  return ret;
}

static int p2storage_flash_interrupt_arm(uint32_t sequence)
{
  uint8_t record[P2STORAGE_RECORD_SIZE];
  char pending_path[PATH_MAX];
  uint32_t checksum;
  unsigned int elapsed;
  int fd = -1;
  int ret;

  ret = p2storage_make_path(pending_path, sizeof(pending_path), &g_flash,
                            P2STORAGE_INTERRUPT_FILE);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_mount(&g_flash);
  if (ret < 0)
    {
      return ret;
    }

  if (unlink(pending_path) < 0 && errno != ENOENT)
    {
      ret = p2storage_errno();
      goto out;
    }

  ret = p2storage_verify_stream(&g_flash, sequence, &checksum);
  if (ret < 0)
    {
      goto out;
    }

  p2storage_make_record(&g_flash, sequence + 1, record);
  fd = open(pending_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  ret = p2storage_write_all(fd, record, P2STORAGE_INTERRUPT_SIZE);
  if (ret < 0)
    {
      goto out;
    }

  printf("P2STORAGE:FLASH:INTERRUPT:ARMED:BASE_SEQUENCE=%08" PRIX32
         ":PENDING_SEQUENCE=%08" PRIX32 ":WRITTEN=%u\n",
         sequence, sequence + 1, P2STORAGE_INTERRUPT_SIZE);
  printf("P2STORAGE:READY:POWER-CUT=FLASH:SEQUENCE=%08" PRIX32 "\n",
         sequence);
  fflush(stdout);

  for (elapsed = 0;
       elapsed < CONFIG_TESTING_P2STORAGE_INTERRUPT_HOLD_MSEC;
       elapsed += 100)
    {
      usleep(100000);
    }

  ret = -ETIMEDOUT;

out:
  if (fd >= 0)
    {
      close(fd);
    }

  p2storage_unmount(&g_flash);
  return ret;
}

static int p2storage_flash_interrupt_verify(uint32_t sequence)
{
  uint8_t expected[P2STORAGE_RECORD_SIZE];
  uint8_t actual[P2STORAGE_RECORD_SIZE + 1];
  struct stat file_stat;
  char pending_path[PATH_MAX];
  FAR const char *state;
  uint32_t checksum;
  size_t pending_size = 0;
  int fd = -1;
  int ret;
  int unmount_ret;

  ret = p2storage_make_path(pending_path, sizeof(pending_path), &g_flash,
                            P2STORAGE_INTERRUPT_FILE);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_mount(&g_flash);
  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_verify_stream(&g_flash, sequence, &checksum);
  if (ret < 0)
    {
      goto out;
    }

  if (stat(pending_path, &file_stat) < 0)
    {
      if (errno != ENOENT)
        {
          ret = p2storage_errno();
          goto out;
        }

      state = "ABSENT";
    }
  else
    {
      if (file_stat.st_size < 0 ||
          file_stat.st_size > P2STORAGE_INTERRUPT_SIZE)
        {
          ret = -EIO;
          goto out;
        }

      fd = open(pending_path, O_RDONLY);
      if (fd < 0)
        {
          ret = p2storage_errno();
          goto out;
        }

      pending_size = (size_t)file_stat.st_size;
      ret = p2storage_read_exact(fd, actual, pending_size);
      if (ret < 0)
        {
          goto out;
        }

      p2storage_make_record(&g_flash, sequence + 1, expected);
      if (memcmp(actual, expected, pending_size) != 0)
        {
          ret = -EIO;
          goto out;
        }

      if (close(fd) < 0)
        {
          fd = -1;
          ret = p2storage_errno();
          goto out;
        }

      fd = -1;
      state = "PREFIX";
    }

  ret = 0;

out:
  if (fd >= 0)
    {
      close(fd);
    }

  unmount_ret = p2storage_unmount(&g_flash);
  if (ret == 0)
    {
      ret = unmount_ret;
    }

  if (ret == 0)
    {
      printf("P2STORAGE:FLASH:INTERRUPT:PENDING=%s:BYTES=%u:PASS\n",
             state, (unsigned int)pending_size);
      printf("P2STORAGE:FLASH:INTERRUPT:RECOVERY:SEQUENCE=%08" PRIX32
             ":PASS\n", sequence);
    }

  return ret;
}
#endif /* CONFIG_TESTING_P2STORAGE_DESTRUCTIVE */

static void p2storage_usage(FAR const char *progname)
{
  printf("usage: %s probe\n", progname);
  printf("       %s flash-verify 8HEX\n", progname);
  printf("       %s sd-verify 8HEX\n", progname);
#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
  printf("       %s flash-format %s\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s flash-write %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s flash-cycle %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s flash-full %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s flash-interrupt-arm %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s flash-interrupt-verify 8HEX\n", progname);
  printf("       %s sd-format %s\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s sd-write %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s sd-rename-delete %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s sd-stress %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
  printf("       %s alternate %s 8HEX\n", progname,
         P2STORAGE_DESTRUCTIVE_MAGIC);
#endif
}

static bool p2storage_is_destructive_command(FAR const char *command)
{
  return strcmp(command, "flash-format") == 0 ||
         strcmp(command, "flash-write") == 0 ||
         strcmp(command, "flash-cycle") == 0 ||
         strcmp(command, "flash-full") == 0 ||
         strcmp(command, "flash-interrupt-arm") == 0 ||
         strcmp(command, "sd-format") == 0 ||
         strcmp(command, "sd-write") == 0 ||
         strcmp(command, "sd-rename-delete") == 0 ||
         strcmp(command, "sd-stress") == 0 ||
         strcmp(command, "alternate") == 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *terminal;
  uint32_t sequence = 0;
  int ret;

  if (argc < 2)
    {
      p2storage_usage(argv[0]);
      return p2storage_fail("ARGS", -EINVAL);
    }

  /* NSH starts builtin applications asynchronously and may redraw its prompt
   * before this task first runs.  Start on a fresh line so the strict HIL
   * protocol never accepts a marker joined to terminal control text.
   */

  printf("\nP2STORAGE:BEGIN:COMMAND=%s\n", argv[1]);

  if (strcmp(argv[1], "probe") == 0)
    {
      if (argc != 2)
        {
          return p2storage_fail("ARGS", -EINVAL);
        }

      terminal = "PROBE";
      ret = p2storage_probe();
    }
  else if (strcmp(argv[1], "flash-verify") == 0 ||
           strcmp(argv[1], "sd-verify") == 0 ||
           strcmp(argv[1], "flash-interrupt-verify") == 0)
    {
      if (argc != 3 || p2storage_parse_sequence(argv[2], &sequence) < 0)
        {
          return p2storage_fail("SEQUENCE", -EINVAL);
        }

      if (strcmp(argv[1], "flash-verify") == 0)
        {
          terminal = "FLASH-VERIFY";
          ret = p2storage_verify_persistence(&g_flash, sequence);
        }
      else if (strcmp(argv[1], "sd-verify") == 0)
        {
          terminal = "SD-VERIFY";
          ret = p2storage_verify_persistence(&g_sd, sequence);
        }
      else
        {
          terminal = "FLASH-INTERRUPT-VERIFY";
#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
          ret = p2storage_flash_interrupt_verify(sequence);
#else
          ret = -ENOSYS;
#endif
        }
    }
#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
  else if (p2storage_is_destructive_command(argv[1]))
    {
      bool is_format = strcmp(argv[1], "flash-format") == 0 ||
                       strcmp(argv[1], "sd-format") == 0;

      if ((is_format && argc != 3) || (!is_format && argc != 4))
        {
          return p2storage_fail("ARGS", -EINVAL);
        }

      ret = p2storage_auth(argv[2]);
      if (ret < 0)
        {
          return p2storage_fail("AUTH", ret);
        }

      if (!is_format &&
          p2storage_parse_sequence(argv[3], &sequence) < 0)
        {
          return p2storage_fail("SEQUENCE", -EINVAL);
        }

      if (strcmp(argv[1], "flash-format") == 0)
        {
          terminal = "FLASH-FORMAT";
          ret = p2storage_format(&g_flash);
        }
      else if (strcmp(argv[1], "flash-write") == 0)
        {
          terminal = "FLASH-WRITE";
          ret = p2storage_write(&g_flash, sequence);
        }
      else if (strcmp(argv[1], "flash-cycle") == 0)
        {
          terminal = "FLASH-CYCLE";
          ret = p2storage_flash_cycle(sequence);
        }
      else if (strcmp(argv[1], "flash-full") == 0)
        {
          terminal = "FLASH-FULL";
          ret = p2storage_flash_full(sequence);
        }
      else if (strcmp(argv[1], "flash-interrupt-arm") == 0)
        {
          terminal = "FLASH-INTERRUPT-ARM";
          ret = p2storage_flash_interrupt_arm(sequence);
        }
      else if (strcmp(argv[1], "sd-format") == 0)
        {
          terminal = "SD-FORMAT";
          ret = p2storage_format(&g_sd);
        }
      else if (strcmp(argv[1], "sd-write") == 0)
        {
          terminal = "SD-WRITE";
          ret = p2storage_write(&g_sd, sequence);
        }
      else if (strcmp(argv[1], "sd-rename-delete") == 0)
        {
          uint32_t checksum;

          terminal = "SD-RENAME-DELETE";
          ret = p2storage_sd_rename_delete(sequence, true, &checksum);
        }
      else if (strcmp(argv[1], "sd-stress") == 0)
        {
          terminal = "SD-STRESS";
          ret = p2storage_sd_stress(sequence);
        }
      else
        {
          terminal = "ALTERNATE";
          ret = p2storage_alternate(sequence);
        }
    }
#else
  else if (p2storage_is_destructive_command(argv[1]))
    {
      return p2storage_fail("KCONFIG", -ENOSYS);
    }
#endif
  else
    {
      p2storage_usage(argv[0]);
      return p2storage_fail("COMMAND", -EINVAL);
    }

  if (ret < 0)
    {
      return p2storage_fail(terminal, ret);
    }

  printf("P2STORAGE:PASS:%s\n", terminal);
  return EXIT_SUCCESS;
}
