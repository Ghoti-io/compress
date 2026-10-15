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
 * @file xz_decode.c
 *
 * The xz decoder: a state machine over the container, with a chain of
 * decoders (LZMA2, then the filters in reverse) for each block.
 *
 * ## What is checked, and where
 *
 * Everything the container says twice is compared: the block header's CRC,
 * the sizes it may declare against what was read, the check against the
 * decoded bytes, the index against the blocks that were actually seen, the
 * index's CRC, the footer's CRC, its backward size against the index that was
 * read, and its stream flags against the header's. Padding must be zero. A
 * reader that checks one copy and trusts the other is a reader that a file
 * with two different answers can steer.
 *
 * Streams may follow one another, with zero padding in multiples of four
 * between them, and are read as one, as `xz -d` does. Anything else after a
 * stream is an error.
 *
 * ## Limits
 *
 * `limits.max_window_bytes` bounds the dictionary a block may name: it is the
 * only thing a header can make this decoder allocate that the caller did not
 * choose. The block's LZMA2 stage is given that dictionary as its window, so a
 * match reaching farther back than the header said is corrupt, as in liblzma.
 *
 * ## Checks it will not verify
 *
 * The format reserves twelve more integrity checks than the four implemented.
 * A stream that names one is refused rather than decoded without its check,
 * since a caller who asked for a stream to be verified cannot be told it was
 * not.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/parallel_block.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "../../filters/filter_internal.h"
#include "../../filters/filter_transform.h"
#include "../lzma/lzma_internal.h"
#include "xz_chain.h"
#include "xz_internal.h"

#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/xz.h>

#include <stdio.h>
#include <string.h>

typedef enum {
  D_STREAM_HDR,
  D_BLOCK_START,
  D_BLOCK_HDR,
  D_BLOCK_DATA,
  D_BLOCK_PAD,
  D_BLOCK_CHECK,
  D_INDEX,
  D_FOOTER,
  D_STREAM_PAD,
  D_DEFER,  /* threads: an error is held until the blocks before it are out */
  D_FAILED
} xz_phase_t;

struct xz_mt;

typedef struct {
  gcomp_decoder_t * pub;
  gcomp_registry_t * registry;
  const gcomp_allocator_t * alloc;
  uint64_t max_out, max_mem, max_window, max_ratio;

  xz_phase_t phase;
  gcomp_status_t err;
  uint8_t buf[XZ_BLOCK_HEADER_MAX];
  size_t have, want;

  unsigned check;
  uint8_t flags[2];
  unsigned streams_done;
  size_t pad_run;

  /* the block being read */
  size_t hdr_size;
  int has_comp, has_unc;
  uint64_t decl_comp, decl_unc;
  xz_filter_t filters[XZ_FILTERS_MAX - 1u];
  unsigned nfilters;
  uint8_t dict_prop;
  xz_chain_t chain;
  int chain_live, chain_finishing;
  uint64_t comp_in, unc_out;
  xz_check_t chk;
  size_t pad_left;

  /* the blocks seen so far in this stream */
  uint64_t * rec;
  size_t rec_n, rec_cap;

  /* the index being read */
  uint64_t idx_len, idx_count, idx_i;
  uint32_t idx_crc;
  int idx_state;
  xz_vli_t vli;

  uint64_t in_total, produced;

  struct xz_mt * mt; /* threads: blocks decoded by workers; NULL: in line */
} xz_dec_t;

#define DFAIL(st, status, ...)                                                 \
  ((st)->phase = D_FAILED,                                                     \
      (st)->err = gcomp_decoder_set_error((st)->pub, (status), __VA_ARGS__))

static void block_release(xz_dec_t * st) {
  if (st->chain_live) {
    xz_chain_clear(&st->chain);
    st->chain_live = 0;
  }
  st->chain_finishing = 0;
}

static void mt_reset(xz_dec_t * st);
static void mt_destroy(xz_dec_t * st);
static gcomp_status_t mt_create(xz_dec_t * st, uint64_t threads);

static void state_restart(xz_dec_t * st) {
  block_release(st);
  mt_reset(st);
  st->phase = D_STREAM_HDR;
  st->err = GCOMP_OK;
  st->have = 0;
  st->want = XZ_STREAM_HEADER_SIZE;
  st->streams_done = 0;
  st->pad_run = 0;
  st->rec_n = 0;
  st->in_total = st->produced = 0;
}

gcomp_status_t xz_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  xz_dec_t * st;
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
        decoder, GCOMP_ERR_MEMORY, "xz: out of memory");
  }
  st->pub = decoder;
  st->registry = registry;
  st->alloc = alloc;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_mem = gcomp_limits_read_memory_max(options, 0);
  st->max_window = gcomp_limits_read_window_max(options, GCOMP_XZ_DEFAULT_WINDOW);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_XZ_MAX_EXPANSION_RATIO);
  xz_chain_init(&st->chain, alloc, 0);
  decoder->method_state = st;
  {
    uint64_t threads = 1;
    if (options) {
      (void)gcomp_options_get_uint64(options, "threads.count", &threads);
    }
    if (threads > 1 && st->max_mem == 0) {
      const gcomp_status_t ms = mt_create(st, threads);
      if (ms != GCOMP_OK) {
        mt_destroy(st);
        gcomp_free(alloc, st);
        decoder->method_state = NULL;
        return gcomp_decoder_set_error(
            decoder, ms, "xz: could not start the worker threads");
      }
    }
  }
  state_restart(st);
  return GCOMP_OK;
}

void xz_decoder_destroy(gcomp_decoder_t * decoder) {
  xz_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  block_release(st);
  mt_destroy(st);
  gcomp_free(st->alloc, st->rec);
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

gcomp_status_t xz_decoder_reset(gcomp_decoder_t * decoder) {
  xz_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  state_restart(st);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* ---- reading fixed-size pieces ------------------------------------------ */

static int collect(xz_dec_t * st, const uint8_t ** in, const uint8_t * end) {
  size_t n = st->want - st->have;
  if ((size_t)(end - *in) < n) {
    n = (size_t)(end - *in);
  }
  memcpy(st->buf + st->have, *in, n);
  st->have += n;
  *in += n;
  st->in_total += n;
  return st->have == st->want;
}

/* One variable-length integer out of a buffer; false if it is not one. */
static int buf_vli(const uint8_t * b, size_t * p, size_t limit, uint64_t * v) {
  xz_vli_t x = {0, 0};
  while (*p < limit) {
    const int r = xz_vli_feed(&x, b[(*p)++]);
    if (r < 0) {
      return 0;
    }
    if (r == 1) {
      *v = x.value;
      return 1;
    }
  }
  return 0;
}

/* ---- stream header and footer ------------------------------------------- */

static gcomp_status_t parse_stream_header(xz_dec_t * st) {
  static const uint8_t magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
  if (memcmp(st->buf, magic, 6) != 0) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, st->streams_done
            ? "xz: data after the last stream is not another stream"
            : "xz: not an xz stream (the magic bytes are wrong)");
  }
  if (xz_crc32(st->buf + 6, 2) != xz_get_le32(st->buf + 8)) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: stream header CRC mismatch");
  }
  if (st->buf[6] != 0 || (st->buf[7] & 0xF0u) != 0) {
    return DFAIL(st, GCOMP_ERR_UNSUPPORTED,
        "xz: stream flags use bits this decoder does not know");
  }
  st->check = st->buf[7] & 0x0Fu;
  if (!xz_check_known(st->check)) {
    return DFAIL(st, GCOMP_ERR_UNSUPPORTED,
        "xz: integrity check type %u is not supported", st->check);
  }
  st->flags[0] = st->buf[6];
  st->flags[1] = st->buf[7];
  st->rec_n = 0;
  return GCOMP_OK;
}

static gcomp_status_t parse_footer(xz_dec_t * st, uint64_t index_total) {
  const uint8_t * b = st->buf;
  if (xz_crc32(b + 4, 6) != xz_get_le32(b)) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: stream footer CRC mismatch");
  }
  if (((uint64_t)xz_get_le32(b + 4) + 1u) * 4u != index_total) {
    return DFAIL(st, GCOMP_ERR_CORRUPT,
        "xz: the footer's backward size is not the index's size");
  }
  if (b[8] != st->flags[0] || b[9] != st->flags[1]) {
    return DFAIL(st, GCOMP_ERR_CORRUPT,
        "xz: the footer's stream flags differ from the header's");
  }
  if (b[10] != 'Y' || b[11] != 'Z') {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: stream footer magic is wrong");
  }
  return GCOMP_OK;
}

/* ---- the block header --------------------------------------------------- */

/* How an instruction set wants a converter's start offset aligned. */
static uint32_t bcj_alignment(unsigned id) {
  switch (id) {
  case FILTER_ARCH_POWERPC:
  case FILTER_ARCH_ARM:
  case FILTER_ARCH_SPARC:
  case FILTER_ARCH_ARM64:
    return 4;
  case FILTER_ARCH_ARMTHUMB:
  case FILTER_ARCH_RISCV:
    return 2;
  case FILTER_ARCH_IA64:
    return 16;
  default:
    return 1;
  }
}

/* The decoders of a block: LZMA2 first, which is what reads the block's
 * bytes, then the filters in the reverse of the order they were applied. */
static gcomp_status_t make_dec_chain(gcomp_registry_t * registry,
    const gcomp_allocator_t * alloc, const xz_filter_t * filters,
    unsigned nfilters, uint8_t dict_prop, uint64_t max_mem, xz_chain_t * chain) {
  gcomp_options_t * o = NULL;
  gcomp_decoder_t * stage = NULL;
  gcomp_status_t s;
  unsigned i;
  const uint64_t dict = xz_dict_size(dict_prop);
  xz_chain_init(chain, alloc, 0);
  s = gcomp_options_create(&o);
  if (s == GCOMP_OK) {
    s = gcomp_options_set_uint64(o, "limits.max_window_bytes", dict);
  }
  if (s == GCOMP_OK && max_mem != 0) {
    s = gcomp_options_set_uint64(o, "limits.max_memory_bytes", max_mem);
  }
  if (s == GCOMP_OK) {
    s = gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0);
  }
  if (s == GCOMP_OK) {
    s = gcomp_decoder_create(registry, "lzma2", o, &stage);
  }
  gcomp_options_destroy(o);
  if (s == GCOMP_OK) {
    s = xz_chain_add(chain, stage);
  }
  for (i = nfilters; s == GCOMP_OK && i-- > 0;) {
    const xz_filter_t * f = &filters[i];
    const char * method;
    o = NULL;
    stage = NULL;
    s = gcomp_options_create(&o);
    if (s != GCOMP_OK) {
      break;
    }
    if (f->id == XZ_FILTER_DELTA) {
      method = "delta";
      s = gcomp_options_set_int64(o, "delta.distance", (int64_t)f->arg);
    }
    else {
      method = "bcj";
      s = gcomp_options_set_string(o, "bcj.arch", filter_arch_name(f->id));
      if (s == GCOMP_OK) {
        s = gcomp_options_set_uint64(o, "bcj.start_offset", f->arg);
      }
    }
    if (s == GCOMP_OK) {
      s = gcomp_decoder_create(registry, method, o, &stage);
    }
    gcomp_options_destroy(o);
    if (s == GCOMP_OK) {
      s = xz_chain_add(chain, stage);
    }
  }
  if (s != GCOMP_OK) {
    xz_chain_clear(chain);
  }
  return s;
}

static gcomp_status_t build_decode_chain(xz_dec_t * st) {
  gcomp_status_t s = make_dec_chain(st->registry, st->alloc, st->filters,
      st->nfilters, st->dict_prop, st->max_mem, &st->chain);
  st->chain_live = 1;
  if (s != GCOMP_OK) {
    block_release(st);
    return DFAIL(st, s == GCOMP_ERR_MEMORY ? s : GCOMP_ERR_UNSUPPORTED,
        "xz: could not make the decoders for this block (lzma2 and the filters "
        "must be registered)");
  }
  return GCOMP_OK;
}

static gcomp_status_t parse_block_header(xz_dec_t * st) {
  const uint8_t * b = st->buf;
  const size_t hs = st->hdr_size;
  size_t p = 2, nf, i;
  unsigned flags;
  if (xz_crc32(b, hs - 4u) != xz_get_le32(b + hs - 4u)) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: block header CRC mismatch");
  }
  flags = b[1];
  if (flags & 0x3Cu) {
    return DFAIL(st, GCOMP_ERR_UNSUPPORTED,
        "xz: block flags use bits this decoder does not know");
  }
  nf = (size_t)(flags & 3u) + 1u;
  st->has_comp = (flags & 0x40u) != 0;
  st->has_unc = (flags & 0x80u) != 0;
  if (st->has_comp &&
      (!buf_vli(b, &p, hs - 4u, &st->decl_comp) || st->decl_comp == 0)) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: block header's compressed size");
  }
  if (st->has_unc && !buf_vli(b, &p, hs - 4u, &st->decl_unc)) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: block header's uncompressed size");
  }
  st->nfilters = 0;
  for (i = 0; i < nf; i++) {
    uint64_t id, psize;
    const uint8_t * props;
    if (!buf_vli(b, &p, hs - 4u, &id) || !buf_vli(b, &p, hs - 4u, &psize) ||
        psize > hs - 4u - p) {
      return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: block header's filter flags");
    }
    props = b + p;
    p += (size_t)psize;
    if (i + 1 == nf) {
      if (id != XZ_FILTER_LZMA2) {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "xz: the last filter of a block must be LZMA2");
      }
      if (psize != 1 || props[0] > XZ_DICT_PROP_MAX) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: LZMA2 filter properties");
      }
      st->dict_prop = props[0];
    }
    else if (id == XZ_FILTER_DELTA) {
      if (psize != 1) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: delta filter properties");
      }
      st->filters[st->nfilters].id = XZ_FILTER_DELTA;
      st->filters[st->nfilters].arg = (uint32_t)props[0] + 1u;
      st->nfilters++;
    }
    else if (id < 0x100 && filter_bcj_known((unsigned)id)) {
      uint32_t start = 0;
      if (psize != 0 && psize != 4) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: branch filter properties");
      }
      if (psize == 4) {
        start = xz_get_le32(props);
        if (start & (bcj_alignment((unsigned)id) - 1u)) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "xz: a branch filter's start offset is not aligned");
        }
      }
      st->filters[st->nfilters].id = (unsigned)id;
      st->filters[st->nfilters].arg = start;
      st->nfilters++;
    }
    else {
      return DFAIL(st, GCOMP_ERR_UNSUPPORTED,
          "xz: filter id 0x%llx is not supported", (unsigned long long)id);
    }
  }
  for (; p < hs - 4u; p++) {
    if (b[p] != 0) {
      return DFAIL(
          st, GCOMP_ERR_CORRUPT, "xz: block header padding is not zero");
    }
  }
  if (xz_dict_size(st->dict_prop) > st->max_window) {
    return DFAIL(st, GCOMP_ERR_LIMIT,
        "xz: the block's dictionary exceeds limits.max_window_bytes");
  }
  if (st->has_unc && st->max_out != 0 && st->decl_unc > st->max_out) {
    return DFAIL(st, GCOMP_ERR_LIMIT,
        "xz: the block's declared size exceeds limits.max_output_bytes");
  }
  if (!st->mt) {
    gcomp_status_t s = build_decode_chain(st);
    if (s != GCOMP_OK) {
      return s;
    }
  }
  xz_check_init(&st->chk, st->check);
  st->comp_in = st->unc_out = 0;
  st->chain_finishing = 0;
  return GCOMP_OK;
}

/* ---- block data --------------------------------------------------------- */

/* Account for what a stage wrote into the caller's output. */
static gcomp_status_t note_output(
    xz_dec_t * st, gcomp_buffer_t * output, size_t before) {
  size_t n = output->used - before;
  if (n == 0) {
    return GCOMP_OK;
  }
  if (st->max_out != 0 && st->produced + n > st->max_out) {
    const size_t keep = (size_t)(st->max_out - st->produced);
    output->used = before + keep;
    st->produced = st->max_out;
    return DFAIL(
        st, GCOMP_ERR_LIMIT, "xz: decompressed output exceeds the limit");
  }
  xz_check_update(&st->chk, (const uint8_t *)output->data + before, n);
  st->produced += n;
  st->unc_out += n;
  if (st->has_unc && st->unc_out > st->decl_unc) {
    return DFAIL(st, GCOMP_ERR_CORRUPT,
        "xz: the block holds more than its header says");
  }
  return GCOMP_OK;
}

static gcomp_status_t chain_failed(xz_dec_t * st, gcomp_status_t s) {
  return DFAIL(st, s, "xz: %s",
      st->chain.detail[0] ? st->chain.detail : "a block's data is invalid");
}

/* Returns ::GCOMP_OK with *blocked set when it needs input or output room. */
static gcomp_status_t block_data(xz_dec_t * st, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * output, int * blocked) {
  for (;;) {
    gcomp_status_t s;
    size_t before;
    if (!st->chain_finishing) {
      gcomp_buffer_t view;
      size_t avail = (size_t)(end - *in), consumed;
      if (st->has_comp && (uint64_t)avail > st->decl_comp - st->comp_in) {
        avail = (size_t)(st->decl_comp - st->comp_in);
      }
      view.data = (void *)*in;
      view.size = avail;
      view.used = 0;
      before = output->used;
      s = xz_chain_update(&st->chain, &view, output);
      consumed = view.used;
      *in += consumed;
      st->comp_in += consumed;
      st->in_total += consumed;
      if (note_output(st, output, before) != GCOMP_OK) {
        return st->err;
      }
      if (s != GCOMP_OK) {
        return chain_failed(st, s);
      }
      if (xz_chain_first_done(&st->chain)) {
        st->chain_finishing = 1;
      }
      else {
        if (st->has_comp && st->comp_in == st->decl_comp) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "xz: the block's compressed size ends before its LZMA2 data does");
        }
        if (consumed == 0 && output->used == before) {
          *blocked = 1;
          return GCOMP_OK;
        }
        continue;
      }
    }
    before = output->used;
    s = xz_chain_finish(&st->chain, output);
    if (note_output(st, output, before) != GCOMP_OK) {
      return st->err;
    }
    if (s == GCOMP_ERR_LIMIT) {
      *blocked = 1;
      return GCOMP_OK;
    }
    if (s != GCOMP_OK) {
      return chain_failed(st, s);
    }
    if (st->has_comp && st->comp_in != st->decl_comp) {
      return DFAIL(st, GCOMP_ERR_CORRUPT,
          "xz: the block's compressed size is not what its header says");
    }
    if (st->has_unc && st->unc_out != st->decl_unc) {
      return DFAIL(st, GCOMP_ERR_CORRUPT,
          "xz: the block's uncompressed size is not what its header says");
    }
    block_release(st);
    st->pad_left = (4u - (size_t)(st->comp_in & 3u)) & 3u;
    st->phase = D_BLOCK_PAD;
    return GCOMP_OK;
  }
}

static gcomp_status_t push_record(xz_dec_t * st, uint64_t unpadded, uint64_t unc) {
  if (st->rec_n == st->rec_cap) {
    size_t cap = st->rec_cap ? st->rec_cap * 2u : 16u;
    uint64_t * grown;
    if (gcomp_limits_check_memory(cap * 2u * sizeof(uint64_t), st->max_mem) !=
        GCOMP_OK) {
      return DFAIL(st, GCOMP_ERR_LIMIT,
          "xz: the list of blocks exceeds limits.max_memory_bytes");
    }
    grown = gcomp_realloc(st->alloc, st->rec, cap * 2u * sizeof(uint64_t));
    if (!grown) {
      return DFAIL(st, GCOMP_ERR_MEMORY, "xz: out of memory for the index");
    }
    st->rec = grown;
    st->rec_cap = cap;
  }
  st->rec[2u * st->rec_n] = unpadded;
  st->rec[2u * st->rec_n + 1u] = unc;
  st->rec_n++;
  return GCOMP_OK;
}

/* ---- threads: blocks decoded by workers -------------------------------- */

/* With `threads.count` above one the container is still read here, one
 * thread, but a block's data is not decoded here. Where a block's LZMA2 data
 * ends is found without decoding it, by reading the chunk headers (a chunk
 * says how many bytes it holds and how many it makes), and the bytes are
 * copied to a job that a worker decodes into a buffer of exactly the size the
 * chunks add up to. Jobs come back in the order they went, and a finished one
 * is handed to the caller as there is room. The expansion ratio is checked as
 * output is delivered, against all the input read so far, which can only be
 * more lenient than one thread's; a block is not refused for its own ratio,
 * which an earlier block may have paid for.
 *
 * What stays as it was: every check the serial decoder makes is made, by the
 * same code or the same comparison, and an error is reported after the output
 * of every block before it. A container error found here while blocks are
 * still out is held (D_DEFER) until they are in, so that a stream gets the
 * verdict it would get on one thread. What differs, and is documented: a block
 * that fails, or a stream that is truncated inside a block, delivers none of
 * that block's output where one thread would have delivered some; and a block
 * is held whole in memory, compressed and decoded, so memory is about the
 * blocks in flight. A memory limit turns threads off, since limiting memory
 * and buffering blocks are at odds. */

#define XZ_MT_BLOCK_MAX (1ull << 30)

typedef struct xz_mjob_s {
  gcomp_block_job_t base; /* first */
  gcomp_registry_t * registry;
  const gcomp_allocator_t * alloc;
  struct xz_mjob_s * next;
  xz_filter_t filters[XZ_FILTERS_MAX - 1u];
  unsigned nfilters;
  uint8_t dict_prop;
  unsigned check;
  uint8_t * in; /* the block's LZMA2 data */
  size_t in_n, in_cap;
  uint8_t * out;
  uint64_t unc;
  uint8_t sum[32];    /* the check of what was decoded */
  uint8_t expect[32]; /* the check the file holds */
  uint64_t unpadded;
  int sealed;
  size_t pos; /* how much of the output has gone to the caller */
  gcomp_status_t status;
  char detail[256];
} xz_mjob_t;

typedef struct xz_mt {
  gcomp_parallel_block_ctx_t * pb;
  xz_mjob_t ** all;
  unsigned made, max_jobs, max_inflight, inflight;
  xz_mjob_t * free_list;
  xz_mjob_t * cur;     /* being filled from the input */
  xz_mjob_t * sealing; /* out for decoding, its check not yet read */
  xz_mjob_t * pend;    /* decoded, going out */
  /* the LZMA2 chunk walker */
  int w_state; /* 0 control, 1 chunk header, 2 chunk body */
  uint8_t w_hdr[6];
  unsigned w_have, w_need;
  uint64_t w_body, w_unc;
  int fatal; /* a failure from a job or the caller's limits: not to be held */
  /* an error held until the blocks before it are out */
  gcomp_status_t d_status;
  char d_msg[GCOMP_ERROR_DETAIL_MAX];
} xz_mt_t;

static void mjob_free(xz_dec_t * st, xz_mjob_t * j) {
  if (j) {
    gcomp_free(st->alloc, j->in);
    gcomp_free(st->alloc, j->out);
    gcomp_free(st->alloc, j);
  }
}

static void mjob_give(xz_dec_t * st, xz_mjob_t * j) {
  xz_mt_t * m = st->mt;
  gcomp_free(st->alloc, j->out);
  j->out = NULL;
  j->in_n = 0;
  j->pos = 0;
  j->sealed = 0;
  j->next = m->free_list;
  m->free_list = j;
}

/* The worker: decode one block's data and make its check. */
static int mjob_run(void * ctx) {
  xz_mjob_t * j = ctx;
  xz_chain_t chain;
  gcomp_buffer_t view, ob;
  xz_check_t chk;
  gcomp_status_t s = make_dec_chain(j->registry, j->alloc, j->filters,
      j->nfilters, j->dict_prop, 0, &chain);
  j->detail[0] = '\0';
  if (s != GCOMP_OK) {
    j->status = s == GCOMP_ERR_MEMORY ? s : GCOMP_ERR_UNSUPPORTED;
    snprintf(j->detail, sizeof(j->detail), "could not make the decoders for "
        "this block (lzma2 and the filters must be registered)");
    return (int)j->status;
  }
  view.data = j->in;
  view.size = j->in_n;
  view.used = 0;
  ob.data = j->out;
  ob.size = (size_t)j->unc;
  ob.used = 0;
  s = xz_chain_update(&chain, &view, &ob);
  if (s == GCOMP_OK && !xz_chain_first_done(&chain)) {
    s = GCOMP_ERR_CORRUPT; /* the data stopped before the LZMA2 stream did */
  }
  if (s == GCOMP_OK) {
    s = xz_chain_finish(&chain, &ob);
    if (s == GCOMP_ERR_LIMIT) {
      s = GCOMP_ERR_CORRUPT; /* it made more than the chunks said */
    }
  }
  if (s == GCOMP_OK && (view.used != j->in_n || ob.used != j->unc)) {
    s = GCOMP_ERR_CORRUPT;
  }
  if (s != GCOMP_OK) {
    snprintf(j->detail, sizeof(j->detail), "%s",
        chain.detail[0] ? chain.detail : "a block's data is invalid");
  }
  else {
    xz_check_init(&chk, j->check);
    xz_check_update(&chk, j->out, (size_t)j->unc);
    xz_check_final(&chk, j->sum);
  }
  xz_chain_clear(&chain);
  j->status = s;
  return (int)s;
}

static void mt_destroy(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  unsigned i;
  if (!m) {
    return;
  }
  if (m->pb) {
    while (m->inflight > 0) {
      gcomp_block_job_t * base = NULL;
      (void)gcomp_parallel_block_get_result(m->pb, &base);
      if (!base) {
        break;
      }
      m->inflight--;
    }
    gcomp_parallel_block_wait(m->pb);
    gcomp_parallel_block_destroy(m->pb);
  }
  for (i = 0; i < m->made; i++) {
    mjob_free(st, m->all[i]);
  }
  gcomp_free(st->alloc, m->all);
  gcomp_free(st->alloc, m);
  st->mt = NULL;
}

/* Back to nothing out, as for a new stream. */
static void mt_reset(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  if (!m) {
    return;
  }
  while (m->inflight > 0) {
    gcomp_block_job_t * base = NULL;
    (void)gcomp_parallel_block_get_result(m->pb, &base);
    if (!base) {
      break;
    }
    m->inflight--;
    mjob_give(st, (xz_mjob_t *)base);
  }
  if (m->pend) {
    mjob_give(st, m->pend);
  }
  if (m->cur) {
    mjob_give(st, m->cur);
  }
  m->pend = m->cur = m->sealing = NULL;
  m->w_state = 0;
  m->fatal = 0;
}

static gcomp_status_t mt_create(xz_dec_t * st, uint64_t threads) {
  xz_mt_t * m;
  gcomp_parallel_block_config_t cfg;
  if (threads > 64) {
    threads = 64;
  }
  m = gcomp_calloc(st->alloc, 1, sizeof(*m));
  if (!m) {
    return GCOMP_ERR_MEMORY;
  }
  st->mt = m;
  m->max_inflight = (unsigned)threads;
  m->max_jobs = m->max_inflight + 2u;
  m->all = gcomp_calloc(st->alloc, m->max_jobs, sizeof(*m->all));
  if (!m->all) {
    return GCOMP_ERR_MEMORY;
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.allocator = st->alloc;
  cfg.num_threads = (uint32_t)threads;
  cfg.max_in_flight = (uint32_t)threads;
  return gcomp_parallel_block_create(&cfg, &m->pb);
}

static xz_mjob_t * mt_take(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  xz_mjob_t * j = m->free_list;
  if (j) {
    m->free_list = j->next;
    return j;
  }
  if (m->made == m->max_jobs) {
    return NULL;
  }
  j = gcomp_calloc(st->alloc, 1, sizeof(*j));
  if (!j) {
    return NULL;
  }
  m->all[m->made++] = j;
  return j;
}

/* A coded block comes back. It is for the caller next; if its check is wrong
 * that is said once the caller has all of its data, as it would be on one
 * thread. The pending slot is empty. */
static gcomp_status_t mt_collect(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  gcomp_block_job_t * base = NULL;
  xz_mjob_t * j;
  gcomp_status_t s = gcomp_parallel_block_get_result(m->pb, &base);
  if (!base) {
    m->fatal = 1;
    return DFAIL(st, s == GCOMP_OK ? GCOMP_ERR_INTERNAL : s,
        "xz: the block queue failed");
  }
  j = (xz_mjob_t *)base;
  m->inflight--;
  if (j->status != GCOMP_OK) {
    s = j->status;
    m->fatal = 1;
    DFAIL(st, s, "xz: %s", j->detail);
    mjob_give(st, j);
    return st->err;
  }
  if (xz_check_size(j->check) != 0 &&
      memcmp(j->sum, j->expect, xz_check_size(j->check)) != 0) {
    j->status = GCOMP_ERR_CORRUPT; /* said after its data */
  }
  s = push_record(st, j->unpadded, j->unc);
  if (s != GCOMP_OK) {
    m->fatal = 1;
    mjob_give(st, j);
    return s;
  }
  j->pos = 0;
  m->pend = j;
  return GCOMP_OK;
}

/* Send the pending block to the caller. 1: all of it has gone. */
static int mt_deliver(xz_dec_t * st, gcomp_buffer_t * out, gcomp_status_t * err) {
  xz_mt_t * m = st->mt;
  xz_mjob_t * j = m->pend;
  size_t room = out->size - out->used, n = (size_t)j->unc - j->pos;
  *err = GCOMP_OK;
  if (n > room) {
    n = room;
  }
  if (st->max_out != 0 && st->produced + n > st->max_out) {
    n = (size_t)(st->max_out - st->produced);
    memcpy((uint8_t *)out->data + out->used, j->out + j->pos, n);
    out->used += n;
    st->produced += n;
    m->fatal = 1;
    *err = DFAIL(
        st, GCOMP_ERR_LIMIT, "xz: decompressed output exceeds the limit");
    return 1;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, j->out + j->pos, n);
    out->used += n;
    j->pos += n;
    st->produced += n;
  }
  if (j->pos < j->unc) {
    return 0;
  }
  m->pend = NULL;
  if (j->status != GCOMP_OK) {
    m->fatal = 1;
    *err = DFAIL(st, j->status,
        "xz: the integrity check does not match the decoded data");
  }
  mjob_give(st, j);
  return 1;
}

/* The oldest block that may come back: not the one whose check is still to
 * be read. */
static int mt_collectable(const xz_mt_t * m) {
  return m->inflight > (m->sealing ? 1u : 0u);
}

/* Pump: deliver what is pending, take what is ready (or, with `wait`, all of
 * it). Returns an error, or sets *blocked when the caller's output is full. */
static gcomp_status_t mt_service(xz_dec_t * st, gcomp_buffer_t * out, int wait,
    int * blocked) {
  xz_mt_t * m = st->mt;
  *blocked = 0;
  for (;;) {
    gcomp_status_t s;
    if (m->pend) {
      if (!mt_deliver(st, out, &s)) {
        *blocked = 1;
        return GCOMP_OK;
      }
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (mt_collectable(m) &&
        (wait || gcomp_parallel_block_result_ready(m->pb))) {
      s = mt_collect(st);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    return GCOMP_OK;
  }
}

/* A job to fill. *blocked: wait for the caller to take output first. */
static gcomp_status_t mt_acquire(xz_dec_t * st, gcomp_buffer_t * out, int * blocked) {
  xz_mt_t * m = st->mt;
  *blocked = 0;
  for (;;) {
    gcomp_status_t s;
    if (m->inflight < m->max_inflight) {
      m->cur = mt_take(st);
      if (m->cur) {
        m->w_state = 0;
        m->w_unc = 0;
        m->cur->in_n = 0;
        return GCOMP_OK;
      }
    }
    if (m->inflight == 0 && !m->pend) {
      m->fatal = 1;
      return DFAIL(st, GCOMP_ERR_MEMORY, "xz: out of memory for a block");
    }
    s = mt_service(st, out, 1, blocked);
    if (s != GCOMP_OK || *blocked) {
      return s;
    }
  }
}

static gcomp_status_t job_put(xz_dec_t * st, xz_mjob_t * j, const uint8_t * p, size_t n) {
  if (j->in_n + n > j->in_cap) {
    size_t cap = j->in_cap ? j->in_cap * 2u : 65536u;
    uint8_t * grown;
    while (cap < j->in_n + n) {
      cap *= 2u;
    }
    grown = gcomp_realloc(st->alloc, j->in, cap);
    if (!grown) {
      st->mt->fatal = 1;
      return DFAIL(st, GCOMP_ERR_MEMORY, "xz: out of memory for a block");
    }
    j->in = grown;
    j->in_cap = cap;
  }
  memcpy(j->in + j->in_n, p, n);
  j->in_n += n;
  return GCOMP_OK;
}

/* Send the block that has been read to a worker. */
static gcomp_status_t mt_submit(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  xz_mjob_t * j = m->cur;
  gcomp_status_t s;
  if (j->unc > XZ_MT_BLOCK_MAX) {
    return DFAIL(st, GCOMP_ERR_LIMIT,
        "xz: a block of this size cannot be decoded with threads.count above "
        "one; use one thread or a smaller block size");
  }
  j->out = gcomp_malloc(st->alloc, j->unc ? (size_t)j->unc : 1u);
  if (!j->out) {
    m->fatal = 1;
    return DFAIL(st, GCOMP_ERR_MEMORY, "xz: out of memory for a block");
  }
  j->registry = st->registry;
  j->alloc = st->alloc;
  memcpy(j->filters, st->filters, sizeof(j->filters));
  j->nfilters = st->nfilters;
  j->dict_prop = st->dict_prop;
  j->check = st->check;
  j->unpadded = (uint64_t)st->hdr_size + j->in_n + xz_check_size(st->check);
  j->sealed = 0;
  j->status = GCOMP_OK;
  j->base.input = j->in;
  j->base.input_size = j->in_n;
  s = gcomp_parallel_block_try_submit(m->pb, j, mjob_run);
  if (s != GCOMP_OK) {
    m->fatal = 1;
    return DFAIL(st, s, "xz: could not queue a block");
  }
  m->inflight++;
  m->sealing = j;
  m->cur = NULL;
  return GCOMP_OK;
}

/* Read the block's LZMA2 data from the input, finding its end from the chunk
 * headers, and copy it to the job. *blocked: more input is wanted (or output
 * room, if the job could not be had). */
static gcomp_status_t mt_block_data(xz_dec_t * st, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * output, int * blocked) {
  xz_mt_t * m = st->mt;
  gcomp_status_t s;
  *blocked = 0;
  if (!m->cur) {
    s = mt_acquire(st, output, blocked);
    if (s != GCOMP_OK || *blocked) {
      return s;
    }
  }
  while (*in < end) {
    xz_mjob_t * j = m->cur;
    size_t avail = (size_t)(end - *in), n;
    if (m->w_state == 2) {
      n = avail < m->w_body ? avail : (size_t)m->w_body;
      s = job_put(st, j, *in, n);
      if (s != GCOMP_OK) {
        return s;
      }
      *in += n;
      m->w_body -= n;
      st->in_total += n;
      st->comp_in += n;
      if (m->w_body == 0) {
        m->w_state = 0;
      }
    }
    else if (m->w_state == 0) {
      const uint8_t c = **in;
      s = job_put(st, j, *in, 1);
      if (s != GCOMP_OK) {
        return s;
      }
      (*in)++;
      st->in_total++;
      st->comp_in++;
      m->w_hdr[0] = c;
      m->w_have = 0;
      if (c == 0) {
        /* the end of the LZMA2 stream, and so of the block's data */
        j->unc = m->w_unc;
        if (st->has_comp && st->comp_in != st->decl_comp) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "xz: the block's compressed size is not what its header says");
        }
        if (st->has_unc && j->unc != st->decl_unc) {
          return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: the block's uncompressed "
                                              "size is not what its header says");
        }
        s = mt_submit(st);
        if (s != GCOMP_OK) {
          return s;
        }
        st->unc_out = j->unc;
        st->pad_left = (4u - (size_t)(st->comp_in & 3u)) & 3u;
        st->phase = D_BLOCK_PAD;
        return GCOMP_OK;
      }
      if (c == 1 || c == 2) {
        m->w_need = 2;
      }
      else if (c >= 0x80) {
        m->w_need = c >= 0xC0 ? 5u : 4u;
      }
      else {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "xz: a block's LZMA2 data has an invalid chunk header");
      }
      m->w_state = 1;
    }
    else {
      n = m->w_need - m->w_have;
      if (n > avail) {
        n = avail;
      }
      memcpy(m->w_hdr + 1 + m->w_have, *in, n);
      s = job_put(st, j, *in, n);
      if (s != GCOMP_OK) {
        return s;
      }
      *in += n;
      m->w_have += (unsigned)n;
      st->in_total += n;
      st->comp_in += n;
      if (m->w_have == m->w_need) {
        if (m->w_hdr[0] <= 2) {
          m->w_body = (((uint64_t)m->w_hdr[1] << 8) | m->w_hdr[2]) + 1u;
          m->w_unc += m->w_body;
        }
        else {
          m->w_unc += ((((uint64_t)m->w_hdr[0] & 0x1Fu) << 16) |
                          ((uint64_t)m->w_hdr[1] << 8) | m->w_hdr[2]) + 1u;
          m->w_body = (((uint64_t)m->w_hdr[3] << 8) | m->w_hdr[4]) + 1u;
        }
        m->w_state = 2;
      }
    }
    if (st->has_comp && st->comp_in > st->decl_comp) {
      return DFAIL(st, GCOMP_ERR_CORRUPT,
          "xz: the block's compressed size ends before its LZMA2 data does");
    }
  }
  *blocked = 1;
  return GCOMP_OK;
}

/* The block whose data was read has its check read: hand it to the job. */
static void mt_seal(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  memcpy(m->sealing->expect, st->buf, st->want);
  m->sealing->sealed = 1;
  m->sealing = NULL;
}

/* An error found while blocks are out waits for them. */
static void mt_defer(xz_dec_t * st) {
  xz_mt_t * m = st->mt;
  m->d_status = st->pub->last_error;
  snprintf(m->d_msg, sizeof(m->d_msg), "%s", st->pub->error_detail);
  st->pub->last_error = GCOMP_OK;
  st->pub->error_detail[0] = '\0';
  st->phase = D_DEFER;
}

static int mt_busy(const xz_mt_t * m) {
  return m->inflight > 0 || m->pend != NULL;
}

/* ---- the driver --------------------------------------------------------- */

static void index_begin(xz_dec_t * st) {
  st->idx_len = 1;
  st->idx_crc = gcomp_crc32_update(GCOMP_CRC32_INIT, (const uint8_t *)"\0", 1);
  st->idx_state = 0;
  st->vli.count = 0;
  st->have = 0;
}

static gcomp_status_t index_bytes(xz_dec_t * st, const uint8_t ** in,
    const uint8_t * end, int * need_input) {
  while (*in < end) {
    const uint8_t byte = **in;
    int r;
    if (st->idx_state == 4) {
      st->buf[st->have++] = byte;
      (*in)++;
      st->in_total++;
      if (st->have < 4) {
        continue;
      }
      if (gcomp_crc32_finalize(st->idx_crc) != xz_get_le32(st->buf)) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: index CRC mismatch");
      }
      st->idx_len += 4;
      st->phase = D_FOOTER;
      st->have = 0;
      st->want = XZ_STREAM_FOOTER_SIZE;
      return GCOMP_OK;
    }
    (*in)++;
    st->in_total++;
    st->idx_crc = gcomp_crc32_update(st->idx_crc, &byte, 1);
    st->idx_len++;
    switch (st->idx_state) {
    case 0:
      r = xz_vli_feed(&st->vli, byte);
      if (r < 0) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: index record count");
      }
      if (r == 1) {
        if (st->vli.value != (uint64_t)st->rec_n) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "xz: the index lists a different number of blocks than were read");
        }
        st->idx_i = 0;
        st->idx_state = st->rec_n == 0 ? 3 : 1;
      }
      break;
    case 1:
    case 2:
      r = xz_vli_feed(&st->vli, byte);
      if (r < 0) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: index record");
      }
      if (r == 1) {
        const uint64_t want = st->rec[2u * st->idx_i + (st->idx_state - 1)];
        if (st->vli.value != want) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "xz: the index disagrees with the blocks that were read");
        }
        if (st->idx_state == 1) {
          st->idx_state = 2;
        }
        else {
          st->idx_i++;
          st->idx_state = st->idx_i == st->rec_n ? 3 : 1;
        }
      }
      break;
    default: /* 3: padding */
      if (byte != 0) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: index padding is not zero");
      }
      break;
    }
    if (st->idx_state == 3 && (st->idx_len & 3u) == 0) {
      st->idx_state = 4;
      st->have = 0;
    }
  }
  *need_input = 1;
  return GCOMP_OK;
}

static gcomp_status_t drive(xz_dec_t * st, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * output, int * blocked_on_output) {
  *blocked_on_output = 0;
  for (;;) {
    gcomp_status_t s;
    int blocked = 0;
    if (st->mt && st->phase != D_FAILED) {
      /* Blocks that workers have finished go to the caller first, in order;
       * in D_DEFER, all of them are waited for. */
      s = mt_service(st, output, st->phase == D_DEFER, &blocked);
      if (s != GCOMP_OK) {
        return s;
      }
      if (blocked) {
        *blocked_on_output = 1;
        return GCOMP_OK;
      }
    }
    switch (st->phase) {
    case D_STREAM_HDR:
      if (!collect(st, in, end)) {
        return GCOMP_OK;
      }
      s = parse_stream_header(st);
      if (s != GCOMP_OK) {
        return s;
      }
      st->phase = D_BLOCK_START;
      break;
    case D_BLOCK_START: {
      uint8_t b0;
      if (*in == end) {
        return GCOMP_OK;
      }
      b0 = **in;
      if (b0 == 0 && st->mt) {
        /* The index lists the blocks, so every block is to be in first. */
        s = mt_service(st, output, 1, &blocked);
        if (s != GCOMP_OK) {
          return s;
        }
        if (blocked) {
          *blocked_on_output = 1;
          return GCOMP_OK;
        }
      }
      (*in)++;
      st->in_total++;
      if (b0 == 0) {
        index_begin(st);
        st->phase = D_INDEX;
      }
      else {
        st->hdr_size = ((size_t)b0 + 1u) * 4u;
        st->buf[0] = b0;
        st->have = 1;
        st->want = st->hdr_size;
        st->phase = D_BLOCK_HDR;
      }
      break;
    }
    case D_BLOCK_HDR:
      if (!collect(st, in, end)) {
        return GCOMP_OK;
      }
      s = parse_block_header(st);
      if (s != GCOMP_OK) {
        return s;
      }
      st->phase = D_BLOCK_DATA;
      break;
    case D_BLOCK_DATA:
      s = st->mt ? mt_block_data(st, in, end, output, &blocked)
                 : block_data(st, in, end, output, &blocked);
      if (s != GCOMP_OK) {
        return s;
      }
      if (blocked) {
        *blocked_on_output = output->used == output->size;
        return GCOMP_OK;
      }
      break;
    case D_BLOCK_PAD:
      while (st->pad_left != 0) {
        if (*in == end) {
          return GCOMP_OK;
        }
        if (**in != 0) {
          return DFAIL(
              st, GCOMP_ERR_CORRUPT, "xz: block padding is not zero");
        }
        (*in)++;
        st->in_total++;
        st->pad_left--;
      }
      st->have = 0;
      st->want = xz_check_size(st->check);
      st->phase = D_BLOCK_CHECK;
      break;
    case D_BLOCK_CHECK: {
      uint8_t sum[32];
      if (!collect(st, in, end)) {
        return GCOMP_OK;
      }
      if (st->mt) {
        /* The worker has the data's check; the file's goes with the block. */
        mt_seal(st);
        st->phase = D_BLOCK_START;
        break;
      }
      xz_check_final(&st->chk, sum);
      if (st->want != 0 && memcmp(sum, st->buf, st->want) != 0) {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "xz: the integrity check does not match the decoded data");
      }
      s = push_record(st,
          (uint64_t)st->hdr_size + st->comp_in + st->want, st->unc_out);
      if (s != GCOMP_OK) {
        return s;
      }
      st->phase = D_BLOCK_START;
      break;
    }
    case D_INDEX: {
      int need_input = 0;
      s = index_bytes(st, in, end, &need_input);
      if (s != GCOMP_OK) {
        return s;
      }
      if (need_input) {
        return GCOMP_OK;
      }
      break;
    }
    case D_FOOTER:
      if (!collect(st, in, end)) {
        return GCOMP_OK;
      }
      s = parse_footer(st, st->idx_len);
      if (s != GCOMP_OK) {
        return s;
      }
      st->streams_done++;
      st->rec_n = 0;
      st->pad_run = 0;
      st->phase = D_STREAM_PAD;
      break;
    case D_STREAM_PAD:
      while (*in < end && **in == 0) {
        (*in)++;
        st->in_total++;
        st->pad_run++;
      }
      if (*in == end) {
        return GCOMP_OK;
      }
      if (st->pad_run & 3u) {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "xz: stream padding is not a multiple of four bytes");
      }
      st->pad_run = 0;
      st->have = 0;
      st->want = XZ_STREAM_HEADER_SIZE;
      st->phase = D_STREAM_HDR;
      break;
    case D_DEFER:
      *in = end; /* nothing more is read from a stream that has failed */
      if (st->mt->pend || mt_collectable(st->mt)) {
        break; /* the service at the top waits for them */
      }
      st->phase = D_FAILED;
      st->err = gcomp_decoder_set_error(
          st->pub, st->mt->d_status, "%s", st->mt->d_msg);
      return st->err;
    default:
      return st->err;
    }
  }
}

/* drive(), and an error that comes while blocks are out is held until they
 * are in, so that the stream gets the verdict it would get on one thread. */
static gcomp_status_t drive_top(xz_dec_t * st, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * output, int * blocked_on_output) {
  gcomp_status_t s = drive(st, in, end, output, blocked_on_output);
  if (s != GCOMP_OK && st->mt && !st->mt->fatal && st->phase == D_FAILED &&
      mt_busy(st->mt)) {
    mt_defer(st);
    *in = end;
    s = drive(st, in, end, output, blocked_on_output);
  }
  return s;
}

static const uint8_t g_no_input[1] = {0};

gcomp_status_t xz_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  xz_dec_t * st = decoder->method_state;
  const uint8_t * in;
  const uint8_t * end;
  int on_output = 0;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->phase == D_FAILED) {
    return st->err;
  }
  if (input->data) {
    in = (const uint8_t *)input->data + input->used;
    end = in + (input->size - input->used);
  }
  else {
    in = end = g_no_input;
  }
  s = drive_top(st, &in, end, output, &on_output);
  if (input->data) {
    input->used = (size_t)(in - (const uint8_t *)input->data);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  if (gcomp_limits_check_expansion_ratio(st->in_total, st->produced,
          st->max_ratio) != GCOMP_OK) {
    return DFAIL(
        st, GCOMP_ERR_LIMIT, "xz: expansion ratio exceeds the limit");
  }
  return GCOMP_OK;
}

gcomp_status_t xz_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  xz_dec_t * st = decoder->method_state;
  const uint8_t * none = g_no_input;
  int on_output = 0;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->phase == D_FAILED) {
    return st->err;
  }
  if (st->mt) {
    /* Whatever is out for decoding is waited for and delivered, whether or
     * not the stream turns out to be whole. */
    int blocked = 0;
    s = mt_service(st, output, 1, &blocked);
    if (s != GCOMP_OK) {
      return s;
    }
    if (blocked) {
      return GCOMP_ERR_LIMIT;
    }
  }
  s = drive_top(st, &none, none, output, &on_output);
  if (s != GCOMP_OK) {
    return s;
  }
  if (st->phase == D_STREAM_PAD && st->streams_done > 0 &&
      (st->pad_run & 3u) == 0) {
    return GCOMP_OK;
  }
  if (on_output) {
    return GCOMP_ERR_LIMIT;
  }
  return DFAIL(st, GCOMP_ERR_CORRUPT, "xz: truncated stream");
}
