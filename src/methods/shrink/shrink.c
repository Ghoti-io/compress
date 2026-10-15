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
 * @file shrink.c
 *
 * Shrink (ZIP method 1): Lempel-Ziv-Welch with partial clearing, as PKWARE's
 * APPNOTE section 5.1 describes it. Codes are read least significant bit
 * first, 9 bits to start; the pair 256, 1 makes them one bit longer (to 13)
 * and the pair 256, 2 frees every entry that no other entry builds on, the
 * freed codes then being reused lowest first.
 *
 * There is no end marker: the stream ends where the input does, and the
 * fewer than eight bits of padding after the last code are not a code.
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
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/shrink.h>
#include <string.h>

#define SH_CODES 8192u
#define SH_FIRST 257u
#define SH_MIN_BITS 9u
#define SH_MAX_BITS 13u

/* ---- the decoder ------------------------------------------------------------ */

typedef struct {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  uint64_t max_out, max_ratio;
  uint64_t bb;
  unsigned bn, codesize, scan;
  uint16_t prefix[SH_CODES];
  uint8_t suffix[SH_CODES];
  uint8_t used[SH_CODES];
  int have_prev, ctrl;
  unsigned prev;
  uint8_t str[SH_CODES + 1u];
  unsigned str_len, str_pos;
  uint64_t produced, in_total;
  int failed;
  gcomp_status_t err;
} sh_dec_t;

#define DFAIL(st, status, ...)                                                 \
  ((st)->failed = 1,                                                           \
      (st)->err = gcomp_decoder_set_error((st)->pub, (status), __VA_ARGS__))

static void dec_restart(sh_dec_t * st) {
  memset(st->used, 0, sizeof(st->used));
  memset(st->used, 1, SH_FIRST); /* the bytes, and 256, which is a control */
  st->bb = 0;
  st->bn = 0;
  st->codesize = SH_MIN_BITS;
  st->scan = SH_FIRST;
  st->have_prev = st->ctrl = 0;
  st->prev = 0;
  st->str_len = st->str_pos = 0;
  st->produced = st->in_total = 0;
  st->failed = 0;
  st->err = GCOMP_OK;
}

static unsigned dec_free_code(sh_dec_t * st) {
  while (st->scan < SH_CODES && st->used[st->scan]) {
    st->scan++;
  }
  return st->scan;
}

static void dec_add(sh_dec_t * st, unsigned prefix, uint8_t suffix) {
  const unsigned c = dec_free_code(st);
  if (c < SH_CODES) {
    st->prefix[c] = (uint16_t)prefix;
    st->suffix[c] = suffix;
    st->used[c] = 1;
  }
}

/* The string for a code into st->str; false if the chain is broken. */
static int dec_expand(sh_dec_t * st, unsigned code) {
  unsigned depth = 0, c = code, i;
  while (c >= SH_FIRST) {
    if (++depth > SH_CODES || !st->used[c]) {
      return 0;
    }
    c = st->prefix[c];
  }
  st->str_len = depth + 1;
  st->str_pos = 0;
  st->str[0] = (uint8_t)c; /* the first byte of the string is the root */
  c = code;
  for (i = depth; i > 0; i--) {
    st->str[i] = st->suffix[c];
    c = st->prefix[c];
  }
  return 1;
}

/* Free every entry no other entry builds on. */
static void dec_partial_clear(sh_dec_t * st) {
  uint8_t has_child[SH_CODES];
  unsigned c;
  memset(has_child, 0, sizeof(has_child));
  for (c = SH_FIRST; c < SH_CODES; c++) {
    if (st->used[c] && st->prefix[c] >= SH_FIRST) {
      has_child[st->prefix[c]] = 1;
    }
  }
  for (c = SH_FIRST; c < SH_CODES; c++) {
    if (st->used[c] && !has_child[c]) {
      st->used[c] = 0;
    }
  }
  st->scan = SH_FIRST;
}

static gcomp_status_t sh_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  sh_dec_t * st;
  uint64_t max_mem;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  alloc = gcomp_registry_get_allocator(registry);
  max_mem = gcomp_limits_read_memory_max(options, 0);
  if (gcomp_limits_check_memory(sizeof(*st), max_mem) != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "shrink: the table exceeds limits.max_memory_bytes");
  }
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "shrink: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_SHRINK_MAX_EXPANSION_RATIO);
  dec_restart(st);
  decoder->method_state = st;
  return GCOMP_OK;
}

static void sh_decoder_destroy(gcomp_decoder_t * decoder) {
  sh_dec_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

static gcomp_status_t sh_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  dec_restart(decoder->method_state);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* Run as far as the input and the output allow. *out_full is set if it
 * stopped for want of room to write. */
static gcomp_status_t dec_run(sh_dec_t * st, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * output, int * out_full) {
  *out_full = 0;
  for (;;) {
    unsigned code;
    /* the string being written */
    while (st->str_pos < st->str_len) {
      const size_t room = output->size - output->used;
      size_t n = st->str_len - st->str_pos;
      if (room == 0) {
        *out_full = 1;
        return GCOMP_OK;
      }
      if (n > room) {
        n = room;
      }
      if (st->max_out != 0 && st->produced + n > st->max_out) {
        return DFAIL(
            st, GCOMP_ERR_LIMIT, "shrink: decompressed output exceeds the limit");
      }
      memcpy((uint8_t *)output->data + output->used, st->str + st->str_pos, n);
      output->used += n;
      st->str_pos += (unsigned)n;
      st->produced += n;
    }
    while (st->bn < st->codesize && *in < end) {
      st->bb |= (uint64_t)**in << st->bn;
      (*in)++;
      st->in_total++;
      st->bn += 8;
    }
    if (st->bn < st->codesize) {
      return GCOMP_OK;
    }
    code = (unsigned)(st->bb & ((1u << st->codesize) - 1u));
    st->bb >>= st->codesize;
    st->bn -= st->codesize;
    if (st->ctrl) {
      st->ctrl = 0;
      if (code == 1) {
        if (st->codesize >= SH_MAX_BITS) {
          return DFAIL(st, GCOMP_ERR_CORRUPT,
              "shrink: the code size is raised past 13 bits");
        }
        st->codesize++;
      }
      else if (code == 2) {
        dec_partial_clear(st);
      }
      else {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "shrink: control code 256 is followed by %u, not 1 or 2", code);
      }
      continue;
    }
    if (code == 256) {
      st->ctrl = 1;
      continue;
    }
    if (code >= SH_FIRST && !st->used[code]) {
      /* only the entry about to be made may be named before it exists */
      if (!st->have_prev || code != dec_free_code(st)) {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "shrink: code %u is not in the table", code);
      }
      if (!dec_expand(st, st->prev)) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "shrink: a broken table entry");
      }
      st->str[st->str_len] = st->str[0];
      st->str_len++;
      dec_add(st, st->prev, st->str[0]);
    }
    else {
      if (!dec_expand(st, code)) {
        return DFAIL(st, GCOMP_ERR_CORRUPT, "shrink: a broken table entry");
      }
      if (st->have_prev) {
        dec_add(st, st->prev, st->str[0]);
      }
      else if (code >= SH_FIRST) {
        return DFAIL(st, GCOMP_ERR_CORRUPT,
            "shrink: the stream starts with a code that is not a byte");
      }
    }
    st->prev = code;
    st->have_prev = 1;
  }
}

static gcomp_status_t sh_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  sh_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  const uint8_t * in;
  const uint8_t * end;
  int full;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->failed) {
    return st->err;
  }
  if (input->data) {
    in = (const uint8_t *)input->data + input->used;
    end = (const uint8_t *)input->data + input->size;
  }
  else {
    in = end = &none;
  }
  s = dec_run(st, &in, end, output, &full);
  if (input->data) {
    input->used = (size_t)(in - (const uint8_t *)input->data);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  if (gcomp_limits_check_expansion_ratio(st->in_total, st->produced,
          st->max_ratio) != GCOMP_OK) {
    return DFAIL(
        st, GCOMP_ERR_LIMIT, "shrink: expansion ratio exceeds the limit");
  }
  return GCOMP_OK;
}

static gcomp_status_t sh_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  sh_dec_t * st = decoder->method_state;
  static const uint8_t none = 0;
  const uint8_t * in = &none;
  int full;
  gcomp_status_t s;
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (st->failed) {
    return st->err;
  }
  s = dec_run(st, &in, &none, output, &full);
  if (s != GCOMP_OK) {
    return s;
  }
  if (full) {
    return GCOMP_ERR_LIMIT;
  }
  /* what is left is the padding to the byte, or the stream was cut */
  if (st->ctrl || st->bn >= 8) {
    return DFAIL(st, GCOMP_ERR_CORRUPT, "shrink: truncated stream");
  }
  return GCOMP_OK;
}

/* ---- the encoder ------------------------------------------------------------- */

#define SH_HASH 32768u

typedef struct {
  uint32_t key; /* (prefix << 8 | byte) + 1, or 0 */
  uint16_t code;
} sh_slot_t;

typedef struct {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  uint16_t prefix[SH_CODES];
  uint8_t suffix[SH_CODES];
  uint8_t used[SH_CODES];
  uint16_t nchild[SH_CODES];
  sh_slot_t hash[SH_HASH];
  unsigned scan, codesize, nent; /* nent: the entries made, 257 and up */
  int w;    /* the code of the string so far, or -1 */
  int pend; /* the last code sent, whose entry waits for the next byte, or -1 */
  uint64_t acc;
  unsigned nacc;
  uint8_t ob[64];
  size_t ob_len, ob_pos;
  int finished;
} sh_enc_t;

static unsigned slot_of(uint32_t key) {
  return (key * 2654435761u) >> (32u - 15u);
}

static void enc_restart(sh_enc_t * e) {
  memset(e->used, 0, sizeof(e->used));
  memset(e->used, 1, SH_FIRST);
  memset(e->nchild, 0, sizeof(e->nchild));
  memset(e->hash, 0, sizeof(e->hash));
  e->scan = SH_FIRST;
  e->nent = 0;
  e->codesize = SH_MIN_BITS;
  e->w = e->pend = -1;
  e->acc = 0;
  e->nacc = 0;
  e->ob_len = e->ob_pos = 0;
  e->finished = 0;
}

static int enc_find(sh_enc_t * e, unsigned prefix, unsigned c) {
  const uint32_t key = ((prefix << 8) | c) + 1u;
  unsigned i = slot_of(key);
  while (e->hash[i].key != 0) {
    if (e->hash[i].key == key) {
      return e->hash[i].code;
    }
    i = (i + 1) & (SH_HASH - 1u);
  }
  return -1;
}

static void enc_hash_insert(sh_enc_t * e, unsigned prefix, unsigned c, unsigned code) {
  const uint32_t key = ((prefix << 8) | c) + 1u;
  unsigned i = slot_of(key);
  while (e->hash[i].key != 0) {
    i = (i + 1) & (SH_HASH - 1u);
  }
  e->hash[i].key = key;
  e->hash[i].code = (uint16_t)code;
}

static unsigned enc_free_code(sh_enc_t * e) {
  while (e->scan < SH_CODES && e->used[e->scan]) {
    e->scan++;
  }
  return e->scan;
}

/* Make the entry (prefix, c) if a code is free; false if the table is full. */
static int enc_add(sh_enc_t * e, unsigned prefix, unsigned c) {
  const unsigned code = enc_free_code(e);
  if (code >= SH_CODES) {
    return 0;
  }
  e->prefix[code] = (uint16_t)prefix;
  e->suffix[code] = (uint8_t)c;
  e->used[code] = 1;
  e->nchild[code] = 0;
  if (prefix >= SH_FIRST) {
    e->nchild[prefix]++;
  }
  e->nent++;
  enc_hash_insert(e, prefix, c, code);
  return 1;
}

static void enc_put(sh_enc_t * e, unsigned v, unsigned bits) {
  e->acc |= (uint64_t)v << e->nacc;
  e->nacc += bits;
  while (e->nacc >= 8) {
    e->ob[e->ob_len++] = (uint8_t)e->acc;
    e->acc >>= 8;
    e->nacc -= 8;
  }
}

/* Free every entry nothing builds on, and rebuild the lookup. */
static void enc_partial_clear(sh_enc_t * e) {
  uint8_t leaf[SH_CODES];
  unsigned c;
  /* the leaves are found first, all at once, as the decoder finds them: a
   * parent whose only children are leaves stands until the next time */
  for (c = SH_FIRST; c < SH_CODES; c++) {
    leaf[c] = (uint8_t)(e->used[c] && e->nchild[c] == 0);
  }
  for (c = SH_FIRST; c < SH_CODES; c++) {
    if (leaf[c]) {
      e->used[c] = 0;
      e->nent--;
      if (e->prefix[c] >= SH_FIRST) {
        e->nchild[e->prefix[c]]--;
      }
    }
  }
  memset(e->hash, 0, sizeof(e->hash));
  for (c = SH_FIRST; c < SH_CODES; c++) {
    if (e->used[c]) {
      enc_hash_insert(e, e->prefix[c], e->suffix[c], c);
    }
  }
  e->scan = SH_FIRST;
}

/* Send the code of the string so far, and make the entry that follows it. */
static void enc_emit(sh_enc_t * e, unsigned code) {
  while (code >= (1u << e->codesize)) {
    enc_put(e, 256, e->codesize);
    enc_put(e, 1, e->codesize);
    e->codesize++;
  }
  enc_put(e, code, e->codesize);
}

static gcomp_status_t sh_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc;
  sh_enc_t * e;
  (void)options;
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  alloc = gcomp_registry_get_allocator(registry);
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "shrink: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  enc_restart(e);
  encoder->method_state = e;
  return GCOMP_OK;
}

static void sh_encoder_destroy(gcomp_encoder_t * encoder) {
  sh_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

static gcomp_status_t sh_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  enc_restart(encoder->method_state);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

static void enc_drain(sh_enc_t * e, gcomp_buffer_t * out) {
  size_t room = out->size - out->used;
  size_t n = e->ob_len - e->ob_pos;
  if (n > room) {
    n = room;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->ob + e->ob_pos, n);
    out->used += n;
    e->ob_pos += n;
  }
  if (e->ob_pos == e->ob_len) {
    e->ob_pos = e->ob_len = 0;
  }
}

/* How near the end of the table the encoder frees its leaves: early enough
 * that a moment when it is safe to is certain to come. */
#define SH_CLEAR_MARGIN 64u

/* One more byte of input. */
static void enc_byte(sh_enc_t * e, unsigned c) {
  int next;
  unsigned sent, made;
  if (e->w < 0) {
    e->w = (int)c;
    if (e->pend >= 0) {
      (void)enc_add(e, (unsigned)e->pend, c);
      e->pend = -1;
    }
    return;
  }
  next = enc_find(e, (unsigned)e->w, c);
  if (next >= 0) {
    e->w = next;
    return;
  }
  sent = (unsigned)e->w;
  enc_emit(e, sent);
  made = enc_add(e, sent, c) ? (unsigned)enc_find(e, sent, c) : SH_CODES;
  if (SH_CODES - SH_FIRST - e->nent <= SH_CLEAR_MARGIN) {
    /* The decoder does not have the entry just made until it reads the next
     * code, so what it frees is what is left without that one. It frees every
     * leaf, the code just sent among them if it is one, and the entry that
     * follows is built on that code: so the code must not be a leaf. */
    int safe;
    if (made < SH_CODES) {
      e->used[made] = 0;
      if (sent >= SH_FIRST) {
        e->nchild[sent]--;
      }
    }
    safe = sent < SH_FIRST || e->nchild[sent] != 0;
    if (made < SH_CODES && !safe) {
      e->used[made] = 1;
      if (sent >= SH_FIRST) {
        e->nchild[sent]++;
      }
    }
    if (safe) {
      if (made < SH_CODES) {
        e->nent--;
      }
      enc_put(e, 256, e->codesize);
      enc_put(e, 2, e->codesize);
      enc_partial_clear(e);
      (void)enc_add(e, sent, c);
    }
  }
  e->w = (int)c;
}

static gcomp_status_t sh_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  sh_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "shrink: encoder update after finish");
  }
  for (;;) {
    enc_drain(e, output);
    if (e->ob_len != 0 || input->used == input->size) {
      return GCOMP_OK;
    }
    enc_byte(e, ((const uint8_t *)input->data)[input->used++]);
  }
}

static gcomp_status_t sh_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  sh_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  for (;;) {
    enc_drain(e, output);
    if (e->ob_len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (e->finished) {
      return GCOMP_OK;
    }
    if (e->w >= 0) {
      enc_emit(e, (unsigned)e->w);
      e->w = -1;
    }
    if (e->nacc != 0) {
      enc_put(e, 0, 8u - e->nacc);
    }
    e->finished = 1;
  }
}

/* ---- registration ---------------------------------------------------------------- */

static const gcomp_option_schema_t g_sh_option_schemas[] = {
    {"limits.max_output_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum decompressed output", NULL},
    {"limits.max_memory_bytes", GCOMP_OPT_UINT64, 1, {.ui64 = 0}, 0, 0, 0, 0, 0,
        0, "Maximum memory usage", NULL},
    {"limits.max_expansion_ratio", GCOMP_OPT_UINT64, 1,
        {.ui64 = GCOMP_SHRINK_MAX_EXPANSION_RATIO}, 0, 0, 0, 0, 0, 0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL},
};

/* Kept in step with the schema above by SchemaAllMethodsTest. */
static const char * const g_sh_option_keys[] = {"limits.max_output_bytes",
    "limits.max_memory_bytes", "limits.max_expansion_ratio"};

static const gcomp_method_schema_t g_sh_schema = {g_sh_option_schemas,
    sizeof(g_sh_option_schemas) / sizeof(g_sh_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, g_sh_option_keys};

static const gcomp_method_schema_t * sh_get_schema(void) {
  return &g_sh_schema;
}

static gcomp_status_t sh_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = sh_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = sh_encoder_update;
  (*encoder_out)->finish_fn = sh_encoder_finish;
  (*encoder_out)->reset_fn = sh_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t sh_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = sh_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = sh_decoder_update;
  (*decoder_out)->finish_fn = sh_decoder_finish;
  (*decoder_out)->reset_fn = sh_decoder_reset;
  return GCOMP_OK;
}

static void sh_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  sh_encoder_destroy(encoder);
}

static void sh_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  sh_decoder_destroy(decoder);
}

/** Worst case: every byte is a code of 13 bits, and a control pair every
 * so often; 2 bytes per input byte covers it with a margin. */
static gcomp_status_t sh_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  size_t bound = 0;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = gcomp_bound_add_mul(&bound, input_size, 2u);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 16u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static const gcomp_method_t g_sh_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "shrink",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = sh_create_encoder,
    .create_decoder = sh_create_decoder,
    .destroy_encoder = sh_destroy_encoder_wrapper,
    .destroy_decoder = sh_destroy_decoder_wrapper,
    .get_schema = sh_get_schema,
    .encode_bound = sh_encode_bound,
};

gcomp_status_t gcomp_method_shrink_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_sh_method);
}

GCOMP_AUTOREG_METHOD(shrink, gcomp_method_shrink_register)
