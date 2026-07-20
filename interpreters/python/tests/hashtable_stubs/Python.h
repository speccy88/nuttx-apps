#ifndef TEST_CPYTHON_PYTHON_H
#define TEST_CPYTHON_PYTHON_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t Py_uhash_t;

#define _Py_P2_HUB_RESIDENT
#define _Py_RVALUE(value) (value)

void *PyMem_Malloc(size_t size);
void PyMem_Free(void *ptr);

#endif /* TEST_CPYTHON_PYTHON_H */
