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
 * @file implode.c
 *
 * Implode (ZIP method 6), after PKWARE's APPNOTE section 5.3: a sliding
 * dictionary of 4 or 8 KiB whose output is coded with Shannon-Fano trees
 * stored at the start: one for the literals (optional), one for the lengths
 * and one for the high six bits of the distances. The low bits of a distance
 * (six or seven, by the dictionary) are sent as they are.
 *
 * Both directions read and write codes the way the APPNOTE builds them from
 * the stored bit lengths; any prefix code is a valid tree for the encoder, so
 * it uses Huffman lengths, which are the shortest there are.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/alloc_internal.h"
#include "../../core/bound_internal.h"
#include "../../core/huffman_lengths.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/implode.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <string.h>

#define IM_WINDOW 8192u
#define IM_MAX_LEN_SYM 63u

/* ---- the trees --------------------------------------------------------------- */

/* The code for each symbol from its bit length, by the APPNOTE's construction:
 * the lengths sorted ascending (ties in file order), codes handed out from the
 * longest down as sixteen-bit fractions, then reversed so the first bit read is
 * the least significant. codes[i] is the reversed code of symbol i. */
static void im_codes(const uint8_t * lens, unsigned n, uint16_t * codes) {
  unsigned order[256], i, j;
  uint32_t code = 0, inc = 0;
  unsigned last = 0;
  for (i = 0; i < n; i++) {
    order[i] = i;
  }
  /* stable insertion sort by length */
  for (i = 1; i < n; i++) {
    const unsigned s = order[i];
    for (j = i; j > 0 && lens[order[j - 1]] > lens[s]; j--) {
      order[j] = order[j - 1];
    }
    order[j] = s;
  }
  for (i = n; i-- > 0;) {
    const unsigned s = order[i];
    uint32_t r = 0, c;
    unsigned k;
    code += inc;
    if (lens[s] != last) {
      last = lens[s];
      inc = 1u << (16u - last);
    }
    c = code & 0xFFFFu;
    for (k = 0; k < 16; k++) {
      r = (r << 1) | ((c >> k) & 1u);
    }
    codes[s] = (uint16_t)r;
  }
}

/* ---- the decoder ---------------------------------------------------------------- */

typedef struct {
  uint16_t * table; /* by the next maxlen bits: symbol << 5 | length, 0 = none */
  unsigned maxlen, n;
  uint8_t lens[256];
} im_tree_t;

typedef enum {
  D_TREE_COUNT,
  D_TREE_BYTES,
  D_FLAG,
  D_LIT,
  D_DIST_LOW,
  D_DIST_HIGH,
  D_LEN,
  D_LEN_EXTRA,
  D_COPY,
  D_DONE,
  D_FAILED
} im_stage_t;

typedef struct {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  uint64_t max_out, max_ratio, size;
  unsigned dict_bits;   /* 6 or 7: the low bits of a distance */
  int literal_tree;
  unsigned min_len;
  uint64_t bb;
  unsigned bn;
  im_stage_t stage;
  gcomp_status_t err;
  unsigned tree_i, tree_bytes, tree_have;
  uint8_t tree_buf[256];
  im_tree_t trees[3]; /* literal, length, distance */
  unsigned dist_low, dist, len;
  uint8_t * win;
  uint64_t produced, in_total;
} im_dec_t;

#define DFAIL(st, status, ...)                                                 \
  ((st)->stage = D_FAILED,                                                     \
      (st)->err = gcomp_decoder_set_error((st)->pub, (status), __VA_ARGS__))

static void dec_free_trees(im_dec_t * st) {
  unsigned i;
  for (i = 0; i < 3; i++) {
    gcomp_free(st->alloc, st->trees[i].table);
    st->trees[i].table = NULL;
  }
}

static void dec_restart(im_dec_t * st) {
  dec_free_trees(st);
  st->bb = 0;
  st->bn = 0;
  st->stage = D_TREE_COUNT;
  st->err = GCOMP_OK;
  st->tree_i = st->literal_tree ? 0u : 1u;
  st->tree_have = 0;
  memset(st->win, 0, IM_WINDOW);
  st->produced = st->in_total = 0;
}

/* Build the decoding table from the lengths. 0 on success. */
static int tree_build(im_dec_t * st, im_tree_t * t) {
  uint16_t codes[256];
  unsigned i, maxlen = 0;
  uint64_t kraft = 0;
  for (i = 0; i < t->n; i++) {
    if (t->lens[i] > maxlen) {
      maxlen = t->lens[i];
    }
    kraft += (uint64_t)1 << (16u - t->lens[i]);
  }
  if (kraft > ((uint64_t)1 << 16)) {
    return -1; /* more codes than there are bit strings */
  }
  im_codes(t->lens, t->n, codes);
  t->maxlen = maxlen;
  t->table = gcomp_calloc(st->alloc, (size_t)1 << maxlen, sizeof(uint16_t));
  if (!t->table) {
    return -2;
  }
  for (i = 0; i < t->n; i++) {
    const unsigned len = t->lens[i];
    unsigned k;
    for (k = codes[i] & ((1u << len) - 1u); k < (1u << maxlen); k += 1u << len) {
      t->table[k] = (uint16_t)((i << 5) | len);
    }
  }
  return 0;
}

/* Take n bits (n <= bn). */
static unsigned im_take(im_dec_t * st, unsigned n) {
  const unsigned v = (unsigned)(st->bb & (((uint64_t)1 << n) - 1u));
  st->bb >>= n;
  st->bn -= n;
  return v;
}

/* One symbol from a tree: 1 and *sym; 0 if more bits are needed; -1 if the bits
 * are no code. */
static int tree_decode(im_dec_t * st, const im_tree_t * t, unsigned * sym) {
  const unsigned e = t->table[st->bb & (((uint64_t)1 << t->maxlen) - 1u)];
  if (e == 0) {
    return st->bn >= t->maxlen ? -1 : 0;
  }
  if ((e & 31u) > st->bn) {
    return 0;
  }
  st->bb >>= e & 31u;
  st->bn -= e & 31u;
  *sym = e >> 5;
  return 1;
}

static gcomp_status_t im_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  im_dec_t * st;
  uint64_t dict = 8192, size = 0, max_mem;
  int lit = 1;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_uint64(options, "implode.dictionary_size", &dict);
    (void)gcomp_options_get_uint64(options, "implode.uncompressed_size", &size);
    (void)gcomp_options_get_bool(options, "implode.literal_tree", &lit);
  }
  if (dict != 4096 && dict != 8192) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_INVALID_ARG,
        "implode.dictionary_size must be 4096 or 8192");
  }
  alloc = gcomp_registry_get_allocator(registry);
  max_mem = gcomp_limits_read_memory_max(options, 0);
  if (gcomp_limits_check_memory(
          sizeof(*st) + IM_WINDOW + 3u * 131072u, max_mem) != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "implode: the tables exceed limits.max_memory_bytes");
  }
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "implode: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->win = gcomp_calloc(alloc, 1, IM_WINDOW);
  if (!st->win) {
    gcomp_free(alloc, st);
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "implode: out of memory");
  }
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_IMPLODE_MAX_EXPANSION_RATIO);
  st->size = size;
  st->dict_bits = dict == 8192 ? 7u : 6u;
  st->literal_tree = lit != 0;
  st->min_len = lit ? 3u : 2u;
  decoder->method_state = st;
  dec_restart(st);
  return GCOMP_OK;
}

static void im_decoder_destroy(gcomp_decoder_t * decoder) {
  im_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  dec_free_trees(st);
  gcomp_free(st->alloc, st->win);
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

static gcomp_status_t im_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  dec_restart(decoder->method_state);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

typedef struct {
  const uint8_t * in;
  const uint8_t * end;
  gcomp_buffer_t * out;
} im_io_t;

static void im_refill(im_dec_t * st, im_io_t * io) {
  while (st->bn <= 56u && io->in < io->end) {
    st->bb |= (uint64_t)*io->in++ << st->bn;
    st->bn += 8;
    st->in_total++;
  }
}

static void im_emit(im_dec_t * st, im_io_t * io, uint8_t b) {
  ((uint8_t *)io->out->data)[io->out->used++] = b;
  st->win[st->produced & (IM_WINDOW - 1u)] = b;
  st->produced++;
}

/* Room for another byte: 1, 0 if the caller's buffer is full, -1 at the limit. */
static int im_room(const im_dec_t * st, const im_io_t * io) {
  if (st->max_out != 0 && st->produced >= st->max_out) {
    return -1;
  }
  return io->out->used < io->out->size;
}

typedef enum { R_NEED_INPUT, R_NEED_OUTPUT, R_DONE, R_ERROR } im_run_t;

static im_run_t im_run(im_dec_t * st, im_io_t * io) {
  for (;;) {
    int r;
    unsigned sym;
    im_refill(st, io);
    switch (st->stage) {
    case D_DONE:
      return R_DONE;
    case D_FAILED:
      return R_ERROR;
    case D_TREE_COUNT:
      if (st->tree_i == 3) {
        st->stage = D_FLAG;
        break;
      }
      if (st->bn < 8) {
        return R_NEED_INPUT;
      }
      st->tree_bytes = im_take(st, 8) + 1u;
      st->tree_have = 0;
      st->stage = D_TREE_BYTES;
      break;
    case D_TREE_BYTES:
      while (st->tree_have < st->tree_bytes) {
        im_refill(st, io);
        if (st->bn < 8) {
          return R_NEED_INPUT;
        }
        st->tree_buf[st->tree_have++] = (uint8_t)im_take(st, 8);
      }
      {
        im_tree_t * t = &st->trees[st->tree_i];
        const unsigned want = st->tree_i == 0 ? 256u : 64u;
        unsigned i, n = 0;
        for (i = 0; i < st->tree_bytes; i++) {
          const unsigned count = (st->tree_buf[i] >> 4) + 1u;
          const unsigned len = (st->tree_buf[i] & 15u) + 1u;
          unsigned k;
          for (k = 0; k < count && n < 256; k++) {
            t->lens[n++] = (uint8_t)len;
          }
        }
        if (n != want) {
          DFAIL(st, GCOMP_ERR_CORRUPT,
              "implode: a tree describes %u codes, not %u", n, want);
          return R_ERROR;
        }
        t->n = want;
        r = tree_build(st, t);
        if (r != 0) {
          if (r == -2) {
            DFAIL(st, GCOMP_ERR_MEMORY, "implode: out of memory");
          }
          else {
            DFAIL(st, GCOMP_ERR_CORRUPT,
                "implode: a tree has more codes than there are bit strings");
          }
          return R_ERROR;
        }
      }
      st->tree_i++;
      st->stage = D_TREE_COUNT;
      break;
    case D_FLAG:
      if (st->size != 0 && st->produced >= st->size) {
        st->stage = D_DONE;
        break;
      }
      if (st->bn < 1) {
        return R_NEED_INPUT;
      }
      r = im_room(st, io);
      if (r < 0) {
        DFAIL(st, GCOMP_ERR_LIMIT,
            "implode: decompressed output exceeds the limit");
        return R_ERROR;
      }
      if (r == 0) {
        return R_NEED_OUTPUT;
      }
      st->stage = (im_take(st, 1) != 0) ? D_LIT : D_DIST_LOW;
      break;
    case D_LIT:
      if (st->literal_tree) {
        r = tree_decode(st, &st->trees[0], &sym);
        if (r == 0) {
          return R_NEED_INPUT;
        }
        if (r < 0) {
          DFAIL(st, GCOMP_ERR_CORRUPT, "implode: an invalid literal code");
          return R_ERROR;
        }
      }
      else {
        if (st->bn < 8) {
          return R_NEED_INPUT;
        }
        sym = im_take(st, 8);
      }
      im_emit(st, io, (uint8_t)sym);
      st->stage = D_FLAG;
      break;
    case D_DIST_LOW:
      if (st->bn < st->dict_bits) {
        return R_NEED_INPUT;
      }
      st->dist_low = im_take(st, st->dict_bits);
      st->stage = D_DIST_HIGH;
      break;
    case D_DIST_HIGH:
      r = tree_decode(st, &st->trees[2], &sym);
      if (r == 0) {
        return R_NEED_INPUT;
      }
      if (r < 0) {
        DFAIL(st, GCOMP_ERR_CORRUPT, "implode: an invalid distance code");
        return R_ERROR;
      }
      st->dist = ((sym << st->dict_bits) | st->dist_low) + 1u;
      st->stage = D_LEN;
      break;
    case D_LEN:
      r = tree_decode(st, &st->trees[1], &sym);
      if (r == 0) {
        return R_NEED_INPUT;
      }
      if (r < 0) {
        DFAIL(st, GCOMP_ERR_CORRUPT, "implode: an invalid length code");
        return R_ERROR;
      }
      st->len = sym + st->min_len;
      st->stage = sym == IM_MAX_LEN_SYM ? D_LEN_EXTRA : D_COPY;
      break;
    case D_LEN_EXTRA:
      if (st->bn < 8) {
        return R_NEED_INPUT;
      }
      st->len += im_take(st, 8);
      st->stage = D_COPY;
      break;
    case D_COPY:
      while (st->len != 0) {
        if (st->size != 0 && st->produced >= st->size) {
          /* the member ends inside a match: that is the end */
          st->len = 0;
          break;
        }
        r = im_room(st, io);
        if (r < 0) {
          DFAIL(st, GCOMP_ERR_LIMIT,
              "implode: decompressed output exceeds the limit");
          return R_ERROR;
        }
        if (r == 0) {
          return R_NEED_OUTPUT;
        }
        im_emit(st, io, st->win[(st->produced - st->dist) & (IM_WINDOW - 1u)]);
        st->len--;
      }
      st->stage = D_FLAG;
      break;
    }
  }
}

static gcomp_status_t im_step(im_dec_t * st, im_io_t * io, im_run_t * r) {
  *r = im_run(st, io);
  return *r == R_ERROR ? st->err : GCOMP_OK;
}

static gcomp_status_t im_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  im_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  im_io_t io;
  im_run_t r;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->stage == D_FAILED) {
    return st->err;
  }
  if (input->data) {
    io.in = (const uint8_t *)input->data + input->used;
    io.end = (const uint8_t *)input->data + input->size;
  }
  else {
    io.in = io.end = &none;
  }
  io.out = output;
  s = im_step(st, &io, &r);
  if (input->data) {
    input->used = (size_t)(io.in - (const uint8_t *)input->data);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  if (gcomp_limits_check_expansion_ratio(st->in_total, st->produced,
          st->max_ratio) != GCOMP_OK) {
    return DFAIL(
        st, GCOMP_ERR_LIMIT, "implode: expansion ratio exceeds the limit");
  }
  return GCOMP_OK;
}

static gcomp_status_t im_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  im_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  im_io_t io;
  im_run_t r;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->stage == D_FAILED) {
    return st->err;
  }
  io.in = io.end = &none;
  io.out = output;
  s = im_step(st, &io, &r);
  if (s != GCOMP_OK) {
    return s;
  }
  if (r == R_NEED_OUTPUT) {
    return GCOMP_ERR_LIMIT;
  }
  if (st->size != 0) {
    if (st->produced == st->size && (st->stage == D_DONE || st->stage == D_FLAG)) {
      return GCOMP_OK;
    }
    return DFAIL(st, GCOMP_ERR_CORRUPT, "implode: truncated stream");
  }
  /* no size was given: the stream ends where its input does, mid-symbol or
   * not; what is left over is the padding to the byte */
  if (st->stage == D_FLAG || st->stage == D_DONE || st->bn < 8) {
    if (st->stage == D_FLAG || st->stage == D_DONE || st->stage == D_LIT ||
        st->stage == D_DIST_LOW || st->stage == D_DIST_HIGH ||
        st->stage == D_LEN) {
      return GCOMP_OK;
    }
  }
  return DFAIL(st, GCOMP_ERR_CORRUPT, "implode: truncated stream");
}

/* ---- the encoder ----------------------------------------------------------------- */

typedef struct {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  unsigned dict_bits, window, min_len;
  int literal_tree;
  uint8_t * in;
  size_t in_len, in_cap;
  uint8_t * out;
  size_t out_len, out_cap, out_pos;
  uint64_t acc;
  unsigned nacc;
  int finished, built, oom;
} im_enc_t;

static void enc_restart(im_enc_t * e) {
  e->in_len = 0;
  e->out_len = e->out_pos = 0;
  e->acc = 0;
  e->nacc = 0;
  e->finished = e->built = e->oom = 0;
}

static int enc_reserve(im_enc_t * e, size_t extra) {
  if (e->out_len + extra > e->out_cap) {
    size_t cap = e->out_cap ? e->out_cap : 4096;
    uint8_t * p;
    while (cap < e->out_len + extra) {
      cap *= 2;
    }
    p = gcomp_realloc(e->alloc, e->out, cap);
    if (!p) {
      e->oom = 1;
      return 0;
    }
    e->out = p;
    e->out_cap = cap;
  }
  return 1;
}

static void enc_put(im_enc_t * e, uint32_t v, unsigned bits) {
  if (!enc_reserve(e, 8)) {
    return;
  }
  e->acc |= (uint64_t)v << e->nacc;
  e->nacc += bits;
  while (e->nacc >= 8) {
    e->out[e->out_len++] = (uint8_t)e->acc;
    e->acc >>= 8;
    e->nacc -= 8;
  }
}

static gcomp_status_t im_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc;
  im_enc_t * e;
  uint64_t dict = 8192;
  int lit = 1;
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_uint64(options, "implode.dictionary_size", &dict);
    (void)gcomp_options_get_bool(options, "implode.literal_tree", &lit);
  }
  if (dict != 4096 && dict != 8192) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "implode.dictionary_size must be 4096 or 8192");
  }
  alloc = gcomp_registry_get_allocator(registry);
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "implode: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->window = (unsigned)dict;
  e->dict_bits = dict == 8192 ? 7u : 6u;
  e->literal_tree = lit != 0;
  e->min_len = lit ? 3u : 2u;
  enc_restart(e);
  encoder->method_state = e;
  return GCOMP_OK;
}

static void im_encoder_destroy(gcomp_encoder_t * encoder) {
  im_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  gcomp_free(e->alloc, e->in);
  gcomp_free(e->alloc, e->out);
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

static gcomp_status_t im_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  enc_restart(encoder->method_state);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

typedef struct {
  uint16_t len; /* 0 for a literal */
  uint16_t dist;
  uint8_t lit;
} im_tok_t;

/* Write a tree's lengths as the APPNOTE stores them: runs of equal lengths in
 * symbol order, a byte each (count - 1 in the high nibble, length - 1 in the
 * low), the first byte being how many bytes follow, less one. */
static void enc_tree(im_enc_t * e, const uint8_t * lens, unsigned n) {
  uint8_t runs[256];
  unsigned i = 0, nr = 0, k;
  while (i < n) {
    unsigned run = 1;
    while (i + run < n && lens[i + run] == lens[i] && run < 16) {
      run++;
    }
    runs[nr++] = (uint8_t)(((run - 1u) << 4) | (lens[i] - 1u));
    i += run;
  }
  enc_put(e, nr - 1u, 8);
  for (k = 0; k < nr; k++) {
    enc_put(e, runs[k], 8);
  }
}

/* The sliding dictionary's output for the whole input, greedily. */
static im_tok_t * enc_tokens(im_enc_t * e, size_t * ntok_out) {
  const size_t n = e->in_len;
  im_tok_t * tok = gcomp_malloc(e->alloc, (n + 1) * sizeof(*tok));
  int32_t * head = gcomp_calloc(e->alloc, 65536u, sizeof(int32_t));
  int32_t * prev = gcomp_calloc(e->alloc, n + 1, sizeof(int32_t));
  const unsigned maxlen = IM_MAX_LEN_SYM + e->min_len + 255u;
  size_t pos = 0, nt = 0;
  if (!tok || !head || !prev) {
    gcomp_free(e->alloc, tok);
    gcomp_free(e->alloc, head);
    gcomp_free(e->alloc, prev);
    return NULL;
  }
  while (pos < n) {
    unsigned best = 0, bestd = 0, chain = 64;
    const size_t left = n - pos;
    if (left >= e->min_len) {
      const unsigned h = e->in[pos] | ((unsigned)e->in[pos + 1] << 8);
      int32_t cand = head[h];
      const unsigned lim = left > maxlen ? maxlen : (unsigned)left;
      while (cand != 0 && chain-- != 0) {
        const size_t c = (size_t)cand - 1u;
        const size_t d = pos - c;
        unsigned l = 0;
        if (d > e->window) {
          break;
        }
        while (l < lim && e->in[c + l] == e->in[pos + l]) {
          l++;
        }
        if (l > best) {
          best = l;
          bestd = (unsigned)d;
          if (l == lim) {
            break;
          }
        }
        cand = prev[c];
      }
    }
    if (best >= e->min_len) {
      size_t k;
      tok[nt].len = (uint16_t)best;
      tok[nt].dist = (uint16_t)bestd;
      nt++;
      for (k = 0; k < best; k++) {
        if (pos + k + 1 < n) {
          const unsigned h = e->in[pos + k] | ((unsigned)e->in[pos + k + 1] << 8);
          prev[pos + k] = head[h];
          head[h] = (int32_t)(pos + k) + 1;
        }
      }
      pos += best;
    }
    else {
      tok[nt].len = 0;
      tok[nt].lit = e->in[pos];
      nt++;
      if (pos + 1 < n) {
        const unsigned h = e->in[pos] | ((unsigned)e->in[pos + 1] << 8);
        prev[pos] = head[h];
        head[h] = (int32_t)pos + 1;
      }
      pos++;
    }
  }
  gcomp_free(e->alloc, head);
  gcomp_free(e->alloc, prev);
  *ntok_out = nt;
  return tok;
}

static gcomp_status_t enc_build(im_enc_t * e) {
  size_t ntok = 0, i;
  im_tok_t * tok = enc_tokens(e, &ntok);
  uint32_t lf[256], nf[64], df[64];
  uint8_t ll[256], nl[64], dl[64];
  uint16_t lc[256], nc[64], dc[64];
  const unsigned lowmask = (1u << e->dict_bits) - 1u;
  if (!tok) {
    return GCOMP_ERR_MEMORY;
  }
  for (i = 0; i < 256; i++) {
    lf[i] = 1;
  }
  for (i = 0; i < 64; i++) {
    nf[i] = df[i] = 1;
  }
  for (i = 0; i < ntok; i++) {
    if (tok[i].len == 0) {
      lf[tok[i].lit]++;
    }
    else {
      const unsigned sym = tok[i].len - e->min_len;
      nf[sym > IM_MAX_LEN_SYM ? IM_MAX_LEN_SYM : sym]++;
      df[(tok[i].dist - 1u) >> e->dict_bits]++;
    }
  }
  {
    gcomp_status_t hs = gcomp_huffman_code_lengths(e->alloc, lf, 256, 16, ll);
    if (hs == GCOMP_OK) {
      hs = gcomp_huffman_code_lengths(e->alloc, nf, 64, 16, nl);
    }
    if (hs == GCOMP_OK) {
      hs = gcomp_huffman_code_lengths(e->alloc, df, 64, 16, dl);
    }
    if (hs != GCOMP_OK) {
      gcomp_free(e->alloc, tok);
      return hs;
    }
  }
  im_codes(ll, 256, lc);
  im_codes(nl, 64, nc);
  im_codes(dl, 64, dc);
  if (e->literal_tree) {
    enc_tree(e, ll, 256);
  }
  enc_tree(e, nl, 64);
  enc_tree(e, dl, 64);
  for (i = 0; i < ntok; i++) {
    if (tok[i].len == 0) {
      enc_put(e, 1, 1);
      if (e->literal_tree) {
        enc_put(e, lc[tok[i].lit], ll[tok[i].lit]);
      }
      else {
        enc_put(e, tok[i].lit, 8);
      }
    }
    else {
      const unsigned d = tok[i].dist - 1u;
      unsigned sym = tok[i].len - e->min_len;
      enc_put(e, 0, 1);
      enc_put(e, d & lowmask, e->dict_bits);
      enc_put(e, dc[d >> e->dict_bits], dl[d >> e->dict_bits]);
      if (sym >= IM_MAX_LEN_SYM) {
        enc_put(e, nc[IM_MAX_LEN_SYM], nl[IM_MAX_LEN_SYM]);
        enc_put(e, sym - IM_MAX_LEN_SYM, 8);
      }
      else {
        enc_put(e, nc[sym], nl[sym]);
      }
    }
  }
  if (e->nacc != 0) {
    enc_put(e, 0, 8u - e->nacc);
  }
  gcomp_free(e->alloc, tok);
  return e->oom ? GCOMP_ERR_MEMORY : GCOMP_OK;
}

static gcomp_status_t im_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  im_enc_t * e = encoder->method_state;
  size_t n;
  (void)output;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished || e->built) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "implode: encoder update after finish");
  }
  n = input->size - input->used;
  if (n == 0) {
    return GCOMP_OK;
  }
  if (e->in_len + n > e->in_cap) {
    size_t cap = e->in_cap ? e->in_cap : 65536;
    uint8_t * p;
    while (cap < e->in_len + n) {
      cap *= 2;
    }
    p = gcomp_realloc(e->alloc, e->in, cap);
    if (!p) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_MEMORY, "implode: out of memory");
    }
    e->in = p;
    e->in_cap = cap;
  }
  memcpy(e->in + e->in_len, (const uint8_t *)input->data + input->used, n);
  e->in_len += n;
  input->used += n;
  return GCOMP_OK;
}

static gcomp_status_t im_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  im_enc_t * e = encoder->method_state;
  size_t n;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (!e->built) {
    const gcomp_status_t s = enc_build(e);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, s, "implode: could not build the stream");
    }
    e->built = 1;
  }
  n = e->out_len - e->out_pos;
  if (n > output->size - output->used) {
    n = output->size - output->used;
  }
  if (n != 0) {
    memcpy((uint8_t *)output->data + output->used, e->out + e->out_pos, n);
    output->used += n;
    e->out_pos += n;
  }
  if (e->out_pos < e->out_len) {
    return GCOMP_ERR_LIMIT;
  }
  e->finished = 1;
  return GCOMP_OK;
}

/* ---- registration ---------------------------------------------------------------- */

static const gcomp_option_schema_t g_im_option_schemas[] = {
    {"implode.dictionary_size", GCOMP_OPT_UINT64, 1, {.ui64 = 8192}, 0, 0, 0, 0,
        0, 0,
        "The sliding dictionary: 4096 or 8192, as bit 1 of a ZIP member's flags "
        "says.",
        NULL},
    {"implode.literal_tree", GCOMP_OPT_BOOL, 1, {.b = 1}, 0, 0, 0, 0, 0, 0,
        "Whether literals are Shannon-Fano coded (three trees, minimum match 3) "
        "or sent as they are (two trees, minimum match 2): bit 2 of a ZIP "
        "member's flags.",
        NULL},
    {"implode.uncompressed_size", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0,
        0, 0,
        "Where the decoder stops, as the container records it; 0 decodes to the "
        "end of the input.",
        NULL},
    {"limits.max_output_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum decompressed output", NULL},
    {"limits.max_memory_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum memory usage", NULL},
    {"limits.max_expansion_ratio", GCOMP_OPT_UINT64, 1,
        {.ui64 = GCOMP_IMPLODE_MAX_EXPANSION_RATIO}, 0, 0, 0, 0, 0, 0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL},
};

/* Kept in step with the schema above by SchemaAllMethodsTest. */
static const char * const g_im_option_keys[] = {"implode.dictionary_size",
    "implode.literal_tree", "implode.uncompressed_size",
    "limits.max_output_bytes", "limits.max_memory_bytes",
    "limits.max_expansion_ratio"};

static const gcomp_method_schema_t g_im_schema = {g_im_option_schemas,
    sizeof(g_im_option_schemas) / sizeof(g_im_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, g_im_option_keys};

static const gcomp_method_schema_t * im_get_schema(void) {
  return &g_im_schema;
}

static gcomp_status_t im_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = im_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = im_encoder_update;
  (*encoder_out)->finish_fn = im_encoder_finish;
  (*encoder_out)->reset_fn = im_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t im_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = im_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = im_decoder_update;
  (*decoder_out)->finish_fn = im_decoder_finish;
  (*decoder_out)->reset_fn = im_decoder_reset;
  return GCOMP_OK;
}

static void im_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  im_encoder_destroy(encoder);
}

static void im_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  im_decoder_destroy(decoder);
}

/** The trees (at most 3 * 257 bytes) and, worst case, a literal at 17 bits. */
static gcomp_status_t im_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  size_t bound = 0;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = gcomp_bound_add_mul(&bound, input_size, 3u);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 1024u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static const gcomp_method_t g_im_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "implode",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = im_create_encoder,
    .create_decoder = im_create_decoder,
    .destroy_encoder = im_destroy_encoder_wrapper,
    .destroy_decoder = im_destroy_decoder_wrapper,
    .get_schema = im_get_schema,
    .encode_bound = im_encode_bound,
};

gcomp_status_t gcomp_method_implode_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_im_method);
}

GCOMP_AUTOREG_METHOD(implode, gcomp_method_implode_register)
