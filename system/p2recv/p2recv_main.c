/****************************************************************************
 * apps/system/p2recv/p2recv_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define P2RECV_FRAME_MAGIC        "P2RF"
#define P2RECV_FRAME_HEADER_SIZE  16
#define P2RECV_CHUNK_MAX          256
#define P2RECV_TEMP_ATTEMPTS      16
#define P2RECV_FILE_MODE          0755

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t p2recv_getle32(FAR const uint8_t *buffer)
{
  return (uint32_t)buffer[0] |
         (uint32_t)buffer[1] << 8 |
         (uint32_t)buffer[2] << 16 |
         (uint32_t)buffer[3] << 24;
}

static uint32_t p2recv_crc32_part(uint32_t state,
                                  FAR const uint8_t *buffer,
                                  size_t length)
{
  size_t offset;

  for (offset = 0; offset < length; offset++)
    {
      unsigned int bit;

      state ^= buffer[offset];
      for (bit = 0; bit < 8; bit++)
        {
          uint32_t mask = (uint32_t)-(int32_t)(state & 1);
          state = (state >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }

  return state;
}

static uint32_t p2recv_crc32(FAR const uint8_t *buffer, size_t length)
{
  return p2recv_crc32_part(UINT32_MAX, buffer, length) ^ UINT32_MAX;
}

static int p2recv_read_exact(FAR uint8_t *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      struct pollfd descriptor;
      ssize_t nread;
      int ret;

      descriptor.fd      = STDIN_FILENO;
      descriptor.events  = POLLIN;
      descriptor.revents = 0;

      do
        {
          ret = poll(&descriptor, 1, CONFIG_SYSTEM_P2RECV_TIMEOUT_MS);
        }
      while (ret < 0 && errno == EINTR);

      if (ret < 0)
        {
          return -errno;
        }

      if (ret == 0)
        {
          return -ETIMEDOUT;
        }

      if ((descriptor.revents & POLLIN) == 0)
        {
          return -EIO;
        }

      do
        {
          nread = read(STDIN_FILENO, buffer + offset, length - offset);
        }
      while (nread < 0 && errno == EINTR);

      if (nread < 0)
        {
          return -errno;
        }

      if (nread == 0)
        {
          return -EPIPE;
        }

      offset += nread;
    }

  return 0;
}

static int p2recv_write_all(int fd, FAR const uint8_t *buffer,
                            size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t nwritten;

      do
        {
          nwritten = write(fd, buffer + offset, length - offset);
        }
      while (nwritten < 0 && errno == EINTR);

      if (nwritten < 0)
        {
          return -errno;
        }

      if (nwritten == 0)
        {
          return -EIO;
        }

      offset += nwritten;
    }

  return 0;
}

static int p2recv_parse_u32(FAR const char *text, int base,
                            FAR uint32_t *value)
{
  FAR char *end;
  unsigned long parsed;

  if (text == NULL || *text == '\0' || *text == '-')
    {
      return -EINVAL;
    }

  errno  = 0;
  parsed = strtoul(text, &end, base);
  if (errno != 0 || *end != '\0' || parsed > UINT32_MAX)
    {
      return -EINVAL;
    }

  *value = (uint32_t)parsed;
  return 0;
}

static int p2recv_validate_path(FAR const char *path)
{
  FAR const char *component;
  FAR const char *cursor;
  size_t length;

  if (path == NULL || strncmp(path, "/mnt/", 5) != 0)
    {
      return -EACCES;
    }

  length = strlen(path);
  if (length >= PATH_MAX)
    {
      return -ENAMETOOLONG;
    }

  if (path[length - 1] == '/')
    {
      return -EINVAL;
    }

  component = path + 1;
  for (cursor = component; ; cursor++)
    {
      unsigned char character = (unsigned char)*cursor;

      if (character != '\0' && character != '/' && character != '.' &&
          character != '-' && character != '_' && !isalnum(character))
        {
          return -EINVAL;
        }

      if (character == '/' || character == '\0')
        {
          size_t component_length = (size_t)(cursor - component);

          if (component_length == 0 ||
              (component_length == 1 && component[0] == '.') ||
              (component_length == 2 && component[0] == '.' &&
               component[1] == '.'))
            {
              return -EINVAL;
            }

          if (character == '\0')
            {
              break;
            }

          component = cursor + 1;
        }
    }

  return 0;
}

static int p2recv_check_destination(FAR const char *path, bool force)
{
  struct stat status;

  if (stat(path, &status) == 0)
    {
      if (S_ISDIR(status.st_mode))
        {
          return -EISDIR;
        }

      if (!force)
        {
          return -EEXIST;
        }

      return 0;
    }

  return errno == ENOENT ? 0 : -errno;
}

static int p2recv_open_temp(FAR const char *destination,
                            FAR char *temporary, size_t temporary_size)
{
  FAR const char *slash = strrchr(destination, '/');
  size_t directory_length;
  unsigned int attempt;

  if (slash == NULL)
    {
      return -EINVAL;
    }

  directory_length = (size_t)(slash - destination + 1);
  if (directory_length >= temporary_size)
    {
      return -ENAMETOOLONG;
    }

  memcpy(temporary, destination, directory_length);

  for (attempt = 0; attempt < P2RECV_TEMP_ATTEMPTS; attempt++)
    {
      int fd;
      int length;

      /* Keep the leaf at or below SmartFS' default 16-character name
       * limit.  A 32-bit PID and one hexadecimal attempt digit produce at
       * most ".p2r-ffffffff-f" (15 characters).
       */

      length = snprintf(temporary + directory_length,
                        temporary_size - directory_length,
                        ".p2r-%lx-%x", (unsigned long)getpid(), attempt);
      if (length < 0 || (size_t)length >= temporary_size - directory_length)
        {
          return -ENAMETOOLONG;
        }

      /* The temporary file's mode is preserved by the final rename.  ELF
       * modules must retain an execute bit so NuttX binfmt can launch them
       * directly from SmartFS after a verified transfer.
       */

      fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL,
                P2RECV_FILE_MODE);
      if (fd >= 0)
        {
          return fd;
        }

      if (errno != EEXIST)
        {
          return -errno;
        }
    }

  return -EEXIST;
}

static void p2recv_error(FAR const char *stage, int error)
{
  if (error < 0)
    {
      error = -error;
    }

  dprintf(STDOUT_FILENO, "P2RECV:ERROR:STAGE=%s:CODE=%d\n", stage, error);
}

static int p2recv_enter_raw(FAR struct termios *saved)
{
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, saved) < 0)
    {
      return -errno;
    }

  raw = *saved;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
    {
      return -errno;
    }

  return 0;
}

static int p2recv_leave_raw(FAR const struct termios *saved)
{
  if (tcsetattr(STDIN_FILENO, TCSANOW, saved) < 0)
    {
      return -errno;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct termios saved_termios;
  uint8_t header[P2RECV_FRAME_HEADER_SIZE];
  uint8_t payload[P2RECV_CHUNK_MAX];
  char temporary[PATH_MAX];
  FAR const char *destination;
  FAR const char *stage = "ARGS";
  uint32_t expected_size;
  uint32_t expected_crc;
  uint32_t received = 0;
  uint32_t sequence = 0;
  uint32_t file_crc_state = UINT32_MAX;
  bool force = false;
  bool raw_mode = false;
  bool temporary_exists = false;
  int argument = 1;
  int fd = -1;
  int error = -EINVAL;

  if (argument < argc && strcmp(argv[argument], "-f") == 0)
    {
      force = true;
      argument++;
    }

  if (argc - argument != 3)
    {
      dprintf(STDERR_FILENO,
              "Usage: p2recv [-f] /mnt/PATH SIZE CRC32\n");
      goto fail;
    }

  destination = argv[argument++];
  error = p2recv_parse_u32(argv[argument++], 10, &expected_size);
  if (error < 0 || expected_size > CONFIG_SYSTEM_P2RECV_MAXSIZE)
    {
      error = error < 0 ? error : -EFBIG;
      goto fail;
    }

  error = p2recv_parse_u32(argv[argument], 16, &expected_crc);
  if (error < 0)
    {
      goto fail;
    }

  stage = "PATH";
  error = p2recv_validate_path(destination);
  if (error < 0)
    {
      goto fail;
    }

  stage = "DEST";
  error = p2recv_check_destination(destination, force);
  if (error < 0)
    {
      goto fail;
    }

  stage = "TEMP";
  fd = p2recv_open_temp(destination, temporary, sizeof(temporary));
  if (fd < 0)
    {
      error = fd;
      goto fail;
    }

  temporary_exists = true;

  /* NSH uses the console in canonical, echoed mode.  A framed binary stream
   * must be 8-bit transparent: canonical input rewrites CR, consumes erase
   * characters, and can interpret control bytes.  Limit raw mode to this
   * command and restore the shared console before returning to NSH.
   */

  stage = "TTY_RAW";
  error = p2recv_enter_raw(&saved_termios);
  if (error < 0)
    {
      goto fail;
    }

  raw_mode = true;
  dprintf(STDOUT_FILENO,
          "P2RECV:READY:SIZE=%" PRIu32 ":CRC32=%08" PRIX32
          ":CHUNK_MAX=%u:PATH=%s\n",
          expected_size, expected_crc, P2RECV_CHUNK_MAX, destination);

  while (received < expected_size)
    {
      uint32_t frame_sequence;
      uint32_t frame_length;
      uint32_t frame_crc;

      stage = "HEADER";
      error = p2recv_read_exact(header, sizeof(header));
      if (error < 0)
        {
          goto fail;
        }

      if (memcmp(header, P2RECV_FRAME_MAGIC, 4) != 0)
        {
          error = -EPROTO;
          goto fail;
        }

      frame_sequence = p2recv_getle32(header + 4);
      frame_length   = p2recv_getle32(header + 8);
      frame_crc      = p2recv_getle32(header + 12);

      stage = "SEQUENCE";
      if (frame_sequence != sequence)
        {
          error = -EPROTO;
          goto fail;
        }

      stage = "LENGTH";
      if (frame_length == 0 || frame_length > sizeof(payload) ||
          frame_length > expected_size - received)
        {
          error = -EMSGSIZE;
          goto fail;
        }

      stage = "PAYLOAD";
      error = p2recv_read_exact(payload, frame_length);
      if (error < 0)
        {
          goto fail;
        }

      stage = "CHUNK_CRC";
      if (p2recv_crc32(payload, frame_length) != frame_crc)
        {
          error = -EBADMSG;
          goto fail;
        }

      stage = "WRITE";
      error = p2recv_write_all(fd, payload, frame_length);
      if (error < 0)
        {
          goto fail;
        }

      file_crc_state = p2recv_crc32_part(file_crc_state, payload,
                                         frame_length);
      received += frame_length;
      dprintf(STDOUT_FILENO,
              "P2RECV:CHUNK:SEQ=%" PRIu32 ":BYTES=%" PRIu32 "\n",
              sequence, received);
      sequence++;
    }

  stage = "FILE_CRC";
  if ((file_crc_state ^ UINT32_MAX) != expected_crc)
    {
      error = -EBADMSG;
      goto fail;
    }

  stage = "SYNC";
  if (fsync(fd) < 0)
    {
      error = -errno;
      goto fail;
    }

  stage = "CLOSE";
  if (close(fd) < 0)
    {
      error = -errno;
      fd = -1;
      goto fail;
    }

  fd = -1;

  stage = "TTY_RESTORE";
  error = p2recv_leave_raw(&saved_termios);
  if (error < 0)
    {
      goto fail;
    }

  raw_mode = false;

  stage = "DEST";
  error = p2recv_check_destination(destination, force);
  if (error < 0)
    {
      goto fail;
    }

  stage = "COMMIT";
  if (rename(temporary, destination) < 0)
    {
      error = -errno;
      goto fail;
    }

  temporary_exists = false;
  dprintf(STDOUT_FILENO,
          "P2RECV:ACK:BYTES=%" PRIu32 ":CRC32=%08" PRIX32
          ":PATH=%s\n",
          received, expected_crc, destination);
  return EXIT_SUCCESS;

fail:
  if (fd >= 0)
    {
      close(fd);
    }

  if (temporary_exists)
    {
      unlink(temporary);
    }

  if (raw_mode)
    {
      int restore_error = p2recv_leave_raw(&saved_termios);

      if (restore_error < 0 && error >= 0)
        {
          stage = "TTY_RESTORE";
          error = restore_error;
        }
    }

  p2recv_error(stage, error);
  return EXIT_FAILURE;
}
