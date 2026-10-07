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
 * @file bzip2_sais_body.h
 *
 * The body of the SA-IS suffix sort in bzip2_bwt.c, written once and included
 * for each symbol width. It is not a header in the usual sense: it defines
 * static functions named by SAIS_NAME and takes SAIS_SYM as the symbol type,
 * and it has no include guard because it is meant to be included twice.
 */

#include <ghoti.io/compress/macros.h>

#define SAIS_CAT2(a, b) a##b
#define SAIS_CAT(a, b) SAIS_CAT2(a, b)
#define SAIS_F(x) SAIS_CAT(SAIS_NAME, x)

static void SAIS_F(_induce_l)(const uint8_t * t, int32_t * sa,
    const SAIS_SYM * s, int32_t * bkt, int32_t n) {
  int32_t i, j;
  for (i = 0; i < n; i++) {
    if (i + SAIS_LOOKAHEAD < n) {
      int32_t q = sa[i + SAIS_LOOKAHEAD] - 1;
      if (q >= 0) {
        __builtin_prefetch(&s[q]);
        __builtin_prefetch(&t[q]);
      }
    }
    j = sa[i] - 1;
    if (j >= 0 && !t[j]) {
      sa[bkt[s[j]]++] = j;
    }
  }
}

static void SAIS_F(_induce_s)(const uint8_t * t, int32_t * sa,
    const SAIS_SYM * s, int32_t * bkt, int32_t n) {
  int32_t i, j;
  for (i = n - 1; i >= 0; i--) {
    if (i >= SAIS_LOOKAHEAD) {
      int32_t q = sa[i - SAIS_LOOKAHEAD] - 1;
      if (q >= 0) {
        __builtin_prefetch(&s[q]);
        __builtin_prefetch(&t[q]);
      }
    }
    j = sa[i] - 1;
    if (j >= 0 && t[j]) {
      sa[--bkt[s[j]]] = j;
    }
  }
}

/* The suffix array of s[0..n), whose last symbol is a 0 that occurs nowhere
 * else and whose other symbols are in 1..k-1. Returns 0, or -1 if memory ran
 * out. */
static int SAIS_NAME(const gcomp_allocator_t * alloc, const SAIS_SYM * s,
    int32_t * sa, int32_t n, int32_t k) {
  uint8_t * t;
  int32_t * cnt;
  int32_t * bkt;
  int32_t i, j, n1, name, prev;
  int32_t * s1;
  int32_t * sa1;
  int rc = 0;

#define IS_LMS(x) ((x) > 0 && t[x] && !t[(x)-1])

  t = gcomp_malloc(alloc, (size_t)n);
  cnt = gcomp_malloc(alloc, (size_t)k * 2u * sizeof(int32_t));
  if (!t || !cnt) {
    rc = -1;
    goto out;
  }
  bkt = cnt + k;
  memset(cnt, 0, (size_t)k * sizeof(int32_t));
  for (i = 0; i < n; i++) {
    cnt[s[i]]++;
  }
  /* The last symbol is the sentinel, S-type; the one before it is L-type. */
  t[n - 1] = 1;
  t[n - 2] = 0;
  for (i = n - 3; i >= 0; i--) {
    t[i] = (uint8_t)(s[i] < s[i + 1] || (s[i] == s[i + 1] && t[i + 1] == 1));
  }

  /* Stage 1: sort the LMS substrings. */
  bucket_bounds(cnt, bkt, k, 1);
  for (i = 0; i < n; i++) {
    sa[i] = -1;
  }
  for (i = 1; i < n; i++) {
    if (IS_LMS(i)) {
      sa[--bkt[s[i]]] = i;
    }
  }
  bucket_bounds(cnt, bkt, k, 0);
  SAIS_F(_induce_l)(t, sa, s, bkt, n);
  bucket_bounds(cnt, bkt, k, 1);
  SAIS_F(_induce_s)(t, sa, s, bkt, n);

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
    if (SAIS_RECURSE(alloc, s1, sa1, n1, name) != 0) {
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
  bucket_bounds(cnt, bkt, k, 1);
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
  bucket_bounds(cnt, bkt, k, 0);
  SAIS_F(_induce_l)(t, sa, s, bkt, n);
  bucket_bounds(cnt, bkt, k, 1);
  SAIS_F(_induce_s)(t, sa, s, bkt, n);

out:
#undef IS_LMS
  gcomp_free(alloc, t);
  gcomp_free(alloc, cnt);
  return rc;
}

#undef SAIS_F
#undef SAIS_CAT
#undef SAIS_CAT2
