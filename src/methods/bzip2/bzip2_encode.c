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
 * @file bzip2_encode.c
 *
 * The bzip2 encoder.
 *
 * ## The pipeline, per block
 *
 * Input is run-length coded as it arrives (four or more equal bytes become
 * four and a count) into a block of up to `level * 100000 - 19` bytes, with a
 * CRC kept over the bytes as they came in. A full block is sorted by the
 * Burrows-Wheeler transform (bzip2_bwt.c), move-to-front coded with runs of
 * zeros spelled in a two-symbol bijective code, and Huffman coded with two to
 * six tables chosen per group of 50 symbols. The tables are found the way
 * libbz2 finds them: start from a partition of the alphabet by frequency,
 * assign each group to the table that codes it cheapest, rebuild the tables
 * from what they were assigned, and do that four times.
 *
 * ## Flush
 *
 * A bzip2 block ends in the middle of a byte and the next one starts there, so
 * there is no point inside a stream where its bytes stand alone. A flush
 * therefore ends the stream: the block in hand, then the end marker and the
 * stream CRC, then padding to a byte. The next byte fed in begins a new
 * stream, and concatenated streams are one stream to `bzip2 -d` and to this
 * library's decoder. A flush costs 14 bytes of header and footer, and a full
 * flush is the same as a sync flush because no block refers to another.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/huffman_lengths.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "bzip2_internal.h"

#include <string.h>

#define ENC_ITERS 4
#define ENC_MAX_CODE_LEN 17

typedef struct bz_enc_s {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  unsigned level;
  uint32_t block_max; /* bytes of block, after the first run-length pass */

  /* the first run-length pass, and the block it fills */
  uint8_t * blk;
  uint32_t nblock;
  uint32_t block_crc;
  int run_ch;
  unsigned run_len;

  uint32_t combined;
  int in_stream; /* the header has been written and the end has not */
  unsigned streams; /* streams ended so far: a flush may have ended some */
  int finished;

  /* the block's transform */
  uint8_t * last;
  uint16_t * mtfv;
  bzip2_bwt_scratch_t bwt;

  /* the bits */
  uint8_t * stage;
  size_t stage_cap;
  size_t stage_len;
  size_t stage_pos;
  uint64_t acc;
  unsigned nacc;
} bz_enc_t;

/* ---- bits, most significant first --------------------------------------- */

static inline void put(bz_enc_t * e, uint32_t v, unsigned n) {
  e->acc = (e->acc << n) | (v & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)));
  e->nacc += n;
  while (e->nacc >= 8) {
    e->nacc -= 8;
    e->stage[e->stage_len++] = (uint8_t)(e->acc >> e->nacc);
  }
}

static void put48(bz_enc_t * e, uint64_t v) {
  put(e, (uint32_t)(v >> 24), 24);
  put(e, (uint32_t)(v & 0xFFFFFFu), 24);
}

static void put_pad(bz_enc_t * e) {
  if (e->nacc != 0) {
    put(e, 0, 8 - e->nacc);
  }
}

/* ---- one block ----------------------------------------------------------- */

typedef struct block_coding_s {
  unsigned n_in_use;
  uint8_t in_use[256];
  uint8_t unseq_to_seq[256];
  unsigned alpha;
  unsigned n_mtf;
  uint32_t freq[BZIP2_MAX_ALPHA];
  unsigned n_groups;
  uint8_t len[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  uint32_t code[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  unsigned n_selectors;
  uint8_t selector[BZIP2_MAX_SELECTORS];
} block_coding_t;

/* Move-to-front the last column and spell the runs of zeros. */
static void make_mtf(bz_enc_t * e, block_coding_t * c) {
  uint8_t yy[256];
  unsigned i, j, wr = 0;
  uint32_t z_pend = 0;
  const unsigned eob = c->n_in_use + 1;
  memset(c->freq, 0, sizeof(c->freq));
  for (i = 0; i < c->n_in_use; i++) {
    yy[i] = (uint8_t)i;
  }
  for (i = 0; i < e->nblock; i++) {
    uint8_t ll = c->unseq_to_seq[e->last[i]];
    if (yy[0] == ll) {
      z_pend++;
      continue;
    }
    if (z_pend > 0) {
      z_pend--;
      for (;;) {
        unsigned sym = (z_pend & 1u) ? BZIP2_RUNB : BZIP2_RUNA;
        e->mtfv[wr++] = (uint16_t)sym;
        c->freq[sym]++;
        if (z_pend < 2) {
          break;
        }
        z_pend = (z_pend - 2u) / 2u;
      }
      z_pend = 0;
    }
    for (j = 1; yy[j] != ll; j++) {
    }
    memmove(yy + 1, yy, j);
    yy[0] = ll;
    e->mtfv[wr++] = (uint16_t)(j + 1);
    c->freq[j + 1]++;
  }
  if (z_pend > 0) {
    z_pend--;
    for (;;) {
      unsigned sym = (z_pend & 1u) ? BZIP2_RUNB : BZIP2_RUNA;
      e->mtfv[wr++] = (uint16_t)sym;
      c->freq[sym]++;
      if (z_pend < 2) {
        break;
      }
      z_pend = (z_pend - 2u) / 2u;
    }
  }
  e->mtfv[wr++] = (uint16_t)eob;
  c->freq[eob]++;
  c->n_mtf = wr;
}

/* Code lengths of at most ENC_MAX_CODE_LEN for every symbol of the alphabet,
 * whether it occurs or not: a decoder reads a length for each. */
static int lengths_for(bz_enc_t * e, const uint32_t * freq, unsigned alpha,
    uint8_t * len_out) {
  uint32_t f[BZIP2_MAX_ALPHA];
  unsigned i;
  for (i = 0; i < alpha; i++) {
    f[i] = freq[i] ? freq[i] : 1u;
  }
  return gcomp_huffman_code_lengths(e->alloc, f, alpha, ENC_MAX_CODE_LEN, len_out) ==
          GCOMP_OK
      ? 0
      : -1;
}

/* The tables and the choice of table for each group of 50 symbols. */
static int make_tables(bz_enc_t * e, block_coding_t * c) {
  const unsigned alpha = c->alpha;
  unsigned t, v, iter, g;
  uint32_t rfreq[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  c->n_groups = c->n_mtf < 200 ? 2 : c->n_mtf < 600 ? 3 : c->n_mtf < 1200 ? 4
      : c->n_mtf < 2400                                ? 5
                                                       : 6;
  /* A start: split the alphabet into n_groups ranges of about equal
   * frequency, and give each table short codes for its own range. */
  {
    uint32_t rem = c->n_mtf;
    unsigned part = c->n_groups, gs = 0;
    while (part > 0) {
      uint32_t target = rem / part, acc = 0;
      int ge = (int)gs - 1;
      while (acc < target && ge < (int)alpha - 1) {
        ge++;
        acc += c->freq[ge];
      }
      for (v = 0; v < alpha; v++) {
        c->len[part - 1][v] = (v >= gs && (int)v <= ge) ? 1 : 15;
      }
      gs = (unsigned)ge + 1;
      rem -= acc;
      part--;
    }
  }
  c->n_selectors = (c->n_mtf + BZIP2_GROUP_SIZE - 1) / BZIP2_GROUP_SIZE;
  for (iter = 0; iter < ENC_ITERS; iter++) {
    memset(rfreq, 0, sizeof(rfreq));
    for (g = 0; g < c->n_selectors; g++) {
      unsigned lo = g * BZIP2_GROUP_SIZE;
      unsigned hi = lo + BZIP2_GROUP_SIZE;
      uint32_t best_cost = 0xFFFFFFFFu;
      unsigned best = 0, i;
      if (hi > c->n_mtf) {
        hi = c->n_mtf;
      }
      for (t = 0; t < c->n_groups; t++) {
        uint32_t cost = 0;
        for (i = lo; i < hi; i++) {
          cost += c->len[t][e->mtfv[i]];
        }
        if (cost < best_cost) {
          best_cost = cost;
          best = t;
        }
      }
      c->selector[g] = (uint8_t)best;
      for (i = lo; i < hi; i++) {
        rfreq[best][e->mtfv[i]]++;
      }
    }
    for (t = 0; t < c->n_groups; t++) {
      if (lengths_for(e, rfreq[t], alpha, c->len[t]) != 0) {
        return -1;
      }
    }
  }
  /* Canonical codes: by length, then by symbol, as the decoder assigns them. */
  for (t = 0; t < c->n_groups; t++) {
    unsigned min_len = 32, max_len = 0, n;
    uint32_t vec = 0;
    for (v = 0; v < alpha; v++) {
      if (c->len[t][v] > max_len) {
        max_len = c->len[t][v];
      }
      if (c->len[t][v] < min_len) {
        min_len = c->len[t][v];
      }
    }
    for (n = min_len; n <= max_len; n++) {
      for (v = 0; v < alpha; v++) {
        if (c->len[t][v] == n) {
          c->code[t][v] = vec++;
        }
      }
      vec <<= 1;
    }
  }
  return 0;
}

/* Everything between the block's magic and its first symbol, and the symbols. */
static void write_block_body(bz_enc_t * e, block_coding_t * c, uint32_t orig) {
  unsigned i, t, g, v;
  uint16_t groups = 0;
  put(e, 0, 1); /* not randomised */
  put(e, orig, 24);
  for (i = 0; i < 16; i++) {
    for (v = 0; v < 16; v++) {
      if (c->in_use[i * 16 + v]) {
        groups |= (uint16_t)(0x8000u >> i);
        break;
      }
    }
  }
  put(e, groups, 16);
  for (i = 0; i < 16; i++) {
    if (groups & (0x8000u >> i)) {
      uint32_t bits = 0;
      for (v = 0; v < 16; v++) {
        if (c->in_use[i * 16 + v]) {
          bits |= 0x8000u >> v;
        }
      }
      put(e, bits, 16);
    }
  }
  put(e, c->n_groups, 3);
  put(e, c->n_selectors, 15);
  {
    uint8_t pos[BZIP2_MAX_GROUPS];
    for (i = 0; i < c->n_groups; i++) {
      pos[i] = (uint8_t)i;
    }
    for (g = 0; g < c->n_selectors; g++) {
      uint8_t sel = c->selector[g];
      unsigned j = 0;
      while (pos[j] != sel) {
        j++;
      }
      for (i = 0; i < j; i++) {
        put(e, 1, 1);
      }
      put(e, 0, 1);
      memmove(pos + 1, pos, j);
      pos[0] = sel;
    }
  }
  for (t = 0; t < c->n_groups; t++) {
    unsigned curr = c->len[t][0];
    put(e, curr, 5);
    for (v = 0; v < c->alpha; v++) {
      while (curr < c->len[t][v]) {
        put(e, 2, 2);
        curr++;
      }
      while (curr > c->len[t][v]) {
        put(e, 3, 2);
        curr--;
      }
      put(e, 0, 1);
    }
  }
  for (i = 0; i < c->n_mtf; i++) {
    unsigned tbl = c->selector[i / BZIP2_GROUP_SIZE];
    uint16_t sym = e->mtfv[i];
    put(e, c->code[tbl][sym], c->len[tbl][sym]);
  }
}

/* Code the block in hand and append its bits to the stage. */
static gcomp_status_t emit_block(bz_enc_t * e) {
  block_coding_t * c;
  uint32_t orig = 0;
  unsigned i;
  gcomp_status_t status = GCOMP_OK;
  if (e->nblock == 0) {
    return GCOMP_OK;
  }
  c = gcomp_calloc(e->alloc, 1, sizeof(*c));
  if (!c) {
    return gcomp_encoder_set_error(
        e->pub, GCOMP_ERR_MEMORY, "bzip2: out of memory for a block");
  }
  if (bzip2_bwt(e->alloc, &e->bwt, e->blk, e->nblock, e->last, &orig) != 0) {
    status = gcomp_encoder_set_error(
        e->pub, GCOMP_ERR_MEMORY, "bzip2: out of memory sorting a block");
    goto out;
  }
  for (i = 0; i < e->nblock; i++) {
    c->in_use[e->blk[i]] = 1;
  }
  for (i = 0; i < 256; i++) {
    if (c->in_use[i]) {
      c->unseq_to_seq[i] = (uint8_t)c->n_in_use++;
    }
  }
  c->alpha = c->n_in_use + 2;
  make_mtf(e, c);
  if (make_tables(e, c) != 0) {
    status = gcomp_encoder_set_error(
        e->pub, GCOMP_ERR_MEMORY, "bzip2: out of memory building tables");
    goto out;
  }
  {
    uint32_t crc = ~e->block_crc;
    put48(e, BZIP2_BLOCK_MAGIC);
    put(e, crc, 32);
    write_block_body(e, c, orig);
    e->combined = bzip2_combine(e->combined, crc);
  }
out:
  gcomp_free(e->alloc, c);
  e->nblock = 0;
  e->block_crc = 0xFFFFFFFFu;
  return status;
}

/* ---- the first run-length pass ------------------------------------------ */

/* The run in hand becomes bytes of the block: up to three as they are, four or
 * more as four and a count of the rest. */
static void flush_run(bz_enc_t * e) {
  unsigned i;
  if (e->run_len == 0) {
    return;
  }
  for (i = 0; i < e->run_len; i++) {
    e->block_crc = bzip2_crc_update(e->block_crc, (uint8_t)e->run_ch);
  }
  if (e->run_len >= 4) {
    for (i = 0; i < 4; i++) {
      e->blk[e->nblock++] = (uint8_t)e->run_ch;
    }
    e->blk[e->nblock++] = (uint8_t)(e->run_len - 4u);
  }
  else {
    for (i = 0; i < e->run_len; i++) {
      e->blk[e->nblock++] = (uint8_t)e->run_ch;
    }
  }
  e->run_len = 0;
}

static inline void add_byte(bz_enc_t * e, uint8_t b) {
  if (e->run_len > 0 && b == (uint8_t)e->run_ch && e->run_len < 255) {
    e->run_len++;
    return;
  }
  flush_run(e);
  e->run_ch = b;
  e->run_len = 1;
}

/* ---- the stream ---------------------------------------------------------- */

static void begin_stream(bz_enc_t * e) {
  put(e, 'B', 8);
  put(e, 'Z', 8);
  put(e, 'h', 8);
  put(e, '0' + e->level, 8);
  e->combined = 0;
  e->in_stream = 1;
}

static void end_stream(bz_enc_t * e) {
  put48(e, BZIP2_END_MAGIC);
  put(e, e->combined, 32);
  put_pad(e);
  e->in_stream = 0;
  e->streams++;
}

static void drain(bz_enc_t * e, gcomp_buffer_t * out) {
  size_t avail = e->stage_len - e->stage_pos;
  size_t room = out->size - out->used;
  size_t n = avail < room ? avail : room;
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->stage + e->stage_pos, n);
    out->used += n;
    e->stage_pos += n;
  }
  if (e->stage_pos == e->stage_len) {
    e->stage_pos = 0;
    e->stage_len = 0;
  }
}

gcomp_status_t bzip2_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  bz_enc_t * e;
  int64_t level = 9;
  size_t n;
  /* The range is the schema's: gcomp_encoder_create() has validated it. */
  if (options) {
    (void)gcomp_options_get_int64(options, "bzip2.level", &level);
  }
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "bzip2: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->level = (unsigned)level;
  e->block_max = e->level * BZIP2_BLOCK_UNIT - 19u;
  n = e->block_max;
  /* One block's worst case: at most n + 1 symbols of at most 17 bits, and the
   * tables and selectors, which together are under 25 KB. */
  e->stage_cap = n * 9u / 4u + 65536u;
  e->blk = gcomp_malloc(alloc, n + 8u);
  e->last = gcomp_malloc(alloc, n + 8u);
  e->mtfv = gcomp_malloc(alloc, (n + 8u) * sizeof(uint16_t));
  e->bwt.s = gcomp_malloc(alloc, (2u * n + 8u) * sizeof(int32_t));
  e->bwt.sa = gcomp_malloc(alloc, (2u * n + 8u) * sizeof(int32_t));
  e->stage = gcomp_malloc(alloc, e->stage_cap);
  if (!e->blk || !e->last || !e->mtfv || !e->bwt.s || !e->bwt.sa || !e->stage) {
    gcomp_free(alloc, e->blk);
    gcomp_free(alloc, e->last);
    gcomp_free(alloc, e->mtfv);
    gcomp_free(alloc, e->bwt.s);
    gcomp_free(alloc, e->bwt.sa);
    gcomp_free(alloc, e->stage);
    gcomp_free(alloc, e);
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "bzip2: out of memory for the encoder");
  }
  e->block_crc = 0xFFFFFFFFu;
  encoder->method_state = e;
  return GCOMP_OK;
}

void bzip2_encoder_destroy(gcomp_encoder_t * encoder) {
  bz_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  gcomp_free(e->alloc, e->blk);
  gcomp_free(e->alloc, e->last);
  gcomp_free(e->alloc, e->mtfv);
  gcomp_free(e->alloc, e->bwt.s);
  gcomp_free(e->alloc, e->bwt.sa);
  gcomp_free(e->alloc, e->stage);
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

gcomp_status_t bzip2_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  bz_enc_t * e = encoder->method_state;
  const uint8_t * src;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "bzip2: encoder update after finish");
  }
  src = (const uint8_t *)input->data;
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->stage_len != 0) {
      return GCOMP_OK;
    }
    if (input->used >= input->size) {
      return GCOMP_OK;
    }
    if (!e->in_stream) {
      begin_stream(e);
    }
    /* Fill the block; a run adds at most five bytes, and the block keeps 19
     * back for that. */
    while (input->used < input->size && e->nblock < e->block_max) {
      add_byte(e, src[input->used++]);
    }
    if (e->nblock >= e->block_max) {
      s = emit_block(e);
      if (s != GCOMP_OK) {
        return s;
      }
    }
  }
}

/* End the current stream, if one is open, with whatever is pending. */
static gcomp_status_t close_stream(bz_enc_t * e) {
  gcomp_status_t s;
  if (!e->in_stream) {
    return GCOMP_OK;
  }
  flush_run(e);
  s = emit_block(e);
  if (s != GCOMP_OK) {
    return s;
  }
  end_stream(e);
  return GCOMP_OK;
}

gcomp_status_t bzip2_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  bz_enc_t * e = encoder->method_state;
  gcomp_status_t s;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  drain(e, output);
  if (e->stage_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  if (e->finished) {
    return GCOMP_OK;
  }
  if (!e->in_stream && e->streams == 0) {
    /* No input at all is still a stream: the header and the end. Input that a
     * flush has already written out is not owed another. */
    begin_stream(e);
  }
  s = close_stream(e);
  if (s != GCOMP_OK) {
    return s;
  }
  e->finished = 1;
  drain(e, output);
  return e->stage_len != 0 ? GCOMP_ERR_LIMIT : GCOMP_OK;
}

gcomp_status_t bzip2_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  bz_enc_t * e = encoder->method_state;
  gcomp_status_t s;
  (void)mode;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "bzip2: encoder cannot flush after finish");
  }
  drain(e, output);
  if (e->stage_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  s = close_stream(e);
  if (s != GCOMP_OK) {
    return s;
  }
  drain(e, output);
  return e->stage_len != 0 ? GCOMP_ERR_LIMIT : GCOMP_OK;
}

gcomp_status_t bzip2_encoder_reset(gcomp_encoder_t * encoder) {
  bz_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  e = encoder->method_state;
  e->nblock = 0;
  e->block_crc = 0xFFFFFFFFu;
  e->run_len = 0;
  e->combined = 0;
  e->in_stream = 0;
  e->streams = 0;
  e->finished = 0;
  e->stage_len = 0;
  e->stage_pos = 0;
  e->acc = 0;
  e->nacc = 0;
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
