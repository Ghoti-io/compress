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
 * @file brotli_bits.h
 *
 * LSB-first bit reader for RFC 7932. A whole byte is pulled only when the
 * accumulator holds fewer bits than the take asks for. After a take that
 * succeeds, fewer than 8 bits remain. A take that suspends for more input
 * may leave 8 or more, and those bits stay: the next take continues.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BITS_H
#define GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BITS_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>

enum {
  BR_OK = 0,
  BR_NEED = 1,
  BR_FULL = 2,
  BR_LIMIT = 3,
  BR_FAIL = 4
};

typedef struct brotli_bits_s {
  const uint8_t * data;
  size_t size;
  size_t pos;
  uint64_t acc;
  int nbits;
} brotli_bits_t;

static inline int brotli_bits_take(
    brotli_bits_t * b, int n, uint32_t * out) {
  if (n == 0) {
    *out = 0;
    return BR_OK;
  }
  while (b->nbits < n) {
    if (b->pos >= b->size) {
      return BR_NEED;
    }
    b->acc |= (uint64_t)b->data[b->pos++] << b->nbits;
    b->nbits += 8;
  }
  *out = (uint32_t)(b->acc & ((1u << n) - 1u));
  b->acc >>= n;
  b->nbits -= n;
  return BR_OK;
}

/**
 * @brief Drop the bits that fill out the current byte. They must be zero.
 *
 * Called when the format is at a byte boundary. The leftover is already in
 * the accumulator: a successful take never leaves 8 or more bits.
 */
static inline int brotli_bits_align(brotli_bits_t * b) {
  uint32_t pad;
  if (b->nbits <= 0) {
    return BR_OK;
  }
  if (b->nbits >= 8) {
    return BR_FAIL;
  }
  pad = (uint32_t)(b->acc & ((1u << b->nbits) - 1u));
  if (pad != 0) {
    return BR_FAIL;
  }
  b->acc = 0;
  b->nbits = 0;
  return BR_OK;
}

static inline int brotli_bits_byte(brotli_bits_t * b, uint8_t * out) {
  if (b->nbits != 0) {
    return BR_FAIL;
  }
  if (b->pos >= b->size) {
    return BR_NEED;
  }
  *out = b->data[b->pos++];
  return BR_OK;
}

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_BITS_H */
