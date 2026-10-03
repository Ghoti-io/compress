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
 * @file brotli_encode.c
 *
 * Brotli encoder.
 *
 * Level 0 is the trivial compressor from RFC 7932 section 11.1: a non-empty
 * stream opens with the window bits and an empty metadata meta-block, which
 * lands the rest of the stream on a byte boundary, and input is stored in
 * uncompressed meta-blocks of at most 65536 bytes. Level 1 writes a
 * compressed meta-block when that is smaller than storing the chunk, and
 * stores it otherwise. Flush ends the current meta-block so a decoder can
 * produce every byte consumed so far without finish(). Finish writes the
 * empty last meta-block, which is the single byte 0x03 once the stream is
 * aligned. An empty input is only the window bits plus ISLAST and
 * ISLASTEMPTY: for the default 16-bit window that is the byte 0x06.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "brotli_bw.h"
#include "brotli_internal.h"

#include <string.h>

/* A compressed meta-block may be longer than an uncompressed one. One block
 * for this much input pays the Huffman header once; the store fallback still
 * splits at BROTLI_STORE, which is the format's uncompressed-block limit. */
#define BROTLI_BLOCK (256u * 1024u)
#define BROTLI_SLACK 256u

typedef struct brotli_enc_s {
  const gcomp_allocator_t * alloc;
  int lgwin;
  int level;
  int started;
  int finished;
  uint32_t dist_rb[4];
  int rb_fresh;
  uint8_t * hold;
  size_t hold_len;
  size_t hold_cap;
  uint8_t * queue;
  size_t q_cap;
  uint8_t * scratch;
  size_t scratch_cap;
  size_t q_len;
  size_t q_pos;
} brotli_enc_t;

/* The ring buffer a decoder starts with, newest first. Nothing is owed at the
 * start of a stream: a decoder reading from here has exactly these four
 * values, so a short distance code is as good as an absolute one. */
static void reset_dist(brotli_enc_t * st) {
  st->dist_rb[0] = 4;
  st->dist_rb[1] = 11;
  st->dist_rb[2] = 15;
  st->dist_rb[3] = 16;
  st->rb_fresh = 0;
}

static int write_wbits(brotli_bw_t * b, int w) {
  if (w == 16) {
    return brotli_bw_put(b, 0, 1);
  }
  if (brotli_bw_put(b, 1, 1) != 0) {
    return -1;
  }
  if (w >= 18 && w <= 24) {
    return brotli_bw_put(b, (uint32_t)(w - 17), 3);
  }
  if (brotli_bw_put(b, 0, 3) != 0) {
    return -1;
  }
  if (w == 17) {
    return brotli_bw_put(b, 0, 3);
  }
  return brotli_bw_put(b, (uint32_t)(w - 8), 3);
}

static void drain(brotli_enc_t * st, gcomp_buffer_t * output) {
  uint8_t * dst = (uint8_t *)output->data;
  while (st->q_pos < st->q_len && output->used < output->size) {
    dst[output->used++] = st->queue[st->q_pos++];
  }
  if (st->q_pos == st->q_len) {
    st->q_pos = 0;
    st->q_len = 0;
  }
}

static int q_add(brotli_enc_t * st, const uint8_t * bytes, size_t n) {
  if (st->q_len + n > st->q_cap) {
    return -1;
  }
  memcpy(st->queue + st->q_len, bytes, n);
  st->q_len += n;
  return 0;
}

static int write_header(brotli_enc_t * st) {
  uint8_t tmp[8];
  brotli_bw_t bw;
  memset(&bw, 0, sizeof(bw));
  bw.buf = tmp;
  bw.cap = sizeof(tmp);
  if (write_wbits(&bw, st->lgwin) != 0 || brotli_bw_put(&bw, 0, 1) != 0 ||
      brotli_bw_put(&bw, 3, 2) != 0 || brotli_bw_put(&bw, 0, 1) != 0 || brotli_bw_put(&bw, 0, 2) != 0 ||
      brotli_bw_align(&bw) != 0) {
    return -1;
  }
  return q_add(st, tmp, bw.len);
}

static int write_empty(brotli_enc_t * st) {
  uint8_t tmp[8];
  brotli_bw_t bw;
  memset(&bw, 0, sizeof(bw));
  bw.buf = tmp;
  bw.cap = sizeof(tmp);
  if (write_wbits(&bw, st->lgwin) != 0 || brotli_bw_put(&bw, 1, 1) != 0 ||
      brotli_bw_put(&bw, 1, 1) != 0 || brotli_bw_align(&bw) != 0) {
    return -1;
  }
  return q_add(st, tmp, bw.len);
}

static int write_stored(brotli_enc_t * st, const uint8_t * data, size_t len);

static int write_stored_span(brotli_enc_t * st, const uint8_t * data, size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t n = len - off;
    if (n > BROTLI_STORE) {
      n = BROTLI_STORE;
    }
    if (write_stored(st, data + off, n) != 0) {
      return -1;
    }
    off += n;
  }
  return 0;
}

static int write_chunk(brotli_enc_t * st) {
  size_t n = 0;
  int r;
  uint32_t window;
  if (st->hold_len == 0 || st->hold_len > st->hold_cap) {
    return -1;
  }
  if (st->level < 1) {
    return write_stored(st, st->hold, st->hold_len);
  }
  window = (1u << st->lgwin) - 16u;
  r = brotli_compress_chunk(st->alloc, st->scratch, st->scratch_cap, &n,
      st->hold, st->hold_len, window, st->dist_rb, &st->rb_fresh);
  if (r < 0) {
    return -2;
  }
  if (r > 0) {
    return write_stored_span(st, st->hold, st->hold_len);
  }
  return q_add(st, st->scratch, n);
}

static gcomp_status_t emit_hold(gcomp_encoder_t * encoder, brotli_enc_t * st) {
  int r = write_chunk(st);
  if (r == 0) {
    st->hold_len = 0;
    return GCOMP_OK;
  }
  if (r == -2) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "brotli: out of memory");
  }
  return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
      "brotli: block did not fit the output queue");
}

static int write_stored(brotli_enc_t * st, const uint8_t * data, size_t len) {
  uint32_t r;
  uint8_t hdr[3];
  if (len == 0 || len > BROTLI_STORE) {
    return -1;
  }
  r = (uint32_t)len - 1u;
  hdr[0] = (uint8_t)((r & 31u) << 3);
  hdr[1] = (uint8_t)((r >> 5) & 0xffu);
  hdr[2] = (uint8_t)(8u + (r >> 13));
  if (q_add(st, hdr, 3) != 0 || q_add(st, data, len) != 0) {
    return -1;
  }
  return 0;
}

gcomp_status_t brotli_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  brotli_enc_t * st;
  int64_t lgwin = 16;
  int64_t level = 1;

  /* The ranges are the schema's: gcomp_encoder_create() validates the
   * caller's options against g_brotli_option_schemas before this runs, so an
   * lgwin outside 10..24 or a level outside 0..1 is refused with
   * GCOMP_ERR_INVALID_ARG before the method sees it. Repeating the check here
   * spelled two arms no input could reach, which is worse than none: the
   * numbers would drift apart with nothing to say so. What keeps them honest
   * is that the schema's range is what write_wbits can express -
   * OptionValidationTest.MistakesAreRejectedAtCreateTime asserts the refusal
   * at each boundary, and BrotliRegisterTest.PeekReadsEveryWindowTheEncoderWrites
   * asserts that all fifteen windows inside it come back distinct. */
  if (options) {
    (void)gcomp_options_get_int64(options, "brotli.lgwin", &lgwin);
    (void)gcomp_options_get_int64(options, "brotli.level", &level);
  }
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }
  st->alloc = alloc;
  st->lgwin = (int)lgwin;
  st->level = (int)level;
  st->hold_cap = st->level >= 1 ? BROTLI_BLOCK : BROTLI_STORE;
  st->q_cap = st->hold_cap + BROTLI_SLACK;
  st->scratch_cap = st->level >= 1 ? st->q_cap : 0;
  reset_dist(st);
  st->hold = gcomp_malloc(alloc, st->hold_cap);
  st->queue = gcomp_malloc(alloc, st->q_cap);
  if (!st->hold || !st->queue) {
    gcomp_free(alloc, st->hold);
    gcomp_free(alloc, st->queue);
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }
  if (st->scratch_cap != 0) {
    st->scratch = gcomp_malloc(alloc, st->scratch_cap);
    if (!st->scratch) {
      gcomp_free(alloc, st->hold);
      gcomp_free(alloc, st->queue);
      gcomp_free(alloc, st);
      return GCOMP_ERR_MEMORY;
    }
  }
  encoder->method_state = st;
  return GCOMP_OK;
}

void brotli_encoder_destroy(gcomp_encoder_t * encoder) {
  brotli_enc_t * st;
  if (!encoder || !encoder->method_state) {
    return;
  }
  st = encoder->method_state;
  gcomp_free(st->alloc, st->scratch);
  gcomp_free(st->alloc, st->hold);
  gcomp_free(st->alloc, st->queue);
  gcomp_free(st->alloc, st);
  encoder->method_state = NULL;
}

gcomp_status_t brotli_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  brotli_enc_t * st;
  const uint8_t * src;
  gcomp_status_t chk;
  /* gcomp_encoder_update() refuses an inconsistent or unbacked buffer on
   * either side before this is called, so what is left to check here is this
   * encoder's own state. */
  if (!encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  if (st->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: encoder update after finish");
  }
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_OK;
  }
  src = input->data ? (const uint8_t *)input->data : NULL;
  for (;;) {
    {
      size_t room = st->hold_cap - st->hold_len;
      size_t avail = input->size - input->used;
      size_t take = room < avail ? room : avail;
      if (take != 0) {
        memcpy(st->hold + st->hold_len, src + input->used, take);
        st->hold_len += take;
        input->used += take;
      }
    }
    if (st->hold_len < st->hold_cap) {
      break;
    }
    if (!st->started) {
      if (write_header(st) != 0) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
            "brotli: header did not fit the output queue");
      }
      st->started = 1;
    }
    chk = emit_hold(encoder, st);
    if (chk != GCOMP_OK) {
      return chk;
    }
    drain(st, output);
    if (st->q_len != 0) {
      break;
    }
  }
  return GCOMP_OK;
}

gcomp_status_t brotli_encoder_finish(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output) {
  brotli_enc_t * st;
  uint8_t term = 0x03;
  gcomp_status_t chk;
  if (!encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  if (st->finished) {
    return GCOMP_OK;
  }
  if (!st->started && st->hold_len == 0) {
    if (write_empty(st) != 0) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
          "brotli: empty stream did not fit the output queue");
    }
  }
  else {
    if (!st->started) {
      if (write_header(st) != 0) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
            "brotli: header did not fit the output queue");
      }
      st->started = 1;
    }
    if (st->hold_len != 0) {
      chk = emit_hold(encoder, st);
      if (chk != GCOMP_OK) {
        return chk;
      }
    }
    if (q_add(st, &term, 1) != 0) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
          "brotli: terminator did not fit the output queue");
    }
  }
  st->finished = 1;
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

/**
 * Both modes end the current meta-block, which is all a sync flush is: level 1
 * never matches outside the chunk it is compressing, so no match can reach
 * back across a flush of either kind.
 *
 * A full flush owes one thing more. The distance ring buffer is the only
 * decoder state that survives a meta-block boundary, so a short or implicit
 * distance code after the flush names a distance established before it - and
 * a decoder that lost the earlier bytes resolves that code against a
 * different ring buffer and produces the wrong output, silently. The mode used
 * to be ignored outright, which made both modes write identical bytes and left
 * that hole open. `rb_fresh` closes it by making the next four distances
 * absolute, which is every slot the short codes can read.
 *
 * It is set on every path rather than only after a block is emitted, because
 * an output buffer too small to drain makes the caller flush again with the
 * same mode, and that second call has nothing buffered left to emit.
 */
gcomp_status_t brotli_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode) {
  brotli_enc_t * st;
  gcomp_status_t chk;
  if (!encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  if (st->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: encoder cannot flush after finish");
  }
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  if (st->hold_len != 0) {
    if (!st->started) {
      if (write_header(st) != 0) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
            "brotli: header did not fit the output queue");
      }
      st->started = 1;
    }
    chk = emit_hold(encoder, st);
    if (chk != GCOMP_OK) {
      return chk;
    }
  }
  if (mode == GCOMP_FLUSH_FULL) {
    st->rb_fresh = 4;
  }
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

/* The window and the level survive a reset, because they are what the caller
 * asked for at create and reset does not take options. Nothing here writes
 * them, which is why the save-and-restore pair this used to carry around
 * st->lgwin did nothing. */
gcomp_status_t brotli_encoder_reset(gcomp_encoder_t * encoder) {
  brotli_enc_t * st;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  st->started = 0;
  st->finished = 0;
  st->hold_len = 0;
  st->q_len = 0;
  st->q_pos = 0;
  reset_dist(st);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
