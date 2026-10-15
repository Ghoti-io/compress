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
 * @file xz_encode.c
 *
 * The xz encoder: a stream header, blocks, an index and a footer around a
 * chain of filters that ends in LZMA2.
 *
 * ## Shape
 *
 * Input goes into the current block's chain until the block is full (when
 * `xz.block_size` is set) or the stream ends. A block is closed by finishing
 * the chain, which leaves its compressed size known; then the padding to a
 * multiple of four and the check go out, and the block's two sizes are
 * remembered for the index. The block header does not carry the sizes (the
 * encoder cannot back up to write them), which the format allows: the index
 * is where they are.
 *
 * Small fixed pieces (the stream header, a block header, padding and a check)
 * are queued in `q` and drained into the caller's output before anything
 * else, so a caller with a one-byte output buffer makes progress through all
 * of them. The index is built whole when the stream ends and drained the same
 * way.
 *
 * Each block starts a fresh LZMA2 stream with a fresh dictionary, so blocks
 * are independent: the output for a block depends only on its bytes, which
 * is what lets a reader decode them separately.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/parallel_block.h"
#include "../../core/bound_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "../../filters/filter_internal.h"
#include "../lzma/lzma_internal.h"
#include "xz_chain.h"
#include "xz_internal.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/limits.h>

#include <stdio.h>
#include <string.h>

#define Q_CAP 256u

typedef struct {
  gcomp_registry_t * registry;
  const gcomp_allocator_t * alloc;

  unsigned check;
  uint64_t block_size; /* 0: one block for the whole stream */
  xz_filter_t filters[XZ_FILTERS_MAX - 1u];
  unsigned nfilters;
  gcomp_options_t * lzma_opts; /* what the LZMA2 stage is made from */
  uint64_t max_memory;

  xz_chain_t chain;
  int chain_built;
  int flushable; /* every filter in front of LZMA2 can flush: only delta can */
  uint8_t hdr[XZ_BLOCK_HEADER_MAX];
  size_t hdr_size;

  int header_done, block_open, closing, finished, tail_built;
  uint8_t q[Q_CAP];
  size_t qpos, qlen;
  uint64_t block_in, block_out;
  xz_check_t chk;

  uint64_t * rec; /* pairs: unpadded size, uncompressed size */
  size_t rec_n, rec_cap;
  uint8_t * tail;
  size_t tail_len, tail_pos;

  struct xz_par * par; /* threads: blocks coded by workers; NULL: in line */
} xz_enc_t;

/* The threaded encoder, further down. */
static gcomp_status_t par_init(
    xz_enc_t * e, gcomp_options_t * options, uint64_t threads);
static void par_destroy(xz_enc_t * e);
static void par_abandon(xz_enc_t * e);
static gcomp_status_t par_update(gcomp_encoder_t * encoder, xz_enc_t * e,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
static gcomp_status_t par_flush_all(gcomp_encoder_t * encoder, xz_enc_t * e,
    gcomp_buffer_t * output);

/* ---- the small queue ---------------------------------------------------- */

static void q_put(xz_enc_t * e, const uint8_t * data, size_t n) {
  /* Callers queue only what fits and only after the queue has drained. */
  memcpy(e->q + e->qlen, data, n);
  e->qlen += n;
}

static void q_zeros(xz_enc_t * e, size_t n) {
  memset(e->q + e->qlen, 0, n);
  e->qlen += n;
}

static void drain(xz_enc_t * e, gcomp_buffer_t * out) {
  size_t room = out->size - out->used;
  size_t n = e->qlen - e->qpos;
  if (n > room) {
    n = room;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->q + e->qpos, n);
    out->used += n;
    e->qpos += n;
  }
  if (e->qpos == e->qlen) {
    e->qpos = e->qlen = 0;
  }
}

static void drain_tail(xz_enc_t * e, gcomp_buffer_t * out) {
  size_t room = out->size - out->used;
  size_t n = e->tail_len - e->tail_pos;
  if (n > room) {
    n = room;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->tail + e->tail_pos, n);
    out->used += n;
    e->tail_pos += n;
  }
}

/* ---- pieces of the container -------------------------------------------- */

static void queue_stream_header(xz_enc_t * e) {
  uint8_t h[XZ_STREAM_HEADER_SIZE] = {0xFD, '7', 'z', 'X', 'Z', 0x00, 0x00, 0};
  h[7] = (uint8_t)e->check;
  xz_put_le32(h + 8, xz_crc32(h + 6, 2));
  q_put(e, h, sizeof(h));
}

/* The filter flags, the same for every block, written once the LZMA2 stage
 * exists and its dictionary is known. */
static void build_block_header(xz_enc_t * e, uint8_t dict_prop) {
  uint8_t * h = e->hdr;
  size_t n = 2, size;
  unsigned i;
  h[1] = (uint8_t)(e->nfilters); /* (filters + LZMA2) - 1, no sizes */
  for (i = 0; i < e->nfilters; i++) {
    const xz_filter_t * f = &e->filters[i];
    n += xz_vli_put(h + n, f->id);
    if (f->id == XZ_FILTER_DELTA) {
      h[n++] = 1;
      h[n++] = (uint8_t)(f->arg - 1u);
    }
    else if (f->arg == 0) {
      h[n++] = 0;
    }
    else {
      h[n++] = 4;
      xz_put_le32(h + n, f->arg);
      n += 4;
    }
  }
  n += xz_vli_put(h + n, XZ_FILTER_LZMA2);
  h[n++] = 1;
  h[n++] = dict_prop;
  size = (n + 4u + 3u) & ~(size_t)3u;
  memset(h + n, 0, size - 4u - n);
  h[0] = (uint8_t)(size / 4u - 1u);
  xz_put_le32(h + size - 4u, xz_crc32(h, size - 4u));
  e->hdr_size = size;
}

/* ---- the chain ---------------------------------------------------------- */

static gcomp_status_t build_chain(xz_enc_t * e, gcomp_encoder_t * encoder) {
  unsigned i;
  gcomp_status_t s;
  xz_chain_init(&e->chain, e->alloc, 1);
  for (i = 0; i < e->nfilters; i++) {
    const xz_filter_t * f = &e->filters[i];
    gcomp_options_t * o = NULL;
    gcomp_encoder_t * stage = NULL;
    const char * method;
    s = gcomp_options_create(&o);
    if (s != GCOMP_OK) {
      goto fail;
    }
    if (f->id == XZ_FILTER_DELTA) {
      method = "delta";
      s = gcomp_options_set_int64(o, "delta.distance", (int64_t)f->arg);
    }
    else {
      method = "bcj";
      s = gcomp_options_set_string(
          o, "bcj.arch", filter_arch_name(f->id));
      if (s == GCOMP_OK) {
        s = gcomp_options_set_uint64(o, "bcj.start_offset", f->arg);
      }
    }
    if (s == GCOMP_OK) {
      s = gcomp_encoder_create(e->registry, method, o, &stage);
    }
    gcomp_options_destroy(o);
    if (s != GCOMP_OK) {
      goto fail;
    }
    s = xz_chain_add(&e->chain, stage);
    if (s != GCOMP_OK) {
      goto fail;
    }
  }
  {
    gcomp_encoder_t * lz = NULL;
    s = gcomp_encoder_create(e->registry, "lzma2", e->lzma_opts, &lz);
    if (s != GCOMP_OK) {
      goto fail;
    }
    build_block_header(e, xz_dict_prop(lzma_encoder_dict_size(lz)));
    s = xz_chain_add(&e->chain, lz);
    if (s != GCOMP_OK) {
      goto fail;
    }
  }
  e->chain_built = 1;
  return GCOMP_OK;
fail:
  xz_chain_clear(&e->chain);
  return gcomp_encoder_set_error(encoder, s,
      "xz: could not make the filter chain (the filters and lzma2 must be "
      "registered)");
}

/* ---- init and teardown -------------------------------------------------- */

static const char * const g_checks[] = {"none", "crc32", "crc64", "sha256"};
static const unsigned g_check_ids[] = {
    XZ_CHECK_NONE, XZ_CHECK_CRC32, XZ_CHECK_CRC64, XZ_CHECK_SHA256};

static void state_reset(xz_enc_t * e) {
  e->header_done = e->block_open = e->closing = e->finished = 0;
  e->tail_built = 0;
  e->qpos = e->qlen = 0;
  e->block_in = e->block_out = 0;
  e->rec_n = 0;
  gcomp_free(e->alloc, e->tail);
  e->tail = NULL;
  e->tail_len = e->tail_pos = 0;
}

static gcomp_status_t encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  xz_enc_t * e;
  int64_t preset = 6, lc = 3, lp = 0, pb = 2;
  uint64_t dict = 0, threads = 1;
  const char * str = NULL;
  char err[96];
  gcomp_status_t s;

  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "xz: out of memory");
  }
  e->registry = registry;
  e->alloc = alloc;
  e->check = XZ_CHECK_CRC64;
  encoder->method_state = e;
  if (options) {
    unsigned i;
    (void)gcomp_options_get_int64(options, "xz.preset", &preset);
    (void)gcomp_options_get_uint64(options, "xz.dict_size", &dict);
    (void)gcomp_options_get_int64(options, "xz.lc", &lc);
    (void)gcomp_options_get_int64(options, "xz.lp", &lp);
    (void)gcomp_options_get_int64(options, "xz.pb", &pb);
    (void)gcomp_options_get_uint64(options, "xz.block_size", &e->block_size);
    (void)gcomp_options_get_uint64(options, "threads.count", &threads);
    if (gcomp_options_get_string(options, "xz.check", &str) == GCOMP_OK && str) {
      for (i = 0; i < sizeof(g_checks) / sizeof(g_checks[0]); i++) {
        if (strcmp(str, g_checks[i]) == 0) {
          e->check = g_check_ids[i];
        }
      }
    }
    str = NULL;
    if (gcomp_options_get_string(options, "xz.filters", &str) == GCOMP_OK &&
        str && !xz_filters_parse(str, e->filters, &e->nfilters, err, sizeof(err))) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "xz.filters: %s", err);
    }
    e->max_memory = gcomp_limits_read_memory_max(options, 0);
  }
  s = gcomp_options_create(&e->lzma_opts);
  if (s == GCOMP_OK) {
    s = gcomp_options_set_int64(e->lzma_opts, "lzma2.preset", preset);
  }
  if (s == GCOMP_OK && dict != 0) {
    s = gcomp_options_set_uint64(e->lzma_opts, "lzma2.dict_size", dict);
  }
  if (s == GCOMP_OK) {
    s = gcomp_options_set_int64(e->lzma_opts, "lzma2.lc", lc);
  }
  if (s == GCOMP_OK) {
    s = gcomp_options_set_int64(e->lzma_opts, "lzma2.lp", lp);
  }
  if (s == GCOMP_OK) {
    s = gcomp_options_set_int64(e->lzma_opts, "lzma2.pb", pb);
  }
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, s, "xz: could not set the lzma2 options");
  }
  /* Made now rather than at the first byte, so that a registry without lzma2
   * or a filter fails at creation, where the caller can see why. */
  s = build_chain(e, encoder);
  if (s != GCOMP_OK) {
    return s;
  }
  e->flushable = 1;
  {
    unsigned fi;
    for (fi = 0; fi < e->nfilters; fi++) {
      if (e->filters[fi].id != XZ_FILTER_DELTA) {
        e->flushable = 0;
      }
    }
  }
  s = par_init(e, options, threads);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, s, "xz: could not start the worker threads");
  }
  state_reset(e);
  return GCOMP_OK;
}

void xz_encoder_destroy(gcomp_encoder_t * encoder) {
  xz_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  par_destroy(e);
  xz_chain_clear(&e->chain);
  gcomp_options_destroy(e->lzma_opts);
  gcomp_free(e->alloc, e->rec);
  gcomp_free(e->alloc, e->tail);
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

/* The registry frees the encoder shell when a method's create fails and knows
 * nothing of the state hung from it, so a failure here tears that down. */
gcomp_status_t xz_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  gcomp_status_t s = encoder_init(registry, options, encoder);
  if (s != GCOMP_OK) {
    xz_encoder_destroy(encoder);
  }
  return s;
}



static gcomp_status_t push_record(xz_enc_t * e, uint64_t unpadded, uint64_t unc);

/* ---- threads ------------------------------------------------------------ */

/* With `threads.count` above one and a block size, the blocks are coded by
 * workers. A worker does not share the chain: it codes its block as a stream
 * of one block, through the same registry and the same options, and the
 * block is cut out of that stream (the 12 bytes of header in front, the index
 * and footer behind). Each block is independent, so what comes out is the
 * serial encoder's block byte for byte, and the stream is the same bytes at
 * any number of threads. The buffers of a block in flight are its input and
 * its coded stream. */

typedef struct xz_pjob_s {
  gcomp_block_job_t base; /* first: the helper keeps its ticket here */
  gcomp_registry_t * registry;
  gcomp_options_t * opts; /* shared, read only */
  struct xz_pjob_s * next;
  uint8_t * in;
  size_t in_n, in_cap;
  uint8_t * out;
  size_t out_cap, out_len;
  size_t blk_off, blk_len; /* the block, inside out */
  uint64_t unpadded, unc;
  size_t pos; /* how much of the block has gone to the caller */
  gcomp_status_t status;
} xz_pjob_t;

typedef struct xz_par {
  gcomp_parallel_block_ctx_t * pb;
  gcomp_options_t * opts;
  xz_pjob_t ** all;
  unsigned made, max_jobs, max_inflight, inflight;
  xz_pjob_t * free_list;
  xz_pjob_t * cur;  /* being filled */
  xz_pjob_t * pend; /* coded, going out */
} xz_par_t;

/* One variable-length integer out of a buffer; 0 when it is not there. */
static int par_vli(const uint8_t * b, size_t n, size_t * pos, uint64_t * out) {
  xz_vli_t v = {0, 0};
  while (*pos < n) {
    const int r = xz_vli_feed(&v, b[(*pos)++]);
    if (r < 0) {
      return 0;
    }
    if (r > 0) {
      *out = v.value;
      return 1;
    }
  }
  return 0;
}

/* Find the block, and its two sizes, in a stream of one block. */
static gcomp_status_t par_cut(xz_pjob_t * j) {
  const uint8_t * s = j->out;
  size_t idx_size, idx_off, at = 1;
  uint64_t count = 0;
  if (j->out_len < XZ_STREAM_HEADER_SIZE + XZ_STREAM_FOOTER_SIZE + 8u) {
    return GCOMP_ERR_INTERNAL;
  }
  idx_size = ((size_t)xz_get_le32(s + j->out_len - XZ_STREAM_FOOTER_SIZE + 4u) +
                 1u) * 4u;
  if (idx_size > j->out_len - XZ_STREAM_HEADER_SIZE - XZ_STREAM_FOOTER_SIZE) {
    return GCOMP_ERR_INTERNAL;
  }
  idx_off = j->out_len - XZ_STREAM_FOOTER_SIZE - idx_size;
  if (s[idx_off] != 0 ||
      !par_vli(s + idx_off, idx_size, &at, &count) || count != 1 ||
      !par_vli(s + idx_off, idx_size, &at, &j->unpadded) ||
      !par_vli(s + idx_off, idx_size, &at, &j->unc) || j->unc != j->in_n) {
    return GCOMP_ERR_INTERNAL;
  }
  j->blk_off = XZ_STREAM_HEADER_SIZE;
  j->blk_len = idx_off - XZ_STREAM_HEADER_SIZE;
  return GCOMP_OK;
}

static int par_run(void * ctx) {
  xz_pjob_t * j = ctx;
  size_t len = 0;
  gcomp_status_t s = gcomp_encode_buffer(j->registry, "xz", j->opts, j->in,
      j->in_n, j->out, j->out_cap, &len);
  if (s == GCOMP_OK) {
    j->out_len = len;
    s = par_cut(j);
  }
  j->status = s;
  return (int)s;
}

static void par_free_job(xz_enc_t * e, xz_pjob_t * j) {
  if (j) {
    gcomp_free(e->alloc, j->in);
    gcomp_free(e->alloc, j->out);
    gcomp_free(e->alloc, j);
  }
}

static void par_give(xz_enc_t * e, xz_pjob_t * j) {
  xz_par_t * p = e->par;
  gcomp_free(e->alloc, j->out);
  j->out = NULL;
  j->out_cap = j->out_len = 0;
  j->in_n = 0;
  j->pos = 0;
  j->next = p->free_list;
  p->free_list = j;
}

static xz_pjob_t * par_take(xz_enc_t * e) {
  xz_par_t * p = e->par;
  xz_pjob_t * j = p->free_list;
  if (j) {
    p->free_list = j->next;
    return j;
  }
  if (p->made == p->max_jobs) {
    return NULL;
  }
  j = gcomp_calloc(e->alloc, 1, sizeof(*j));
  if (!j) {
    return NULL;
  }
  j->registry = e->registry;
  j->opts = p->opts;
  p->all[p->made++] = j;
  return j;
}

/* Wait for every block out for coding and drop it. */
static void par_abandon(xz_enc_t * e) {
  xz_par_t * p = e->par;
  while (p->inflight > 0) {
    gcomp_block_job_t * base = NULL;
    (void)gcomp_parallel_block_get_result(p->pb, &base);
    if (!base) {
      break;
    }
    p->inflight--;
    par_give(e, (xz_pjob_t *)base);
  }
  if (p->pend) {
    par_give(e, p->pend);
    p->pend = NULL;
  }
  if (p->cur) {
    par_give(e, p->cur);
    p->cur = NULL;
  }
}

static void par_destroy(xz_enc_t * e) {
  xz_par_t * p = e->par;
  unsigned i;
  if (!p) {
    return;
  }
  if (p->pb) {
    par_abandon(e);
    gcomp_parallel_block_wait(p->pb);
    gcomp_parallel_block_destroy(p->pb);
  }
  for (i = 0; i < p->made; i++) {
    par_free_job(e, p->all[i]);
  }
  gcomp_free(e->alloc, p->all);
  gcomp_options_destroy(p->opts);
  gcomp_free(e->alloc, p);
  e->par = NULL;
}

/* Set the workers up, or leave `e->par` NULL for the in-line encoder: that is
 * the answer for one thread, for no block size, and for a memory limit that
 * could not hold two blocks. */
static gcomp_status_t par_init(xz_enc_t * e, gcomp_options_t * options,
    uint64_t threads) {
  xz_par_t * p;
  gcomp_parallel_block_config_t cfg;
  gcomp_status_t s;
  if (threads <= 1 || e->block_size == 0 || !options) {
    return GCOMP_OK;
  }
  if (threads > 64) {
    threads = 64;
  }
  if (e->max_memory != 0) {
    /* A block in flight holds its input and its coded stream. */
    const uint64_t per = e->block_size > UINT64_MAX / 4u
        ? UINT64_MAX : e->block_size * 2u;
    const uint64_t fit = e->max_memory / per;
    if (fit < 2) {
      return GCOMP_OK;
    }
    if (threads > fit - 1u) {
      threads = fit - 1u;
    }
  }
  p = gcomp_calloc(e->alloc, 1, sizeof(*p));
  if (!p) {
    return GCOMP_ERR_MEMORY;
  }
  e->par = p;
  p->max_inflight = (unsigned)threads;
  p->max_jobs = p->max_inflight + 2u;
  p->all = gcomp_calloc(e->alloc, p->max_jobs, sizeof(*p->all));
  s = gcomp_options_clone(options, &p->opts);
  if (s == GCOMP_OK) {
    s = gcomp_options_set_uint64(p->opts, "threads.count", 1);
  }
  if (s == GCOMP_OK) {
    s = gcomp_options_set_uint64(p->opts, "xz.block_size", 0);
  }
  if (!p->all || s != GCOMP_OK) {
    return s != GCOMP_OK ? s : GCOMP_ERR_MEMORY;
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.allocator = e->alloc;
  cfg.num_threads = (uint32_t)threads;
  cfg.max_in_flight = (uint32_t)threads;
  return gcomp_parallel_block_create(&cfg, &p->pb);
}

static gcomp_status_t par_fail(gcomp_encoder_t * encoder, gcomp_status_t s) {
  return gcomp_encoder_set_error(encoder,
      s == GCOMP_OK ? GCOMP_ERR_INTERNAL : s, "xz: a block could not be coded");
}

/* Take the next coded block, waiting for it. `pend` is empty. */
static gcomp_status_t par_collect(gcomp_encoder_t * encoder, xz_enc_t * e) {
  xz_par_t * p = e->par;
  gcomp_block_job_t * base = NULL;
  xz_pjob_t * j;
  gcomp_status_t s = gcomp_parallel_block_get_result(p->pb, &base);
  if (!base) {
    return par_fail(encoder, s);
  }
  j = (xz_pjob_t *)base;
  p->inflight--;
  if (j->status != GCOMP_OK) {
    s = j->status;
    par_give(e, j);
    return par_fail(encoder, s);
  }
  s = push_record(e, j->unpadded, j->unc);
  if (s != GCOMP_OK) {
    par_give(e, j);
    return gcomp_encoder_set_error(encoder, s, s == GCOMP_ERR_LIMIT
            ? "xz: the index exceeds limits.max_memory_bytes"
            : "xz: out of memory for the index");
  }
  j->pos = 0;
  p->pend = j;
  return GCOMP_OK;
}

/* Hand the block being filled to a worker. There is room for it. */
static gcomp_status_t par_submit(gcomp_encoder_t * encoder, xz_enc_t * e) {
  xz_par_t * p = e->par;
  xz_pjob_t * j = p->cur;
  size_t cap = 0;
  gcomp_status_t s = gcomp_encode_bound(e->registry, "xz", p->opts, j->in_n, &cap);
  if (s != GCOMP_OK) {
    return par_fail(encoder, s);
  }
  j->out = gcomp_malloc(e->alloc, cap);
  if (!j->out) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "xz: out of memory for a block");
  }
  j->out_cap = cap;
  j->base.input = j->in;
  j->base.input_size = j->in_n;
  p->cur = NULL;
  s = gcomp_parallel_block_try_submit(p->pb, j, par_run);
  if (s != GCOMP_OK) {
    par_give(e, j);
    return par_fail(encoder, s);
  }
  p->inflight++;
  return GCOMP_OK;
}

/* Send the going-out block to the caller. 1: it is all gone. */
static int par_drain_pend(xz_enc_t * e, gcomp_buffer_t * out) {
  xz_par_t * p = e->par;
  xz_pjob_t * j = p->pend;
  size_t room = out->size - out->used, n = j->blk_len - j->pos;
  if (n > room) {
    n = room;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, j->out + j->blk_off + j->pos, n);
    out->used += n;
    j->pos += n;
  }
  if (j->pos < j->blk_len) {
    return 0;
  }
  p->pend = NULL;
  par_give(e, j);
  return 1;
}

static gcomp_status_t par_update(gcomp_encoder_t * encoder, xz_enc_t * e,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  xz_par_t * p = e->par;
  for (;;) {
    gcomp_status_t s;
    size_t take;
    xz_pjob_t * j;
    drain(e, output);
    if (e->qlen != 0) {
      return GCOMP_OK;
    }
    if (p->pend) {
      if (!par_drain_pend(e, output)) {
        return GCOMP_OK;
      }
      continue;
    }
    if (!e->header_done) {
      queue_stream_header(e);
      e->header_done = 1;
      continue;
    }
    if (p->inflight > 0 && gcomp_parallel_block_result_ready(p->pb)) {
      s = par_collect(encoder, e);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (p->cur && p->cur->in_n == e->block_size) {
      s = p->inflight >= p->max_inflight ? par_collect(encoder, e)
                                         : par_submit(encoder, e);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (input->used >= input->size) {
      return GCOMP_OK;
    }
    if (!p->cur) {
      p->cur = par_take(e);
      if (!p->cur) {
        if (p->inflight == 0) {
          return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
              "xz: out of memory for a block");
        }
        s = par_collect(encoder, e);
        if (s != GCOMP_OK) {
          return s;
        }
        continue;
      }
    }
    j = p->cur;
    take = input->size - input->used;
    if (take > e->block_size - j->in_n) {
      take = (size_t)(e->block_size - j->in_n);
    }
    if (j->in_n + take > j->in_cap) {
      size_t cap = j->in_cap ? j->in_cap * 2u : 65536u;
      uint8_t * grown;
      while (cap < j->in_n + take) {
        cap *= 2u;
      }
      if (cap > e->block_size) {
        cap = (size_t)e->block_size;
      }
      grown = gcomp_realloc(e->alloc, j->in, cap);
      if (!grown) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "xz: out of memory for a block");
      }
      j->in = grown;
      j->in_cap = cap;
    }
    memcpy(j->in + j->in_n, (const uint8_t *)input->data + input->used, take);
    j->in_n += take;
    input->used += take;
  }
}

/* Get every block out: the one being filled, those being coded, the one
 * going out. ::GCOMP_ERR_LIMIT: the output filled; call again. */
static gcomp_status_t par_flush_all(gcomp_encoder_t * encoder, xz_enc_t * e,
    gcomp_buffer_t * output) {
  xz_par_t * p = e->par;
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->qlen != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (p->pend) {
      if (!par_drain_pend(e, output)) {
        return GCOMP_ERR_LIMIT;
      }
      continue;
    }
    if (p->cur && p->cur->in_n > 0) {
      s = p->inflight >= p->max_inflight ? par_collect(encoder, e)
                                         : par_submit(encoder, e);
    }
    else if (p->inflight > 0) {
      s = par_collect(encoder, e);
    }
    else {
      return GCOMP_OK;
    }
    if (s != GCOMP_OK) {
      return s;
    }
  }
}

/* ---- closing a block ---------------------------------------------------- */

static gcomp_status_t push_record(xz_enc_t * e, uint64_t unpadded, uint64_t unc) {
  if (e->rec_n == e->rec_cap) {
    size_t cap = e->rec_cap ? e->rec_cap * 2u : 16u;
    uint64_t * grown;
    if (gcomp_limits_check_memory(cap * 2u * sizeof(uint64_t), e->max_memory) !=
        GCOMP_OK) {
      return GCOMP_ERR_LIMIT;
    }
    grown = gcomp_realloc(e->alloc, e->rec, cap * 2u * sizeof(uint64_t));
    if (!grown) {
      return GCOMP_ERR_MEMORY;
    }
    e->rec = grown;
    e->rec_cap = cap;
  }
  e->rec[2u * e->rec_n] = unpadded;
  e->rec[2u * e->rec_n + 1u] = unc;
  e->rec_n++;
  return GCOMP_OK;
}

/* Finish the chain, then queue the padding and the check and note the block.
 * ::GCOMP_ERR_LIMIT: the output filled; call again. */
static gcomp_status_t close_block(
    gcomp_encoder_t * encoder, xz_enc_t * e, gcomp_buffer_t * out) {
  const size_t before = out->used;
  gcomp_status_t s = xz_chain_finish(&e->chain, out);
  uint8_t sum[32];
  size_t pad, csize;
  e->block_out += out->used - before;
  if (s == GCOMP_ERR_LIMIT) {
    return s;
  }
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "xz: %s",
        e->chain.detail[0] ? e->chain.detail : "the filter chain failed");
  }
  pad = (4u - (size_t)(e->block_out & 3u)) & 3u;
  csize = xz_check_size(e->check);
  xz_check_final(&e->chk, sum);
  q_zeros(e, pad);
  q_put(e, sum, csize);
  s = push_record(e, (uint64_t)e->hdr_size + e->block_out + csize, e->block_in);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, s == GCOMP_ERR_LIMIT
            ? "xz: the index exceeds limits.max_memory_bytes"
            : "xz: out of memory for the index");
  }
  e->block_open = 0;
  e->closing = 0;
  return GCOMP_OK;
}

static gcomp_status_t open_block(gcomp_encoder_t * encoder, xz_enc_t * e) {
  gcomp_status_t s = xz_chain_reset(&e->chain);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "xz: could not start a block");
  }
  q_put(e, e->hdr, e->hdr_size);
  xz_check_init(&e->chk, e->check);
  e->block_in = e->block_out = 0;
  e->block_open = 1;
  return GCOMP_OK;
}

/* ---- the stream --------------------------------------------------------- */

gcomp_status_t xz_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  xz_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished || e->tail_built) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "xz: encoder update after finish");
  }
  if (e->par) {
    return par_update(encoder, e, input, output);
  }
  for (;;) {
    size_t avail, take;
    gcomp_buffer_t sub;
    size_t out_before;
    gcomp_status_t s;
    drain(e, output);
    if (e->qlen != 0) {
      return GCOMP_OK;
    }
    if (e->closing) {
      s = close_block(encoder, e, output);
      if (s == GCOMP_ERR_LIMIT) {
        return GCOMP_OK;
      }
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (!e->header_done) {
      queue_stream_header(e);
      e->header_done = 1;
      continue;
    }
    avail = input->size - input->used;
    if (avail == 0) {
      return GCOMP_OK;
    }
    if (!e->block_open) {
      s = open_block(encoder, e);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    take = avail;
    if (e->block_size != 0 && take > e->block_size - e->block_in) {
      take = (size_t)(e->block_size - e->block_in);
    }
    sub.data = (uint8_t *)input->data + input->used;
    sub.size = take;
    sub.used = 0;
    out_before = output->used;
    s = xz_chain_update(&e->chain, &sub, output);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, s, "xz: %s",
          e->chain.detail[0] ? e->chain.detail : "the filter chain failed");
    }
    xz_check_update(&e->chk, sub.data, sub.used);
    input->used += sub.used;
    e->block_in += sub.used;
    e->block_out += output->used - out_before;
    if (e->block_size != 0 && e->block_in == e->block_size) {
      e->closing = 1;
      continue;
    }
    if (sub.used == 0 && output->used == out_before) {
      return GCOMP_OK;
    }
  }
}

/* The index, then the footer, built whole. */
static gcomp_status_t build_tail(gcomp_encoder_t * encoder, xz_enc_t * e) {
  size_t i, n = 1 + xz_vli_size(e->rec_n), idx, total;
  uint8_t * t;
  for (i = 0; i < e->rec_n; i++) {
    n += xz_vli_size(e->rec[2u * i]) + xz_vli_size(e->rec[2u * i + 1u]);
  }
  idx = ((n + 3u) & ~(size_t)3u) + 4u;
  total = idx + XZ_STREAM_FOOTER_SIZE;
  t = gcomp_malloc(e->alloc, total);
  if (!t) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "xz: out of memory for the index");
  }
  n = 0;
  t[n++] = 0;
  n += xz_vli_put(t + n, e->rec_n);
  for (i = 0; i < e->rec_n; i++) {
    n += xz_vli_put(t + n, e->rec[2u * i]);
    n += xz_vli_put(t + n, e->rec[2u * i + 1u]);
  }
  memset(t + n, 0, idx - 4u - n);
  xz_put_le32(t + idx - 4u, xz_crc32(t, idx - 4u));
  /* Footer: CRC of the backward size and the flags, the backward size (the
   * index's length over four, less one), the flags, then "YZ". */
  xz_put_le32(t + idx + 4u, (uint32_t)(idx / 4u - 1u));
  t[idx + 8u] = 0;
  t[idx + 9u] = (uint8_t)e->check;
  xz_put_le32(t + idx, xz_crc32(t + idx + 4u, 6));
  t[idx + 10u] = 'Y';
  t[idx + 11u] = 'Z';
  e->tail = t;
  e->tail_len = total;
  e->tail_pos = 0;
  e->tail_built = 1;
  return GCOMP_OK;
}

gcomp_status_t xz_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  xz_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->qlen != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (e->finished) {
      return GCOMP_OK;
    }
    if (e->par && !e->tail_built) {
      s = par_flush_all(encoder, e, output);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    if (e->block_open) {
      e->closing = 1;
      s = close_block(encoder, e, output);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (!e->header_done) {
      queue_stream_header(e);
      e->header_done = 1;
      continue;
    }
    if (!e->tail_built) {
      s = build_tail(encoder, e);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    drain_tail(e, output);
    if (e->tail_pos < e->tail_len) {
      return GCOMP_ERR_LIMIT;
    }
    e->finished = 1;
  }
}

/* A sync flush makes everything given so far available at the output and
 * keeps the block open: the dictionary and the filters' state go on, so what
 * follows still matches what came before (LZMA2 chunks end where the flush
 * is, and a reader can decode as far as that). A branch converter cannot be
 * flushed, since it holds back bytes that may start a branch, so with one in
 * front of LZMA2 a sync flush ends the block instead. A full flush always
 * does: it means forgetting everything before this point, and a block is the
 * unit that does. With threads the blocks are coded whole by workers, so
 * there a sync flush ends the block as well, which is why the bytes differ
 * from one thread's for a stream that is flushed. What follows a closed block
 * starts a new one. */
gcomp_status_t xz_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  xz_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished || e->tail_built) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "xz: encoder cannot flush after finish");
  }
  if (e->par) {
    return par_flush_all(encoder, e, output);
  }
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->qlen != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (!e->block_open) {
      return GCOMP_OK;
    }
    if (mode == GCOMP_FLUSH_SYNC && e->flushable) {
      const size_t before = output->used;
      s = xz_chain_flush(&e->chain, output, mode);
      e->block_out += output->used - before;
      if (s == GCOMP_ERR_LIMIT) {
        return s;
      }
      if (s != GCOMP_OK) {
        return gcomp_encoder_set_error(encoder, s, "xz: %s",
            e->chain.detail[0] ? e->chain.detail : "the filter chain failed");
      }
      return GCOMP_OK;
    }
    e->closing = 1;
    s = close_block(encoder, e, output);
    if (s != GCOMP_OK) {
      return s;
    }
  }
}

gcomp_status_t xz_encoder_reset(gcomp_encoder_t * encoder) {
  xz_enc_t * e;
  gcomp_status_t s;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  e = encoder->method_state;
  if (e->par) {
    par_abandon(e);
  }
  s = xz_chain_reset(&e->chain);
  if (s != GCOMP_OK) {
    return s;
  }
  state_reset(e);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
