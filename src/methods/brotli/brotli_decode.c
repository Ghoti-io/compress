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
 * @file brotli_decode.c
 *
 * Streaming decoder for RFC 7932.
 *
 * Every step that reads bits can suspend. The step number stays where it
 * was, and the bit accumulator keeps whatever it already pulled. A full
 * output buffer suspends the same way: the byte just read is held until
 * emit() can write it. GCOMP_ERR_LIMIT from update() is a limits option.
 * A full output buffer is GCOMP_OK, with output->used saying how far it got.
 * finish() uses GCOMP_ERR_LIMIT when it still has bytes to write, and
 * GCOMP_ERR_CORRUPT when the input ended inside a meta-block.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "brotli_internal.h"

#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/limits.h>

#include <string.h>

enum {
  PH_WINDOW = 0,
  PH_META,
  PH_UNCOMP,
  PH_SKIP,
  PH_COMMAND,
  PH_DONE
};

enum { CAT_L = 0, CAT_I = 1, CAT_D = 2 };

static const uint8_t k_ins_extra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,
    4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint32_t k_ins_base[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26,
    34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t k_copy_extra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3,
    3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
static const uint32_t k_copy_base[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18,
    22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t k_blk_extra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};
static const uint32_t k_blk_base[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81,
    97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433,
    16625};

typedef struct brotli_dec_s {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  brotli_bits_t br;
  uint64_t input_total;
  uint64_t produced;
  uint64_t max_out;
  uint64_t max_mem;
  uint64_t max_window;
  uint64_t max_ratio;
  int failed;
  gcomp_status_t err;

  int phase;
  int step;
  int wb_step;
  int wbits;
  uint8_t * ring;
  size_t window;
  size_t ring_pos;
  uint8_t p1;
  uint8_t p2;
  uint32_t dist_rb[4];

  int is_last;
  int is_meta;
  int is_uncomp;
  uint32_t mremain;
  int nibble_i;
  int nibbles;
  uint32_t mlen_acc;
  int skip_i;
  int skip_n;
  int vi;
  int vi_n;
  int cat_i;
  int csub;
  int tmp;
  int ntypes[3];
  uint32_t blen[3];
  int rb[6];
  brotli_huff_t ht_type[3];
  brotli_huff_t ht_len[3];
  int npostfix;
  int ndirect;
  int dist_alpha;
  uint8_t cmode[256];
  int mode_i;
  int ntrees_l;
  int ntrees_d;
  uint8_t cmap_l[64 * 256];
  uint8_t cmap_d[4 * 256];
  brotli_huff_t * lit;
  brotli_huff_t * ins;
  brotli_huff_t * dist;
  int nlit;
  int nins;
  int ndist;
  int tree_i;
  int tree_ready;
  brotli_huff_t cmap_huff;
  brotli_tree_t tree;
  brotli_sym_t sym;
  int mstep;
  int rlemax;
  int map_i;
  int rep_sym;
  int map_size;
  int map_trees;
  uint8_t * map_dst;

  int cstep;
  int sw;
  int ins_code;
  int copy_code;
  int implicit;
  uint32_t ins_len;
  uint32_t copy_len;
  uint32_t lit_left;
  int lstep;
  int lit_byte;
  int dist_sym;
  int dist_extra_n;
  uint32_t drest;
  uint64_t distance;
  int copy_is_dict;
  int copy_pos;
  int copy_total;
  int did_setup;
  uint8_t word[40];
  int holding;
  uint8_t held;
} brotli_dec_t;

static int dec_fail(brotli_dec_t * st, gcomp_status_t status, const char * msg) {
  st->failed = 1;
  st->err = status;
  gcomp_decoder_set_error(st->pub, status, "%s", msg);
  return BR_FAIL;
}

static int dec_limit(brotli_dec_t * st, const char * msg) {
  st->failed = 1;
  st->err = GCOMP_ERR_LIMIT;
  gcomp_decoder_set_error(st->pub, GCOMP_ERR_LIMIT, "%s", msg);
  return BR_LIMIT;
}

static void free_huffs(brotli_dec_t * st, brotli_huff_t ** arr, int n) {
  int i;
  if (!*arr) {
    return;
  }
  for (i = 0; i < n; i++) {
    brotli_huff_free(st->alloc, &(*arr)[i]);
  }
  gcomp_free(st->alloc, *arr);
  *arr = NULL;
}

static void free_trees(brotli_dec_t * st) {
  int i;
  free_huffs(st, &st->lit, st->nlit);
  free_huffs(st, &st->ins, st->nins);
  free_huffs(st, &st->dist, st->ndist);
  st->nlit = st->nins = st->ndist = 0;
  for (i = 0; i < 3; i++) {
    brotli_huff_free(st->alloc, &st->ht_type[i]);
    brotli_huff_free(st->alloc, &st->ht_len[i]);
  }
  brotli_huff_free(st->alloc, &st->cmap_huff);
  brotli_huff_free(st->alloc, &st->tree.cl_huff);
}

static void begin_meta(brotli_dec_t * st) {
  st->step = 0;
  st->is_last = 0;
  st->is_meta = 0;
  st->is_uncomp = 0;
  st->mremain = 0;
  st->nibble_i = 0;
  st->nibbles = 0;
  st->mlen_acc = 0;
  st->skip_i = 0;
  st->skip_n = 0;
  st->vi = 0;
  st->cat_i = 0;
  st->csub = 0;
  st->mode_i = 0;
  st->tree_i = 0;
  st->tree_ready = 0;
  st->mstep = 0;
  st->ntypes[0] = st->ntypes[1] = st->ntypes[2] = 1;
  st->blen[0] = st->blen[1] = st->blen[2] = 16777216u;
  st->rb[0] = 1;
  st->rb[1] = 0;
  st->rb[2] = 1;
  st->rb[3] = 0;
  st->rb[4] = 1;
  st->rb[5] = 0;
}

static int emit_byte(brotli_dec_t * st, gcomp_buffer_t * out, uint8_t byte) {
  uint64_t in_bytes;
  uint8_t * dst;
  if (out->used >= out->size) {
    return BR_FULL;
  }
  if (st->produced == UINT64_MAX ||
      gcomp_limits_check_output((size_t)st->produced + 1u, st->max_out) !=
          GCOMP_OK) {
    return dec_limit(st, "brotli: decompressed output exceeds the limit");
  }
  in_bytes = st->input_total + (uint64_t)st->br.pos;
  if (gcomp_limits_check_expansion_ratio(in_bytes, st->produced + 1u,
          st->max_ratio) != GCOMP_OK) {
    return dec_limit(st, "brotli: expansion ratio exceeds the limit");
  }
  dst = (uint8_t *)out->data;
  dst[out->used++] = byte;
  if (st->ring && st->window != 0) {
    st->ring[st->ring_pos] = byte;
    st->ring_pos++;
    if (st->ring_pos == st->window) {
      st->ring_pos = 0;
    }
  }
  st->p2 = st->p1;
  st->p1 = byte;
  st->produced++;
  return BR_OK;
}

static int read_varint(brotli_dec_t * st, int * out) {
  for (;;) {
    uint32_t bits = 0;
    int r;
    switch (st->vi) {
    case 0:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits == 0) {
        *out = 1;
        st->vi = 0;
        return BR_OK;
      }
      st->vi = 1;
      break;
    case 1:
      r = brotli_bits_take(&st->br, 3, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits == 0) {
        *out = 2;
        st->vi = 0;
        return BR_OK;
      }
      st->vi_n = (int)bits;
      st->vi = 2;
      break;
    case 2:
      r = brotli_bits_take(&st->br, st->vi_n, &bits);
      if (r != BR_OK) {
        return r;
      }
      *out = (1 << st->vi_n) + (int)bits + 1;
      st->vi = 0;
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad varint state");
    }
  }
}

static int tree_step(brotli_dec_t * st) {
  int r = brotli_tree_read(&st->tree, &st->br, &st->sym, st->alloc);
  if (r == BR_FAIL) {
    const char * msg =
        st->tree.err[0] ? st->tree.err : "brotli: invalid prefix code";
    gcomp_status_t status = st->tree.err_status != GCOMP_OK
                                ? st->tree.err_status
                                : GCOMP_ERR_CORRUPT;
    return dec_fail(st, status, msg);
  }
  return r;
}

static int take_block(brotli_dec_t * st, int cat) {
  for (;;) {
    int sym = 0;
    int r;
    uint32_t bits = 0;
    switch (st->sw) {
    case 0:
      if (st->ntypes[cat] < 2 || st->blen[cat] != 0) {
        st->blen[cat]--;
        return BR_OK;
      }
      st->sw = 1;
      break;
    case 1: {
      int prev;
      int cur;
      int neu;
      r = brotli_read_sym(&st->br, &st->ht_type[cat], &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-type code");
      }
      if (r != BR_OK) {
        return r;
      }
      prev = st->rb[cat * 2];
      cur = st->rb[cat * 2 + 1];
      if (sym == 1) {
        neu = cur + 1;
      }
      else if (sym == 0) {
        neu = prev;
      }
      else {
        neu = sym - 2;
      }
      if (neu >= st->ntypes[cat]) {
        neu -= st->ntypes[cat];
      }
      if (neu < 0 || neu >= st->ntypes[cat]) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: block type out of range");
      }
      st->rb[cat * 2] = cur;
      st->rb[cat * 2 + 1] = neu;
      st->sw = 2;
      break;
    }
    case 2:
      r = brotli_read_sym(&st->br, &st->ht_len[cat], &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-count code");
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym < 0 || sym > 25) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-count symbol");
      }
      st->tmp = sym;
      st->sw = 3;
      break;
    case 3:
      r = brotli_bits_take(&st->br, k_blk_extra[st->tmp], &bits);
      if (r != BR_OK) {
        return r;
      }
      st->blen[cat] = k_blk_base[st->tmp] + bits;
      if (st->blen[cat] == 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: empty block count");
      }
      st->blen[cat]--;
      st->sw = 0;
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-switch state");
    }
  }
}

static int lit_cid(int mode, uint8_t p1, uint8_t p2) {
  if (mode == 0) {
    return p1 & 0x3f;
  }
  if (mode == 1) {
    return p1 >> 2;
  }
  if (mode == 2) {
    return brotli_lut0()[p1] | brotli_lut1()[p2];
  }
  return (brotli_lut2()[p1] << 3) | brotli_lut2()[p2];
}

static void split_ic(int sym, int * ins, int * copy, int * implicit) {
  static const int bins[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
  static const int bco[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
  int cell = sym >> 6;
  int within = sym & 63;
  *ins = bins[cell] + (within >> 3);
  *copy = bco[cell] + (within & 7);
  *implicit = sym < 128;
}

static void push_dist(brotli_dec_t * st, uint32_t distance) {
  st->dist_rb[3] = st->dist_rb[2];
  st->dist_rb[2] = st->dist_rb[1];
  st->dist_rb[1] = st->dist_rb[0];
  st->dist_rb[0] = distance;
}

static int short_dist(int sym, const uint32_t * d, uint64_t * out) {
  int64_t last = d[0];
  int64_t prev = d[1];
  int64_t v;
  switch (sym) {
  case 0:
    v = last;
    break;
  case 1:
    v = prev;
    break;
  case 2:
    v = d[2];
    break;
  case 3:
    v = d[3];
    break;
  case 4:
    v = last - 1;
    break;
  case 5:
    v = last + 1;
    break;
  case 6:
    v = last - 2;
    break;
  case 7:
    v = last + 2;
    break;
  case 8:
    v = last - 3;
    break;
  case 9:
    v = last + 3;
    break;
  case 10:
    v = prev - 1;
    break;
  case 11:
    v = prev + 1;
    break;
  case 12:
    v = prev - 2;
    break;
  case 13:
    v = prev + 2;
    break;
  case 14:
    v = prev - 3;
    break;
  case 15:
    v = prev + 3;
    break;
  default:
    return -1;
  }
  if (v <= 0) {
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static void inverse_mtf(uint8_t * v, int n) {
  uint8_t mtf[256];
  int i;
  for (i = 0; i < 256; i++) {
    mtf[i] = (uint8_t)i;
  }
  for (i = 0; i < n; i++) {
    uint8_t index = v[i];
    uint8_t value = mtf[index];
    v[i] = value;
    while (index) {
      mtf[index] = mtf[index - 1];
      index--;
    }
    mtf[0] = value;
  }
}

static int read_wbits(brotli_dec_t * st, int * out) {
  for (;;) {
    uint32_t n = 0;
    int r;
    switch (st->wb_step) {
    case 0:
      r = brotli_bits_take(&st->br, 1, &n);
      if (r != BR_OK) {
        return r;
      }
      if (n == 0) {
        *out = 16;
        st->wb_step = 0;
        return BR_OK;
      }
      st->wb_step = 1;
      break;
    case 1:
      r = brotli_bits_take(&st->br, 3, &n);
      if (r != BR_OK) {
        return r;
      }
      if (n != 0) {
        *out = 17 + (int)n;
        st->wb_step = 0;
        return BR_OK;
      }
      st->wb_step = 2;
      break;
    case 2:
      r = brotli_bits_take(&st->br, 3, &n);
      if (r != BR_OK) {
        return r;
      }
      st->wb_step = 0;
      if (n == 1) {
        return BR_FAIL;
      }
      if (n != 0) {
        *out = 8 + (int)n;
        return BR_OK;
      }
      *out = 17;
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad window state");
    }
  }
}

static int phase_window(brotli_dec_t * st) {
  int wbits = 0;
  int r = read_wbits(st, &wbits);
  size_t window;
  if (r == BR_FAIL) {
    return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: invalid window");
  }
  if (r != BR_OK) {
    return r;
  }
  if (wbits < 10 || wbits > 24) {
    return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: invalid window");
  }
  window = ((size_t)1u << wbits) - 16u;
  if (st->max_window != 0 && (uint64_t)window > st->max_window) {
    return dec_limit(st, "brotli: window exceeds limits.max_window_bytes");
  }
  if (gcomp_limits_check_memory(window, st->max_mem) != GCOMP_OK) {
    return dec_limit(st, "brotli: window exceeds limits.max_memory_bytes");
  }
  st->ring = gcomp_calloc(st->alloc, window, 1);
  if (!st->ring) {
    return dec_fail(st, GCOMP_ERR_MEMORY, "brotli: out of memory");
  }
  st->window = window;
  st->wbits = wbits;
  return BR_OK;
}

static int read_cmap(brotli_dec_t * st) {
  if (st->map_trees < 2) {
    memset(st->map_dst, 0, (size_t)st->map_size);
    st->mstep = 0;
    return BR_OK;
  }
  for (;;) {
    uint32_t bits = 0;
    int sym = 0;
    int r;
    switch (st->mstep) {
    case 0:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits == 0) {
        st->rlemax = 0;
        st->mstep = 2;
        break;
      }
      st->mstep = 1;
      break;
    case 1:
      r = brotli_bits_take(&st->br, 4, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->rlemax = (int)bits + 1;
      st->mstep = 2;
      break;
    case 2:
      if (!st->tree_ready) {
        int alpha = st->map_trees + st->rlemax;
        brotli_tree_begin(&st->tree, &st->cmap_huff, alpha, st->alloc);
        st->tree_ready = 1;
      }
      r = tree_step(st);
      if (r != BR_OK) {
        return r;
      }
      st->tree_ready = 0;
      st->map_i = 0;
      st->mstep = 3;
      break;
    case 3:
      if (st->map_i >= st->map_size) {
        st->mstep = 5;
        break;
      }
      r = brotli_read_sym(&st->br, &st->cmap_huff, &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad context-map code");
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym == 0) {
        st->map_dst[st->map_i++] = 0;
        break;
      }
      if (sym <= st->rlemax) {
        st->rep_sym = sym;
        st->mstep = 4;
        break;
      }
      if (sym - st->rlemax >= st->map_trees) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: context map out of range");
      }
      st->map_dst[st->map_i++] = (uint8_t)(sym - st->rlemax);
      break;
    case 4: {
      uint32_t reps;
      r = brotli_bits_take(&st->br, st->rep_sym, &bits);
      if (r != BR_OK) {
        return r;
      }
      reps = (1u << st->rep_sym) + bits;
      if ((uint32_t)st->map_i + reps > (uint32_t)st->map_size) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: context-map run too long");
      }
      memset(st->map_dst + st->map_i, 0, reps);
      st->map_i += (int)reps;
      st->mstep = 3;
      break;
    }
    case 5:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits) {
        inverse_mtf(st->map_dst, st->map_size);
      }
      for (sym = 0; sym < st->map_size; sym++) {
        if (st->map_dst[sym] >= st->map_trees) {
          return dec_fail(
              st, GCOMP_ERR_CORRUPT, "brotli: context map out of range");
        }
      }
      st->mstep = 0;
      st->tree_ready = 0;
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad context-map state");
    }
  }
}

static int alloc_trees(brotli_dec_t * st) {
  int i;
  /* Block-type trees were built earlier in this meta-block and are still
   * needed. Only the per-meta-block literal, command, and distance trees
   * are replaced here. */
  free_huffs(st, &st->lit, st->nlit);
  free_huffs(st, &st->ins, st->nins);
  free_huffs(st, &st->dist, st->ndist);
  st->nlit = 0;
  st->nins = 0;
  st->ndist = 0;
  brotli_huff_free(st->alloc, &st->cmap_huff);
  st->nlit = st->ntrees_l;
  st->nins = st->ntypes[CAT_I];
  st->ndist = st->ntrees_d;
  st->lit = gcomp_calloc(st->alloc, (size_t)st->nlit, sizeof(brotli_huff_t));
  st->ins = gcomp_calloc(st->alloc, (size_t)st->nins, sizeof(brotli_huff_t));
  st->dist = gcomp_calloc(st->alloc, (size_t)st->ndist, sizeof(brotli_huff_t));
  if (!st->lit || !st->ins || !st->dist) {
    free_huffs(st, &st->lit, st->nlit);
    free_huffs(st, &st->ins, st->nins);
    free_huffs(st, &st->dist, st->ndist);
    st->nlit = 0;
    st->nins = 0;
    st->ndist = 0;
    return dec_fail(st, GCOMP_ERR_MEMORY, "brotli: out of memory");
  }
  for (i = 0; i < st->nlit; i++) {
    st->lit[i].simple = -1;
  }
  for (i = 0; i < st->nins; i++) {
    st->ins[i].simple = -1;
  }
  for (i = 0; i < st->ndist; i++) {
    st->dist[i].simple = -1;
  }
  return BR_OK;
}

static int read_tree_array(brotli_dec_t * st, brotli_huff_t * arr, int n,
    int alphabet) {
  for (;;) {
    int r;
    if (st->tree_i >= n) {
      st->tree_i = 0;
      st->tree_ready = 0;
      return BR_OK;
    }
    if (!st->tree_ready) {
      brotli_tree_begin(&st->tree, &arr[st->tree_i], alphabet, st->alloc);
      st->tree_ready = 1;
    }
    r = tree_step(st);
    if (r != BR_OK) {
      return r;
    }
    st->tree_i++;
    st->tree_ready = 0;
  }
}

/**
 * The three block-type categories of RFC 7932 section 9.2: literals, then
 * insert-and-copy, then distances. Each one declares how many block types it
 * has, and a category with two or more carries a prefix code for the type, a
 * prefix code for the block count, and - the part this used to skip - the
 * count for its *first* block, read with that second code.
 *
 * Skipping it was invisible for as long as nothing produced more than one
 * block type. `begin_meta` sets every block count to 16777216, which is larger
 * than any meta-block, so `take_block` never reaches zero and never reads a
 * switch; with one block type that is exactly right and the sentinel is the
 * whole story. With two or more it is not: the bits for the initial count are
 * in the stream whether or not anything reads them, so every field after them
 * was read from the wrong bit position. The first symptom was a block-type
 * count of 162 for the insert-and-copy category and then a prefix code that
 * could not be built.
 *
 * libbrotli emits more than one block type from quality 4 upward on input
 * heterogeneous enough to be worth splitting, so this rejected a large part of
 * what the reference encoder produces - including every quality from 4 to 9 on
 * 24000 bytes of noise followed by markup.
 */
static int read_categories(brotli_dec_t * st) {
  for (;;) {
    int value = 0;
    int r;
    if (st->cat_i >= 3) {
      return BR_OK;
    }
    switch (st->csub) {
    case 0:
      r = read_varint(st, &value);
      if (r != BR_OK) {
        return r;
      }
      if (value < 1 || value > 256) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-type count");
      }
      st->ntypes[st->cat_i] = value;
      st->tree_ready = 0;
      st->csub = value < 2 ? 3 : 1;
      break;
    case 1:
      if (!st->tree_ready) {
        brotli_tree_begin(&st->tree, &st->ht_type[st->cat_i],
            st->ntypes[st->cat_i] + 2, st->alloc);
        st->tree_ready = 1;
      }
      r = tree_step(st);
      if (r != BR_OK) {
        return r;
      }
      st->tree_ready = 0;
      st->csub = 2;
      break;
    case 2:
      if (!st->tree_ready) {
        brotli_tree_begin(&st->tree, &st->ht_len[st->cat_i], 26, st->alloc);
        st->tree_ready = 1;
      }
      r = tree_step(st);
      if (r != BR_OK) {
        return r;
      }
      st->tree_ready = 0;
      st->csub = 4;
      break;
    case 4: {
      int sym = 0;
      r = brotli_read_sym(&st->br, &st->ht_len[st->cat_i], &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(
            st, GCOMP_ERR_CORRUPT, "brotli: bad first block-count code");
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym < 0 || sym > 25) {
        return dec_fail(
            st, GCOMP_ERR_CORRUPT, "brotli: bad first block-count symbol");
      }
      st->tmp = sym;
      st->csub = 5;
      break;
    }
    case 5: {
      uint32_t bits = 0;
      r = brotli_bits_take(&st->br, k_blk_extra[st->tmp], &bits);
      if (r != BR_OK) {
        return r;
      }
      /* No decrement here, unlike take_block's: that one is serving the
       * element it just switched for, and nothing has been read from this
       * block yet. The first take_block call takes the count down to the
       * right place. */
      st->blen[st->cat_i] = k_blk_base[st->tmp] + bits;
      if (st->blen[st->cat_i] == 0) {
        return dec_fail(
            st, GCOMP_ERR_CORRUPT, "brotli: empty first block count");
      }
      st->csub = 3;
      break;
    }
    case 3:
      st->cat_i++;
      st->csub = 0;
      st->vi = 0;
      break;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad block-type state");
    }
  }
}

static int phase_meta(brotli_dec_t * st) {
  for (;;) {
    uint32_t bits = 0;
    int r;
    int value = 0;
    switch (st->step) {
    case 0:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->is_last = (int)bits;
      st->step = st->is_last ? 1 : 2;
      break;
    case 1:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits) {
        r = brotli_bits_align(&st->br);
        if (r == BR_FAIL) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: non-zero padding");
        }
        st->phase = PH_DONE;
        return BR_OK;
      }
      st->step = 2;
      break;
    case 2:
      r = brotli_bits_take(&st->br, 2, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits == 3) {
        st->is_meta = 1;
        st->step = 3;
        break;
      }
      st->nibbles = (int)bits + 4;
      st->nibble_i = 0;
      st->mlen_acc = 0;
      st->step = 6;
      break;
    case 3:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (bits != 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: reserved metadata bit");
      }
      st->step = 4;
      break;
    case 4:
      r = brotli_bits_take(&st->br, 2, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->skip_n = (int)bits;
      st->skip_i = 0;
      st->mlen_acc = 0;
      st->step = st->skip_n == 0 ? 8 : 5;
      break;
    case 5:
      if (st->skip_i >= st->skip_n) {
        st->mremain = st->mlen_acc + 1u;
        st->step = 8;
        break;
      }
      r = brotli_bits_take(&st->br, 8, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (st->skip_i + 1 == st->skip_n && st->skip_n > 1 && bits == 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: exuberant metadata length");
      }
      st->mlen_acc |= bits << (st->skip_i * 8);
      st->skip_i++;
      break;
    case 6:
      if (st->nibble_i >= st->nibbles) {
        st->mremain = st->mlen_acc + 1u;
        st->step = st->is_last ? 9 : 7;
        break;
      }
      r = brotli_bits_take(&st->br, 4, &bits);
      if (r != BR_OK) {
        return r;
      }
      if (st->nibble_i + 1 == st->nibbles && st->nibbles > 4 && bits == 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: exuberant meta-block length");
      }
      st->mlen_acc |= bits << (st->nibble_i * 4);
      st->nibble_i++;
      break;
    case 7:
      r = brotli_bits_take(&st->br, 1, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->is_uncomp = (int)bits;
      if (st->is_uncomp) {
        r = brotli_bits_align(&st->br);
        if (r == BR_FAIL) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: non-zero padding");
        }
        st->holding = 0;
        st->phase = PH_UNCOMP;
        return BR_OK;
      }
      st->step = 9;
      break;
    case 8:
      r = brotli_bits_align(&st->br);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: non-zero padding");
      }
      if (r != BR_OK) {
        return r;
      }
      st->phase = PH_SKIP;
      return BR_OK;
    case 9:
      if (st->cat_i >= 3) {
        st->step = 10;
        break;
      }
      r = read_categories(st);
      if (r != BR_OK) {
        return r;
      }
      break;
    case 10:
      r = brotli_bits_take(&st->br, 2, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->npostfix = (int)bits;
      st->step = 11;
      break;
    case 11:
      r = brotli_bits_take(&st->br, 4, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->ndirect = (int)bits << st->npostfix;
      st->dist_alpha = 16 + st->ndirect + (48 << st->npostfix);
      if (st->dist_alpha < 16 || st->dist_alpha > BROTLI_MAX_ALPHABET) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance alphabet");
      }
      st->mode_i = 0;
      st->step = 12;
      break;
    case 12:
      if (st->mode_i >= st->ntypes[CAT_L]) {
        st->vi = 0;
        st->step = 13;
        break;
      }
      r = brotli_bits_take(&st->br, 2, &bits);
      if (r != BR_OK) {
        return r;
      }
      st->cmode[st->mode_i++] = (uint8_t)bits;
      break;
    case 13:
      r = read_varint(st, &value);
      if (r != BR_OK) {
        return r;
      }
      if (value < 1 || value > 256) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad literal tree count");
      }
      st->ntrees_l = value;
      st->map_dst = st->cmap_l;
      st->map_size = 64 * st->ntypes[CAT_L];
      st->map_trees = st->ntrees_l;
      st->mstep = 0;
      st->tree_ready = 0;
      st->step = 14;
      break;
    case 14:
      r = read_cmap(st);
      if (r != BR_OK) {
        return r;
      }
      st->vi = 0;
      st->step = 15;
      break;
    case 15:
      r = read_varint(st, &value);
      if (r != BR_OK) {
        return r;
      }
      if (value < 1 || value > 256) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance tree count");
      }
      st->ntrees_d = value;
      st->map_dst = st->cmap_d;
      st->map_size = 4 * st->ntypes[CAT_D];
      st->map_trees = st->ntrees_d;
      st->mstep = 0;
      st->tree_ready = 0;
      st->step = 16;
      break;
    case 16:
      r = read_cmap(st);
      if (r != BR_OK) {
        return r;
      }
      r = alloc_trees(st);
      if (r != BR_OK) {
        return r;
      }
      st->tree_i = 0;
      st->tree_ready = 0;
      st->step = 17;
      break;
    case 17:
      r = read_tree_array(st, st->lit, st->nlit, 256);
      if (r != BR_OK) {
        return r;
      }
      st->step = 18;
      break;
    case 18:
      r = read_tree_array(st, st->ins, st->nins, 704);
      if (r != BR_OK) {
        return r;
      }
      st->step = 19;
      break;
    case 19:
      r = read_tree_array(st, st->dist, st->ndist, st->dist_alpha);
      if (r != BR_OK) {
        return r;
      }
      st->cstep = 0;
      st->sw = 0;
      st->phase = PH_COMMAND;
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad meta-block state");
    }
  }
}

static int end_block(brotli_dec_t * st) {
  int r;
  free_trees(st);
  if (st->is_last) {
    r = brotli_bits_align(&st->br);
    if (r == BR_FAIL) {
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: non-zero padding");
    }
    st->phase = PH_DONE;
    return BR_OK;
  }
  begin_meta(st);
  st->phase = PH_META;
  return BR_OK;
}

static int phase_uncomp(brotli_dec_t * st, gcomp_buffer_t * out) {
  while (st->mremain != 0) {
    uint8_t byte = 0;
    int r;
    if (!st->holding) {
      r = brotli_bits_byte(&st->br, &byte);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: uncompressed data is not aligned");
      }
      if (r != BR_OK) {
        return r;
      }
      st->held = byte;
      st->holding = 1;
    }
    r = emit_byte(st, out, st->held);
    if (r != BR_OK) {
      return r;
    }
    st->holding = 0;
    st->mremain--;
  }
  return BR_OK;
}

static int phase_skip(brotli_dec_t * st) {
  while (st->mremain != 0) {
    uint8_t byte = 0;
    int r = brotli_bits_byte(&st->br, &byte);
    if (r == BR_FAIL) {
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: metadata is not byte aligned");
    }
    if (r != BR_OK) {
      return r;
    }
    st->mremain--;
  }
  return BR_OK;
}

static int ring_byte(brotli_dec_t * st, uint64_t distance, uint8_t * out) {
  size_t back;
  size_t src;
  if (st->window == 0 || distance == 0 || distance > st->window) {
    return -1;
  }
  back = (size_t)distance;
  if (back == st->window) {
    src = st->ring_pos;
  }
  else {
    src = (st->ring_pos + st->window - back) % st->window;
  }
  *out = st->ring[src];
  return 0;
}

static int phase_command(brotli_dec_t * st, gcomp_buffer_t * out) {
  for (;;) {
    int r;
    int sym = 0;
    uint32_t bits = 0;
    switch (st->cstep) {
    case 0:
      if (st->mremain == 0) {
        return end_block(st);
      }
      st->sw = 0;
      st->cstep = 1;
      break;
    case 1:
      r = take_block(st, CAT_I);
      if (r != BR_OK) {
        return r;
      }
      st->cstep = 2;
      break;
    case 2: {
      int btype = st->rb[CAT_I * 2 + 1];
      if (btype < 0 || btype >= st->nins) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad insert-copy type");
      }
      r = brotli_read_sym(&st->br, &st->ins[btype], &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad insert-copy code");
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym < 0 || sym >= 704) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad insert-copy symbol");
      }
      split_ic(sym, &st->ins_code, &st->copy_code, &st->implicit);
      st->cstep = 3;
      break;
    }
    case 3:
      r = brotli_bits_take(&st->br, k_ins_extra[st->ins_code], &bits);
      if (r != BR_OK) {
        return r;
      }
      st->ins_len = k_ins_base[st->ins_code] + bits;
      st->cstep = 4;
      break;
    case 4:
      r = brotli_bits_take(&st->br, k_copy_extra[st->copy_code], &bits);
      if (r != BR_OK) {
        return r;
      }
      st->copy_len = k_copy_base[st->copy_code] + bits;
      if (st->ins_len > st->mremain) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: insert exceeds meta-block");
      }
      st->lit_left = st->ins_len;
      st->lstep = 0;
      st->cstep = 5;
      break;
    case 5:
      if (st->lit_left == 0) {
        st->cstep = 6;
        break;
      }
      if (st->lstep == 0) {
        st->sw = 0;
        st->lstep = 1;
      }
      if (st->lstep == 1) {
        r = take_block(st, CAT_L);
        if (r != BR_OK) {
          return r;
        }
        st->lstep = 2;
      }
      if (st->lstep == 2) {
        int btype = st->rb[CAT_L * 2 + 1];
        int cid;
        int ti;
        if (btype < 0 || btype >= st->ntypes[CAT_L]) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad literal type");
        }
        cid = lit_cid(st->cmode[btype], st->p1, st->p2);
        ti = st->cmap_l[64 * btype + cid];
        if (ti < 0 || ti >= st->nlit) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad literal tree");
        }
        r = brotli_read_sym(&st->br, &st->lit[ti], &st->sym, &sym);
        if (r == BR_FAIL) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad literal code");
        }
        if (r != BR_OK) {
          return r;
        }
        st->lit_byte = sym;
        st->lstep = 3;
      }
      if (st->lstep == 3) {
        r = emit_byte(st, out, (uint8_t)st->lit_byte);
        if (r != BR_OK) {
          return r;
        }
        st->lit_left--;
        st->mremain--;
        st->lstep = 0;
      }
      break;
    case 6:
      if (st->mremain == 0) {
        st->cstep = 0;
        break;
      }
      if (st->implicit) {
        st->distance = st->dist_rb[0];
        st->did_setup = 0;
        st->cstep = 10;
        break;
      }
      st->sw = 0;
      st->cstep = 7;
      break;
    case 7:
      r = take_block(st, CAT_D);
      if (r != BR_OK) {
        return r;
      }
      st->cstep = 8;
      break;
    case 8: {
      int btype = st->rb[CAT_D * 2 + 1];
      int dcid;
      int ti;
      uint32_t xcode;
      if (btype < 0 || btype >= st->ntypes[CAT_D]) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance type");
      }
      if (st->copy_len <= 2) {
        dcid = 0;
      }
      else if (st->copy_len == 3) {
        dcid = 1;
      }
      else if (st->copy_len == 4) {
        dcid = 2;
      }
      else {
        dcid = 3;
      }
      ti = st->cmap_d[4 * btype + dcid];
      if (ti < 0 || ti >= st->ndist) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance tree");
      }
      r = brotli_read_sym(&st->br, &st->dist[ti], &st->sym, &sym);
      if (r == BR_FAIL) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance code");
      }
      if (r != BR_OK) {
        return r;
      }
      if (sym < 0 || sym >= st->dist_alpha) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance symbol");
      }
      st->dist_sym = sym;
      if (sym < 16) {
        if (short_dist(sym, st->dist_rb, &st->distance) != 0) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: distance is not positive");
        }
        st->did_setup = 0;
        st->cstep = 10;
        break;
      }
      if (sym < 16 + st->ndirect) {
        st->distance = (uint64_t)(sym - 15);
        st->did_setup = 0;
        st->cstep = 10;
        break;
      }
      xcode = (uint32_t)sym - (uint32_t)st->ndirect - 16u;
      st->dist_extra_n = 1 + (int)(xcode >> (st->npostfix + 1));
      if (st->dist_extra_n < 1 || st->dist_extra_n > 24) {
        return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad distance extra bits");
      }
      st->drest = xcode;
      st->cstep = 9;
      break;
    }
    case 9: {
      uint32_t hcode;
      uint32_t lcode;
      uint64_t offset;
      r = brotli_bits_take(&st->br, st->dist_extra_n, &bits);
      if (r != BR_OK) {
        return r;
      }
      hcode = st->drest >> st->npostfix;
      lcode = st->drest & ((1u << st->npostfix) - 1u);
      offset = ((2ull + (uint64_t)(hcode & 1u)) << st->dist_extra_n) - 4ull;
      st->distance = ((offset + bits) << st->npostfix) + lcode +
                     (uint64_t)st->ndirect + 1ull;
      st->did_setup = 0;
      st->cstep = 10;
      break;
    }
    case 10:
      if (!st->did_setup) {
        uint64_t max_dist =
            st->produced < st->window ? st->produced : (uint64_t)st->window;
        if (st->distance == 0) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: distance is not positive");
        }
        if (st->distance > max_dist) {
          int wlen = 0;
          if (st->copy_len < 4 || st->copy_len > 24 ||
              brotli_dict_word((int)st->copy_len, st->distance - max_dist - 1ull,
                  st->word, 40, &wlen) != 0) {
            return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad dictionary word");
          }
          if ((uint32_t)wlen > st->mremain) {
            return dec_fail(
                st, GCOMP_ERR_CORRUPT, "brotli: dictionary word exceeds meta-block");
          }
          st->copy_is_dict = 1;
          st->copy_total = wlen;
        }
        else {
          if (st->copy_len > st->mremain) {
            return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: copy exceeds meta-block");
          }
          st->copy_is_dict = 0;
          st->copy_total = (int)st->copy_len;
          if (!st->implicit && st->dist_sym != 0) {
            if (st->distance > 0xffffffffull) {
              return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: distance out of range");
            }
            push_dist(st, (uint32_t)st->distance);
          }
        }
        st->copy_pos = 0;
        st->did_setup = 1;
      }
      while (st->copy_pos < st->copy_total) {
        uint8_t byte = 0;
        if (st->copy_is_dict) {
          byte = st->word[st->copy_pos];
        }
        else if (ring_byte(st, st->distance, &byte) != 0) {
          return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: copy is outside the window");
        }
        r = emit_byte(st, out, byte);
        if (r != BR_OK) {
          return r;
        }
        st->copy_pos++;
        st->mremain--;
      }
      st->cstep = 0;
      st->did_setup = 0;
      break;
    default:
      return dec_fail(st, GCOMP_ERR_CORRUPT, "brotli: bad command state");
    }
  }
}

static int drive(brotli_dec_t * st, gcomp_buffer_t * out) {
  for (;;) {
    int r;
    switch (st->phase) {
    case PH_WINDOW:
      r = phase_window(st);
      if (r != BR_OK) {
        return r;
      }
      begin_meta(st);
      st->phase = PH_META;
      break;
    case PH_META:
      r = phase_meta(st);
      if (r != BR_OK) {
        return r;
      }
      break;
    case PH_UNCOMP:
      r = phase_uncomp(st, out);
      if (r != BR_OK) {
        return r;
      }
      r = end_block(st);
      if (r != BR_OK) {
        return r;
      }
      break;
    case PH_SKIP:
      r = phase_skip(st);
      if (r != BR_OK) {
        return r;
      }
      r = end_block(st);
      if (r != BR_OK) {
        return r;
      }
      break;
    case PH_COMMAND:
      r = phase_command(st, out);
      if (r != BR_OK) {
        return r;
      }
      break;
    case PH_DONE:
      return BR_OK;
    default:
      return dec_fail(st, GCOMP_ERR_INTERNAL, "brotli: bad decoder phase");
    }
  }
}

static void mark_unset(brotli_dec_t * st) {
  int i;
  for (i = 0; i < 3; i++) {
    st->ht_type[i].simple = -1;
    st->ht_len[i].simple = -1;
  }
  st->cmap_huff.simple = -1;
  st->tree.cl_huff.simple = -1;
}

static void attach(brotli_dec_t * st, const gcomp_buffer_t * input) {
  size_t avail = input->size - input->used;
  st->br.data = avail ? (const uint8_t *)input->data + input->used : NULL;
  st->br.size = avail;
  st->br.pos = 0;
}

static void detach(brotli_dec_t * st, gcomp_buffer_t * input) {
  input->used += st->br.pos;
  st->input_total += (uint64_t)st->br.pos;
  st->br.pos = 0;
  st->br.data = NULL;
  st->br.size = 0;
}


static gcomp_status_t map_status(brotli_dec_t * st, int r) {
  if (r == BR_OK || r == BR_NEED || r == BR_FULL) {
    return GCOMP_OK;
  }
  if (r == BR_LIMIT) {
    return GCOMP_ERR_LIMIT;
  }
  if (st->err != GCOMP_OK) {
    return st->err;
  }
  return GCOMP_ERR_CORRUPT;
}

static void init_distances(brotli_dec_t * st) {
  st->dist_rb[0] = 4;
  st->dist_rb[1] = 11;
  st->dist_rb[2] = 15;
  st->dist_rb[3] = 16;
}

gcomp_status_t brotli_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  brotli_dec_t * st;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  alloc = gcomp_registry_get_allocator(registry);
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "brotli: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_mem = gcomp_limits_read_memory_max(options, 0);
  st->max_window = gcomp_limits_read_window_max(options, 1ull << 24);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_BROTLI_MAX_EXPANSION_RATIO);
  init_distances(st);
  mark_unset(st);
  decoder->method_state = st;
  return GCOMP_OK;
}

void brotli_decoder_destroy(gcomp_decoder_t * decoder) {
  brotli_dec_t * st;
  const gcomp_allocator_t * alloc;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  alloc = st->alloc;
  free_trees(st);
  gcomp_free(alloc, st->ring);
  gcomp_free(alloc, st);
  decoder->method_state = NULL;
}

gcomp_status_t brotli_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  brotli_dec_t * st;
  int r;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  if (st->failed) {
    return st->err;
  }
  if (st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  attach(st, input);
  r = drive(st, output);
  detach(st, input);
  return map_status(st, r);
}

gcomp_status_t brotli_decoder_finish(gcomp_decoder_t * decoder,
    gcomp_buffer_t * output) {
  brotli_dec_t * st;
  int r;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  if (st->failed) {
    return st->err;
  }
  if (st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  r = drive(st, output);
  if (r == BR_OK && st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  if (r == BR_FULL || r == BR_LIMIT) {
    return GCOMP_ERR_LIMIT;
  }
  if (r == BR_NEED) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_CORRUPT, "brotli: truncated stream");
  }
  if (st->err != GCOMP_OK) {
    return st->err;
  }
  return gcomp_decoder_set_error(
      decoder, GCOMP_ERR_CORRUPT, "brotli: truncated stream");
}

gcomp_status_t brotli_decoder_reset(gcomp_decoder_t * decoder) {
  brotli_dec_t * st;
  const gcomp_allocator_t * alloc;
  uint64_t max_out;
  uint64_t max_mem;
  uint64_t max_window;
  uint64_t max_ratio;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  alloc = st->alloc;
  max_out = st->max_out;
  max_mem = st->max_mem;
  max_window = st->max_window;
  max_ratio = st->max_ratio;
  free_trees(st);
  gcomp_free(alloc, st->ring);
  memset(st, 0, sizeof(*st));
  st->pub = decoder;
  st->alloc = alloc;
  st->max_out = max_out;
  st->max_mem = max_mem;
  st->max_window = max_window;
  st->max_ratio = max_ratio;
  init_distances(st);
  mark_unset(st);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
