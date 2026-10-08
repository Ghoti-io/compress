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
 * @file xz_chain.c
 *
 * Stages run into one another. See xz_chain.h.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../lzma/lzma_internal.h"
#include "xz_chain.h"

#include <stdio.h>
#include <string.h>

#define CHAIN_BUF 32768u

/* A pointer to nothing that is still a pointer, for a stage that is being
 * asked to drain and has no input to be given. */
static const uint8_t g_none[1] = {0};

void xz_chain_init(
    xz_chain_t * c, const gcomp_allocator_t * alloc, int encode) {
  memset(c, 0, sizeof(*c));
  c->alloc = alloc;
  c->encode = encode;
}

void xz_chain_clear(xz_chain_t * c) {
  unsigned i;
  for (i = 0; i < c->count; i++) {
    if (c->st[i].enc) {
      gcomp_encoder_destroy(c->st[i].enc);
    }
    if (c->st[i].dec) {
      gcomp_decoder_destroy(c->st[i].dec);
    }
    gcomp_free(c->alloc, c->st[i].buf);
  }
  memset(c->st, 0, sizeof(c->st));
  c->count = 0;
  c->fin = 0;
  c->fin_started = 0;
  c->status = GCOMP_OK;
  c->detail[0] = '\0';
}

gcomp_status_t xz_chain_add(xz_chain_t * c, void * codec) {
  xz_chain_stage_t * s;
  if (c->count >= XZ_CHAIN_STAGES_MAX) {
    if (c->encode) {
      gcomp_encoder_destroy((gcomp_encoder_t *)codec);
    }
    else {
      gcomp_decoder_destroy((gcomp_decoder_t *)codec);
    }
    return GCOMP_ERR_INTERNAL;
  }
  s = &c->st[c->count++];
  if (c->encode) {
    s->enc = codec;
  }
  else {
    s->dec = codec;
  }
  return GCOMP_OK;
}

/* A buffer for every stage but the last, made the first time it is wanted so
 * that a chain is not told how many stages there will be before it is
 * built. */
static gcomp_status_t ensure_buffers(xz_chain_t * c) {
  unsigned i;
  for (i = 0; i + 1 < c->count; i++) {
    if (!c->st[i].buf) {
      c->st[i].buf = gcomp_malloc(c->alloc, CHAIN_BUF);
      if (!c->st[i].buf) {
        return GCOMP_ERR_MEMORY;
      }
    }
  }
  return GCOMP_OK;
}

static void remember(xz_chain_t * c, unsigned i, gcomp_status_t s) {
  const char * why = NULL;
  if (c->status != GCOMP_OK) {
    return;
  }
  c->status = s;
  if (c->encode && c->st[i].enc) {
    why = gcomp_encoder_get_error_detail(c->st[i].enc);
  }
  else if (c->st[i].dec) {
    why = gcomp_decoder_get_error_detail(c->st[i].dec);
  }
  snprintf(c->detail, sizeof(c->detail), "%s", why ? why : "");
}

/* The room after a stage's output: compacted if the front has been taken. */
static gcomp_buffer_t room_after(xz_chain_stage_t * s) {
  gcomp_buffer_t b;
  if (s->pos == s->len) {
    s->pos = s->len = 0;
  }
  else if (s->len == CHAIN_BUF && s->pos > 0) {
    memmove(s->buf, s->buf + s->pos, s->len - s->pos);
    s->len -= s->pos;
    s->pos = 0;
  }
  b.data = s->buf + s->len;
  b.size = CHAIN_BUF - s->len;
  b.used = 0;
  return b;
}

/* One call to one stage; returns what moved. */
static size_t step(xz_chain_t * c, unsigned i, gcomp_buffer_t * input,
    gcomp_buffer_t * output, int finishing, gcomp_status_t * status) {
  xz_chain_stage_t * s = &c->st[i];
  const int last = i + 1 == c->count;
  gcomp_buffer_t empty = {(void *)g_none, 0, 0};
  gcomp_buffer_t src, dst;
  gcomp_buffer_t * srcp;
  size_t moved;
  if (i == 0) {
    srcp = input ? input : &empty;
  }
  else {
    xz_chain_stage_t * prev = &c->st[i - 1];
    src.data = prev->len > prev->pos ? prev->buf + prev->pos : (void *)g_none;
    src.size = prev->len - prev->pos;
    src.used = 0;
    srcp = &src;
  }
  if (last) {
    dst = *output;
    dst.used = output->used;
  }
  else {
    dst = room_after(s);
  }
  {
    const size_t used_before = srcp->used;
    const size_t out_before = dst.used;
    if (finishing) {
      *status = c->encode ? gcomp_encoder_finish(s->enc, &dst)
                          : gcomp_decoder_finish(s->dec, &dst);
    }
    else {
      *status = c->encode ? gcomp_encoder_update(s->enc, srcp, &dst)
                          : gcomp_decoder_update(s->dec, srcp, &dst);
    }
    moved = (srcp->used - used_before) + (dst.used - out_before);
    if (i > 0) {
      c->st[i - 1].pos += srcp->used;
    }
    if (last) {
      output->used = dst.used;
    }
    else {
      s->len += dst.used;
    }
  }
  return moved;
}

/* Run every stage until none can move. Returns what moved in all. */
static size_t pump(xz_chain_t * c, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  size_t total = 0;
  for (;;) {
    size_t round = 0;
    unsigned i;
    /* A stage that has been finished has nothing more to give and would
     * refuse an update, and so would one whose finish() has begun but not
     * ended (it is out of room, and is asked again to finish, not to
     * update), so the pump starts after them. */
    for (i = c->fin + (c->fin_started ? 1u : 0u); i < c->count; i++) {
      gcomp_status_t s = GCOMP_OK;
      round += step(c, i, input, output, 0, &s);
      if (s != GCOMP_OK && s != GCOMP_ERR_LIMIT) {
        remember(c, i, s);
        return total + round;
      }
    }
    total += round;
    if (round == 0) {
      return total;
    }
  }
}

gcomp_status_t xz_chain_update(
    xz_chain_t * c, gcomp_buffer_t * input, gcomp_buffer_t * output) {
  gcomp_status_t s = ensure_buffers(c);
  if (s != GCOMP_OK) {
    return s;
  }
  if (c->status != GCOMP_OK) {
    return c->status;
  }
  (void)pump(c, input, output);
  return c->status;
}

gcomp_status_t xz_chain_finish(xz_chain_t * c, gcomp_buffer_t * output) {
  gcomp_status_t s = ensure_buffers(c);
  if (s != GCOMP_OK) {
    return s;
  }
  for (;;) {
    unsigned i;
    int last;
    if (c->status != GCOMP_OK) {
      return c->status;
    }
    (void)pump(c, NULL, output);
    if (c->status != GCOMP_OK) {
      return c->status;
    }
    if (c->fin == c->count) {
      return GCOMP_OK;
    }
    i = c->fin;
    last = i + 1 == c->count;
    if (i > 0 && c->st[i - 1].pos != c->st[i - 1].len) {
      /* What the stage before made has not all gone in, and the pump has just
       * shown that it cannot: the output is full. */
      return GCOMP_ERR_LIMIT;
    }
    {
      const size_t moved = step(c, i, NULL, output, 1, &s);
      c->fin_started = 1;
      if (s == GCOMP_OK) {
        c->fin++;
        c->fin_started = 0;
        continue;
      }
      if (s != GCOMP_ERR_LIMIT) {
        remember(c, i, s);
        return c->status;
      }
      if (last) {
        return GCOMP_ERR_LIMIT;
      }
      /* The buffer after this stage is full: let the next ones take from it,
       * and if they cannot, it is the output that is full. */
      if (moved == 0 && pump(c, NULL, output) == 0) {
        return GCOMP_ERR_LIMIT;
      }
    }
  }
}

gcomp_status_t xz_chain_reset(xz_chain_t * c) {
  unsigned i;
  for (i = 0; i < c->count; i++) {
    gcomp_status_t s = c->encode ? gcomp_encoder_reset(c->st[i].enc)
                                 : gcomp_decoder_reset(c->st[i].dec);
    if (s != GCOMP_OK) {
      return s;
    }
    c->st[i].pos = c->st[i].len = 0;
  }
  c->fin = 0;
  c->fin_started = 0;
  c->status = GCOMP_OK;
  c->detail[0] = '\0';
  return GCOMP_OK;
}

int xz_chain_first_done(const xz_chain_t * c) {
  return c->count != 0 && c->st[0].dec && lzma_decoder_done(c->st[0].dec);
}
