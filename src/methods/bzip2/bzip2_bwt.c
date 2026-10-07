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
 * test_bzip2_bwt.cpp checks it against sorting the rotations directly.
 */

#include "../../core/alloc_internal.h"
#include "bzip2_internal.h"

#include <string.h>

static void get_buckets(
    const int32_t * s, int32_t * bkt, int32_t n, int32_t k, int end) {
  int32_t i, sum = 0;
  memset(bkt, 0, (size_t)k * sizeof(int32_t));
  for (i = 0; i < n; i++) {
    bkt[s[i]]++;
  }
  for (i = 0; i < k; i++) {
    sum += bkt[i];
    bkt[i] = end ? sum : sum - bkt[i];
  }
}

static void induce_l(const uint8_t * t, int32_t * sa, const int32_t * s,
    int32_t * bkt, int32_t n, int32_t k) {
  int32_t i, j;
  get_buckets(s, bkt, n, k, 0);
  for (i = 0; i < n; i++) {
    j = sa[i] - 1;
    if (j >= 0 && !t[j]) {
      sa[bkt[s[j]]++] = j;
    }
  }
}

static void induce_s(const uint8_t * t, int32_t * sa, const int32_t * s,
    int32_t * bkt, int32_t n, int32_t k) {
  int32_t i, j;
  get_buckets(s, bkt, n, k, 1);
  for (i = n - 1; i >= 0; i--) {
    j = sa[i] - 1;
    if (j >= 0 && t[j]) {
      sa[--bkt[s[j]]] = j;
    }
  }
}

/* The suffix array of s[0..n), whose last symbol is a 0 that occurs nowhere
 * else and whose other symbols are in 1..k-1. Returns 0, or -1 if memory ran
 * out. */
static int sais(const gcomp_allocator_t * alloc, const int32_t * s, int32_t * sa,
    int32_t n, int32_t k) {
  uint8_t * t;
  int32_t * bkt;
  int32_t i, j, n1, name, prev;
  int32_t * s1;
  int32_t * sa1;
  int rc = 0;

#define IS_LMS(x) ((x) > 0 && t[x] && !t[(x)-1])

  t = gcomp_malloc(alloc, (size_t)n);
  bkt = gcomp_malloc(alloc, (size_t)k * sizeof(int32_t));
  if (!t || !bkt) {
    rc = -1;
    goto out;
  }
  /* The last symbol is the sentinel, S-type; the one before it is L-type. */
  t[n - 1] = 1;
  t[n - 2] = 0;
  for (i = n - 3; i >= 0; i--) {
    t[i] = (uint8_t)(s[i] < s[i + 1] || (s[i] == s[i + 1] && t[i + 1] == 1));
  }

  /* Stage 1: sort the LMS substrings. */
  get_buckets(s, bkt, n, k, 1);
  for (i = 0; i < n; i++) {
    sa[i] = -1;
  }
  for (i = 1; i < n; i++) {
    if (IS_LMS(i)) {
      sa[--bkt[s[i]]] = i;
    }
  }
  induce_l(t, sa, s, bkt, n, k);
  induce_s(t, sa, s, bkt, n, k);

  n1 = 0;
  for (i = 0; i < n; i++) {
    if (IS_LMS(sa[i])) {
      sa[n1++] = sa[i];
    }
  }
  for (i = n1; i < n; i++) {
    sa[i] = -1;
  }
  name = 0;
  prev = -1;
  for (i = 0; i < n1; i++) {
    int32_t pos = sa[i];
    int diff = 0;
    int32_t d;
    for (d = 0; d < n; d++) {
      if (prev == -1 || s[pos + d] != s[prev + d] || t[pos + d] != t[prev + d]) {
        diff = 1;
        break;
      }
      if (d > 0 && (IS_LMS(pos + d) || IS_LMS(prev + d))) {
        break;
      }
    }
    if (diff) {
      name++;
      prev = pos;
    }
    sa[n1 + pos / 2] = name - 1;
  }
  for (i = n - 1, j = n - 1; i >= n1; i--) {
    if (sa[i] >= 0) {
      sa[j--] = sa[i];
    }
  }

  /* Stage 2: the reduced problem, solved here or by recursion. */
  s1 = sa + n - n1;
  sa1 = sa;
  if (name < n1) {
    if (sais(alloc, s1, sa1, n1, name) != 0) {
      rc = -1;
      goto out;
    }
  }
  else {
    for (i = 0; i < n1; i++) {
      sa1[s1[i]] = i;
    }
  }

  /* Stage 3: induce the suffix array from the sorted LMS suffixes. */
  get_buckets(s, bkt, n, k, 1);
  for (i = 1, j = 0; i < n; i++) {
    if (IS_LMS(i)) {
      s1[j++] = i;
    }
  }
  for (i = 0; i < n1; i++) {
    sa1[i] = s1[sa1[i]];
  }
  for (i = n1; i < n; i++) {
    sa[i] = -1;
  }
  for (i = n1 - 1; i >= 0; i--) {
    j = sa[i];
    sa[i] = -1;
    sa[--bkt[s[j]]] = j;
  }
  induce_l(t, sa, s, bkt, n, k);
  induce_s(t, sa, s, bkt, n, k);

out:
#undef IS_LMS
  gcomp_free(alloc, t);
  gcomp_free(alloc, bkt);
  return rc;
}

int bzip2_bwt(const gcomp_allocator_t * alloc, bzip2_bwt_scratch_t * scratch,
    const uint8_t * block, uint32_t n, uint8_t * last, uint32_t * orig_ptr) {
  int32_t total = (int32_t)(2u * n + 1u);
  int32_t i, row;
  if (n == 0) {
    return -1;
  }
  for (i = 0; i < (int32_t)n; i++) {
    scratch->s[i] = (int32_t)block[i] + 1;
    scratch->s[(int32_t)n + i] = (int32_t)block[i] + 1;
  }
  scratch->s[total - 1] = 0;
  if (sais(alloc, scratch->s, scratch->sa, total, 257) != 0) {
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
