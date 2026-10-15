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
 * @file deflate64_decode.c
 *
 * The deflate64 decoder: a resumable decoder for the block structure of RFC
 * 1951 with the changes APPNOTE 4.4.5 makes (see deflate64.h). It is written
 * for clarity before speed - a symbol at a time, a match a byte at a time - and
 * is what 7-Zip's and PKWARE's streams are checked against.
 *
 * Input is read into a 64-bit bit buffer as a symbol needs it. When the input
 * runs out part way through a symbol the bits read so far stay in the buffer
 * and the next call carries on. When the stream ends (or the output fills),
 * whole bytes the buffer holds that this call took from the input are handed
 * back, so a container that follows the stream sees where it ended.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "deflate64_internal.h"
#include <ghoti.io/compress/deflate64.h>
#include <ghoti.io/compress/limits.h>
#include <string.h>

#define D64_FAST_BITS 10u

/* A canonical Huffman decoding table: the first D64_FAST_BITS bits index a
 * table of (symbol << 4 | length); a longer code is found by the counts. */
typedef struct {
  uint16_t fast[1u << D64_FAST_BITS];
  uint16_t count[16];
  uint16_t sym[288];
  unsigned codes; /* how many symbols have a code */
} d64_huff_t;

typedef enum {
  S_BLOCK_HEADER,
  S_STORED_LEN,
  S_STORED_COPY,
  S_DYN_COUNTS,
  S_DYN_CODELENS,
  S_DYN_LENGTHS,
  S_DATA,
  S_DONE,
  S_FAILED
} d64_stage_t;

typedef enum { M_LITLEN, M_LEN_EXTRA, M_DIST, M_DIST_EXTRA } d64_sub_t;

typedef struct {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  uint64_t max_out, max_mem, max_ratio;

  uint64_t bb; /* the bit buffer, low bits first */
  unsigned bn; /* how many bits it holds */
  size_t loaded; /* bytes this call took from the input */

  d64_stage_t stage;
  gcomp_status_t err;
  int final;

  uint8_t * win; /* the last D64_WINDOW bytes of output */
  uint64_t produced, in_total;

  /* a stored block */
  uint32_t stored_left;
  /* a dynamic block's header */
  unsigned hlit, hdist, hclen, filled, pend_sym;
  uint8_t lens[286 + 32];
  uint8_t cl_lens[19];
  d64_huff_t * cl;
  /* the tables in use */
  d64_huff_t * lit;
  d64_huff_t * dist;
  d64_huff_t * dyn_lit;
  d64_huff_t * dyn_dist;
  d64_huff_t * fixed_lit;
  d64_huff_t * fixed_dist;
  /* a match in progress */
  d64_sub_t sub;
  unsigned msym;
  uint32_t mlen, mdist, copy_left;
} d64_dec_t;

#define FAIL(st, status, ...)                                                  \
  ((st)->stage = S_FAILED,                                                     \
      (st)->err = gcomp_decoder_set_error((st)->pub, (status), __VA_ARGS__))

/* ---- Huffman tables ------------------------------------------------------ */

static unsigned rev_bits(unsigned code, unsigned len) {
  unsigned r = 0;
  while (len--) {
    r = (r << 1) | (code & 1u);
    code >>= 1;
  }
  return r;
}

/* Build a table from code lengths. 0 on success; -1 if the lengths over-
 * subscribe the code space; -2 if they leave some of it unused, which is
 * allowed only for one code of one bit (the single distance code a block of
 * literals may send) and, when `allow_empty`, for no codes at all. */
static int huff_build(d64_huff_t * h, const uint8_t * lens, unsigned n,
    int allow_empty) {
  unsigned i, len, left = 1, offs[16], next[16];
  unsigned code = 0;
  memset(h, 0, sizeof(*h));
  for (i = 0; i < n; i++) {
    h->count[lens[i]]++;
  }
  h->codes = n - h->count[0];
  h->count[0] = 0;
  for (len = 1; len < 16; len++) {
    left <<= 1;
    if (h->count[len] > left) {
      return -1;
    }
    left -= h->count[len];
  }
  if (left != 0) {
    if (h->codes == 0 ? !allow_empty : !(h->codes == 1 && h->count[1] == 1)) {
      return -2;
    }
  }
  offs[1] = 0;
  for (len = 1; len < 15; len++) {
    offs[len + 1] = offs[len] + h->count[len];
  }
  for (i = 0; i < n; i++) {
    if (lens[i] != 0) {
      h->sym[offs[lens[i]]++] = (uint16_t)i;
    }
  }
  for (len = 1; len < 16; len++) {
    code = (code + h->count[len - 1]) << 1;
    next[len] = code;
  }
  /* The fast table: every code of at most D64_FAST_BITS bits, reversed because
   * the stream sends a code's bits from the most significant down. */
  for (i = 0; i < n; i++) {
    len = lens[i];
    if (len != 0 && len <= D64_FAST_BITS) {
      unsigned c = rev_bits(next[len]++, len);
      unsigned step = 1u << len, k;
      for (k = c; k < (1u << D64_FAST_BITS); k += step) {
        h->fast[k] = (uint16_t)((i << 4) | len);
      }
    }
    else if (len != 0) {
      next[len]++;
    }
  }
  return 0;
}

/* Take n bits (n <= bn). */
static unsigned take(d64_dec_t * st, unsigned n) {
  const unsigned v = (unsigned)(st->bb & (((uint64_t)1 << n) - 1u));
  st->bb >>= n;
  st->bn -= n;
  return v;
}

/* One symbol: 1 and *sym set; 0 if more bits are needed; -1 if the bits are
 * not a code. */
static int huff_decode(d64_dec_t * st, const d64_huff_t * h, unsigned * sym) {
  const unsigned e = h->fast[st->bb & ((1u << D64_FAST_BITS) - 1u)];
  unsigned len, code = 0, first = 0, index = 0, bits;
  if (e != 0) {
    len = e & 15u;
    if (len > st->bn) {
      return 0;
    }
    st->bb >>= len;
    st->bn -= len;
    *sym = e >> 4;
    return 1;
  }
  bits = (unsigned)st->bb;
  for (len = 1; len < 16; len++) {
    unsigned count;
    if (len > st->bn) {
      return 0;
    }
    code |= (bits >> (len - 1)) & 1u;
    count = h->count[len];
    if (code < first + count) {
      st->bb >>= len;
      st->bn -= len;
      *sym = h->sym[index + (code - first)];
      return 1;
    }
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

/* ---- lifecycle ------------------------------------------------------------ */

static void build_fixed(d64_dec_t * st) {
  uint8_t l[288], d[32];
  unsigned i;
  for (i = 0; i < 144; i++) {
    l[i] = 8;
  }
  for (; i < 256; i++) {
    l[i] = 9;
  }
  for (; i < 280; i++) {
    l[i] = 7;
  }
  for (; i < 288; i++) {
    l[i] = 8;
  }
  for (i = 0; i < 32; i++) {
    d[i] = 5;
  }
  (void)huff_build(st->fixed_lit, l, 288, 0);
  (void)huff_build(st->fixed_dist, d, 32, 0);
}

static void restart(d64_dec_t * st) {
  st->bb = 0;
  st->bn = 0;
  st->stage = S_BLOCK_HEADER;
  st->err = GCOMP_OK;
  st->final = 0;
  st->produced = st->in_total = 0;
  st->copy_left = 0;
  st->sub = M_LITLEN;
}

gcomp_status_t d64_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  d64_dec_t * st;
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
        decoder, GCOMP_ERR_MEMORY, "deflate64: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_mem = gcomp_limits_read_memory_max(options, 0);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_DEFLATE64_MAX_EXPANSION_RATIO);
  decoder->method_state = st;
  if (gcomp_limits_check_memory(
          D64_WINDOW + 6u * sizeof(d64_huff_t) + sizeof(*st), st->max_mem) !=
      GCOMP_OK) {
    gcomp_free(alloc, st);
    decoder->method_state = NULL;
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "deflate64: the window exceeds limits.max_memory_bytes");
  }
  st->win = gcomp_calloc(alloc, 1, D64_WINDOW);
  st->cl = gcomp_calloc(alloc, 1, sizeof(d64_huff_t));
  st->dyn_lit = gcomp_calloc(alloc, 1, sizeof(d64_huff_t));
  st->dyn_dist = gcomp_calloc(alloc, 1, sizeof(d64_huff_t));
  st->fixed_lit = gcomp_calloc(alloc, 1, sizeof(d64_huff_t));
  st->fixed_dist = gcomp_calloc(alloc, 1, sizeof(d64_huff_t));
  if (!st->win || !st->cl || !st->dyn_lit || !st->dyn_dist || !st->fixed_lit ||
      !st->fixed_dist) {
    d64_decoder_destroy(decoder);
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "deflate64: out of memory");
  }
  build_fixed(st);
  restart(st);
  return GCOMP_OK;
}

void d64_decoder_destroy(gcomp_decoder_t * decoder) {
  d64_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  gcomp_free(st->alloc, st->win);
  gcomp_free(st->alloc, st->cl);
  gcomp_free(st->alloc, st->dyn_lit);
  gcomp_free(st->alloc, st->dyn_dist);
  gcomp_free(st->alloc, st->fixed_lit);
  gcomp_free(st->alloc, st->fixed_dist);
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

gcomp_status_t d64_decoder_reset(gcomp_decoder_t * decoder) {
  d64_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  restart(st);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* ---- decoding ------------------------------------------------------------- */

typedef struct {
  const uint8_t * in;
  const uint8_t * end;
  uint8_t * out;
  size_t out_size, out_used;
} d64_io_t;

/* Top the bit buffer up from the input. */
static void refill(d64_dec_t * st, d64_io_t * io) {
  while (st->bn <= 56u && io->in < io->end) {
    st->bb |= (uint64_t)*io->in++ << st->bn;
    st->bn += 8;
    st->loaded++;
  }
}

/* Room for one more byte of output, within the limit: 1 yes, 0 the caller's
 * buffer is full, -1 the limit. */
static int out_room(d64_dec_t * st, const d64_io_t * io) {
  if (st->max_out != 0 && st->produced >= st->max_out) {
    return -1;
  }
  return io->out_used < io->out_size;
}

static void emit(d64_dec_t * st, d64_io_t * io, uint8_t b) {
  io->out[io->out_used++] = b;
  st->win[st->produced & D64_WINDOW_MASK] = b;
  st->produced++;
}

/* Carry on a match as far as the output has room. Returns 1 if it finished,
 * 0 if the output is full, -1 on the limit. */
static int do_copy(d64_dec_t * st, d64_io_t * io) {
  while (st->copy_left != 0) {
    const int r = out_room(st, io);
    if (r <= 0) {
      return r;
    }
    emit(st, io, st->win[(st->produced - st->mdist) & D64_WINDOW_MASK]);
    st->copy_left--;
  }
  return 1;
}

typedef enum { R_NEED_INPUT, R_NEED_OUTPUT, R_DONE, R_ERROR } d64_run_t;

static d64_run_t run(d64_dec_t * st, d64_io_t * io) {
  for (;;) {
    int r;
    unsigned sym;
    refill(st, io);
    switch (st->stage) {
    case S_DONE:
      return R_DONE;
    case S_FAILED:
      return R_ERROR;

    case S_BLOCK_HEADER: {
      unsigned v;
      if (st->bn < 3) {
        return R_NEED_INPUT;
      }
      v = take(st, 3);
      st->final = (int)(v & 1u);
      switch (v >> 1) {
      case 0:
        st->stage = S_STORED_LEN;
        break;
      case 1:
        st->lit = st->fixed_lit;
        st->dist = st->fixed_dist;
        st->sub = M_LITLEN;
        st->stage = S_DATA;
        break;
      case 2:
        st->stage = S_DYN_COUNTS;
        break;
      default:
        FAIL(st, GCOMP_ERR_CORRUPT, "deflate64: invalid block type");
        return R_ERROR;
      }
      break;
    }

    case S_STORED_LEN: {
      unsigned len, nlen;
      st->bb >>= st->bn & 7u; /* to a byte boundary */
      st->bn &= ~7u;
      if (st->bn < 32) {
        return R_NEED_INPUT;
      }
      len = take(st, 16);
      nlen = take(st, 16);
      if ((len ^ 0xFFFFu) != nlen) {
        FAIL(st, GCOMP_ERR_CORRUPT,
            "deflate64: a stored block's length does not match its complement");
        return R_ERROR;
      }
      st->stored_left = len;
      st->stage = S_STORED_COPY;
      break;
    }

    case S_STORED_COPY:
      while (st->stored_left != 0) {
        uint8_t b;
        r = out_room(st, io);
        if (r < 0) {
          FAIL(st, GCOMP_ERR_LIMIT,
              "deflate64: decompressed output exceeds the limit");
          return R_ERROR;
        }
        if (r == 0) {
          return R_NEED_OUTPUT;
        }
        if (st->bn >= 8) {
          b = (uint8_t)take(st, 8);
        }
        else {
          return R_NEED_INPUT;
        }
        emit(st, io, b);
        st->stored_left--;
        if (st->bn < 8) {
          refill(st, io);
        }
      }
      st->stage = st->final ? S_DONE : S_BLOCK_HEADER;
      break;

    case S_DYN_COUNTS:
      if (st->bn < 14) {
        return R_NEED_INPUT;
      }
      st->hlit = take(st, 5) + 257u;
      st->hdist = take(st, 5) + 1u;
      st->hclen = take(st, 4) + 4u;
      if (st->hlit > 286u) {
        FAIL(st, GCOMP_ERR_CORRUPT,
            "deflate64: a dynamic block names more than 286 length codes");
        return R_ERROR;
      }
      memset(st->cl_lens, 0, sizeof(st->cl_lens));
      st->filled = 0;
      st->stage = S_DYN_CODELENS;
      break;

    case S_DYN_CODELENS:
      while (st->filled < st->hclen) {
        refill(st, io);
        if (st->bn < 3) {
          return R_NEED_INPUT;
        }
        st->cl_lens[g_d64_cl_order[st->filled++]] = (uint8_t)take(st, 3);
      }
      if (huff_build(st->cl, st->cl_lens, 19, 0) != 0) {
        FAIL(st, GCOMP_ERR_CORRUPT,
            "deflate64: a dynamic block's code length code is not a complete "
            "prefix code");
        return R_ERROR;
      }
      st->filled = 0;
      st->pend_sym = 0;
      st->stage = S_DYN_LENGTHS;
      break;

    case S_DYN_LENGTHS:
      while (st->filled < st->hlit + st->hdist) {
        unsigned rep, val = 0, extra_bits;
        refill(st, io);
        if (st->pend_sym == 0) {
          r = huff_decode(st, st->cl, &sym);
          if (r == 0) {
            return R_NEED_INPUT;
          }
          if (r < 0) {
            FAIL(st, GCOMP_ERR_CORRUPT,
                "deflate64: an invalid code length code");
            return R_ERROR;
          }
          if (sym < 16) {
            st->lens[st->filled++] = (uint8_t)sym;
            continue;
          }
          st->pend_sym = sym;
        }
        sym = st->pend_sym;
        extra_bits = sym == 16 ? 2u : sym == 17 ? 3u : 7u;
        if (st->bn < extra_bits) {
          return R_NEED_INPUT;
        }
        rep = take(st, extra_bits);
        st->pend_sym = 0;
        if (sym == 16) {
          if (st->filled == 0) {
            FAIL(st, GCOMP_ERR_CORRUPT,
                "deflate64: a length repeat with no length before it");
            return R_ERROR;
          }
          val = st->lens[st->filled - 1];
          rep += 3u;
        }
        else {
          rep += sym == 17 ? 3u : 11u;
        }
        if (st->filled + rep > st->hlit + st->hdist) {
          FAIL(st, GCOMP_ERR_CORRUPT,
              "deflate64: the code lengths run past the end of the table");
          return R_ERROR;
        }
        while (rep--) {
          st->lens[st->filled++] = (uint8_t)val;
        }
      }
      if (st->lens[256] == 0) {
        FAIL(st, GCOMP_ERR_CORRUPT,
            "deflate64: a dynamic block has no end-of-block code");
        return R_ERROR;
      }
      if (huff_build(st->dyn_lit, st->lens, st->hlit, 0) != 0 ||
          huff_build(st->dyn_dist, st->lens + st->hlit, st->hdist, 1) != 0) {
        FAIL(st, GCOMP_ERR_CORRUPT,
            "deflate64: a dynamic block's codes are not a valid prefix code");
        return R_ERROR;
      }
      st->lit = st->dyn_lit;
      st->dist = st->dyn_dist;
      st->sub = M_LITLEN;
      st->stage = S_DATA;
      break;

    case S_DATA:
      if (st->copy_left != 0) {
        r = do_copy(st, io);
        if (r < 0) {
          FAIL(st, GCOMP_ERR_LIMIT,
              "deflate64: decompressed output exceeds the limit");
          return R_ERROR;
        }
        if (r == 0) {
          return R_NEED_OUTPUT;
        }
      }
      switch (st->sub) {
      case M_LITLEN: {
        const uint64_t save_bb = st->bb;
        const unsigned save_bn = st->bn;
        r = huff_decode(st, st->lit, &sym);
        if (r == 0) {
          return R_NEED_INPUT;
        }
        if (r < 0) {
          FAIL(st, GCOMP_ERR_CORRUPT, "deflate64: an invalid literal/length code");
          return R_ERROR;
        }
        if (sym != 256) {
          /* A literal or a match writes at least a byte: there has to be room
           * for it, and the symbol is read again once there is. */
          const int room = out_room(st, io);
          if (room <= 0) {
            st->bb = save_bb;
            st->bn = save_bn;
            if (room < 0) {
              FAIL(st, GCOMP_ERR_LIMIT,
                  "deflate64: decompressed output exceeds the limit");
              return R_ERROR;
            }
            return R_NEED_OUTPUT;
          }
        }
        if (sym < 256) {
          emit(st, io, (uint8_t)sym);
        }
        else if (sym == 256) {
          st->stage = st->final ? S_DONE : S_BLOCK_HEADER;
        }
        else if (sym > 285) {
          FAIL(st, GCOMP_ERR_CORRUPT,
              "deflate64: literal/length code %u is not defined", sym);
          return R_ERROR;
        }
        else {
          st->msym = sym - 257u;
          st->mlen = g_d64_len_base[st->msym];
          st->sub = g_d64_len_extra[st->msym] != 0 ? M_LEN_EXTRA : M_DIST;
        }
        break;
      }
      case M_LEN_EXTRA:
        if (st->bn < g_d64_len_extra[st->msym]) {
          return R_NEED_INPUT;
        }
        st->mlen += take(st, g_d64_len_extra[st->msym]);
        st->sub = M_DIST;
        break;
      case M_DIST:
        if (st->dist->codes == 0) {
          FAIL(st, GCOMP_ERR_CORRUPT,
              "deflate64: a match in a block that has no distance codes");
          return R_ERROR;
        }
        r = huff_decode(st, st->dist, &sym);
        if (r == 0) {
          return R_NEED_INPUT;
        }
        if (r < 0 || sym > 31) {
          FAIL(st, GCOMP_ERR_CORRUPT, "deflate64: an invalid distance code");
          return R_ERROR;
        }
        st->msym = sym;
        st->mdist = g_d64_dist_base[sym];
        st->sub = g_d64_dist_extra[sym] != 0 ? M_DIST_EXTRA : M_LITLEN;
        if (st->sub == M_LITLEN) {
          goto start_copy;
        }
        break;
      case M_DIST_EXTRA:
        if (st->bn < g_d64_dist_extra[st->msym]) {
          return R_NEED_INPUT;
        }
        st->mdist += take(st, g_d64_dist_extra[st->msym]);
        st->sub = M_LITLEN;
      start_copy:
        if (st->mdist > st->produced || st->mdist > D64_WINDOW) {
          FAIL(st, GCOMP_ERR_CORRUPT,
              "deflate64: a match reaches back before the start of the output");
          return R_ERROR;
        }
        st->copy_left = st->mlen;
        break;
      }
      break;
    }
  }
}

/* Give back the whole bytes this call took that the stream did not need. */
static void give_back(d64_dec_t * st, d64_io_t * io) {
  size_t n = st->bn / 8u;
  if (n > st->loaded) {
    n = st->loaded;
  }
  io->in -= n;
  st->bn -= (unsigned)(8u * n);
  st->bb &= st->bn ? (((uint64_t)1 << st->bn) - 1u) : 0u;
  st->loaded -= n;
}

static gcomp_status_t step(d64_dec_t * st, d64_io_t * io, d64_run_t * out) {
  const uint8_t * start = io->in;
  d64_run_t r;
  st->loaded = 0;
  r = run(st, io);
  if (r == R_DONE) {
    st->bb >>= st->bn & 7u; /* the padding to the byte */
    st->bn &= ~7u;
    give_back(st, io);
    st->bb = 0;
    st->bn = 0;
  }
  else if (r == R_NEED_OUTPUT) {
    give_back(st, io);
  }
  st->in_total += (uint64_t)(io->in - start);
  *out = r;
  return r == R_ERROR ? st->err : GCOMP_OK;
}

gcomp_status_t d64_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  d64_dec_t * st = decoder->method_state;
  d64_io_t io;
  d64_run_t r;
  gcomp_status_t s;
  static const uint8_t none = 0;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->stage == S_FAILED) {
    return st->err;
  }
  if (input->data) {
    io.in = (const uint8_t *)input->data + input->used;
    io.end = (const uint8_t *)input->data + input->size;
  }
  else {
    io.in = io.end = &none;
  }
  io.out = (uint8_t *)output->data;
  io.out_size = output->size;
  io.out_used = output->used;
  s = step(st, &io, &r);
  if (input->data) {
    input->used = (size_t)(io.in - (const uint8_t *)input->data);
  }
  output->used = io.out_used;
  if (s != GCOMP_OK) {
    return s;
  }
  if (gcomp_limits_check_expansion_ratio(st->in_total, st->produced,
          st->max_ratio) != GCOMP_OK) {
    return FAIL(st, GCOMP_ERR_LIMIT, "deflate64: expansion ratio exceeds the limit");
  }
  return GCOMP_OK;
}

gcomp_status_t d64_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  d64_dec_t * st = decoder->method_state;
  d64_io_t io;
  d64_run_t r;
  gcomp_status_t s;
  static const uint8_t none = 0;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->stage == S_FAILED) {
    return st->err;
  }
  io.in = io.end = &none;
  io.out = (uint8_t *)output->data;
  io.out_size = output->size;
  io.out_used = output->used;
  s = step(st, &io, &r);
  output->used = io.out_used;
  if (s != GCOMP_OK) {
    return s;
  }
  if (r == R_DONE) {
    return GCOMP_OK;
  }
  if (r == R_NEED_OUTPUT) {
    return GCOMP_ERR_LIMIT;
  }
  return FAIL(st, GCOMP_ERR_CORRUPT, "deflate64: truncated stream");
}
