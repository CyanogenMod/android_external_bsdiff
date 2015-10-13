// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _BSDIFF_EXTENTS_H_
#define _BSDIFF_EXTENTS_H_

#include "extents_file.h"

namespace bsdiff {

/* Parses a string representation |ex_str| and populates an array |ex_arr|
 * consisting of |*ex_count_p| extents. The string is expected to be a
 * comma-separated list of pairs of the form "offset:length". An offset may be
 * -1 or a non-negative integer; the former indicates a sparse extent
 * (consisting of zeros). A length is a positive integer.  If |ex_arr| is NULL,
 * |*ex_count_p| is ignored and a new array is allocated based on the actual
 * number of extents parsed.  Upon success, returns a pointer to the populated
 * array of extents and stores the actual number of extents at the location
 * pointed to be |ex_count_p| (if provided).  If the string parses correctly but
 * the operation otherwise fails (allocation error, array too small), returns
 * NULL but still store the number of parsed extents.  Otherwise, returns NULL
 * and does not store anything. If a new array was allocated, then it should be
 * deallocated with free(3). */
ex_t* extents_parse(const char* ex_str, ex_t* ex_arr, size_t* ex_count_p);

}  // namespace bsdiff

#endif /* _BSDIFF_EXTENTS_H_ */
