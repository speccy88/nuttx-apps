/* Behavioral regression harness for CPython's actual patched hashtable.c.
 *
 * test_p2_hashtable.py supplies HASHTABLE_SOURCE after extracting CPython and
 * applying the apps patch series.  Including the implementation here keeps
 * static hashtable_rehash() visible for the overflow test without adding a
 * production-only hook.
 */

#include "Python.h"
#include "pycore_hashtable.h"
#include "pycore_pyhash.h"

#include <inttypes.h>
#include <stdio.h>

#ifndef HASHTABLE_SOURCE
#  error "HASHTABLE_SOURCE must name the post-patch Python/hashtable.c"
#endif

#define ALLOCATION_MAGIC UINT64_C(0xa110ca7ed00dbeef)
#define TEST_ITEM_COUNT 40

#define CHECK(condition)                                                     \
  do                                                                         \
    {                                                                        \
      if (!(condition))                                                      \
        {                                                                    \
          fprintf(stderr, "FAIL:%s:%d: %s\n", __FILE__, __LINE__,           \
                  #condition);                                               \
          return -1;                                                         \
        }                                                                    \
    }                                                                        \
  while (0)

union allocation_header_u
{
  struct
  {
    size_t size;
    uint64_t magic;
  } fields;
  max_align_t alignment;
};

struct allocation_stats_s
{
  size_t attempts;
  size_t successes;
  size_t failures;
  size_t frees;
  size_t live_blocks;
  size_t live_bytes;
  size_t fail_on_attempt;
};

struct test_item_s
{
  unsigned int id;
  unsigned int collision_hash;
};

static struct allocation_stats_s g_alloc;
static struct test_item_s g_keys[TEST_ITEM_COUNT];
static struct test_item_s g_values[TEST_ITEM_COUNT];

static void *
tracked_malloc(size_t size)
{
  union allocation_header_u *header;

  g_alloc.attempts++;
  if (g_alloc.fail_on_attempt != 0 &&
      g_alloc.attempts == g_alloc.fail_on_attempt)
    {
      g_alloc.failures++;
      return NULL;
    }

  header = malloc(sizeof(*header) + size);
  if (header == NULL)
    {
      fprintf(stderr, "host allocator unexpectedly failed for %zu bytes\n",
              size);
      abort();
    }

  header->fields.size = size;
  header->fields.magic = ALLOCATION_MAGIC;
  g_alloc.successes++;
  g_alloc.live_blocks++;
  g_alloc.live_bytes += size;
  return header + 1;
}

static void
tracked_free(void *ptr)
{
  union allocation_header_u *header;

  if (ptr == NULL)
    {
      fprintf(stderr, "CPython hashtable unexpectedly freed NULL\n");
      abort();
    }

  header = (union allocation_header_u *)ptr - 1;
  if (header->fields.magic != ALLOCATION_MAGIC || g_alloc.live_blocks == 0 ||
      g_alloc.live_bytes < header->fields.size)
    {
      fprintf(stderr, "invalid or duplicate tracked free\n");
      abort();
    }

  header->fields.magic = 0;
  g_alloc.frees++;
  g_alloc.live_blocks--;
  g_alloc.live_bytes -= header->fields.size;
  free(header);
}

void *
PyMem_Malloc(size_t size)
{
  return malloc(size);
}

void
PyMem_Free(void *ptr)
{
  free(ptr);
}

Py_uhash_t
_Py_HashPointerRaw(const void *key)
{
  uintptr_t value = (uintptr_t)key;
  return (Py_uhash_t)(value ^ (value >> 4));
}

/* Compile the real, post-patch implementation in this translation unit. */
#include HASHTABLE_SOURCE

static Py_uhash_t
collision_hash(const void *key)
{
  const struct test_item_s *item = key;
  return (Py_uhash_t)item->collision_hash;
}

static int
item_compare(const void *left, const void *right)
{
  const struct test_item_s *left_item = left;
  const struct test_item_s *right_item = right;
  return left_item->id == right_item->id;
}

static _Py_hashtable_t *
new_table(void)
{
  _Py_hashtable_allocator_t allocator =
    {
      .malloc = tracked_malloc,
      .free = tracked_free,
    };

  return _Py_hashtable_new_full(collision_hash, item_compare, NULL, NULL,
                                &allocator);
}

static int
reset_accounting(void)
{
  CHECK(g_alloc.live_blocks == 0);
  CHECK(g_alloc.live_bytes == 0);
  memset(&g_alloc, 0, sizeof(g_alloc));
  return 0;
}

static int
check_accounting(void)
{
  CHECK(g_alloc.attempts == g_alloc.successes + g_alloc.failures);
  CHECK(g_alloc.successes == g_alloc.frees);
  CHECK(g_alloc.live_blocks == 0);
  CHECK(g_alloc.live_bytes == 0);
  return 0;
}

static int
set_item(_Py_hashtable_t *ht, unsigned int id)
{
  return _Py_hashtable_set(ht, &g_keys[id], &g_values[id]);
}

static int
check_present(_Py_hashtable_t *ht, unsigned int first, unsigned int last)
{
  unsigned int id;

  for (id = first; id <= last; id++)
    {
      CHECK(_Py_hashtable_get(ht, &g_keys[id]) == &g_values[id]);
    }

  return 0;
}

static int
test_growth_shrink_and_collisions(void)
{
  _Py_hashtable_t *ht;
  unsigned int id;

  CHECK(reset_accounting() == 0);
  ht = new_table();
  CHECK(ht != NULL);
  CHECK(ht->nbuckets == 16);
  CHECK(ht->nentries == 0);

  for (id = 1; id <= 8; id++)
    {
      CHECK(set_item(ht, id) == 0);
      CHECK(ht->nbuckets == 16);
    }

  CHECK(set_item(ht, 9) == 0);
  CHECK(ht->nbuckets == 32);
  CHECK(ht->nentries == 9);
  CHECK(check_present(ht, 1, 9) == 0);

  for (id = 10; id <= 16; id++)
    {
      CHECK(set_item(ht, id) == 0);
      CHECK(ht->nbuckets == 32);
    }

  CHECK(set_item(ht, 17) == 0);
  CHECK(ht->nbuckets == 64);
  CHECK(ht->nentries == 17);
  CHECK(check_present(ht, 1, 17) == 0);

  for (id = 17; id >= 8; id--)
    {
      CHECK(_Py_hashtable_steal(ht, &g_keys[id]) == &g_values[id]);
      CHECK(ht->nbuckets == 64);
    }

  CHECK(ht->nentries == 7);
  CHECK(_Py_hashtable_steal(ht, &g_keys[7]) == &g_values[7]);
  CHECK(ht->nentries == 6);
  CHECK(ht->nbuckets == 32);
  CHECK(check_present(ht, 1, 6) == 0);

  CHECK(_Py_hashtable_steal(ht, &g_keys[6]) == &g_values[6]);
  CHECK(ht->nbuckets == 32);
  CHECK(_Py_hashtable_steal(ht, &g_keys[5]) == &g_values[5]);
  CHECK(ht->nbuckets == 32);
  CHECK(_Py_hashtable_steal(ht, &g_keys[4]) == &g_values[4]);
  CHECK(ht->nentries == 3);
  CHECK(ht->nbuckets == 16);
  CHECK(check_present(ht, 1, 3) == 0);

  for (id = 4; id <= 17; id++)
    {
      CHECK(_Py_hashtable_get(ht, &g_keys[id]) == NULL);
    }

  _Py_hashtable_destroy(ht);
  return check_accounting();
}

static int
test_growth_allocation_rollback_and_retry(void)
{
  _Py_hashtable_t *ht;
  size_t live_blocks;
  size_t live_bytes;
  unsigned int id;

  CHECK(reset_accounting() == 0);
  ht = new_table();
  CHECK(ht != NULL);
  for (id = 1; id <= 8; id++)
    {
      CHECK(set_item(ht, id) == 0);
    }

  live_blocks = g_alloc.live_blocks;
  live_bytes = g_alloc.live_bytes;
  g_alloc.fail_on_attempt = g_alloc.attempts + 2;
  CHECK(set_item(ht, 9) == -1);
  CHECK(g_alloc.failures == 1);
  CHECK(ht->nentries == 8);
  CHECK(ht->nbuckets == 16);
  CHECK(_Py_hashtable_get(ht, &g_keys[9]) == NULL);
  CHECK(g_alloc.live_blocks == live_blocks);
  CHECK(g_alloc.live_bytes == live_bytes);
  CHECK(check_present(ht, 1, 8) == 0);

  g_alloc.fail_on_attempt = 0;
  CHECK(set_item(ht, 9) == 0);
  CHECK(ht->nentries == 9);
  CHECK(ht->nbuckets == 32);
  CHECK(check_present(ht, 1, 9) == 0);

  _Py_hashtable_destroy(ht);
  return check_accounting();
}

static int
test_ignored_shrink_allocation_failure(void)
{
  _Py_hashtable_t *ht;
  _Py_slist_t *old_buckets;
  unsigned int id;

  CHECK(reset_accounting() == 0);
  ht = new_table();
  CHECK(ht != NULL);
  for (id = 1; id <= 17; id++)
    {
      CHECK(set_item(ht, id) == 0);
    }
  CHECK(ht->nbuckets == 64);

  for (id = 17; id >= 8; id--)
    {
      CHECK(_Py_hashtable_steal(ht, &g_keys[id]) == &g_values[id]);
    }
  CHECK(ht->nentries == 7);
  CHECK(ht->nbuckets == 64);

  old_buckets = ht->buckets;
  g_alloc.fail_on_attempt = g_alloc.attempts + 1;
  CHECK(_Py_hashtable_steal(ht, &g_keys[7]) == &g_values[7]);
  CHECK(g_alloc.failures == 1);
  CHECK(ht->nentries == 6);
  CHECK(ht->nbuckets == 64);
  CHECK(ht->buckets == old_buckets);
  CHECK(_Py_hashtable_get(ht, &g_keys[7]) == NULL);
  CHECK(check_present(ht, 1, 6) == 0);

  g_alloc.fail_on_attempt = 0;
  _Py_hashtable_destroy(ht);
  return check_accounting();
}

static int
test_overflow_rehash_rejection(void)
{
  _Py_hashtable_t *ht;
  _Py_slist_t *old_buckets;
  size_t allocation_attempts;

  CHECK(reset_accounting() == 0);
  ht = new_table();
  CHECK(ht != NULL);
  old_buckets = ht->buckets;
  allocation_attempts = g_alloc.attempts;

  ht->nentries = SIZE_MAX / 10 + 1;
  CHECK(hashtable_rehash(ht) == -1);
  CHECK(g_alloc.attempts == allocation_attempts);
  CHECK(ht->nbuckets == 16);
  CHECK(ht->buckets == old_buckets);

  ht->nentries = 0;
  _Py_hashtable_destroy(ht);
  return check_accounting();
}

int
main(void)
{
  unsigned int id;

  for (id = 0; id < TEST_ITEM_COUNT; id++)
    {
      g_keys[id].id = id;
      g_keys[id].collision_hash = id & 1u;
      g_values[id].id = id + 1000u;
      g_values[id].collision_hash = 0;
    }

  CHECK(test_growth_shrink_and_collisions() == 0);
  CHECK(test_growth_allocation_rollback_and_retry() == 0);
  CHECK(test_ignored_shrink_allocation_failure() == 0);
  CHECK(test_overflow_rehash_rejection() == 0);

  puts("PASS: actual patched P2 CPython hashtable behavior");
  return 0;
}
