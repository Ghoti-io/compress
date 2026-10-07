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
 * @file lzma_common.c
 *
 * What the LZMA encoder and decoder both need: the initial probabilities and
 * the properties byte.
 */

#include "lzma_internal.h"

void lzma_probs_init(lzma_probs_t * probs, uint16_t * lit, size_t lit_count) {
  /* lzma_probs_t is nothing but uint16_t arrays, so it is filled as one. */
  uint16_t * p = (uint16_t *)probs;
  size_t n = sizeof(*probs) / sizeof(uint16_t);
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint16_t)LZMA_PROB_INIT;
  }
  for (i = 0; i < lit_count; i++) {
    lit[i] = (uint16_t)LZMA_PROB_INIT;
  }
}

int lzma_props_decode(uint8_t byte, unsigned * lc, unsigned * lp, unsigned * pb) {
  unsigned d = byte;
  if (d >= 9u * 5u * 5u) {
    return 0;
  }
  *lc = d % 9u;
  d /= 9u;
  *lp = d % 5u;
  *pb = d / 5u;
  return 1;
}

uint8_t lzma_props_encode(unsigned lc, unsigned lp, unsigned pb) {
  return (uint8_t)((pb * 5u + lp) * 9u + lc);
}
