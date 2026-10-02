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
 * @file brotli_prefix.c
 *
 * Canonical prefix codes, RFC 7932 sections 3.2 through 3.5.
 *
 * Complex codes store a code length per symbol. Those lengths are themselves
 * prefix-coded, and codes 16 and 17 repeat the previous length or zero. A
 * repeat that follows the same repeat code extends the previous count instead
 * of starting a new run: the RFC's example (7, then 16 with bits 11, then 16
 * with bits 10) is twenty-two lengths of 7, which is one length, then six,
 * then fifteen more. Only the newly added lengths are written.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "brotli_internal.h"

#include <stdio.h>
#include <string.h>

static const int k_cl_order[18] = {
    1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static void tree_error(brotli_tree_t * t, gcomp_status_t status, const char * msg) {
  t->err_status = status;
  snprintf(t->err, sizeof(t->err), "%s", msg);
}

void brotli_huff_free(const gcomp_allocator_t * alloc, brotli_huff_t * h) {
  if (!h) {
    return;
  }
  if (h->syms) {
    gcomp_free(alloc, h->syms);
  }
  memset(h, 0, sizeof(*h));
  h->simple = -1;
}

void brotli_tree_begin(brotli_tree_t * t, brotli_huff_t * dest, int alphabet,
    const gcomp_allocator_t * alloc) {
  brotli_huff_free(alloc, &t->cl_huff);
  memset(t, 0, sizeof(*t));
  t->dest = dest;
  t->alphabet = alphabet;
  t->cl_huff.simple = -1;
  t->err_status = GCOMP_OK;
}

static int alphabet_bits(int n) {
  int bits = 0;
  int v = n - 1;
  while (v > 0) {
    v >>= 1;
    bits++;
  }
  return bits;
}

static int huff_build(const gcomp_allocator_t * alloc, brotli_huff_t * h,
    const uint8_t * lens, int alphabet, int allow_one, brotli_tree_t * t) {
  int count[16];
  int nnz = 0;
  int maxb = 0;
  int only = -1;
  int i;
  uint32_t code;
  int filled[16];
  int cursor;

  brotli_huff_free(alloc, h);
  memset(count, 0, sizeof(count));
  for (i = 0; i < alphabet; i++) {
    int len = lens[i];
    if (len > 15) {
      tree_error(t, GCOMP_ERR_CORRUPT, "brotli: code length out of range");
      return BR_FAIL;
    }
    if (len != 0) {
      count[len]++;
      nnz++;
      only = i;
      if (len > maxb) {
        maxb = len;
      }
    }
  }
  if (nnz == 0) {
    tree_error(t, GCOMP_ERR_CORRUPT, "brotli: empty prefix code");
    return BR_FAIL;
  }
  if (nnz == 1) {
    if (!allow_one) {
      tree_error(t, GCOMP_ERR_CORRUPT, "brotli: prefix code has one symbol");
      return BR_FAIL;
    }
    h->simple = only;
    return BR_OK;
  }

  code = 0;
  for (i = 1; i <= maxb; i++) {
    code = (code + (uint32_t)count[i - 1]) << 1;
    if (count[i] != 0 && code + (uint32_t)count[i] > (1u << i)) {
      tree_error(t, GCOMP_ERR_CORRUPT, "brotli: over-subscribed prefix code");
      return BR_FAIL;
    }
    h->first[i] = (uint16_t)code;
    h->count[i] = (uint16_t)count[i];
  }

  h->syms = gcomp_calloc(alloc, (size_t)nnz, sizeof(uint16_t));
  if (!h->syms) {
    tree_error(t, GCOMP_ERR_MEMORY, "brotli: out of memory");
    return BR_FAIL;
  }
  cursor = 0;
  for (i = 1; i <= maxb; i++) {
    h->offset[i] = (uint16_t)cursor;
    cursor += count[i];
  }
  memset(filled, 0, sizeof(filled));
  for (i = 0; i < alphabet; i++) {
    int len = lens[i];
    if (len != 0) {
      h->syms[h->offset[len] + filled[len]] = (uint16_t)i;
      filled[len]++;
    }
  }
  h->max_bits = maxb;
  h->simple = -1;
  return BR_OK;
}

int brotli_read_sym(brotli_bits_t * bits, const brotli_huff_t * h,
    brotli_sym_t * sym, int * out) {
  if (h->simple >= 0) {
    *out = h->simple;
    sym->n = 0;
    sym->code = 0;
    return BR_OK;
  }
  while (sym->n < h->max_bits) {
    uint32_t bit = 0;
    int r = brotli_bits_take(bits, 1, &bit);
    int n;
    if (r != BR_OK) {
      return r;
    }
    sym->code = (sym->code << 1) | bit;
    sym->n++;
    n = sym->n;
    if (h->count[n] != 0 && sym->code >= h->first[n] &&
        sym->code < (uint32_t)h->first[n] + h->count[n]) {
      *out = h->syms[h->offset[n] + (sym->code - h->first[n])];
      sym->n = 0;
      sym->code = 0;
      return BR_OK;
    }
  }
  return BR_FAIL;
}

/* Static code over the code-length alphabet. First bit read is bit 0. */
static int read_cl_symbol(brotli_tree_t * t, brotli_bits_t * bits, int * out) {
  for (;;) {
    uint32_t bit = 0;
    int r;
    switch (t->cl_step) {
    case 0:
      r = brotli_bits_take(bits, 1, &bit);
      if (r != BR_OK) {
        return r;
      }
      t->cl_b0 = (int)bit;
      t->cl_step = 1;
      break;
    case 1:
      r = brotli_bits_take(bits, 1, &bit);
      if (r != BR_OK) {
        return r;
      }
      if (t->cl_b0 == 0) {
        *out = bit ? 3 : 0;
        t->cl_step = 0;
        return BR_OK;
      }
      if (bit == 0) {
        *out = 4;
        t->cl_step = 0;
        return BR_OK;
      }
      t->cl_step = 2;
      break;
    case 2:
      r = brotli_bits_take(bits, 1, &bit);
      if (r != BR_OK) {
        return r;
      }
      if (bit == 0) {
        *out = 2;
        t->cl_step = 0;
        return BR_OK;
      }
      t->cl_step = 3;
      break;
    case 3:
      r = brotli_bits_take(bits, 1, &bit);
      if (r != BR_OK) {
        return r;
      }
      *out = bit ? 5 : 1;
      t->cl_step = 0;
      return BR_OK;
    default:
      tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad code-length state");
      return BR_FAIL;
    }
  }
}

static int finish_complex(brotli_tree_t * t, const gcomp_allocator_t * alloc) {
  int nnz = 0;
  int i;
  if (t->space != 0) {
    tree_error(t, GCOMP_ERR_CORRUPT, "brotli: prefix code is incomplete");
    return BR_FAIL;
  }
  for (i = 0; i < t->alphabet; i++) {
    if (t->lens[i] != 0) {
      nnz++;
    }
  }
  if (nnz < 2) {
    tree_error(t, GCOMP_ERR_CORRUPT, "brotli: prefix code has one symbol");
    return BR_FAIL;
  }
  brotli_huff_free(alloc, &t->cl_huff);
  return huff_build(alloc, t->dest, t->lens, t->alphabet, 0, t);
}

int brotli_tree_read(brotli_tree_t * t, brotli_bits_t * bits, brotli_sym_t * sym,
    const gcomp_allocator_t * alloc) {
  if (t->alphabet < 1 || t->alphabet > BROTLI_MAX_ALPHABET) {
    tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad alphabet size");
    return BR_FAIL;
  }
  for (;;) {
    uint32_t val = 0;
    int r;
    int sym_v = 0;
    switch (t->step) {
    case 0:
      r = brotli_bits_take(bits, 2, &val);
      if (r != BR_OK) {
        return r;
      }
      if (val == 1) {
        t->step = 10;
        break;
      }
      t->idx = (int)val;
      t->space = 32;
      t->nnz = 0;
      t->step = 1;
      break;

    case 1:
      if (t->idx >= 18 || t->space == 0) {
        if (t->nnz == 1 && t->idx == 18) {
          t->cl_huff.simple = t->only;
          t->step = 2;
          break;
        }
        if (t->space != 0 || t->nnz < 1) {
          tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad code-length code");
          return BR_FAIL;
        }
        r = huff_build(alloc, &t->cl_huff, t->clen, 18, 0, t);
        if (r != BR_OK) {
          return r;
        }
        t->step = 2;
        break;
      }
      r = read_cl_symbol(t, bits, &sym_v);
      if (r != BR_OK) {
        return r;
      }
      t->clen[k_cl_order[t->idx]] = (uint8_t)sym_v;
      if (sym_v != 0) {
        t->space -= 32 >> sym_v;
        t->nnz++;
        t->only = k_cl_order[t->idx];
        if (t->space < 0) {
          tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad code-length code");
          return BR_FAIL;
        }
      }
      t->idx++;
      break;

    case 2:
      memset(t->lens, 0, (size_t)t->alphabet);
      t->idx = 0;
      t->space = 32768;
      t->prev_len = 8;
      t->repeat = 0;
      t->repeat_len = 0;
      sym->n = 0;
      sym->code = 0;
      t->step = 3;
      break;

    case 3:
      if (t->idx >= t->alphabet || t->space <= 0) {
        return finish_complex(t, alloc);
      }
      r = brotli_read_sym(bits, &t->cl_huff, sym, &sym_v);
      if (r == BR_FAIL) {
        tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad code-length symbol");
        return BR_FAIL;
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym_v < 16) {
        t->repeat = 0;
        t->lens[t->idx++] = (uint8_t)sym_v;
        if (sym_v != 0) {
          t->space -= 32768 >> sym_v;
          t->prev_len = sym_v;
          if (t->space < 0) {
            tree_error(t, GCOMP_ERR_CORRUPT, "brotli: prefix code over-full");
            return BR_FAIL;
          }
        }
        break;
      }
      if (sym_v > 17) {
        tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad code-length symbol");
        return BR_FAIL;
      }
      t->rep_sym = sym_v;
      t->step = 4;
      break;

    case 4: {
      int extra = t->rep_sym == 16 ? 2 : 3;
      int new_len = t->rep_sym == 16 ? t->prev_len : 0;
      uint32_t old;
      uint32_t rep;
      uint32_t emit;
      uint32_t i;
      r = brotli_bits_take(bits, extra, &val);
      if (r != BR_OK) {
        return r;
      }
      if (t->repeat_len != new_len) {
        t->repeat = 0;
        t->repeat_len = new_len;
      }
      old = (uint32_t)t->repeat;
      rep = old;
      if (rep > 0) {
        if (rep < 2 || (rep - 2) > (UINT32_MAX >> extra)) {
          tree_error(t, GCOMP_ERR_CORRUPT, "brotli: repeat count overflow");
          return BR_FAIL;
        }
        rep = (rep - 2) << extra;
      }
      if (rep > UINT32_MAX - (val + 3)) {
        tree_error(t, GCOMP_ERR_CORRUPT, "brotli: repeat count overflow");
        return BR_FAIL;
      }
      rep += val + 3;
      emit = rep - old;
      if ((uint32_t)t->idx + emit > (uint32_t)t->alphabet) {
        tree_error(t, GCOMP_ERR_CORRUPT, "brotli: repeat runs past the alphabet");
        return BR_FAIL;
      }
      for (i = 0; i < emit; i++) {
        t->lens[t->idx++] = (uint8_t)new_len;
      }
      if (new_len != 0) {
        t->space -= (int)emit * (32768 >> new_len);
        if (t->space < 0) {
          tree_error(t, GCOMP_ERR_CORRUPT, "brotli: prefix code over-full");
          return BR_FAIL;
        }
      }
      t->repeat = (int)rep;
      t->step = 3;
      break;
    }

    case 10:
      r = brotli_bits_take(bits, 2, &val);
      if (r != BR_OK) {
        return r;
      }
      t->nsym = (int)val + 1;
      t->idx = 0;
      t->abits = alphabet_bits(t->alphabet);
      t->step = 11;
      break;

    case 11:
      if (t->idx >= t->nsym) {
        int a;
        int b;
        for (a = 0; a < t->nsym; a++) {
          for (b = a + 1; b < t->nsym; b++) {
            if (t->got[a] == t->got[b]) {
              tree_error(t, GCOMP_ERR_CORRUPT, "brotli: duplicate simple symbol");
              return BR_FAIL;
            }
          }
        }
        t->step = t->nsym == 4 ? 12 : 13;
        break;
      }
      r = brotli_bits_take(bits, t->abits, &val);
      if (r != BR_OK) {
        return r;
      }
      if (val >= (uint32_t)t->alphabet) {
        tree_error(t, GCOMP_ERR_CORRUPT, "brotli: simple symbol out of range");
        return BR_FAIL;
      }
      t->got[t->idx++] = (uint16_t)val;
      break;

    case 12:
      r = brotli_bits_take(bits, 1, &val);
      if (r != BR_OK) {
        return r;
      }
      t->tree_select = (int)val;
      t->step = 13;
      break;

    case 13: {
      uint8_t lens_of[4];
      int i;
      memset(t->lens, 0, (size_t)t->alphabet);
      if (t->nsym == 1) {
        t->dest->simple = t->got[0];
        brotli_huff_free(alloc, &t->cl_huff);
        return BR_OK;
      }
      if (t->nsym == 2) {
        lens_of[0] = 1;
        lens_of[1] = 1;
      }
      else if (t->nsym == 3) {
        lens_of[0] = 1;
        lens_of[1] = 2;
        lens_of[2] = 2;
      }
      else if (t->tree_select == 0) {
        lens_of[0] = lens_of[1] = lens_of[2] = lens_of[3] = 2;
      }
      else {
        lens_of[0] = 1;
        lens_of[1] = 2;
        lens_of[2] = 3;
        lens_of[3] = 3;
      }
      for (i = 0; i < t->nsym; i++) {
        t->lens[t->got[i]] = lens_of[i];
      }
      brotli_huff_free(alloc, &t->cl_huff);
      return huff_build(alloc, t->dest, t->lens, t->alphabet, 0, t);
    }

    default:
      tree_error(t, GCOMP_ERR_CORRUPT, "brotli: bad prefix-code state");
      return BR_FAIL;
    }
  }
}
