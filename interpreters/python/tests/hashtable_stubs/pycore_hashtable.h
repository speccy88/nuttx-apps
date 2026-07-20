#ifndef TEST_CPYTHON_PYCORE_HASHTABLE_H
#define TEST_CPYTHON_PYCORE_HASHTABLE_H

#include "Python.h"

typedef struct _Py_slist_item_s
{
  struct _Py_slist_item_s *next;
} _Py_slist_item_t;

typedef struct
{
  _Py_slist_item_t *head;
} _Py_slist_t;

#define _Py_SLIST_ITEM_NEXT(item) \
  _Py_RVALUE(((_Py_slist_item_t *)(item))->next)
#define _Py_SLIST_HEAD(list) _Py_RVALUE(((_Py_slist_t *)(list))->head)

typedef struct
{
  _Py_slist_item_t slist_item;
  Py_uhash_t key_hash;
  void *key;
  void *value;
} _Py_hashtable_entry_t;

struct _Py_hashtable_t;
typedef struct _Py_hashtable_t _Py_hashtable_t;

typedef Py_uhash_t (*_Py_hashtable_hash_func)(const void *key);
typedef int (*_Py_hashtable_compare_func)(const void *key1,
                                          const void *key2);
typedef void (*_Py_hashtable_destroy_func)(void *key);
typedef _Py_hashtable_entry_t *(*_Py_hashtable_get_entry_func)(
    _Py_hashtable_t *ht, const void *key);

typedef struct
{
  void *(*malloc)(size_t size);
  void (*free)(void *ptr);
} _Py_hashtable_allocator_t;

struct _Py_hashtable_t
{
  size_t nentries;
  size_t nbuckets;
  _Py_slist_t *buckets;
  _Py_hashtable_get_entry_func get_entry_func;
  _Py_hashtable_hash_func hash_func;
  _Py_hashtable_compare_func compare_func;
  _Py_hashtable_destroy_func key_destroy_func;
  _Py_hashtable_destroy_func value_destroy_func;
  _Py_hashtable_allocator_t alloc;
};

typedef int (*_Py_hashtable_foreach_func)(_Py_hashtable_t *ht,
                                          const void *key,
                                          const void *value,
                                          void *user_data);

#endif /* TEST_CPYTHON_PYCORE_HASHTABLE_H */
