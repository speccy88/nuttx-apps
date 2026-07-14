/****************************************************************************
 * apps/system/p2bank/p2bank_logic.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_P2BANK_P2BANK_LOGIC_H
#define __APPS_SYSTEM_P2BANK_P2BANK_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool p2bank_path_safe(const char *path, const char *root,
                                    size_t maximum)
{
  const char *segment;
  size_t root_length = strlen(root);
  size_t length;

  if (path == NULL)
    {
      return false;
    }

  length = strnlen(path, maximum + 1);
  if (length == 0 || length > maximum ||
      strncmp(path, root, root_length) != 0 || path[root_length] == '\0')
    {
      return false;
    }

  segment = path + root_length;
  while (*segment != '\0')
    {
      const char *end = strchr(segment, '/');
      size_t segment_length = end == NULL ? strlen(segment) :
                                             (size_t)(end - segment);
      size_t offset;

      if (segment_length == 0 ||
          (segment_length == 1 && segment[0] == '.') ||
          (segment_length == 2 && segment[0] == '.' && segment[1] == '.'))
        {
          return false;
        }

      for (offset = 0; offset < segment_length; offset++)
        {
          unsigned char value = (unsigned char)segment[offset];

          if (value < 0x20 || value == 0x7f || value == '\\')
            {
              return false;
            }
        }

      if (end == NULL)
        {
          return true;
        }

      segment = end + 1;
    }

  return false;
}

#endif /* __APPS_SYSTEM_P2BANK_P2BANK_LOGIC_H */
