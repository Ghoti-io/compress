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
 * bzip2 sorts the *rotations* of a block, not its suffixes, and the two are
 * the same sort for one rotation of the block: the least one. A Lyndon word
 * (a word strictly smaller than each of its proper suffixes) has its rotations
 * in the order of its suffixes. If suffix j is a prefix of suffix i, the
 * suffix order puts j first, and so does the rotation order, because what the
 * rotation of i has after that prefix, a proper suffix of the word, begins with
 * a character the word's own beginning is smaller than; and the word being
 * unbordered, never begins with that suffix. So the block is rotated to its
 * least rotation (found in linear time by the two-pointer minimal-rotation
 * scan), that rotation's suffixes are sorted with a smaller sentinel after
 * them, and row r of the sort is rotation SA[r] of it.
 *
 * A block that is a repetition (a power of a Lyndon word) is the one case that
 * is not a Lyndon word. Its distinct rotations still come out in the right
 * order, by the same argument applied to the repeated word, and equal rotations
 * come out in some order, and any order gives the same last column, which is
 * all a decoder reads.
 *
 * The earlier version sorted the block written twice over (a rotation is a
 * prefix of a suffix of TT), which is a sort of 2n + 1 symbols. This one is a
 * sort of n + 1, so about half the time and half the memory, for a scan of the
 * block.
 *
 * The suffix array is built by SA-IS (Nong, Zhang and Chan, 2009), which is
 * linear: the suffixes are classed as L or S type, the LMS substrings are
 * sorted by one induced sort, named, solved recursively if the names are not
 * yet distinct, and the whole array is then induced from the sorted LMS
 * suffixes. libbz2 sorts blocks with a bucketed multikey quicksort that falls
 * back to a different algorithm on repetitive input, because it has no
 * guarantee. This one has.
 *
 * The text is held as 16-bit symbols at the top level, where every symbol fits
 * (a byte plus one, and the sentinel), and as 32-bit symbols below it, where
 * the reduced text can have up to n/2 distinct names. The body of the sort is
 * written once, in bzip2_sais_body.h, and included for each width. The symbol
 * counts are taken once per level and the bucket boundaries derived from them,
 * which is what keeps the passes over the text to the ones the algorithm
 * needs; and the two induced sorts prefetch the text and type bytes of the
 * suffixes a few steps ahead, since those are the random accesses that cost
 * the time.
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

/* The start of the least rotation of block[0..n). The two-pointer scan:
 * candidates i and j are compared k characters in; the loser, and the k
 * starts after it that the match proves are no better, are dropped. Linear,
 * and for a repetition it stops with the smaller candidate, which starts a
 * least rotation. */
static uint32_t least_rotation(const uint8_t * block, uint32_t n) {
  uint32_t i = 0, j = 1, k = 0;
  while (i < n && j < n && k < n) {
    uint32_t x = i + k;
    uint32_t y = j + k;
    uint8_t a, b;
    if (x >= n) {
      x -= n;
    }
    if (y >= n) {
      y -= n;
    }
    a = block[x];
    b = block[y];
    if (a == b) {
      k++;
      continue;
    }
    if (a > b) {
      i += k + 1u;
    }
    else {
      j += k + 1u;
    }
    if (i == j) {
      j++;
    }
    k = 0;
  }
  return i < j ? i : j;
}

int bzip2_bwt(const gcomp_allocator_t * alloc, bzip2_bwt_scratch_t * scratch,
    const uint8_t * block, uint32_t n, uint8_t * last, uint32_t * orig_ptr) {
  int32_t total = (int32_t)(n + 1u);
  uint32_t r;
  uint32_t p0;
  int32_t i;
  if (n == 0) {
    return -1;
  }
  r = n > 1 ? least_rotation(block, n) : 0;
  /* The least rotation, one past each byte, then the sentinel. */
  for (i = 0; i < (int32_t)n; i++) {
    uint32_t x = (uint32_t)i + r;
    if (x >= n) {
      x -= n;
    }
    scratch->s[i] = (uint16_t)(block[x] + 1u);
  }
  scratch->s[n] = 0;
  if (sais_u16(alloc, scratch->s, scratch->sa, total, 257) != 0) {
    return -1;
  }
  /* Row i - 1 is the rotation of the least rotation that starts at sa[i]; the
   * first entry is the sentinel's, which is no rotation. Rotation 0 of the
   * block starts at n - r of the least rotation. */
  p0 = r == 0 ? 0 : n - r;
  for (i = 1; i < total; i++) {
    uint32_t p = (uint32_t)scratch->sa[i];
    last[i - 1] = (uint8_t)(scratch->s[p == 0 ? n - 1 : p - 1] - 1u);
    if (p == p0) {
      *orig_ptr = (uint32_t)(i - 1);
    }
  }
  return 0;
}
