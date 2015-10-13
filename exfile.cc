// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "exfile.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Extent files implementation.  Some things worth noting:
 *
 * - We are using glibc's buffered FILE objects for the underlying file I/O;
 *   this contributes to improved performance, especially with badly fragmented
 *   extents.  However, the FILE handle we return to the caller is decidedly
 *   unbuffered: making it buffered too seems superfluous, causing excess data
 *   copying and memory use.
 *
 * - We maintain the "logical" file position separately from the "physical"
 *   (underlying) file position. The latter is updated lazily whenever actual
 *   file I/O is about to be performed.
 *
 * - The logical position of an extent file is internally represented by the
 *   current extent index (curr_ex_idx) and the position within the current
 *   extent (curr_ex_pos), as well as an absolute logical position (curr_pos).
 *   In general, curr_pos should equal the total length of all extents prior to
 *   curr_ex_idx, plus curr_ex_pos.  Also, curr_ex_idx may range between 0 and
 *   the total extent count; if it is exactly the latter, then curr_ex_pos must
 *   be zero, representing the fact that the we are at the logical end of the
 *   file.  Otherwise, curr_ex_pos may range between 0 and the length of the
 *   current extent; if it is exactly the latter, then this is equivalent to
 *   position zero on the next extent.  All functions should honor this
 *   duality.
 *
 * - Seeking is done efficiently at O(log(D)), where D is the
 *   number of extents between the current position and the new one. This seems
 *   like a good midway for supporting both sequential and random access.
 */


#define TRUE 1
#define FALSE 0

#define arraysize(x) (sizeof(x) / sizeof(*(x)))


/* Extent prefix length. */
typedef struct {
  size_t prec;  /* total length of preceding extents */
  size_t total; /* total length including current extent */
} prefix_len_t;

/* Extent file logical modes. Used as index to the mapping from logical modes
 * to open(2) and fopen(3) modes below. */
typedef enum {
  EXFILE_MODE_RO,
  EXFILE_MODE_WO,
  EXFILE_MODE_RW,
  EXFILE_MODE_MAX /* sentinel */
} exfile_mode_t;

/* An extent file control object (aka "cookie"). */
typedef struct {
  int fd;                       /* underlying file descriptor */
  size_t ex_count;              /* number of extents (non-zero) */
  ex_t* ex_arr;                 /* array of extents */
  prefix_len_t* prefix_len_arr; /* total lengths of extent prefixes */
  void (*ex_free)(void*);       /* extent array free function */
  size_t total_ex_len;          /* total length of all extents (constant) */
  off_t curr_file_pos;          /* current underlying file position */
  size_t curr_ex_idx;           /* current extent index */
  size_t curr_ex_pos;           /* current position within extent */
  off_t curr_pos;               /* current logical file position */
} exfile_t;


/* Mapping from fopen(3) modes to extent file logical modes. */
static const struct {
  const char* fopen_mode;
  exfile_mode_t mode;
} fopen_mode_to_mode[] = {
    {"r", EXFILE_MODE_RO},
    {"r+", EXFILE_MODE_RW},
    {"w", EXFILE_MODE_WO},
    {"w+", EXFILE_MODE_RW},
};


/* Mapping from extent file logical modes to open(2) flags. */
static const int mode_to_open_flags[EXFILE_MODE_MAX] = {
    O_RDONLY,
    O_WRONLY,
    O_RDWR,
};


/* Searches an array |ex_arr| of |ex_count| extents and returns the index of
 * the extent that contains the location |pos|.  Uses an array |prefix_len_arr|
 * of corresponding prefix lengths. The total complexity is O(log(D)), where D
 * is the distance between the returned extent index and |init_ex_idx|. */
static size_t ex_arr_search(size_t ex_count,
                            const ex_t* ex_arr,
                            const prefix_len_t* prefix_len_arr,
                            size_t pos,
                            size_t init_ex_idx) {
  assert(ex_arr && ex_count);
  const ssize_t last_ex_idx = ex_count - 1;
  assert(init_ex_idx <= ex_count);
  assert(pos < prefix_len_arr[last_ex_idx].total);
  if (init_ex_idx == ex_count)
    init_ex_idx = last_ex_idx; /* adjustment for purposes of the search below */

  /* First, search in exponentially increasing leaps from the current extent,
   * until an interval bounding the target position was obtained. Here i and j
   * are the left and right (inclusive) index boundaries, respectively. */
  ssize_t i = init_ex_idx;
  ssize_t j = i;
  size_t leap = 1;
  /* Go left, as needed. */
  while (i > 0 && pos < prefix_len_arr[i].prec) {
    j = i - 1;
    if ((i -= leap) < 0)
      i = 0;
    leap <<= 1;
  }
  /* Go right, as needed. */
  while (j < last_ex_idx && pos >= prefix_len_arr[j].total) {
    i = j + 1;
    if ((j += leap) > last_ex_idx)
      j = last_ex_idx;
    leap <<= 1;
  }

  /* Then, perform a binary search between i and j. */
  size_t k = 0;
  while (1) {
    k = (i + j) / 2;
    if (pos < prefix_len_arr[k].prec)
      j = k - 1;
    else if (pos >= prefix_len_arr[k].total)
      i = k + 1;
    else
      break;
  }

  return k;
}

/* Performs I/O operations (either read or write) on an extent file, advancing
 * through consecutive extents and updating the logical/physical file position
 * as we go. */
static ssize_t exfile_io(exfile_t* xf, int do_read, char* buf, size_t size) {
  if (xf->curr_ex_idx == xf->ex_count)
    return 0; /* end-of-extent-file */

  /* Reading or writing? */
  typedef ssize_t(io_func_t)(int, void*, size_t);
  io_func_t* io_func;
  ssize_t error_ret_val;
  if (do_read) {
    io_func = read;
    error_ret_val = -1;
  } else {
    io_func = (io_func_t*)write;
    error_ret_val = 0; /* must not return a negative value when writing */
  }

  /* Start processing data along extents. */
  const ex_t* curr_ex = xf->ex_arr + xf->curr_ex_idx;
  assert(curr_ex->len >= xf->curr_ex_pos);
  size_t curr_ex_rem_len = curr_ex->len - xf->curr_ex_pos;
  ssize_t total_bytes = 0;
  while (size) {
    /* Advance to the next extent of non-zero length. */
    while (curr_ex_rem_len == 0) {
      xf->curr_ex_idx++;
      xf->curr_ex_pos = 0;
      if (xf->curr_ex_idx == xf->ex_count)
        return total_bytes; /* end-of-extent-file */
      curr_ex++;
      curr_ex_rem_len = curr_ex->len;
    }

    const int is_real_ex = (curr_ex->off >= 0);

    /* Seek to the correct file position, as necessary. */
    if (is_real_ex) {
      const off_t file_pos = curr_ex->off + xf->curr_ex_pos;
      if (xf->curr_file_pos != file_pos) {
        if (lseek(xf->fd, file_pos, SEEK_SET) == (off_t)-1) {
          xf->curr_file_pos = -1; /* unknown file position */
          return total_bytes ? total_bytes : error_ret_val;
        }
        xf->curr_file_pos = file_pos;
      }
    }

    /* Process data to the end of the current extent or the requested
     * count, whichever is smaller. */
    size_t io_count = (size < curr_ex_rem_len ? size : curr_ex_rem_len);
    ssize_t io_bytes = io_count;
    if (is_real_ex)
      io_bytes = io_func(xf->fd, buf, io_count);
    else if (do_read)
      memset(buf, 0, io_count);

    /* Stop on error. */
    if (io_bytes < 0) {
      if (total_bytes == 0)
        total_bytes = error_ret_val;
      break;
    }

    /* Update read state. */
    total_bytes += io_bytes;
    if (is_real_ex)
      xf->curr_file_pos += io_bytes;
    xf->curr_ex_pos += io_bytes;
    xf->curr_pos += io_bytes;

    /* If we didn't read the whole extent, finish; delegate handling of
     * partial read/write back to the caller. */
    if ((curr_ex_rem_len -= io_bytes) > 0)
      break;

    /* Update total count and buffer pointer. */
    size -= io_bytes;
    buf += io_bytes;
  }

  return total_bytes;
}

/* Reads up to |size| bytes from an extent file into |buf|. This implements the
 * cookie_read_function_t interface and is a thin wrapper around exfile_io()
 * (see above). Returns the number of bytes read, or -1 on error. */
static ssize_t exfile_read(void* cookie, char* buf, size_t size) {
  return exfile_io((exfile_t*)cookie, TRUE, buf, size);
}

/* Writes up to |size| bytes from |buf| to an extent file. This implements the
 * cookie_write_function_t interface and is a thin wrapper around exfile_io()
 * (see above). Returns the number of bytes written; must NOT return a negative
 * value. */
static ssize_t exfile_write(void* cookie, const char* buf, size_t size) {
  return exfile_io((exfile_t*)cookie, FALSE, (char*)buf, size);
}

/* Performs seek on an extent file, repositioning it to the value of |*pos_p|
 * according to |whence|. This implements the cookie_seek_function_t interface.
 * On success, stores the resulting logical position measured in bytes along
 * contiguous extents into |*pos_p| and returns 0; otherwise returns -1. */
static int exfile_seek(void* cookie, off64_t* pos_p, int whence) {
  exfile_t* xf = (exfile_t*)cookie;

  /* Compute the absolute logical target position. */
  off64_t new_pos = *pos_p;
  if (whence == SEEK_CUR)
    new_pos += xf->curr_pos;
  else if (whence == SEEK_END)
    new_pos += xf->total_ex_len;

  /* Ensure that the target position is valid.  Note that repositioning the
   * file right past the last extent is considered valid, in line with normal
   * seek behavior, although no write (nor read) can be performed there. */
  if (new_pos < 0 || new_pos > (off64_t)xf->total_ex_len)
    return -1;

  if (new_pos != (off64_t)xf->curr_pos) {
    /* Find the extend that contains the requested logical position; handle
     * special case upfront, for efficiency. */
    size_t new_ex_idx = 0;
    if (new_pos == (off64_t)xf->total_ex_len)
      new_ex_idx = xf->ex_count;
    else if (new_pos)
      new_ex_idx = ex_arr_search(xf->ex_count, xf->ex_arr, xf->prefix_len_arr,
                                 new_pos, xf->curr_ex_idx);

    /* Set the logical position markers. */
    xf->curr_ex_idx = new_ex_idx;
    xf->curr_ex_pos =
        (new_ex_idx < xf->ex_count
             ? (size_t)(new_pos - xf->prefix_len_arr[new_ex_idx].prec)
             : 0);
    xf->curr_pos = (off_t)new_pos;
  }

  *pos_p = new_pos;
  return 0;
}

/* Closes an open extent file. This implements the cookie_close_function_t
 * interface. Always returns 0 (success). */
static int exfile_close(void* cookie) {
  exfile_t* xf = (exfile_t*)cookie;
  if (xf) {
    if (xf->fd >= 0)
      close(xf->fd);
    free(xf->prefix_len_arr);
    if (xf->ex_free)
      xf->ex_free(xf->ex_arr);
    free(xf);
  }
  return 0;
}

static const cookie_io_functions_t cookie_io_funcs = {
    .read = exfile_read,
    .write = exfile_write,
    .seek = exfile_seek,
    .close = exfile_close,
};

static FILE* exfile_open(int fd,
                         const char* path,
                         const char* fopen_mode,
                         ex_t* ex_arr,
                         size_t ex_count,
                         void (*ex_free)(void*)) {
  /* Argument sanity check. */
  if (!(ex_arr && ex_count && (fd >= 0 || path) && (fd < 0 || !path)))
    return NULL;

  /* Validate mode argument. */
  exfile_mode_t mode = EXFILE_MODE_MAX;
  size_t i;
  for (i = 0; i < arraysize(fopen_mode_to_mode); i++)
    if (!strcmp(fopen_mode_to_mode[i].fopen_mode, fopen_mode)) {
      mode = fopen_mode_to_mode[i].mode;
      break;
    }
  if (mode == EXFILE_MODE_MAX)
    return NULL;

  /* Open the underlying file, if not already provided. */
  int do_close_fd = FALSE;
  if (fd < 0) {
    if ((fd = open(path, mode_to_open_flags[mode])) < 0)
      return NULL;
    do_close_fd = TRUE;
  }

  /* Allocate memory and open file streams, for both the underlying file and
   * the handle returned to the caller. */
  exfile_t* xf = NULL;
  prefix_len_t* prefix_len_arr = NULL;
  FILE* handle = NULL;
  if (!((xf = (exfile_t*)calloc(1, sizeof(exfile_t))) &&
        (prefix_len_arr =
             (prefix_len_t*)malloc(sizeof(prefix_len_t) * ex_count)) &&
        (handle = fopencookie(xf, fopen_mode, cookie_io_funcs)))) {
    /* If a file was opened above, close it. */
    if (do_close_fd)
      close(fd);
    if (xf)
      xf->fd = -1; /* invalidate prior to calling exfile_close() */

    free(prefix_len_arr);
    if (handle)
      fclose(handle); /* will call exfile_close already */
    else
      exfile_close(xf);
    return NULL;
  }

  /* Compute the prefix lengths. */
  size_t prefix_len = 0;
  for (i = 0; i < ex_count; i++) {
    prefix_len_arr[i].prec = prefix_len;
    prefix_len += ex_arr[i].len;
    prefix_len_arr[i].total = prefix_len;
  }

  /* Configure control object, including physical/logical file position. */
  xf->fd = fd;
  xf->ex_count = ex_count;
  xf->ex_arr = ex_arr;
  xf->prefix_len_arr = prefix_len_arr;
  xf->ex_free = ex_free;
  xf->total_ex_len = prefix_len_arr[ex_count - 1].total;
  xf->curr_file_pos = lseek(fd, 0, SEEK_CUR);
  xf->curr_ex_idx = 0;
  xf->curr_ex_pos = 0;
  xf->curr_pos = 0;

  /* Return the external stream handle. */
  return handle;
}

FILE* exfile_fopen(const char* path,
                   const char* fopen_mode,
                   ex_t* ex_arr,
                   size_t ex_count,
                   void (*ex_free)(void*)) {
  return exfile_open(-1, path, fopen_mode, ex_arr, ex_count, ex_free);
}

FILE* exfile_fdopen(int fd,
                    const char* fopen_mode,
                    ex_t* ex_arr,
                    size_t ex_count,
                    void (*ex_free)(void*)) {
  return exfile_open(fd, NULL, fopen_mode, ex_arr, ex_count, ex_free);
}
