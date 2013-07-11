/*
 * Copyright 2013 University of Chicago and Argonne National Laboratory
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

/*
  BLOB.C
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <tools.h>
#include "src/tcl/blob/blob.h"

turbine_blob*
blobutils_make_test(void)
{
  turbine_blob* d = (turbine_blob*) malloc(sizeof(turbine_blob));
  char* t = (char*) malloc(64);
  sprintf(t, "howdy");
  d->pointer = t;
  d->length = (int)strlen(t)+1;
  return d;
}

turbine_blob*
blobutils_create(long pointer, int length)
{
  return blobutils_create_ptr((void*) pointer, length);
}

turbine_blob*
blobutils_create_ptr(void* pointer, int length)
{
  turbine_blob* result = malloc(sizeof(turbine_blob));
  result->pointer = pointer;
  result->length = length;
  return result;
}

void*
blobutils_malloc(size_t bytes)
{
  void* result = malloc(bytes);
  assert(result);
  // fprintf(stderr, "malloc: %p\n", result);
  return result;
}

void*
blobutils_ptr_add(void* p, int offset)
{
  return p + offset;
}

int
blobutils_sizeof_int(void)
{
  return sizeof(int);
}

int
blobutils_sizeof_int32(void)
{
  return sizeof(int32_t);
}

int
blobutils_sizeof_float(void)
{
  return sizeof(double);
}

void*
blobutils_cast_to_ptr(int i)
{
  return (void*) (size_t)i;
}

int
blobutils_cast_to_int(void* p)
{
  long i_long = (long) p;
  int result = i_long;
  // fprintf(stderr, "blobutils_cast_to_int: %p -> %li %i\n",
  //         p, i_long, result);
  valgrind_assert_msg(i_long == result,
                      "pointer is too long for int!");
  return result;
}

long
blobutils_cast_to_long(void* p)
{
  long result = (long) p;
  return result;
}


int*
blobutils_cast_int_to_int_ptr(int i)
{
  return (int*) (size_t) i;
}

const int*
blobutils_cast_int_to_const_int_ptr(int i)
{
  return (const int*) (size_t) i;
}

double*
blobutils_cast_int_to_dbl_ptr(int i)
{
  return (double*) (size_t) i;
}

const double*
blobutils_cast_int_to_const_dbl_ptr(int i)
{
  return (const double*) (size_t) i;
}

int*
blobutils_cast_to_int_ptr(void* p)
{
  return (int*) p;
}

int32_t*
blobutils_cast_to_int32_ptr(void* p)
{
  return (int32_t*) p;
}

int64_t*
blobutils_cast_to_int64_ptr(void* p)
{
  return (int64_t*) p;
}

double*
blobutils_cast_to_dbl_ptr(void* p)
{
  return (double*) p;
}

void
blobutils_zeroes_float(double* p, int n)
{
  for (int i = 0; i < n; i++)
    p[i] = 0.0;
}


double
blobutils_get_float(double* pointer, int index)
{
  return pointer[index];
}

void
blobutils_set_float(double* p, int i, double d)
{
  p[i] = d;
}

int
blobutils_get_int(int* pointer, int index)
{
  return pointer[index];
}

int32_t
blobutils_get_int32(int32_t* pointer, int index)
{
  return pointer[index];
}

/**
   Assume blob is array of int- do array lookup
 */
void
blobutils_set_int(int* pointer, int index, int i)
{
  pointer[index] = i;
}

char
blobutils_get_char(turbine_blob* data, int index)
{
  char* d = (char*) data->pointer;
  return d[index];
}

void
blobutils_destroy(turbine_blob* data)
{
  free(data->pointer);
  free(data);
}

static inline int write_all(int fd, void* buffer, int count);

bool
blobutils_write(const char* output, turbine_blob* blob)
{
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
  mode_t mode = S_IRUSR | S_IWUSR;
  int fd = open(output, flags, mode);
  if (fd == -1)
  {
    printf("could not write to: %s\n", output);
    return false;
  }

  bool result = write_all(fd, blob->pointer, blob->length);
  return result;
}

static inline int read_all(int fd, void* buffer, int count);

bool
blobutils_read(const char* input, turbine_blob* blob)
{
  int fd = open(input, O_RDONLY);
  if (fd == -1)
  {
    printf("could not read from: %s\n", input);
    return false;
  }

  struct stat s;
  int rc = fstat(fd, &s);
  assert(rc == 0);

  blob->length = s.st_size;
  blob->pointer = malloc((size_t)blob->length);
  if (!blob->pointer)
  {
    printf("could not allocate memory for: %s\n", input);
    return false;
  }

  bool result = read_all(fd, blob->pointer, blob->length);
  return result;
}

/**
   Utility function to write whole buffer to file
*/
static inline int
write_all(int fd, void* buffer, int count)
{
  int bytes;
  int total = 0;
  int chunk = count;
  while ((bytes = write(fd, buffer, (size_t)chunk)))
  {
    total += bytes;
    if (total == count)
      return true;

    chunk -= bytes;
    buffer += bytes;
  }

  // Must be some kind of error
  return false;
}

/**
   Utility function to read whole file into buffer
*/
static inline int
read_all(int fd, void* buffer, int count)
{
  int bytes;
  int total = 0;
  int chunk = count;
  while ((bytes = read(fd, buffer, (size_t)chunk)))
  {
    total += bytes;
    if (total == count)
      return total;

    chunk -= bytes;
    buffer += bytes;
  }

  return total;
}
