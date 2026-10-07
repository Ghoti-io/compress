/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file bzip2_bwt.c
 *
 * The Burrows-Wheeler transform of a bzip2 block, by suffix sorting.
 *
 * bzip2 sorts the *rotations* of a block, not its suffixes. The two are the
 * same sort when the block is written twice over: a rotation of T is a prefix
 * of length n of a suffix of TT, so the suffixes of TT that start in the first
 * copy come out in rotation order, whatever happens after the first n bytes.
 * Equal rotations (a block that is a repetition of something shorter) come out
 * in some order, and any order gives the same last column, which is all a
 * decoder reads.
 *
 * TT with a sentinel is at most 1,800,001 symbols, and the suffix array is
 * built by SA-IS (Nong, Zhang and Chan, 2009), which is linear in that: the
 * suffixes are classed as L or S type, the LMS substrings are sorted by one
 * induced sort, named, solved recursively if the names are not yet distinct,
 * and the whole array is then induced from the sorted LMS suffixes. libbz2
 * sorts blocks with a bucketed multikey quicksort that falls back to a
 * different algorithm on repetitive input, because it has no guarantee. This
 * one has.
 *
 * The text is held as 16-bit symbols at the top level, where it is 2n + 1 long
 * and every symbol fits (a byte plus one, and the sentinel), and as 32-bit
 * symbols below it, where the reduced text can have up to n/2 distinct names.
 * The body of the sort is written once, in bzip2_sais_body.h, and included for
 * each width. The symbol counts are taken once per level and the bucket
 * boundaries derived from them, which is what keeps the passes over the text
 * to the ones the algorithm needs; and the two induced sorts prefetch the text
 * and type bytes of the suffixes a few steps ahead, since those are the random
 * accesses that cost the time.
 *
 * test_bzip2_bwt.cpp checks it against sorting the rotations directly.
 */

#include "../../core/alloc_internal.h"
#include "bzip2_internal.h"

#include <string.h>

/* The bucket boundaries from the symbol counts: where each symbol's bucket
 * starts, or one past where it ends. */
static void bucket_bounds(
    const int32_t * cnt, int32_t * bkt, int32_t k, int end) {
  int32_t i, sum = 0;
  for (i = 0; i < k; i++) {
    sum += cnt[i];
    bkt[i] = end ? sum : sum - cnt[i];
  }
}

#define SAIS_LOOKAHEAD 12

#define SAIS_SYM int32_t
#define SAIS_NAME sais_i32
#define SAIS_RECURSE sais_i32
#include "bzip2_sais_body.h"
#undef SAIS_SYM
#undef SAIS_NAME
#undef SAIS_RECURSE

#define SAIS_SYM uint16_t
#define SAIS_NAME sais_u16
#define SAIS_RECURSE sais_i32
#include "bzip2_sais_body.h"
#undef SAIS_SYM
#undef SAIS_NAME
#undef SAIS_RECURSE

int bzip2_bwt(const gcomp_allocator_t * alloc, bzip2_bwt_scratch_t * scratch,
    const uint8_t * block, uint32_t n, uint8_t * last, uint32_t * orig_ptr) {
  int32_t total = (int32_t)(2u * n + 1u);
  int32_t i, row;
  if (n == 0) {
    return -1;
  }
  for (i = 0; i < (int32_t)n; i++) {
    scratch->s[i] = (uint16_t)(block[i] + 1u);
  }
  memcpy(scratch->s + n, scratch->s, (size_t)n * sizeof(uint16_t));
  scratch->s[total - 1] = 0;
  if (sais_u16(alloc, scratch->s, scratch->sa, total, 257) != 0) {
    return -1;
  }
  row = 0;
  for (i = 0; i < total; i++) {
    int32_t p = scratch->sa[i];
    if (p < (int32_t)n) {
      last[row] = block[p == 0 ? n - 1 : (uint32_t)p - 1];
      if (p == 0) {
        *orig_ptr = (uint32_t)row;
      }
      row++;
    }
  }
  return 0;
}
