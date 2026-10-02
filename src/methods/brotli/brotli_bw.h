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
 * @file brotli_bw.h
 *
 * LSB-first bit writer for RFC 7932, the counterpart to brotli_bits.h.
 *
 * Both halves of the encoder write bits - the stream header and the stored
 * meta-blocks in brotli_encode.c, the compressed meta-block in brotli_lz.c -
 * and each had its own copy of this, which had already drifted: only one of
 * them refused a put wider than 24 bits, and a put of 32 shifts `1u` by the
 * width of its own type. Neither file asked for more than 24, so the drift
 * cost nothing yet; what it cost was the guarantee that a fix to one reaches
 * the other.
 *
 * A put never exceeding 24 bits is what keeps the accumulator honest: at most
 * 7 bits are left after the previous put drains it, so 31 bits is the most it
 * ever holds.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BW_H
#define GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BW_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>

typedef struct brotli_bw_s {
  uint8_t * buf;
  size_t cap;
  size_t len;
  uint64_t acc;
  int nbits;
} brotli_bw_t;

/**
 * @brief Append the low @p n bits of @p bits, least significant first.
 *
 * @return 0, or -1 when @p n is wider than 24 bits or the buffer is full.
 */
static inline int brotli_bw_put(brotli_bw_t * b, uint32_t bits, int n) {
  if (n <= 0) {
    return 0;
  }
  if (n > 24) {
    return -1;
  }
  b->acc |= (uint64_t)(bits & ((1u << n) - 1u)) << b->nbits;
  b->nbits += n;
  while (b->nbits >= 8) {
    if (b->len >= b->cap) {
      return -1;
    }
    b->buf[b->len++] = (uint8_t)b->acc;
    b->acc >>= 8;
    b->nbits -= 8;
  }
  return 0;
}

/**
 * @brief Pad with zero bits to the next byte boundary.
 */
static inline int brotli_bw_align(brotli_bw_t * b) {
  if (b->nbits == 0) {
    return 0;
  }
  return brotli_bw_put(b, 0, 8 - b->nbits);
}

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BW_H */
