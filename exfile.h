// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _BSDIFF_EXFILE_H_
#define _BSDIFF_EXFILE_H_

#include <stdio.h>

/*
 * Extent files.
 *
 * This modules provides a familiar interface for handling files through an
 * indirection layer of extents, which are contiguous chunks of variable length
 * at arbitrary offsets within a file.  Once an extent file handle is obtained,
 * users may read, write and seek as they do with ordinary files, having the I/O
 * with the underlying file done for them by the extent file implementation. The
 * implementation supports "sparse extents", which are assumed to contain zeros
 * but otherwise have no actual representation in the underlying file; these are
 * denoted by negative offset values.
 *
 * Unlike ordinary files, the size of an extent file is fixed; it is not
 * truncated on open, nor is writing past the extent span allowed. Also, writing
 * to a sparse extent has no effect and will not raise an error.
 */


/* An extent, defined by an offset and a length. */
typedef struct {
  off_t off;  /* the extent offset; negative indicates a sparse extent */
  size_t len; /* the extent length */
} ex_t;


/* Opens a file |path| with use mode |fopen_mode| for use with an array of
 * extents |ex_arr| of length |ex_count|. The mode string can be either of "r"
 * (read-only), "w" (write-only) or "r+"/"w+" (read-write); the underlying file
 * is neither created (if not present) nor truncated (if present) when opened
 * for writing. The function |ex_free|, if not NULL, will be called to
 * deallocate the extent array once the file object is closed.  Returns a FILE
 * pointer that can be used with ordinary stream functions (e.g.  fread), or
 * NULL if opening the file has failed.  */
FILE* exfile_fopen(const char* path,
                   const char* fopen_mode,
                   ex_t* ex_arr,
                   size_t ex_count,
                   void (*ex_free)(void*));

/* Associates an extent file stream with an already open file descriptor |fd|.
 * The |fopen_mode| argument is as decribed above and must be compatible with
 * the mode of |fd|. All other arguments, behaviors and return values are as
 * those of exfile_fopen (see above). */
FILE* exfile_fdopen(int fd,
                    const char* fopen_mode,
                    ex_t* ex_arr,
                    size_t ex_count,
                    void (*ex_free)(void*));

#endif /* _BSDIFF_EXFILE_H_ */
