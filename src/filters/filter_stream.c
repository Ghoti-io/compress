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
 * @file filter_stream.c
 *
 * The `delta` and `bcj` methods: a transform that is size-preserving and
 * has no header, run as an encoder and a decoder like any other method.
 *
 * Both directions are the same machine with the sense of the transform
 * swapped. Bytes go into a small buffer, the converter settles as many of
 * them as it can, and the settled bytes are handed out as the caller's output
 * has room for them. What it cannot settle stays in the buffer for the next
 * call, and at the end of the stream it is let through unchanged, which is
 * what xz does with the last few bytes of a stream that might have been the
 * start of a branch.
 *
 * The ids and the stream positions are xz's, so that these methods are the
 * filters in an xz stream and the coders in a 7z archive rather than
 * look-alikes.
 */

#include <ghoti.io/compress/macros.h>

#include "../autoreg/autoreg_platform.h"
#include "../core/alloc_internal.h"
#include "../core/registry_internal.h"
#include "../core/stream_internal.h"
#include "filter_internal.h"
#include "filter_transform.h"

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/filter.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include <stdint.h>
#include <string.h>

#define FILTER_BUF 4096u

typedef struct {
  const gcomp_allocator_t * alloc;
  int delta;     /* 1: delta, 0: a branch converter */
  int encoding;  /* 1: forward, 0: inverse */
  int finished;  /* finish() has been called */
  unsigned arch; /* bcj */
  unsigned dist; /* delta */
  uint32_t start_offset;
  uint32_t pos; /* stream position of buf[0], modulo 2^32 */
  filter_bcj_state_t bcj;
  filter_delta_state_t dstate;
  size_t len;   /* bytes held in buf */
  size_t ready; /* of those, the settled ones, at the start */
  size_t sent;  /* of those, the ones already handed out */
  uint8_t buf[FILTER_BUF];
} filter_t;

static const struct {
  const char * name;
  unsigned id;
} g_arch[] = {
    {"x86", FILTER_ARCH_X86},
    {"powerpc", FILTER_ARCH_POWERPC},
    {"ia64", FILTER_ARCH_IA64},
    {"arm", FILTER_ARCH_ARM},
    {"armthumb", FILTER_ARCH_ARMTHUMB},
    {"sparc", FILTER_ARCH_SPARC},
    {"arm64", FILTER_ARCH_ARM64},
};

#define ARCH_COUNT (sizeof(g_arch) / sizeof(g_arch[0]))

static const char * const g_arch_names[] = {
    "x86", "powerpc", "ia64", "arm", "armthumb", "sparc", "arm64", NULL};

int filter_arch_id(const char * name, unsigned * id_out) {
  size_t i;
  if (!name) {
    return 0;
  }
  for (i = 0; i < ARCH_COUNT; i++) {
    if (strcmp(name, g_arch[i].name) == 0) {
      *id_out = g_arch[i].id;
      return 1;
    }
  }
  return 0;
}

const char * filter_arch_name(unsigned id) {
  size_t i;
  for (i = 0; i < ARCH_COUNT; i++) {
    if (g_arch[i].id == id) {
      return g_arch[i].name;
    }
  }
  return NULL;
}

/* ---- the machine -------------------------------------------------------- */

static void filter_restart(filter_t * f) {
  f->finished = 0;
  f->pos = f->start_offset;
  f->len = f->ready = f->sent = 0;
  filter_bcj_init(&f->bcj);
  filter_delta_init(&f->dstate);
}

/* Settle what is in the buffer. */
static void filter_settle(filter_t * f) {
  size_t n;
  if (f->delta) {
    filter_delta_code(&f->dstate, f->dist, f->encoding, f->buf, f->len);
    n = f->len;
  }
  else {
    n = filter_bcj_code(
        &f->bcj, f->arch, f->pos, f->encoding, f->buf, f->len);
  }
  f->pos += (uint32_t)n;
  f->ready = n;
}

typedef enum { RUN_DONE, RUN_FULL, RUN_WAIT } filter_run_t;

/* Hand out what is settled, take in what there is room for, settle it, and
 * go round again. With `final` set nothing more is coming and what cannot be
 * settled is let through as it is. Returns RUN_FULL when the output filled
 * with something still to hand out, RUN_WAIT when the input ran out first, and
 * RUN_DONE only for `final`, once everything is out. */
static filter_run_t filter_run(
    filter_t * f, gcomp_buffer_t * input, gcomp_buffer_t * output, int final) {
  for (;;) {
    if (f->sent < f->ready) {
      size_t room = output->size - output->used;
      size_t n = f->ready - f->sent;
      if (n > room) {
        n = room;
      }
      if (n != 0) {
        memcpy((uint8_t *)output->data + output->used, f->buf + f->sent, n);
        output->used += n;
        f->sent += n;
      }
      if (f->sent < f->ready) {
        return RUN_FULL;
      }
    }
    if (f->ready != 0) {
      memmove(f->buf, f->buf + f->ready, f->len - f->ready);
      f->len -= f->ready;
      f->ready = f->sent = 0;
    }
    if (final) {
      if (f->len == 0) {
        return RUN_DONE;
      }
      f->ready = f->len;
      continue;
    }
    {
      size_t avail = input ? input->size - input->used : 0;
      size_t take = FILTER_BUF - f->len;
      if (take > avail) {
        take = avail;
      }
      if (take == 0) {
        return RUN_WAIT;
      }
      memcpy(f->buf + f->len, (const uint8_t *)input->data + input->used, take);
      input->used += take;
      f->len += take;
      filter_settle(f);
    }
  }
}

/* ---- entry points ------------------------------------------------------- */

static int filter_check_buffers(
    const gcomp_buffer_t * input, const gcomp_buffer_t * output) {
  if (input && input->size > 0 && input->data == NULL) {
    return 0;
  }
  return !(output->size > 0 && output->data == NULL);
}

static gcomp_status_t filter_update_common(void * state, int encoding,
    gcomp_buffer_t * input, gcomp_buffer_t * output, const char ** why) {
  filter_t * f = state;
  (void)encoding;
  if (!f) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (!filter_check_buffers(input, output)) {
    *why = "a buffer has a size but no data";
    return GCOMP_ERR_INVALID_ARG;
  }
  if (f->finished) {
    *why = "update after finish";
    return GCOMP_ERR_INVALID_ARG;
  }
  (void)filter_run(f, input, output, 0);
  return GCOMP_OK;
}

static gcomp_status_t filter_finish_common(
    void * state, gcomp_buffer_t * output, const char ** why) {
  filter_t * f = state;
  if (!f) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    *why = "output has a size but no data";
    return GCOMP_ERR_INVALID_ARG;
  }
  f->finished = 1;
  return filter_run(f, NULL, output, 1) == RUN_DONE ? GCOMP_OK
                                                    : GCOMP_ERR_LIMIT;
}

static gcomp_status_t filter_encoder_update(gcomp_encoder_t * e,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  const char * why = "";
  gcomp_status_t s =
      filter_update_common(e->method_state, 1, input, output, &why);
  if (s != GCOMP_OK && s != GCOMP_ERR_LIMIT) {
    return gcomp_encoder_set_error(e, s, "filter: %s", why);
  }
  return s;
}

static gcomp_status_t filter_encoder_finish(
    gcomp_encoder_t * e, gcomp_buffer_t * output) {
  const char * why = "";
  gcomp_status_t s = filter_finish_common(e->method_state, output, &why);
  if (s == GCOMP_ERR_INVALID_ARG) {
    return gcomp_encoder_set_error(e, s, "filter: %s", why);
  }
  return s;
}

/* Delta settles every byte as it arrives, so a flush is only a drain. A
 * branch converter holds back the bytes that may start a branch and cannot
 * flush without giving those up, so it does not. */
static gcomp_status_t filter_encoder_flush(
    gcomp_encoder_t * e, gcomp_buffer_t * output, gcomp_flush_t mode) {
  filter_t * f = e->method_state;
  (void)mode;
  if (!f) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (f->finished) {
    return gcomp_encoder_set_error(
        e, GCOMP_ERR_INVALID_ARG, "filter: flush after finish");
  }
  return filter_run(f, NULL, output, 0) == RUN_FULL ? GCOMP_ERR_LIMIT
                                                   : GCOMP_OK;
}

static gcomp_status_t filter_encoder_reset(gcomp_encoder_t * e) {
  filter_t * f = e->method_state;
  if (!f) {
    return GCOMP_ERR_INVALID_ARG;
  }
  filter_restart(f);
  e->last_error = GCOMP_OK;
  e->error_detail[0] = '\0';
  return GCOMP_OK;
}

static gcomp_status_t filter_decoder_update(gcomp_decoder_t * d,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  const char * why = "";
  gcomp_status_t s =
      filter_update_common(d->method_state, 0, input, output, &why);
  if (s != GCOMP_OK && s != GCOMP_ERR_LIMIT) {
    return gcomp_decoder_set_error(d, s, "filter: %s", why);
  }
  return s;
}

static gcomp_status_t filter_decoder_finish(
    gcomp_decoder_t * d, gcomp_buffer_t * output) {
  const char * why = "";
  gcomp_status_t s = filter_finish_common(d->method_state, output, &why);
  if (s == GCOMP_ERR_INVALID_ARG) {
    return gcomp_decoder_set_error(d, s, "filter: %s", why);
  }
  return s;
}

static gcomp_status_t filter_decoder_reset(gcomp_decoder_t * d) {
  filter_t * f = d->method_state;
  if (!f) {
    return GCOMP_ERR_INVALID_ARG;
  }
  filter_restart(f);
  d->last_error = GCOMP_OK;
  d->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* ---- options and registration ------------------------------------------- */

static gcomp_status_t filter_state_create(gcomp_registry_t * registry,
    gcomp_options_t * options, int delta, int encoding, filter_t ** out) {
  filter_t * f;
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  f = gcomp_calloc(alloc, 1, sizeof(*f));
  if (!f) {
    return GCOMP_ERR_MEMORY;
  }
  f->alloc = alloc;
  f->delta = delta;
  f->encoding = encoding;
  f->dist = 1;
  f->arch = FILTER_ARCH_X86;
  if (options) {
    int64_t i64 = 0;
    uint64_t u64 = 0;
    const char * str = NULL;
    if (delta) {
      if (gcomp_options_get_int64(options, "delta.distance", &i64) ==
          GCOMP_OK) {
        f->dist = (unsigned)i64;
      }
    }
    else {
      if (gcomp_options_get_string(options, "bcj.arch", &str) == GCOMP_OK &&
          str) {
        unsigned id;
        if (filter_arch_id(str, &id)) {
          f->arch = id;
        }
      }
      if (gcomp_options_get_uint64(options, "bcj.start_offset", &u64) ==
          GCOMP_OK) {
        f->start_offset = (uint32_t)u64;
      }
    }
  }
  filter_restart(f);
  *out = f;
  return GCOMP_OK;
}

static gcomp_status_t filter_create_encoder_impl(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out, int delta) {
  filter_t * f = NULL;
  gcomp_status_t s;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = filter_state_create(registry, options, delta, 1, &f);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(
        *encoder_out, s, "filter: out of memory for the encoder");
  }
  (*encoder_out)->method_state = f;
  (*encoder_out)->update_fn = filter_encoder_update;
  (*encoder_out)->finish_fn = filter_encoder_finish;
  if (delta) {
    (*encoder_out)->flush_fn = filter_encoder_flush;
  }
  (*encoder_out)->reset_fn = filter_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t filter_create_decoder_impl(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out, int delta) {
  filter_t * f = NULL;
  gcomp_status_t s;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = filter_state_create(registry, options, delta, 0, &f);
  if (s != GCOMP_OK) {
    return gcomp_decoder_set_error(
        *decoder_out, s, "filter: out of memory for the decoder");
  }
  (*decoder_out)->method_state = f;
  (*decoder_out)->update_fn = filter_decoder_update;
  (*decoder_out)->finish_fn = filter_decoder_finish;
  (*decoder_out)->reset_fn = filter_decoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t delta_create_encoder(gcomp_registry_t * r,
    gcomp_options_t * o, gcomp_encoder_t ** e) {
  return filter_create_encoder_impl(r, o, e, 1);
}
static gcomp_status_t delta_create_decoder(gcomp_registry_t * r,
    gcomp_options_t * o, gcomp_decoder_t ** d) {
  return filter_create_decoder_impl(r, o, d, 1);
}
static gcomp_status_t bcj_create_encoder(gcomp_registry_t * r,
    gcomp_options_t * o, gcomp_encoder_t ** e) {
  return filter_create_encoder_impl(r, o, e, 0);
}
static gcomp_status_t bcj_create_decoder(gcomp_registry_t * r,
    gcomp_options_t * o, gcomp_decoder_t ** d) {
  return filter_create_decoder_impl(r, o, d, 0);
}

static void filter_destroy_encoder(gcomp_encoder_t * e) {
  filter_t * f;
  if (!e || !e->method_state) {
    return;
  }
  f = e->method_state;
  gcomp_free(f->alloc, f);
  e->method_state = NULL;
}

static void filter_destroy_decoder(gcomp_decoder_t * d) {
  filter_t * f;
  if (!d || !d->method_state) {
    return;
  }
  f = d->method_state;
  gcomp_free(f->alloc, f);
  d->method_state = NULL;
}

/* The output is the input, byte for byte in length. */
static gcomp_status_t filter_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *bound_out = input_size;
  return GCOMP_OK;
}

static const gcomp_option_schema_t g_delta_schemas[] = {
    {
        "delta.distance",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 1},
        1,
        1,
        1,
        256,
        0,
        0,
        "How far back the byte to subtract is, 1..256. 1 for a sampled signal, "
        "the sample width in bytes for interleaved samples, or the row width "
        "for an image.",
        NULL,
    },
};

static const char * const g_delta_keys[] = {"delta.distance"};

static const gcomp_method_schema_t g_delta_schema = {
    g_delta_schemas,
    sizeof(g_delta_schemas) / sizeof(g_delta_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_delta_keys,
};

static const gcomp_option_schema_t g_bcj_schemas[] = {
    {
        "bcj.arch",
        GCOMP_OPT_STRING,
        1,
        {.str = "x86"},
        0,
        0,
        0,
        0,
        0,
        0,
        "The instruction set whose branches are converted: x86, powerpc, ia64, "
        "arm, armthumb, sparc or arm64. The decoder must be given the same.",
        g_arch_names,
    },
    {
        "bcj.start_offset",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        1,
        1,
        0,
        0,
        0,
        UINT32_MAX,
        "The address of the first byte, as the code will see it; zero unless "
        "the data is a piece cut from the middle of a program.",
        NULL,
    },
};

static const char * const g_bcj_keys[] = {"bcj.arch", "bcj.start_offset"};

static const gcomp_method_schema_t g_bcj_schema = {
    g_bcj_schemas,
    sizeof(g_bcj_schemas) / sizeof(g_bcj_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_bcj_keys,
};

static const gcomp_method_schema_t * delta_get_schema(void) {
  return &g_delta_schema;
}

static const gcomp_method_schema_t * bcj_get_schema(void) {
  return &g_bcj_schema;
}

static const gcomp_method_t g_delta_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "delta",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = delta_create_encoder,
    .create_decoder = delta_create_decoder,
    .destroy_encoder = filter_destroy_encoder,
    .destroy_decoder = filter_destroy_decoder,
    .get_schema = delta_get_schema,
    .encode_bound = filter_encode_bound,
};

static const gcomp_method_t g_bcj_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "bcj",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = bcj_create_encoder,
    .create_decoder = bcj_create_decoder,
    .destroy_encoder = filter_destroy_encoder,
    .destroy_decoder = filter_destroy_decoder,
    .get_schema = bcj_get_schema,
    .encode_bound = filter_encode_bound,
};

gcomp_status_t gcomp_method_delta_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_delta_method);
}

gcomp_status_t gcomp_method_bcj_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_bcj_method);
}

GCOMP_AUTOREG_METHOD(delta, gcomp_method_delta_register)
GCOMP_AUTOREG_METHOD(bcj, gcomp_method_bcj_register)
