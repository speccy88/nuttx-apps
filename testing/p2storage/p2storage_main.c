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
#  include <nuttx/fs/fs.h>
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

#define P2STORAGE_SD_SECTOR_SIZE        512
#define P2STORAGE_MBR_PARTITION_OFFSET  446
#define P2STORAGE_MBR_SIGNATURE_OFFSET  510
#define P2STORAGE_P2_ROM_MAX_IMAGE_SIZE 507904
#define P2STORAGE_FAT_DIR_ENTRY_SIZE    32
#define P2STORAGE_FAT32_EOC_MIN         UINT32_C(0x0ffffff8)
#define P2STORAGE_FAT32_ENTRY_MASK      UINT32_C(0x0fffffff)

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
#  define P2STORAGE_SD_PARTITION_DEVPATH "/dev/p2sd1"
#  define P2STORAGE_SD_PARTITION_START   UINT32_C(2048)
#  define P2STORAGE_FAT32_PARTITION_TYPE 0x0c
#  define P2STORAGE_FAT32_RESERVED_SECTORS UINT16_C(32)
#  define P2STORAGE_FAT32_FSINFO_SECTOR    UINT16_C(1)
#  define P2STORAGE_FAT32_BACKUP_SECTOR    UINT16_C(6)
#  define P2STORAGE_FAT32_VOLUME_ID        UINT32_C(0x50325344)
#  define P2STORAGE_FAT32_MIN_CLUSTERS     UINT32_C(65524)
#  define P2STORAGE_FAT32_MAX_CLUSTERS     UINT32_C(0x0fffffed)
#endif

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

static uint16_t p2storage_get_u16le(FAR const uint8_t *source)
{
  return (uint16_t)source[0] | (uint16_t)source[1] << 8;
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

static int p2storage_read_all(int fd, FAR uint8_t *buffer, size_t length)
{
  size_t offset = 0;
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

  return 0;
}

static int p2storage_read_exact(int fd, FAR uint8_t *buffer, size_t length)
{
  uint8_t extra;
  ssize_t nread;
  int ret;

  ret = p2storage_read_all(fd, buffer, length);
  if (ret < 0)
    {
      return ret;
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

static int p2storage_sd_read_sector_into(int fd, uint64_t nsectors,
                                         uint64_t sector,
                                         FAR uint8_t *buffer)
{
  uint64_t byte_offset;
  off_t expected;
  off_t position;

  if (sector >= nsectors ||
      sector > UINT64_MAX / P2STORAGE_SD_SECTOR_SIZE)
    {
      return -EINVAL;
    }

  byte_offset = sector * P2STORAGE_SD_SECTOR_SIZE;
  if ((sizeof(off_t) <= sizeof(int32_t) && byte_offset > INT32_MAX) ||
      byte_offset > INT64_MAX)
    {
      return -EOVERFLOW;
    }

  expected = (off_t)byte_offset;
  position = lseek(fd, expected, SEEK_SET);
  if (position < 0)
    {
      return p2storage_errno();
    }

  if (position != expected)
    {
      return -EIO;
    }

  return p2storage_read_all(fd, buffer, P2STORAGE_SD_SECTOR_SIZE);
}

static int p2storage_sd_read_sector(int fd, uint64_t nsectors,
                                    uint64_t sector)
{
  return p2storage_sd_read_sector_into(fd, nsectors, sector,
                                       g_io_buffer);
}

static int p2storage_sd_fat_entry(int fd, uint64_t nsectors,
                                  uint64_t fat_begin, uint32_t fat_sectors,
                                  uint32_t cluster, FAR uint32_t *value)
{
  uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
  uint64_t sector = fat_begin +
                    byte_offset / P2STORAGE_SD_SECTOR_SIZE;
  size_t offset = (size_t)(byte_offset % P2STORAGE_SD_SECTOR_SIZE);
  int ret;

  if (sector < fat_begin || sector >= fat_begin + fat_sectors ||
      offset > P2STORAGE_SD_SECTOR_SIZE - sizeof(uint32_t))
    {
      return -EINVAL;
    }

  ret = p2storage_sd_read_sector(fd, nsectors, sector);
  if (ret < 0)
    {
      return ret;
    }

  *value = p2storage_get_u32le(&g_io_buffer[offset]) &
           P2STORAGE_FAT32_ENTRY_MASK;
  return 0;
}

static int p2storage_sd_rom_verify(void)
{
  static const uint8_t filename[11] =
  {
    '_', 'B', 'O', 'O', 'T', '_', 'P', '2', 'B', 'I', 'X'
  };

  struct geometry geometry;
  FAR const uint8_t *entry;
  FAR const char *fail_stage = "GEOMETRY";
  FAR const char *fail_reason = "IO";
  uint64_t nsectors;
  uint64_t volume_end;
  uint64_t fat_begin;
  uint64_t data_begin;
  uint64_t root_sector;
  uint64_t image_sector;
  uint64_t cluster_bytes;
  uint64_t data_sectors;
  uint32_t partition_start;
  uint32_t partition_sectors;
  uint32_t total_sectors;
  uint32_t fat_sectors;
  uint32_t root_cluster;
  uint32_t cluster_count;
  uint32_t first_cluster = 0;
  uint32_t file_size = 0;
  uint32_t needed_clusters;
  uint32_t needed_sectors;
  uint32_t final_fat_entry = 0;
  uint32_t hash = UINT32_C(2166136261);
  uint32_t remaining;
  uint32_t directory_index = 0;
  uint16_t bytes_per_sector;
  uint16_t reserved_sectors;
  uint16_t fsinfo_sector;
  uint8_t partition_type;
  uint8_t sectors_per_cluster;
  bool found = false;
  bool end_of_directory = false;
  unsigned int sector_index;
  int fd = -1;
  int ret = -EIO;

  fd = open(g_sd.devpath, O_RDONLY);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  memset(&geometry, 0, sizeof(geometry));
  if (ioctl(fd, BIOC_GEOMETRY,
            (unsigned long)((uintptr_t)&geometry)) < 0)
    {
      ret = p2storage_errno();
      goto out;
    }

  if (!geometry.geo_available || geometry.geo_nsectors <= 0 ||
      geometry.geo_sectorsize != P2STORAGE_SD_SECTOR_SIZE)
    {
      fail_reason = "UNAVAILABLE-OR-SECTOR-SIZE";
      ret = -ENODEV;
      goto out;
    }

  nsectors = geometry.geo_nsectors;
  fail_stage = "MBR";
  ret = p2storage_sd_read_sector(fd, nsectors, 0);
  if (ret < 0)
    {
      fail_reason = "READ";
      goto out;
    }

  entry = &g_io_buffer[P2STORAGE_MBR_PARTITION_OFFSET];
  partition_type = entry[4];
  partition_start = p2storage_get_u32le(&entry[8]);
  partition_sectors = p2storage_get_u32le(&entry[12]);

  if ((entry[0] & 0x7f) != 0 ||
      (partition_type != 0x0b && partition_type != 0x0c) ||
      partition_start == 0 || partition_sectors == 0 ||
      partition_start >= nsectors ||
      partition_sectors > nsectors - partition_start ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xaa)
    {
      fail_reason = "FIELDS";
      ret = -EIO;
      goto out;
    }

  if (memcmp(&g_io_buffer[0x17c], "Prop", 4) == 0 ||
      memcmp(&g_io_buffer[0x17c], "ProP", 4) == 0)
    {
      fail_reason = "RAW-BOOT-OVERRIDE";
      ret = -EIO;
      goto out;
    }

  printf("P2STORAGE:SD:ROM-MBR:TYPE=%02X:START=%" PRIu32
         ":SECTORS=%" PRIu32 ":PASS\n",
         (unsigned int)partition_type, partition_start,
         partition_sectors);

  fail_stage = "VBR";
  ret = p2storage_sd_read_sector(fd, nsectors, partition_start);
  if (ret < 0)
    {
      fail_reason = "READ";
      goto out;
    }

  bytes_per_sector = p2storage_get_u16le(&g_io_buffer[11]);
  sectors_per_cluster = g_io_buffer[13];
  reserved_sectors = p2storage_get_u16le(&g_io_buffer[14]);
  total_sectors = p2storage_get_u32le(&g_io_buffer[32]);
  fat_sectors = p2storage_get_u32le(&g_io_buffer[36]);
  root_cluster = p2storage_get_u32le(&g_io_buffer[44]);
  fsinfo_sector = p2storage_get_u16le(&g_io_buffer[48]);

  if (bytes_per_sector != P2STORAGE_SD_SECTOR_SIZE ||
      sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
      (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
      reserved_sectors == 0 || g_io_buffer[16] != 2 ||
      p2storage_get_u16le(&g_io_buffer[17]) != 0 ||
      p2storage_get_u16le(&g_io_buffer[19]) != 0 ||
      p2storage_get_u16le(&g_io_buffer[22]) != 0 ||
      p2storage_get_u32le(&g_io_buffer[28]) != partition_start ||
      total_sectors == 0 || total_sectors > partition_sectors ||
      fat_sectors == 0 || p2storage_get_u16le(&g_io_buffer[42]) != 0 ||
      root_cluster != 2 || fsinfo_sector == 0 ||
      fsinfo_sector >= reserved_sectors ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xaa)
    {
      fail_reason = "FIELDS";
      ret = -EIO;
      goto out;
    }

  if (memcmp(&g_io_buffer[0x17c], "Prop", 4) == 0 ||
      memcmp(&g_io_buffer[0x17c], "ProP", 4) == 0)
    {
      fail_reason = "RAW-BOOT-OVERRIDE";
      ret = -EIO;
      goto out;
    }

  if ((uint64_t)reserved_sectors + (uint64_t)fat_sectors * 2 >=
      total_sectors)
    {
      fail_reason = "REGION-BOUNDS";
      ret = -EIO;
      goto out;
    }

  volume_end = (uint64_t)partition_start + total_sectors;
  fat_begin = (uint64_t)partition_start + reserved_sectors;
  data_begin = fat_begin + (uint64_t)fat_sectors * 2;
  data_sectors = total_sectors - reserved_sectors -
                 (uint64_t)fat_sectors * 2;
  cluster_count = (uint32_t)(data_sectors / sectors_per_cluster);

  if (volume_end > nsectors || data_begin >= volume_end ||
      cluster_count < 65525 ||
      (uint64_t)fat_sectors *
      (P2STORAGE_SD_SECTOR_SIZE / sizeof(uint32_t)) <
      (uint64_t)cluster_count + 2)
    {
      fail_reason = "FAT32-BOUNDS";
      ret = -EIO;
      goto out;
    }

  printf("P2STORAGE:SD:ROM-VBR:BPS=%u:SPC=%u:RESERVED=%u:"
         "FATS=2:FATSZ=%" PRIu32 ":ROOT=%" PRIu32
         ":FSINFO=%u:PASS\n",
         (unsigned int)bytes_per_sector,
         (unsigned int)sectors_per_cluster,
         (unsigned int)reserved_sectors, fat_sectors, root_cluster,
         (unsigned int)fsinfo_sector);

  fail_stage = "FSINFO";
  ret = p2storage_sd_read_sector(fd, nsectors,
                                 (uint64_t)partition_start +
                                 fsinfo_sector);
  if (ret < 0)
    {
      fail_reason = "READ";
      goto out;
    }

  if (p2storage_get_u32le(&g_io_buffer[0]) != UINT32_C(0x41615252) ||
      p2storage_get_u32le(&g_io_buffer[0x1e4]) !=
      UINT32_C(0x61417272) ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      g_io_buffer[P2STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xaa)
    {
      fail_reason = "SIGNATURE";
      ret = -EIO;
      goto out;
    }

  printf("P2STORAGE:SD:ROM-FSINFO:LBA=%" PRIu64 ":PASS\n",
         (uint64_t)partition_start + fsinfo_sector);

  fail_stage = "ROOT";
  root_sector = data_begin;
  for (sector_index = 0;
       sector_index < sectors_per_cluster && !found && !end_of_directory;
       sector_index++)
    {
      unsigned int entry_index;

      ret = p2storage_sd_read_sector(fd, nsectors,
                                     root_sector + sector_index);
      if (ret < 0)
        {
          fail_reason = "READ";
          goto out;
        }

      for (entry_index = 0;
           entry_index < P2STORAGE_SD_SECTOR_SIZE /
                         P2STORAGE_FAT_DIR_ENTRY_SIZE;
           entry_index++)
        {
          size_t offset = entry_index * P2STORAGE_FAT_DIR_ENTRY_SIZE;
          FAR const uint8_t *directory_entry = &g_io_buffer[offset];

          if (directory_entry[0] == 0)
            {
              end_of_directory = true;
              break;
            }

          if (memcmp(directory_entry, filename, sizeof(filename)) == 0 &&
              (directory_entry[11] & 0xd8) == 0)
            {
              uint16_t cluster_high =
                p2storage_get_u16le(&directory_entry[20]);

              if ((cluster_high & 0xf000) != 0)
                {
                  fail_reason = "CLUSTER-HIGH";
                  ret = -EIO;
                  goto out;
                }

              first_cluster = (uint32_t)cluster_high << 16 |
                              p2storage_get_u16le(&directory_entry[26]);
              file_size = p2storage_get_u32le(&directory_entry[28]);
              directory_index = sector_index *
                                (P2STORAGE_SD_SECTOR_SIZE /
                                 P2STORAGE_FAT_DIR_ENTRY_SIZE) +
                                entry_index;
              found = true;
              break;
            }
        }
    }

  if (!found || first_cluster < 2 ||
      first_cluster > (uint64_t)cluster_count + 1 || file_size == 0 ||
      file_size > P2STORAGE_P2_ROM_MAX_IMAGE_SIZE)
    {
      fail_reason = "FILE-NOT-ROM-COMPATIBLE";
      ret = -ENOENT;
      goto out;
    }

  printf("P2STORAGE:SD:ROM-ROOT:LBA=%" PRIu64 ":ENTRY=%" PRIu32
         ":NAME=_BOOT_P2.BIX:CLUSTER=%" PRIu32 ":BYTES=%" PRIu32
         ":PASS\n",
         root_sector, directory_index, first_cluster, file_size);

  fail_stage = "CHAIN";
  cluster_bytes = (uint64_t)sectors_per_cluster *
                  P2STORAGE_SD_SECTOR_SIZE;
  needed_clusters = (uint32_t)(((uint64_t)file_size - 1) /
                               cluster_bytes + 1);

  for (sector_index = 0; sector_index < needed_clusters; sector_index++)
    {
      uint32_t cluster = first_cluster + sector_index;
      uint32_t first_fat_entry;
      uint32_t second_fat_entry;

      if (cluster < first_cluster ||
          cluster > (uint64_t)cluster_count + 1)
        {
          fail_reason = "CLUSTER-BOUNDS";
          ret = -EIO;
          goto out;
        }

      ret = p2storage_sd_fat_entry(fd, nsectors, fat_begin,
                                   fat_sectors, cluster,
                                   &first_fat_entry);
      if (ret < 0)
        {
          fail_reason = "FAT1-READ";
          goto out;
        }

      ret = p2storage_sd_fat_entry(fd, nsectors,
                                   fat_begin + fat_sectors,
                                   fat_sectors, cluster,
                                   &second_fat_entry);
      if (ret < 0)
        {
          fail_reason = "FAT2-READ";
          goto out;
        }

      if (first_fat_entry != second_fat_entry)
        {
          fail_reason = "FAT-COPIES";
          ret = -EIO;
          goto out;
        }

      if (sector_index + 1 < needed_clusters)
        {
          if (first_fat_entry != cluster + 1)
            {
              fail_reason = "FRAGMENTED";
              ret = -EIO;
              goto out;
            }
        }
      else
        {
          final_fat_entry = first_fat_entry;
          if (final_fat_entry < P2STORAGE_FAT32_EOC_MIN)
            {
              fail_reason = "NO-EOC";
              ret = -EIO;
              goto out;
            }
        }
    }

  printf("P2STORAGE:SD:ROM-CHAIN:FIRST=%" PRIu32
         ":CLUSTERS=%" PRIu32 ":CONTIGUOUS=1:EOC=%08" PRIX32
         ":PASS\n",
         first_cluster, needed_clusters, final_fat_entry);

  fail_stage = "IMAGE";
  needed_sectors = (file_size - 1) / P2STORAGE_SD_SECTOR_SIZE + 1;
  image_sector = data_begin +
                 (uint64_t)(first_cluster - 2) * sectors_per_cluster;
  if (image_sector >= volume_end ||
      needed_sectors > volume_end - image_sector)
    {
      fail_reason = "SECTOR-BOUNDS";
      ret = -EIO;
      goto out;
    }

  remaining = file_size;
  for (sector_index = 0; sector_index < needed_sectors; sector_index++)
    {
      size_t length = remaining > P2STORAGE_SD_SECTOR_SIZE ?
                      P2STORAGE_SD_SECTOR_SIZE : remaining;

      ret = p2storage_sd_read_sector(fd, nsectors,
                                     image_sector + sector_index);
      if (ret < 0)
        {
          fail_reason = "READ";
          goto out;
        }

      hash = p2storage_fnv1a_part(g_io_buffer, length, hash);
      remaining -= (uint32_t)length;
    }

  if (remaining != 0)
    {
      fail_reason = "SHORT";
      ret = -EIO;
      goto out;
    }

  printf("P2STORAGE:SD:ROM-IMAGE:LBA=%" PRIu64 ":SECTORS=%" PRIu32
         ":BYTES=%" PRIu32 ":FNV1A=%08" PRIX32 ":PASS\n",
         image_sector, needed_sectors, file_size, hash);
  ret = 0;

out:
  if (close(fd) < 0 && ret == 0)
    {
      fail_stage = "CLOSE";
      fail_reason = "IO";
      ret = p2storage_errno();
    }

  if (ret < 0)
    {
      printf("P2STORAGE:SD:ROM-FAIL:STAGE=%s:REASON=%s\n",
             fail_stage, fail_reason);
    }

  return ret;
}

#ifdef CONFIG_TESTING_P2STORAGE_DESTRUCTIVE
static int p2storage_auth(FAR const char *magic)
{
  return magic != NULL &&
         strcmp(magic, P2STORAGE_DESTRUCTIVE_MAGIC) == 0 ? 0 : -EACCES;
}

static int p2storage_sd_geometry_fd(int fd,
                                   FAR struct geometry *geometry)
{
  memset(geometry, 0, sizeof(*geometry));
  if (ioctl(fd, BIOC_GEOMETRY,
            (unsigned long)((uintptr_t)geometry)) < 0)
    {
      return p2storage_errno();
    }

  if (!geometry->geo_available || !geometry->geo_writeenabled)
    {
      return -ENODEV;
    }

  return 0;
}

static int p2storage_sd_partition_geometry(int fd,
                                           FAR uint64_t *nsectors,
                                           FAR uint32_t *partition_sectors)
{
  struct geometry geometry;
  uint64_t total_sectors;
  int ret;

  ret = p2storage_sd_geometry_fd(fd, &geometry);
  if (ret < 0)
    {
      return ret;
    }

  total_sectors = geometry.geo_nsectors;
  if (geometry.geo_sectorsize != P2STORAGE_SD_SECTOR_SIZE ||
      total_sectors <= P2STORAGE_SD_PARTITION_START ||
      total_sectors - P2STORAGE_SD_PARTITION_START > UINT32_MAX)
    {
      return -EINVAL;
    }

  *nsectors = total_sectors;
  *partition_sectors =
    (uint32_t)(total_sectors - P2STORAGE_SD_PARTITION_START);
  return 0;
}

static int p2storage_sd_geometry_unchanged(int fd,
                                           uint64_t expected_nsectors)
{
  struct geometry geometry;
  int ret;

  ret = p2storage_sd_geometry_fd(fd, &geometry);
  if (ret < 0)
    {
      return ret;
    }

  if (geometry.geo_mediachanged ||
      geometry.geo_sectorsize != P2STORAGE_SD_SECTOR_SIZE ||
      (uint64_t)geometry.geo_nsectors != expected_nsectors)
    {
      return -EAGAIN;
    }

  return 0;
}

static void p2storage_sd_build_mbr(FAR uint8_t *sector,
                                    uint32_t partition_sectors)
{
  FAR uint8_t *entry;

  memset(sector, 0, P2STORAGE_SD_SECTOR_SIZE);
  entry = &sector[P2STORAGE_MBR_PARTITION_OFFSET];

  /* The P2 ROM accepts only partition zero with state 0x00/0x80 and FAT32
   * type 0x0b/0x0c.  Use a conventional active FAT32-LBA partition aligned
   * at 1 MiB.  CHS is retained only for compatibility; boot uses the LBA
   * fields below.
   */

  entry[0] = 0x80;
  entry[1] = 0x20;
  entry[2] = 0x21;
  entry[3] = 0x00;
  entry[4] = P2STORAGE_FAT32_PARTITION_TYPE;
  entry[5] = 0xfe;
  entry[6] = 0xff;
  entry[7] = 0xff;
  p2storage_put_u32le(&entry[8], P2STORAGE_SD_PARTITION_START);
  p2storage_put_u32le(&entry[12], partition_sectors);
  sector[P2STORAGE_MBR_SIGNATURE_OFFSET] = 0x55;
  sector[P2STORAGE_MBR_SIGNATURE_OFFSET + 1] = 0xaa;
}

static int p2storage_sd_write_mbr_fd(int fd, uint64_t nsectors,
                                     uint32_t partition_sectors,
                                     bool revalidate_vbr)
{
  FAR uint8_t *vbr_snapshot = g_io_buffer;
  FAR uint8_t *expected = &g_io_buffer[P2STORAGE_SD_SECTOR_SIZE];
  FAR uint8_t *current_vbr =
    &g_io_buffer[2 * P2STORAGE_SD_SECTOR_SIZE];
  off_t position;
  int ret;

  /* A removable card cannot be locked atomically through this block API.
   * Re-query the already-open device and, for repair, compare the validated
   * VBR snapshot immediately before the only raw write.  A card-change
   * indication, geometry drift, or content drift aborts without touching
   * LBA0.
   */

  ret = p2storage_sd_geometry_unchanged(fd, nsectors);
  if (ret < 0)
    {
      return ret;
    }

  if (revalidate_vbr)
    {
      ret = p2storage_sd_read_sector_into(
        fd, nsectors, P2STORAGE_SD_PARTITION_START, current_vbr);
      if (ret < 0)
        {
          return ret;
        }

      if (memcmp(vbr_snapshot, current_vbr,
                 P2STORAGE_SD_SECTOR_SIZE) != 0)
        {
          return -EAGAIN;
        }
    }

  p2storage_sd_build_mbr(expected, partition_sectors);
  position = lseek(fd, 0, SEEK_SET);
  if (position < 0)
    {
      return p2storage_errno();
    }
  else if (position != 0)
    {
      return -EIO;
    }

  ret = p2storage_write_all(fd, expected, P2STORAGE_SD_SECTOR_SIZE);
  if (ret == 0 && fsync(fd) < 0)
    {
      ret = p2storage_errno();
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = p2storage_sd_read_sector_into(fd, nsectors, 0, g_io_buffer);
  if (ret < 0)
    {
      return ret;
    }

  return memcmp(g_io_buffer, expected, P2STORAGE_SD_SECTOR_SIZE) == 0 ?
         0 : -EIO;
}

static int p2storage_sd_write_mbr(
  FAR const struct p2storage_medium_s *medium,
  FAR uint32_t *partition_sectors)
{
  uint64_t nsectors;
  uint32_t count;
  int ret;
  int fd;

  fd = open(medium->devpath, O_RDWR);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  ret = p2storage_sd_partition_geometry(fd, &nsectors, &count);
  if (ret == 0)
    {
      ret = p2storage_sd_write_mbr_fd(fd, nsectors, count, false);
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret == 0)
    {
      *partition_sectors = count;
    }

  return ret;
}

static int p2storage_sd_validate_mkfatfs(int fd, uint64_t nsectors,
                                         uint32_t partition_sectors)
{
  static const uint8_t zero_reserved[14] =
  {
    0
  };

  static const uint8_t volume_label[11] =
  {
    'P', '2', 'S', 'T', 'O', 'R', 'A', 'G', 'E', ' ', ' '
  };

  FAR uint8_t *primary = g_io_buffer;
  FAR uint8_t *scratch = &g_io_buffer[P2STORAGE_SD_SECTOR_SIZE];
  uint64_t cluster_count;
  uint64_t data_sectors;
  uint64_t fat_entries;
  uint64_t overhead;
  uint32_t fat_sectors;
  uint8_t sectors_per_cluster;
  int ret;

  ret = p2storage_sd_read_sector_into(fd, nsectors,
                                      P2STORAGE_SD_PARTITION_START,
                                      primary);
  if (ret < 0)
    {
      return ret;
    }

  sectors_per_cluster = primary[13];
  fat_sectors = p2storage_get_u32le(&primary[36]);
  if ((uint64_t)P2STORAGE_SD_PARTITION_START + partition_sectors !=
        nsectors ||
      primary[0] != 0xeb || primary[1] != 0x58 || primary[2] != 0x90 ||
      memcmp(&primary[3], "NUTTX   ", 8) != 0 ||
      p2storage_get_u16le(&primary[11]) != P2STORAGE_SD_SECTOR_SIZE ||
      sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
      (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
      p2storage_get_u16le(&primary[14]) !=
        P2STORAGE_FAT32_RESERVED_SECTORS ||
      primary[16] != 2 ||
      p2storage_get_u16le(&primary[17]) != 0 ||
      p2storage_get_u16le(&primary[19]) != 0 ||
      primary[21] != 0xf8 ||
      p2storage_get_u16le(&primary[22]) != 0 ||
      p2storage_get_u16le(&primary[24]) != 63 ||
      p2storage_get_u16le(&primary[26]) != 255 ||
      p2storage_get_u32le(&primary[28]) !=
        P2STORAGE_SD_PARTITION_START ||
      p2storage_get_u32le(&primary[32]) != partition_sectors ||
      fat_sectors == 0 ||
      p2storage_get_u16le(&primary[40]) != 0 ||
      p2storage_get_u16le(&primary[42]) != 0 ||
      p2storage_get_u32le(&primary[44]) != 2 ||
      p2storage_get_u16le(&primary[48]) !=
        P2STORAGE_FAT32_FSINFO_SECTOR ||
      p2storage_get_u16le(&primary[50]) !=
        P2STORAGE_FAT32_BACKUP_SECTOR ||
      memcmp(&primary[52], zero_reserved, sizeof(zero_reserved)) != 0 ||
      primary[66] != 0x29 ||
      p2storage_get_u32le(&primary[67]) !=
        P2STORAGE_FAT32_VOLUME_ID ||
      memcmp(&primary[71], volume_label, sizeof(volume_label)) != 0 ||
      memcmp(&primary[82], "FAT32   ", 8) != 0 ||
      primary[P2STORAGE_MBR_SIGNATURE_OFFSET] != 0x55 ||
      primary[P2STORAGE_MBR_SIGNATURE_OFFSET + 1] != 0xaa)
    {
      return -EINVAL;
    }

  overhead = P2STORAGE_FAT32_RESERVED_SECTORS +
             UINT64_C(2) * fat_sectors;
  if (overhead >= partition_sectors)
    {
      return -EINVAL;
    }

  data_sectors = partition_sectors - overhead;
  cluster_count = data_sectors / sectors_per_cluster;
  fat_entries = (uint64_t)fat_sectors *
                (P2STORAGE_SD_SECTOR_SIZE / sizeof(uint32_t));
  if (cluster_count < P2STORAGE_FAT32_MIN_CLUSTERS ||
      cluster_count > P2STORAGE_FAT32_MAX_CLUSTERS ||
      cluster_count + 3 > fat_entries)
    {
      return -EINVAL;
    }

  /* mkfatfs writes an exact backup of the VBR and a signed FSINFO sector.
   * Those immutable sectors distinguish this layout from a merely plausible
   * FAT32 BPB.  FSINFO free-space hints are intentionally not compared: they
   * legitimately change as files are added to the volume.
   */

  ret = p2storage_sd_read_sector_into(
    fd, nsectors,
    P2STORAGE_SD_PARTITION_START + P2STORAGE_FAT32_BACKUP_SECTOR,
    scratch);
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(primary, scratch, P2STORAGE_SD_SECTOR_SIZE) != 0)
    {
      return -EINVAL;
    }

  ret = p2storage_sd_read_sector_into(
    fd, nsectors,
    P2STORAGE_SD_PARTITION_START + P2STORAGE_FAT32_FSINFO_SECTOR,
    scratch);
  if (ret < 0)
    {
      return ret;
    }

  if (p2storage_get_u32le(&scratch[0]) != UINT32_C(0x41615252) ||
      p2storage_get_u32le(&scratch[0x1e4]) != UINT32_C(0x61417272) ||
      p2storage_get_u32le(&scratch[0x1fc]) != UINT32_C(0xaa550000))
    {
      return -EINVAL;
    }

  /* Detect a VBR change or same-size card swap during validation. */

  ret = p2storage_sd_read_sector_into(fd, nsectors,
                                      P2STORAGE_SD_PARTITION_START,
                                      scratch);
  if (ret < 0)
    {
      return ret;
    }

  return memcmp(primary, scratch, P2STORAGE_SD_SECTOR_SIZE) == 0 ?
         0 : -EAGAIN;
}

static int p2storage_sd_verify_rom_layout_fd(int fd, uint64_t nsectors,
                                              uint32_t partition_sectors)
{
  FAR uint8_t *expected = &g_io_buffer[P2STORAGE_SD_SECTOR_SIZE];
  int ret;

  ret = p2storage_sd_read_sector_into(fd, nsectors, 0, g_io_buffer);
  if (ret < 0)
    {
      return ret;
    }

  p2storage_sd_build_mbr(expected, partition_sectors);
  if (memcmp(g_io_buffer, expected, P2STORAGE_SD_SECTOR_SIZE) != 0)
    {
      return -EIO;
    }

  return p2storage_sd_validate_mkfatfs(fd, nsectors,
                                       partition_sectors);
}

static int p2storage_sd_repair_mbr(
  FAR const struct p2storage_medium_s *medium)
{
  uint64_t nsectors;
  uint32_t partition_sectors;
  int fd;
  int ret;

  /* Raw repair must never race a mounted FAT instance with cached
   * metadata.
   */

  ret = p2storage_ensure_unmounted(medium);
  if (ret < 0)
    {
      return ret;
    }

  /* Repair only a missing or damaged MBR in front of the exact FAT32
   * partition layout produced by sd-format.  Validate the existing VBR
   * before writing sector zero so this command can never invent a
   * partition over unrelated media.
   */

  fd = open(medium->devpath, O_RDWR);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  ret = p2storage_sd_partition_geometry(fd, &nsectors,
                                        &partition_sectors);
  if (ret < 0)
    {
      goto out;
    }

  ret = p2storage_sd_validate_mkfatfs(fd, nsectors,
                                      partition_sectors);
  if (ret == 0)
    {
      ret = p2storage_sd_write_mbr_fd(fd, nsectors,
                                      partition_sectors, true);
    }

  if (ret == 0)
    {
      ret = p2storage_sd_geometry_unchanged(fd, nsectors);
    }

  if (ret == 0)
    {
      ret = p2storage_sd_verify_rom_layout_fd(fd, nsectors,
                                              partition_sectors);
    }

  if (ret == 0)
    {
      ret = p2storage_sd_geometry_unchanged(fd, nsectors);
    }

out:
  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret < 0)
    {
      return ret;
    }

  printf("P2STORAGE:SD:ROM-MBR:TYPE=0C:START=%" PRIu32
         ":SECTORS=%" PRIu32 ":PASS\n",
         P2STORAGE_SD_PARTITION_START, partition_sectors);
  printf("P2STORAGE:SD:MBR-REPAIR:START=%" PRIu32
         ":SECTORS=%" PRIu32 ":PASS\n",
         P2STORAGE_SD_PARTITION_START, partition_sectors);
  return 0;
}

static int p2storage_sd_verify_rom_layout(
  FAR const struct p2storage_medium_s *medium, uint32_t partition_sectors)
{
  uint64_t nsectors;
  uint32_t geometry_sectors;
  int ret;
  int fd;

  fd = open(medium->devpath, O_RDONLY);
  if (fd < 0)
    {
      return p2storage_errno();
    }

  ret = p2storage_sd_partition_geometry(fd, &nsectors,
                                        &geometry_sectors);
  if (ret == 0 && geometry_sectors != partition_sectors)
    {
      ret = -EIO;
    }

  if (ret == 0)
    {
      ret = p2storage_sd_verify_rom_layout_fd(fd, nsectors,
                                              partition_sectors);
    }

  if (close(fd) < 0 && ret == 0)
    {
      ret = p2storage_errno();
    }

  if (ret == 0)
    {
      printf("P2STORAGE:SD:ROM-MBR:TYPE=0C:START=%" PRIu32
             ":SECTORS=%" PRIu32 ":PASS\n",
             P2STORAGE_SD_PARTITION_START, partition_sectors);
    }

  return ret;
}

static int p2storage_sd_format(
  FAR const struct p2storage_medium_s *medium)
{
  struct fat_format_s format = FAT_FORMAT_INITIALIZER;
  static const uint8_t label[11] =
  {
    'P', '2', 'S', 'T', 'O', 'R', 'A', 'G', 'E', ' ', ' '
  };

  uint32_t partition_sectors;
  int unregister_ret;
  int ret;

  ret = p2storage_sd_write_mbr(medium, &partition_sectors);
  if (ret < 0)
    {
      return ret;
    }

  ret = unregister_blockdriver(P2STORAGE_SD_PARTITION_DEVPATH);
  if (ret < 0 && ret != -ENOENT)
    {
      return ret;
    }

  ret = register_blockpartition(P2STORAGE_SD_PARTITION_DEVPATH, 0660,
                                medium->devpath,
                                P2STORAGE_SD_PARTITION_START,
                                partition_sectors);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(format.ff_volumelabel, label, sizeof(label));
  format.ff_fattype = 32;
  format.ff_hidsec = P2STORAGE_SD_PARTITION_START;
  format.ff_volumeid = P2STORAGE_FAT32_VOLUME_ID;
  if (mkfatfs(P2STORAGE_SD_PARTITION_DEVPATH, &format) < 0)
    {
      ret = p2storage_errno();
    }
  else
    {
      ret = 0;
    }

  unregister_ret = unregister_blockdriver(
    P2STORAGE_SD_PARTITION_DEVPATH);
  if (ret == 0)
    {
      ret = unregister_ret;
    }

  if (ret < 0)
    {
      return ret;
    }

  return p2storage_sd_verify_rom_layout(medium, partition_sectors);
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
      ret = p2storage_sd_format(medium);
      if (ret < 0)
        {
          return ret;
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
  printf("       %s sd-rom-verify\n", progname);
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
  printf("       %s sd-mbr-repair %s\n", progname,
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
         strcmp(command, "sd-mbr-repair") == 0 ||
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
  else if (strcmp(argv[1], "sd-rom-verify") == 0)
    {
      if (argc != 2)
        {
          return p2storage_fail("ARGS", -EINVAL);
        }

      terminal = "SD-ROM-VERIFY";
      ret = p2storage_sd_rom_verify();
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
      bool has_sequence = !is_format &&
                          strcmp(argv[1], "sd-mbr-repair") != 0;

      if ((!has_sequence && argc != 3) || (has_sequence && argc != 4))
        {
          return p2storage_fail("ARGS", -EINVAL);
        }

      ret = p2storage_auth(argv[2]);
      if (ret < 0)
        {
          return p2storage_fail("AUTH", ret);
        }

      if (has_sequence &&
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
      else if (strcmp(argv[1], "sd-mbr-repair") == 0)
        {
          terminal = "SD-MBR-REPAIR";
          ret = p2storage_sd_repair_mbr(&g_sd);
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
