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
 * @file xz_common.c
 *
 * The parts of the xz container that the encoder and the decoder both use:
 * the checks, the variable-length integers, the dictionary-size property and
 * the filter list in an option string.
 */

#include <ghoti.io/compress/macros.h>

#include "../../filters/filter_internal.h"
#include "xz_internal.h"

#include <ghoti.io/compress/crc32.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- checks ------------------------------------------------------------- */

unsigned xz_check_size(unsigned type) {
  /* The format fixes the size of all sixteen ids, in fours: 0 has none,
   * 1 to 3 have four bytes, 4 to 6 eight, 7 to 9 sixteen, 10 to 12 thirty-two,
   * 13 to 15 sixty-four. */
  if (type == 0) {
    return 0;
  }
  if (type > 15) {
    return 0;
  }
  return 4u << ((type - 1u) / 3u);
}

int xz_check_known(unsigned type) {
  return type == XZ_CHECK_NONE || type == XZ_CHECK_CRC32 ||
      type == XZ_CHECK_CRC64 || type == XZ_CHECK_SHA256;
}

/* CRC-64/XZ: the ECMA-182 polynomial, reflected. Eight tables of 256 let a
 * word be folded in at a time; they are built on first use. Two threads may
 * arrive together, so the build is claimed with a compare-exchange and the
 * loser waits for the winner to say it is done. */
#define CRC64_POLY 0xC96C5795D7870F42ull

static uint64_t g_crc64_table[8][256];
static atomic_int g_crc64_state; /* 0 unbuilt, 1 building, 2 ready */

static void crc64_build(void) {
  unsigned i, k, j;
  for (i = 0; i < 256; i++) {
    uint64_t crc = i;
    for (j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ ((crc & 1) ? CRC64_POLY : 0);
    }
    g_crc64_table[0][i] = crc;
  }
  for (k = 1; k < 8; k++) {
    for (i = 0; i < 256; i++) {
      const uint64_t prev = g_crc64_table[k - 1][i];
      g_crc64_table[k][i] = (prev >> 8) ^ g_crc64_table[0][prev & 0xFF];
    }
  }
}

static void crc64_ready(void) {
  int expected = 0;
  if (atomic_load_explicit(&g_crc64_state, memory_order_acquire) == 2) {
    return;
  }
  if (atomic_compare_exchange_strong(&g_crc64_state, &expected, 1)) {
    crc64_build();
    atomic_store_explicit(&g_crc64_state, 2, memory_order_release);
    return;
  }
  while (atomic_load_explicit(&g_crc64_state, memory_order_acquire) != 2) {
  }
}

uint64_t xz_crc64_update(uint64_t crc, const uint8_t * data, size_t n) {
  crc64_ready();
  while (n >= 8) {
    uint64_t w = (uint64_t)data[0] | (uint64_t)data[1] << 8 |
        (uint64_t)data[2] << 16 | (uint64_t)data[3] << 24 |
        (uint64_t)data[4] << 32 | (uint64_t)data[5] << 40 |
        (uint64_t)data[6] << 48 | (uint64_t)data[7] << 56;
    w ^= crc;
    crc = g_crc64_table[7][w & 0xFF] ^ g_crc64_table[6][(w >> 8) & 0xFF] ^
        g_crc64_table[5][(w >> 16) & 0xFF] ^ g_crc64_table[4][(w >> 24) & 0xFF] ^
        g_crc64_table[3][(w >> 32) & 0xFF] ^ g_crc64_table[2][(w >> 40) & 0xFF] ^
        g_crc64_table[1][(w >> 48) & 0xFF] ^ g_crc64_table[0][w >> 56];
    data += 8;
    n -= 8;
  }
  while (n--) {
    crc = g_crc64_table[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

uint32_t xz_crc32(const uint8_t * data, size_t n) {
  return gcomp_crc32_finalize(gcomp_crc32_update(GCOMP_CRC32_INIT, data, n));
}

void xz_check_init(xz_check_t * c, unsigned type) {
  c->type = type;
  c->crc32 = GCOMP_CRC32_INIT;
  c->crc64 = ~(uint64_t)0;
  if (type == XZ_CHECK_SHA256) {
    (void)gsec_sha256_init(&c->sha);
  }
}

void xz_check_update(xz_check_t * c, const void * data, size_t n) {
  if (n == 0) {
    return;
  }
  switch (c->type) {
  case XZ_CHECK_CRC32:
    c->crc32 = gcomp_crc32_update(c->crc32, (const uint8_t *)data, n);
    break;
  case XZ_CHECK_CRC64:
    c->crc64 = xz_crc64_update(c->crc64, (const uint8_t *)data, n);
    break;
  case XZ_CHECK_SHA256:
    (void)gsec_sha256_update(&c->sha, data, n);
    break;
  default:
    break;
  }
}

void xz_check_final(xz_check_t * c, uint8_t * out) {
  unsigned i;
  switch (c->type) {
  case XZ_CHECK_CRC32:
    xz_put_le32(out, gcomp_crc32_finalize(c->crc32));
    break;
  case XZ_CHECK_CRC64: {
    const uint64_t v = ~c->crc64;
    for (i = 0; i < 8; i++) {
      out[i] = (uint8_t)(v >> (8 * i));
    }
    break;
  }
  case XZ_CHECK_SHA256:
    (void)gsec_sha256_final(&c->sha, out);
    break;
  default:
    break;
  }
}

/* ---- variable-length integers ------------------------------------------- */

size_t xz_vli_size(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

size_t xz_vli_put(uint8_t * out, uint64_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    out[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  out[n++] = (uint8_t)v;
  return n;
}

int xz_vli_feed(xz_vli_t * v, uint8_t byte) {
  if (v->count == 0) {
    v->value = 0;
  }
  if (v->count >= XZ_VLI_MAX_BYTES) {
    return -1;
  }
  v->value |= (uint64_t)(byte & 0x7F) << (7u * v->count);
  v->count++;
  if (byte & 0x80) {
    return 0;
  }
  /* A final zero byte after other bytes says nothing a shorter spelling
   * would not. */
  if (byte == 0 && v->count > 1) {
    return -1;
  }
  if (v->value > XZ_VLI_MAX) {
    return -1;
  }
  v->count = 0;
  return 1;
}

/* ---- the dictionary property -------------------------------------------- */

uint64_t xz_dict_size(uint8_t prop) {
  if (prop > XZ_DICT_PROP_MAX) {
    return 0;
  }
  if (prop == XZ_DICT_PROP_MAX) {
    return 0xFFFFFFFFull;
  }
  return (uint64_t)(2u | (prop & 1u)) << (prop / 2u + 11u);
}

uint8_t xz_dict_prop(uint64_t dict) {
  unsigned p;
  for (p = 0; p < XZ_DICT_PROP_MAX; p++) {
    if (xz_dict_size((uint8_t)p) >= dict) {
      return (uint8_t)p;
    }
  }
  return XZ_DICT_PROP_MAX;
}

/* ---- the filter list ---------------------------------------------------- */

static int filters_fail(char * err, size_t cap, const char * msg) {
  if (err && cap) {
    snprintf(err, cap, "%s", msg);
  }
  return 0;
}

int xz_filters_parse(const char * text, xz_filter_t * out, unsigned * count,
    char * err, size_t err_cap) {
  unsigned n = 0;
  const char * p = text;
  *count = 0;
  if (!text || !*text) {
    return 1;
  }
  for (;;) {
    char name[16];
    size_t len = 0;
    unsigned id = 0;
    uint64_t arg = 0;
    int has_arg = 0;
    while (*p && *p != ',' && *p != ':') {
      if (len + 1 >= sizeof(name)) {
        return filters_fail(err, err_cap, "a filter name is too long");
      }
      name[len++] = *p++;
    }
    name[len] = '\0';
    if (*p == ':') {
      char * end = NULL;
      p++;
      if (*p < '0' || *p > '9') {
        return filters_fail(err, err_cap, "a filter's argument is not a number");
      }
      arg = strtoull(p, &end, 10);
      if (end == p || arg > 0xFFFFFFFFull) {
        return filters_fail(err, err_cap, "a filter's argument is too large");
      }
      p = end;
      has_arg = 1;
    }
    if (strcmp(name, "delta") == 0) {
      id = XZ_FILTER_DELTA;
      if (!has_arg) {
        arg = 1;
      }
      if (arg < 1 || arg > 256) {
        return filters_fail(err, err_cap, "delta's distance is 1 to 256");
      }
    }
    else if (!filter_arch_id(name, &id)) {
      return filters_fail(err, err_cap, "an unknown filter name");
    }
    if (n >= XZ_FILTERS_MAX - 1u) {
      return filters_fail(
          err, err_cap, "at most three filters may come before LZMA2");
    }
    out[n].id = id;
    out[n].arg = (uint32_t)arg;
    n++;
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p == '\0') {
      break;
    }
    return filters_fail(err, err_cap, "unexpected text after a filter");
  }
  *count = n;
  return 1;
}
