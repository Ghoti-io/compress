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
 * @file xz_crc64_pclmul.c
 *
 * CRC-64/XZ by carry-less multiply folding: the same method as
 * `src/core/crc32_pclmul.c`, whose long comment is the explanation (the bit
 * order, the extra power of x that `PCLMULQDQ` brings and how it is folded
 * into the constants, and the final reduction by the scalar CRC). What differs
 * is the width: a constant here is a polynomial of degree below 64, so it
 * fills a whole 64-bit lane instead of the top half of one, and the running
 * CRC is XORed into the first eight bytes rather than four.
 *
 * The constants are computed, not typed in: x^e mod P by repeated doubling,
 * then bit-reversed into the lane order, for e = 128D + 63 (the lane holding
 * the earlier bytes) and 128D - 1 (the later), with D = 4 chunks for the
 * four-register loop and D = 1 for the collapse. The test checks the result
 * against the table CRC on random buffers of every length from 0 to 300 and
 * at every alignment.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/checksum_internal.h"
#include "xz_internal.h"

#ifdef GCOMP_CRC32_PCLMUL

#include <stdatomic.h>
#include <wmmintrin.h>

#define XZ_TARGET_PCLMUL __attribute__((target("pclmul,sse2")))

/* P(x) without its x^64 term, in the natural order (bit j is the coefficient
 * of x^j): the reflection of 0xC96C5795D7870F42, which the table CRC uses. */
#define XZ_CRC64_P 0x42F0E1EBA9EA3693ull

static uint64_t g_k[4]; /* by4 hi, by4 lo, by1 hi, by1 lo */
static atomic_int g_k_state; /* 0 unbuilt, 1 building, 2 ready */

/* x^e mod P, as a coefficient vector. */
static uint64_t x_pow(unsigned e) {
  uint64_t a = 1;
  while (e--) {
    a = (a << 1) ^ ((a >> 63) ? XZ_CRC64_P : 0);
  }
  return a;
}

static uint64_t reverse64(uint64_t v) {
  uint64_t r = 0;
  unsigned i;
  for (i = 0; i < 64; i++) {
    r = (r << 1) | ((v >> i) & 1u);
  }
  return r;
}

static void build_constants(void) {
  g_k[0] = reverse64(x_pow(128u * 4u + 63u));
  g_k[1] = reverse64(x_pow(128u * 4u - 1u));
  g_k[2] = reverse64(x_pow(128u * 1u + 63u));
  g_k[3] = reverse64(x_pow(128u * 1u - 1u));
}

static void constants_ready(void) {
  int expected = 0;
  if (atomic_load_explicit(&g_k_state, memory_order_acquire) == 2) {
    return;
  }
  if (atomic_compare_exchange_strong(&g_k_state, &expected, 1)) {
    build_constants();
    atomic_store_explicit(&g_k_state, 2, memory_order_release);
    return;
  }
  while (atomic_load_explicit(&g_k_state, memory_order_acquire) != 2) {
  }
}

XZ_TARGET_PCLMUL static inline __m128i fold(
    __m128i acc, __m128i k, __m128i next) {
  __m128i hi = _mm_clmulepi64_si128(acc, k, 0x00);
  __m128i lo = _mm_clmulepi64_si128(acc, k, 0x11);
  return _mm_xor_si128(_mm_xor_si128(hi, lo), next);
}

XZ_TARGET_PCLMUL uint64_t xz_crc64_update_pclmul(
    uint64_t crc, const uint8_t * data, size_t len) {
  __m128i k_by4, k_by1, x0, x1, x2, x3;
  uint8_t folded[16];
  size_t off = 64u;
  if (len < XZ_CRC64_PCLMUL_MIN) {
    return xz_crc64_update_scalar(crc, data, len);
  }
  constants_ready();
  k_by4 = _mm_set_epi64x((long long)g_k[1], (long long)g_k[0]);
  k_by1 = _mm_set_epi64x((long long)g_k[3], (long long)g_k[2]);
  x0 = _mm_loadu_si128((const __m128i *)(const void *)(data));
  x1 = _mm_loadu_si128((const __m128i *)(const void *)(data + 16));
  x2 = _mm_loadu_si128((const __m128i *)(const void *)(data + 32));
  x3 = _mm_loadu_si128((const __m128i *)(const void *)(data + 48));
  x0 = _mm_xor_si128(x0, _mm_cvtsi64_si128((long long)crc));
  while (len - off >= 64u) {
    const uint8_t * p = data + off;
    x0 = fold(x0, k_by4, _mm_loadu_si128((const __m128i *)(const void *)(p)));
    x1 = fold(x1, k_by4, _mm_loadu_si128((const __m128i *)(const void *)(p + 16)));
    x2 = fold(x2, k_by4, _mm_loadu_si128((const __m128i *)(const void *)(p + 32)));
    x3 = fold(x3, k_by4, _mm_loadu_si128((const __m128i *)(const void *)(p + 48)));
    off += 64u;
  }
  x0 = fold(x0, k_by1, x1);
  x0 = fold(x0, k_by1, x2);
  x0 = fold(x0, k_by1, x3);
  while (len - off >= 16u) {
    x0 = fold(x0, k_by1,
        _mm_loadu_si128((const __m128i *)(const void *)(data + off)));
    off += 16u;
  }
  _mm_storeu_si128((__m128i *)(void *)folded, x0);
  crc = xz_crc64_update_scalar(0u, folded, sizeof(folded));
  return xz_crc64_update_scalar(crc, data + off, len - off);
}

int xz_crc64_pclmul_available(void) {
  return gcomp_crc32_pclmul_available();
}

#else

int xz_crc64_pclmul_available(void) {
  return 0;
}

uint64_t xz_crc64_update_pclmul(
    uint64_t crc, const uint8_t * data, size_t len) {
  return xz_crc64_update_scalar(crc, data, len);
}

#endif
