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
 * The trivial compressor from RFC 7932 section 11.1.
 *
 * A non-empty stream opens with the window bits and an empty metadata
 * meta-block, which lands the rest of the stream on a byte boundary. Input
 * is then stored in uncompressed meta-blocks of at most 65536 bytes. Flush
 * ends the current one so a decoder can produce every byte consumed so far
 * without finish(). Finish writes the empty last meta-block, which is the
 * single byte 0x03 once the stream is aligned. An empty input is only the
 * window bits plus ISLAST and ISLASTEMPTY: for the default 16-bit window
 * that is the byte 0x06.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "brotli_internal.h"

#include <string.h>

#define BROTLI_CHUNK 65536u
#define BROTLI_QUEUE (BROTLI_CHUNK + 64u)

typedef struct brotli_bw_s {
  uint8_t * buf;
  size_t cap;
  size_t len;
  uint64_t acc;
  int nbits;
} brotli_bw_t;

typedef struct brotli_enc_s {
  const gcomp_allocator_t * alloc;
  int lgwin;
  int started;
  int finished;
  uint8_t hold[BROTLI_CHUNK];
  size_t hold_len;
  uint8_t * queue;
  size_t q_len;
  size_t q_pos;
} brotli_enc_t;

static int bw_put(brotli_bw_t * b, uint32_t bits, int n) {
  if (n <= 0) {
    return 0;
  }
  b->acc |= (uint64_t)(bits & ((1u << n) - 1u)) << b->nbits;
  b->nbits += n;
  while (b->nbits >= 8) {
    if (b->len >= b->cap) {
      return -1;
    }
    b->buf[b->len++] = (uint8_t)b->acc;
    b->acc >>= 8;
    b->nbits -= 8;
  }
  return 0;
}

static int bw_align(brotli_bw_t * b) {
  if (b->nbits == 0) {
    return 0;
  }
  return bw_put(b, 0, 8 - b->nbits);
}

static int write_wbits(brotli_bw_t * b, int w) {
  if (w == 16) {
    return bw_put(b, 0, 1);
  }
  if (bw_put(b, 1, 1) != 0) {
    return -1;
  }
  if (w >= 18 && w <= 24) {
    return bw_put(b, (uint32_t)(w - 17), 3);
  }
  if (bw_put(b, 0, 3) != 0) {
    return -1;
  }
  if (w == 17) {
    return bw_put(b, 0, 3);
  }
  return bw_put(b, (uint32_t)(w - 8), 3);
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
  if (st->q_len + n > BROTLI_QUEUE) {
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
  if (write_wbits(&bw, st->lgwin) != 0 || bw_put(&bw, 0, 1) != 0 ||
      bw_put(&bw, 3, 2) != 0 || bw_put(&bw, 0, 1) != 0 || bw_put(&bw, 0, 2) != 0 ||
      bw_align(&bw) != 0) {
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
  if (write_wbits(&bw, st->lgwin) != 0 || bw_put(&bw, 1, 1) != 0 ||
      bw_put(&bw, 1, 1) != 0 || bw_align(&bw) != 0) {
    return -1;
  }
  return q_add(st, tmp, bw.len);
}

static int write_stored(brotli_enc_t * st, const uint8_t * data, size_t len) {
  uint32_t r;
  uint8_t hdr[3];
  if (len == 0 || len > BROTLI_CHUNK) {
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

static int output_ok(gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!output || (output->size > 0 && output->data == NULL)) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: output buffer is NULL");
  }
  if (output->used > output->size) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: output->used exceeds output->size");
  }
  return GCOMP_OK;
}

gcomp_status_t brotli_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  brotli_enc_t * st;
  int64_t lgwin = 16;

  if (options &&
      gcomp_options_get_int64(options, "brotli.lgwin", &lgwin) == GCOMP_OK) {
    if (lgwin < 10 || lgwin > 24) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
          "brotli.lgwin must be from 10 to 24");
    }
  }
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }
  st->alloc = alloc;
  st->lgwin = (int)lgwin;
  st->queue = gcomp_malloc(alloc, BROTLI_QUEUE);
  if (!st->queue) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
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
  gcomp_free(st->alloc, st->queue);
  gcomp_free(st->alloc, st);
  encoder->method_state = NULL;
}

gcomp_status_t brotli_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  brotli_enc_t * st;
  const uint8_t * src;
  gcomp_status_t chk = output_ok(encoder, output);
  if (chk != GCOMP_OK) {
    return chk;
  }
  if (!encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  if (st->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: encoder update after finish");
  }
  if (input->size > input->used && input->data == NULL) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "brotli: input buffer is NULL");
  }
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_OK;
  }
  src = input->data ? (const uint8_t *)input->data : NULL;
  for (;;) {
    while (input->used < input->size && st->hold_len < BROTLI_CHUNK) {
      st->hold[st->hold_len++] = src[input->used++];
    }
    if (st->hold_len < BROTLI_CHUNK) {
      break;
    }
    if (!st->started) {
      if (write_header(st) != 0) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
            "brotli: header did not fit the output queue");
      }
      st->started = 1;
    }
    if (write_stored(st, st->hold, st->hold_len) != 0) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
          "brotli: stored block did not fit the output queue");
    }
    st->hold_len = 0;
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
  gcomp_status_t chk = output_ok(encoder, output);
  if (chk != GCOMP_OK) {
    return chk;
  }
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
      if (write_stored(st, st->hold, st->hold_len) != 0) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
            "brotli: stored block did not fit the output queue");
      }
      st->hold_len = 0;
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

gcomp_status_t brotli_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode) {
  brotli_enc_t * st;
  gcomp_status_t chk;
  (void)mode;
  chk = output_ok(encoder, output);
  if (chk != GCOMP_OK) {
    return chk;
  }
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
  if (st->hold_len == 0) {
    return GCOMP_OK;
  }
  if (!st->started) {
    if (write_header(st) != 0) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
          "brotli: header did not fit the output queue");
    }
    st->started = 1;
  }
  if (write_stored(st, st->hold, st->hold_len) != 0) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
        "brotli: stored block did not fit the output queue");
  }
  st->hold_len = 0;
  drain(st, output);
  if (st->q_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

gcomp_status_t brotli_encoder_reset(gcomp_encoder_t * encoder) {
  brotli_enc_t * st;
  int lgwin;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = encoder->method_state;
  lgwin = st->lgwin;
  st->started = 0;
  st->finished = 0;
  st->hold_len = 0;
  st->q_len = 0;
  st->q_pos = 0;
  st->lgwin = lgwin;
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
