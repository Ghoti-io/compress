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
 * @file reduce.c
 *
 * Reduce (ZIP methods 2 to 5), after PKWARE's APPNOTE section 5.2. Two layers,
 * the outer one read first: a bit stream of "follower sets" (for each byte
 * value, up to 32 bytes that may follow it) and then bytes each coded as a flag
 * and an index into the set of the byte before, or as eight bits; and, in the
 * bytes that come out of that, a marker byte (144) that introduces a repeat -
 * a length and a distance packed into one to three bytes by the compression
 * factor - or, followed by a zero, stands for itself.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/alloc_internal.h"
#include "../../core/bound_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/reduce.h>
#include <ghoti.io/compress/registry.h>
#include <string.h>

#define RD_DLE 144u
#define RD_WINDOW 4096u
#define RD_MAX_SET 32u

/* The bits that name one of n followers: those that hold n - 1, and never
 * fewer than one - the value 0 still takes a bit to write, so a set of one
 * member still costs a bit of index. (Read the other way, as no bits, this
 * decodes the first few bytes of a real file and then loses its place.) */
static unsigned set_bits(unsigned n) {
  unsigned b = 1;
  while ((1u << b) < n) {
    b++;
  }
  return b;
}

/* ---- the decoder ------------------------------------------------------------------ */

typedef enum { D_SETS, D_DATA, D_DONE, D_FAILED } rd_stage_t;
typedef enum { X_NORMAL, X_DLE, X_LEN, X_DIST } rd_x_t;

typedef struct {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  uint64_t max_out, max_ratio, size;
  unsigned factor, shift, mask;
  uint64_t bb;
  unsigned bn;
  rd_stage_t stage;
  gcomp_status_t err;
  int set_j;      /* the set being read, 255 down to 0 */
  int set_have_n; /* its size has been read */
  unsigned set_pos;
  uint8_t nset[256];
  uint8_t set[256][RD_MAX_SET];
  unsigned last;
  rd_x_t x;
  unsigned v, len, dist, copy_left;
  uint8_t * win;
  uint64_t produced, in_total;
} rd_dec_t;

#define DFAIL(st, status, ...)                                                 \
  ((st)->stage = D_FAILED,                                                     \
      (st)->err = gcomp_decoder_set_error((st)->pub, (status), __VA_ARGS__))

static void dec_restart(rd_dec_t * st) {
  st->bb = 0;
  st->bn = 0;
  st->stage = D_SETS;
  st->err = GCOMP_OK;
  st->set_j = 255;
  st->set_have_n = 0;
  st->set_pos = 0;
  memset(st->nset, 0, sizeof(st->nset));
  st->last = 0;
  st->x = X_NORMAL;
  st->copy_left = 0;
  memset(st->win, 0, RD_WINDOW);
  st->produced = st->in_total = 0;
}

static gcomp_status_t rd_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  rd_dec_t * st;
  int64_t factor = 4;
  uint64_t size = 0, max_mem;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_int64(options, "reduce.factor", &factor);
    (void)gcomp_options_get_uint64(options, "reduce.uncompressed_size", &size);
  }
  if (factor < 1 || factor > 4) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "reduce.factor must be 1 to 4");
  }
  alloc = gcomp_registry_get_allocator(registry);
  max_mem = gcomp_limits_read_memory_max(options, 0);
  if (gcomp_limits_check_memory(sizeof(*st) + RD_WINDOW, max_mem) != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "reduce: the tables exceed limits.max_memory_bytes");
  }
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "reduce: out of memory");
  }
  st->win = gcomp_calloc(alloc, 1, RD_WINDOW);
  if (!st->win) {
    gcomp_free(alloc, st);
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "reduce: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_REDUCE_MAX_EXPANSION_RATIO);
  st->size = size;
  st->factor = (unsigned)factor;
  st->shift = 8u - (unsigned)factor;
  st->mask = (1u << st->shift) - 1u;
  decoder->method_state = st;
  dec_restart(st);
  return GCOMP_OK;
}

static void rd_decoder_destroy(gcomp_decoder_t * decoder) {
  rd_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  gcomp_free(st->alloc, st->win);
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

static gcomp_status_t rd_decoder_reset(gcomp_decoder_t * decoder) {
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
} rd_io_t;

static void rd_refill(rd_dec_t * st, rd_io_t * io) {
  while (st->bn <= 56u && io->in < io->end) {
    st->bb |= (uint64_t)*io->in++ << st->bn;
    st->bn += 8;
    st->in_total++;
  }
}

static unsigned rd_take(rd_dec_t * st, unsigned n) {
  const unsigned v = (unsigned)(st->bb & (((uint64_t)1 << n) - 1u));
  st->bb >>= n;
  st->bn -= n;
  return v;
}

static int rd_room(const rd_dec_t * st, const rd_io_t * io) {
  if (st->max_out != 0 && st->produced >= st->max_out) {
    return -1;
  }
  return io->out->used < io->out->size;
}

static void rd_emit(rd_dec_t * st, rd_io_t * io, uint8_t b) {
  ((uint8_t *)io->out->data)[io->out->used++] = b;
  st->win[st->produced & (RD_WINDOW - 1u)] = b;
  st->produced++;
}

typedef enum { R_NEED_INPUT, R_NEED_OUTPUT, R_DONE, R_ERROR } rd_run_t;

/* The byte c from the outer layer, through the inner one. Returns 0 to go on,
 * R_NEED_OUTPUT if a byte had no room (and so c is to be offered again - it
 * never is: room is made before the byte is read), or R_ERROR. */
static void rd_feed(rd_dec_t * st, rd_io_t * io, unsigned c) {
  switch (st->x) {
  case X_NORMAL:
    if (c != RD_DLE) {
      rd_emit(st, io, (uint8_t)c);
    }
    else {
      st->x = X_DLE;
    }
    break;
  case X_DLE:
    if (c == 0) {
      rd_emit(st, io, (uint8_t)RD_DLE);
      st->x = X_NORMAL;
    }
    else {
      st->v = c;
      st->len = c & st->mask;
      st->x = st->len == st->mask ? X_LEN : X_DIST;
    }
    break;
  case X_LEN:
    st->len += c;
    st->x = X_DIST;
    break;
  case X_DIST:
    st->dist = ((st->v >> st->shift) << 8) + c + 1u;
    st->copy_left = st->len + 3u;
    st->x = X_NORMAL;
    break;
  }
}

static rd_run_t rd_run(rd_dec_t * st, rd_io_t * io) {
  for (;;) {
    int r;
    rd_refill(st, io);
    switch (st->stage) {
    case D_DONE:
      return R_DONE;
    case D_FAILED:
      return R_ERROR;
    case D_SETS:
      while (st->set_j >= 0) {
        if (!st->set_have_n) {
          unsigned n;
          rd_refill(st, io);
          if (st->bn < 6) {
            return R_NEED_INPUT;
          }
          n = rd_take(st, 6);
          if (n > RD_MAX_SET) {
            DFAIL(st, GCOMP_ERR_CORRUPT,
                "reduce: a follower set has %u members, not at most 32", n);
            return R_ERROR;
          }
          st->nset[st->set_j] = (uint8_t)n;
          st->set_have_n = 1;
          st->set_pos = 0;
        }
        while (st->set_pos < st->nset[st->set_j]) {
          rd_refill(st, io);
          if (st->bn < 8) {
            return R_NEED_INPUT;
          }
          st->set[st->set_j][st->set_pos++] = (uint8_t)rd_take(st, 8);
        }
        st->set_have_n = 0;
        st->set_j--;
      }
      st->stage = D_DATA;
      break;
    case D_DATA: {
      unsigned n, byte;
      /* a repeat in progress */
      while (st->copy_left != 0) {
        if (st->size != 0 && st->produced >= st->size) {
          st->copy_left = 0;
          break;
        }
        r = rd_room(st, io);
        if (r < 0) {
          DFAIL(st, GCOMP_ERR_LIMIT,
              "reduce: decompressed output exceeds the limit");
          return R_ERROR;
        }
        if (r == 0) {
          return R_NEED_OUTPUT;
        }
        rd_emit(st, io, st->win[(st->produced - st->dist) & (RD_WINDOW - 1u)]);
        st->copy_left--;
      }
      if (st->size != 0 && st->produced >= st->size) {
        st->stage = D_DONE;
        break;
      }
      r = rd_room(st, io);
      if (r < 0) {
        DFAIL(st, GCOMP_ERR_LIMIT,
            "reduce: decompressed output exceeds the limit");
        return R_ERROR;
      }
      if (r == 0) {
        return R_NEED_OUTPUT;
      }
      n = st->nset[st->last];
      if (n == 0) {
        if (st->bn < 8) {
          return R_NEED_INPUT;
        }
        byte = rd_take(st, 8);
      }
      else if ((st->bb & 1u) != 0) {
        if (st->bn < 9) {
          return R_NEED_INPUT;
        }
        (void)rd_take(st, 1);
        byte = rd_take(st, 8);
      }
      else {
        const unsigned b = set_bits(n);
        unsigned idx;
        if (st->bn < 1u + b) {
          return R_NEED_INPUT;
        }
        (void)rd_take(st, 1);
        idx = rd_take(st, b);
        if (idx >= n) {
          DFAIL(st, GCOMP_ERR_CORRUPT,
              "reduce: an index past the end of a follower set");
          return R_ERROR;
        }
        byte = st->set[st->last][idx];
      }
      st->last = byte;
      rd_feed(st, io, byte);
      break;
    }
    }
  }
}

static gcomp_status_t rd_step(rd_dec_t * st, rd_io_t * io, rd_run_t * r) {
  *r = rd_run(st, io);
  return *r == R_ERROR ? st->err : GCOMP_OK;
}

static gcomp_status_t rd_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  rd_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  rd_io_t io;
  rd_run_t r;
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
  s = rd_step(st, &io, &r);
  if (input->data) {
    input->used = (size_t)(io.in - (const uint8_t *)input->data);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  if (gcomp_limits_check_expansion_ratio(st->in_total, st->produced,
          st->max_ratio) != GCOMP_OK) {
    return DFAIL(
        st, GCOMP_ERR_LIMIT, "reduce: expansion ratio exceeds the limit");
  }
  return GCOMP_OK;
}

static gcomp_status_t rd_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  rd_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  rd_io_t io;
  rd_run_t r;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->stage == D_FAILED) {
    return st->err;
  }
  io.in = io.end = &none;
  io.out = output;
  s = rd_step(st, &io, &r);
  if (s != GCOMP_OK) {
    return s;
  }
  if (r == R_NEED_OUTPUT) {
    return GCOMP_ERR_LIMIT;
  }
  if (st->size != 0) {
    if (st->produced == st->size) {
      return GCOMP_OK;
    }
    return DFAIL(st, GCOMP_ERR_CORRUPT, "reduce: truncated stream");
  }
  /* No size was given, so nothing says where the data stops: the padding to
   * the byte can read as a byte or two more (a follower set of one member
   * costs two bits a byte), and whatever stage 3 was part way through is the
   * same. The sets, though, have to be whole. */
  if (st->stage == D_DATA) {
    return GCOMP_OK;
  }
  return DFAIL(st, GCOMP_ERR_CORRUPT, "reduce: truncated stream");
}

/* ---- the encoder ------------------------------------------------------------------ */

typedef struct {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  unsigned factor, shift, mask, window;
  uint8_t * in;
  size_t in_len, in_cap;
  uint8_t * out;
  size_t out_len, out_cap, out_pos;
  uint64_t acc;
  unsigned nacc;
  int finished, built, oom;
} rd_enc_t;

static void enc_restart(rd_enc_t * e) {
  e->in_len = 0;
  e->out_len = e->out_pos = 0;
  e->acc = 0;
  e->nacc = 0;
  e->finished = e->built = e->oom = 0;
}

static void enc_put(rd_enc_t * e, uint32_t v, unsigned bits) {
  if (e->out_len + 8 > e->out_cap) {
    size_t cap = e->out_cap ? e->out_cap : 4096;
    uint8_t * p;
    while (cap < e->out_len + 8) {
      cap *= 2;
    }
    p = gcomp_realloc(e->alloc, e->out, cap);
    if (!p) {
      e->oom = 1;
      return;
    }
    e->out = p;
    e->out_cap = cap;
  }
  e->acc |= (uint64_t)v << e->nacc;
  e->nacc += bits;
  while (e->nacc >= 8) {
    e->out[e->out_len++] = (uint8_t)e->acc;
    e->acc >>= 8;
    e->nacc -= 8;
  }
}

static gcomp_status_t rd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc;
  rd_enc_t * e;
  int64_t factor = 4;
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_int64(options, "reduce.factor", &factor);
  }
  if (factor < 1 || factor > 4) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "reduce.factor must be 1 to 4");
  }
  alloc = gcomp_registry_get_allocator(registry);
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "reduce: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->factor = (unsigned)factor;
  e->shift = 8u - (unsigned)factor;
  e->mask = (1u << e->shift) - 1u;
  e->window = 256u << e->factor;
  enc_restart(e);
  encoder->method_state = e;
  return GCOMP_OK;
}

static void rd_encoder_destroy(gcomp_encoder_t * encoder) {
  rd_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  gcomp_free(e->alloc, e->in);
  gcomp_free(e->alloc, e->out);
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

static gcomp_status_t rd_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  enc_restart(encoder->method_state);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* The inner layer: the input with its repeats made into marker sequences.
 * Returns the bytes and their count, or NULL. */
static uint8_t * enc_inner(rd_enc_t * e, size_t * len_out) {
  const size_t n = e->in_len;
  uint8_t * o = gcomp_malloc(e->alloc, 2 * n + 16);
  int32_t * head = gcomp_calloc(e->alloc, 65536u, sizeof(int32_t));
  int32_t * prev = gcomp_calloc(e->alloc, n + 1, sizeof(int32_t));
  const unsigned maxlen = e->mask + 255u + 3u;
  size_t pos = 0, no = 0;
  if (!o || !head || !prev) {
    gcomp_free(e->alloc, o);
    gcomp_free(e->alloc, head);
    gcomp_free(e->alloc, prev);
    return NULL;
  }
  while (pos < n) {
    unsigned best = 0, bestd = 0, chain = 32;
    const size_t left = n - pos;
    if (left >= 4) {
      int32_t cand = head[e->in[pos] | ((unsigned)e->in[pos + 1] << 8)];
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
    if (best >= 4) {
      const unsigned len = best - 3u; /* 1 or more */
      const unsigned d = bestd - 1u;
      size_t k;
      o[no++] = (uint8_t)RD_DLE;
      if (len >= e->mask) {
        o[no++] = (uint8_t)(((d >> 8) << e->shift) | e->mask);
        o[no++] = (uint8_t)(len - e->mask);
      }
      else {
        o[no++] = (uint8_t)(((d >> 8) << e->shift) | len);
      }
      o[no++] = (uint8_t)(d & 255u);
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
      const uint8_t b = e->in[pos];
      o[no++] = b;
      if (b == RD_DLE) {
        o[no++] = 0;
      }
      if (pos + 1 < n) {
        const unsigned h = b | ((unsigned)e->in[pos + 1] << 8);
        prev[pos] = head[h];
        head[h] = (int32_t)pos + 1;
      }
      pos++;
    }
  }
  gcomp_free(e->alloc, head);
  gcomp_free(e->alloc, prev);
  *len_out = no;
  return o;
}

static gcomp_status_t enc_build(rd_enc_t * e) {
  size_t ni = 0, i;
  uint8_t * inner = enc_inner(e, &ni);
  uint32_t (*cnt)[256] = gcomp_calloc(e->alloc, 256, sizeof(*cnt));
  uint8_t nset[256];
  uint8_t set[256][RD_MAX_SET];
  int8_t index[256][256];
  unsigned last = 0, j, k;
  if (!inner || !cnt) {
    gcomp_free(e->alloc, inner);
    gcomp_free(e->alloc, cnt);
    return GCOMP_ERR_MEMORY;
  }
  for (i = 0; i < ni; i++) {
    cnt[last][inner[i]]++;
    last = inner[i];
  }
  memset(index, -1, sizeof(index));
  for (j = 0; j < 256; j++) {
    unsigned n = 0, pick;
    /* the followers in order of how often they follow, the common first */
    while (n < RD_MAX_SET) {
      uint32_t best = 2; /* a follower must follow at least three times */
      int bi = -1;
      for (k = 0; k < 256; k++) {
        if (cnt[j][k] > best && index[j][k] < 0) {
          best = cnt[j][k];
          bi = (int)k;
        }
      }
      if (bi < 0) {
        break;
      }
      set[j][n] = (uint8_t)bi;
      index[j][bi] = (int8_t)n;
      n++;
    }
    nset[j] = (uint8_t)n;
    (void)pick;
  }
  for (j = 256; j-- > 0;) {
    enc_put(e, nset[j], 6);
    for (k = 0; k < nset[j]; k++) {
      enc_put(e, set[j][k], 8);
    }
  }
  last = 0;
  for (i = 0; i < ni; i++) {
    const unsigned b = inner[i];
    if (nset[last] == 0) {
      enc_put(e, b, 8);
    }
    else if (index[last][b] >= 0) {
      enc_put(e, 0, 1);
      enc_put(e, (unsigned)index[last][b], set_bits(nset[last]));
    }
    else {
      enc_put(e, 1, 1);
      enc_put(e, b, 8);
    }
    last = b;
  }
  if (e->nacc != 0) {
    enc_put(e, 0, 8u - e->nacc);
  }
  gcomp_free(e->alloc, inner);
  gcomp_free(e->alloc, cnt);
  return e->oom ? GCOMP_ERR_MEMORY : GCOMP_OK;
}

static gcomp_status_t rd_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  rd_enc_t * e = encoder->method_state;
  size_t n;
  (void)output;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished || e->built) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "reduce: encoder update after finish");
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
          encoder, GCOMP_ERR_MEMORY, "reduce: out of memory");
    }
    e->in = p;
    e->in_cap = cap;
  }
  memcpy(e->in + e->in_len, (const uint8_t *)input->data + input->used, n);
  e->in_len += n;
  input->used += n;
  return GCOMP_OK;
}

static gcomp_status_t rd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  rd_enc_t * e = encoder->method_state;
  size_t n;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (!e->built) {
    const gcomp_status_t s = enc_build(e);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, s, "reduce: could not build the stream");
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

/* ---- registration ----------------------------------------------------------------- */

static const gcomp_option_schema_t g_rd_option_schemas[] = {
    {"reduce.factor", GCOMP_OPT_INT64, 1, {.i64 = 4}, 1, 1, 1, 4, 0, 0,
        "The compression factor, 1 to 4: ZIP method 2 to 5 is factor 1 to 4.",
        NULL},
    {"reduce.uncompressed_size", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0,
        "Where the decoder stops, as the container records it; 0 decodes to the "
        "end of the input.",
        NULL},
    {"limits.max_output_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum decompressed output", NULL},
    {"limits.max_memory_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum memory usage", NULL},
    {"limits.max_expansion_ratio", GCOMP_OPT_UINT64, 1,
        {.ui64 = GCOMP_REDUCE_MAX_EXPANSION_RATIO}, 0, 0, 0, 0, 0, 0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL},
};

/* Kept in step with the schema above by SchemaAllMethodsTest. */
static const char * const g_rd_option_keys[] = {"reduce.factor",
    "reduce.uncompressed_size", "limits.max_output_bytes",
    "limits.max_memory_bytes", "limits.max_expansion_ratio"};

static const gcomp_method_schema_t g_rd_schema = {g_rd_option_schemas,
    sizeof(g_rd_option_schemas) / sizeof(g_rd_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, g_rd_option_keys};

static const gcomp_method_schema_t * rd_get_schema(void) {
  return &g_rd_schema;
}

static gcomp_status_t rd_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = rd_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = rd_encoder_update;
  (*encoder_out)->finish_fn = rd_encoder_finish;
  (*encoder_out)->reset_fn = rd_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t rd_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = rd_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = rd_decoder_update;
  (*decoder_out)->finish_fn = rd_decoder_finish;
  (*decoder_out)->reset_fn = rd_decoder_reset;
  return GCOMP_OK;
}

static void rd_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  rd_encoder_destroy(encoder);
}

static void rd_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  rd_decoder_destroy(decoder);
}

/** The sets (256 * 6 bits and at most 32 bytes each), and every byte of the
 * inner layer at nine bits, which is twice the input for a stream of markers. */
static gcomp_status_t rd_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  size_t bound = 0;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = gcomp_bound_add_mul(&bound, input_size, 5u);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 9000u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static const gcomp_method_t g_rd_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "reduce",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = rd_create_encoder,
    .create_decoder = rd_create_decoder,
    .destroy_encoder = rd_destroy_encoder_wrapper,
    .destroy_decoder = rd_destroy_decoder_wrapper,
    .get_schema = rd_get_schema,
    .encode_bound = rd_encode_bound,
};

gcomp_status_t gcomp_method_reduce_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_rd_method);
}

GCOMP_AUTOREG_METHOD(reduce, gcomp_method_reduce_register)
