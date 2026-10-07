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
 * @file lzma_register.c
 *
 * LZMA and LZMA2 method registration.
 *
 * Limit options use the `limits.*` prefix shared with core. For both methods
 * `limits.max_window_bytes` bounds the window a stream may claim; the decoder
 * grows its dictionary as output arrives, so a large claim costs nothing until
 * the stream backs it up.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/bound_internal.h"
#include "../../core/stream_internal.h"
#include "lzma_internal.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzma.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include <stdint.h>
#include <string.h>

#define LZMA_LIMIT_OPTIONS(ratio, window)                                       \
  {                                                                            \
      "limits.max_output_bytes",                                               \
      GCOMP_OPT_UINT64,                                                        \
      1,                                                                       \
      {.ui64 = 0},                                                             \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      "Maximum decompressed output bytes",                                     \
      NULL,                                                                    \
  },                                                                           \
  {                                                                            \
      "limits.max_memory_bytes",                                               \
      GCOMP_OPT_UINT64,                                                        \
      1,                                                                       \
      {.ui64 = 0},                                                             \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      "Maximum memory usage",                                                  \
      NULL,                                                                    \
  },                                                                           \
  {                                                                            \
      "limits.max_expansion_ratio",                                            \
      GCOMP_OPT_UINT64,                                                        \
      1,                                                                       \
      {.ui64 = (ratio)},                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      "Maximum output/input ratio; default is this format's own ceiling",      \
      NULL,                                                                    \
  },                                                                           \
  {                                                                            \
      "limits.max_window_bytes",                                               \
      GCOMP_OPT_UINT64,                                                        \
      1,                                                                       \
      {.ui64 = (window)},                                                      \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      0,                                                                       \
      "Maximum window a stream may claim. Default is 1.5 GiB, xz's largest.",  \
      NULL,                                                                    \
  }

#define LZMA_LIMIT_KEYS                                                        \
  "limits.max_output_bytes", "limits.max_memory_bytes",                        \
      "limits.max_expansion_ratio", "limits.max_window_bytes"

#define LZMA_DEFAULT_WINDOW (3ull << 29)

static const gcomp_option_schema_t g_lzma_option_schemas[] = {
    {
        "lzma.raw",
        GCOMP_OPT_BOOL,
        1,
        {.b = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "No 13-byte header: the stream is bare and lc, lp, pb, dict_size and "
        "uncompressed_size say what the header would have.",
        NULL,
    },
    {
        "lzma.lc",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 3},
        1,
        1,
        0,
        8,
        0,
        0,
        "Literal context bits, 0..8. Raw streams only; a header carries it.",
        NULL,
    },
    {
        "lzma.lp",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 0},
        1,
        1,
        0,
        4,
        0,
        0,
        "Literal position bits, 0..4. Raw streams only; a header carries it.",
        NULL,
    },
    {
        "lzma.pb",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 2},
        1,
        1,
        0,
        4,
        0,
        0,
        "Position bits, 0..4. Raw streams only; a header carries it.",
        NULL,
    },
    {
        "lzma.dict_size",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 1u << 23},
        1,
        1,
        0,
        0,
        LZMA_DICT_MIN,
        0xFFFFFFFFull,
        "Dictionary size in bytes, 4096..4294967295. Raw streams only.",
        NULL,
    },
    {
        "lzma.uncompressed_size",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = UINT64_MAX},
        0,
        0,
        0,
        0,
        0,
        0,
        "Raw decoder: bytes the stream holds, which then needs no end marker. "
        "The default, the largest value, means the stream ends with a marker.",
        NULL,
    },
    LZMA_LIMIT_OPTIONS(GCOMP_LZMA_MAX_EXPANSION_RATIO, LZMA_DEFAULT_WINDOW),
};

/* One entry per g_lzma_option_schemas entry, in the same order: see the note
 * on g_brotli_option_keys. SchemaAllMethodsTest.EveryKeyArrayMatchesItsSchema
 * checks it. */
static const char * const g_lzma_option_keys[] = {
    "lzma.raw",
    "lzma.lc",
    "lzma.lp",
    "lzma.pb",
    "lzma.dict_size",
    "lzma.uncompressed_size",
    LZMA_LIMIT_KEYS,
};

static const gcomp_method_schema_t g_lzma_schema = {
    g_lzma_option_schemas,
    sizeof(g_lzma_option_schemas) / sizeof(g_lzma_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_lzma_option_keys,
};

static const gcomp_option_schema_t g_lzma2_option_schemas[] = {
    LZMA_LIMIT_OPTIONS(GCOMP_LZMA_MAX_EXPANSION_RATIO, LZMA_DEFAULT_WINDOW),
};

static const char * const g_lzma2_option_keys[] = {
    LZMA_LIMIT_KEYS,
};

static const gcomp_method_schema_t g_lzma2_schema = {
    g_lzma2_option_schemas,
    sizeof(g_lzma2_option_schemas) / sizeof(g_lzma2_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_lzma2_option_keys,
};

static const gcomp_method_schema_t * lzma_get_schema(void) {
  return &g_lzma_schema;
}

static const gcomp_method_schema_t * lzma2_get_schema(void) {
  return &g_lzma2_schema;
}

static gcomp_status_t lzma_wire_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out, int lzma2) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = lzma_decoder_init(registry, options, *decoder_out, lzma2);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = lzma_decoder_update;
  (*decoder_out)->finish_fn = lzma_decoder_finish;
  (*decoder_out)->reset_fn = lzma_decoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t lzma_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  return lzma_wire_decoder(registry, options, decoder_out, 0);
}

static gcomp_status_t lzma2_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  return lzma_wire_decoder(registry, options, decoder_out, 1);
}

static void lzma_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  lzma_decoder_destroy(decoder);
}

/**
 * @brief What the 13-byte header of an `.lzma` file says.
 *
 * The properties byte has to be one the format can spell; that and the length
 * are all that can be checked, which is why detect does not claim this format.
 * A raw stream has no header to read.
 */
static gcomp_status_t lzma_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  const uint8_t * p = (const uint8_t *)input;
  unsigned lc, lp, pb;
  uint64_t dict = 0, size = 0;
  unsigned i;
  int raw = 0;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_bool(options, "lzma.raw", &raw);
  }
  if (raw) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (input_size < 13u || p == NULL) {
    if (needed_out) {
      *needed_out = 13;
    }
    return GCOMP_ERR_LIMIT;
  }
  if (!lzma_props_decode(p[0], &lc, &lp, &pb)) {
    return GCOMP_ERR_CORRUPT;
  }
  for (i = 0; i < 4; i++) {
    dict |= (uint64_t)p[1 + i] << (8u * i);
  }
  for (i = 0; i < 8; i++) {
    size |= (uint64_t)p[5 + i] << (8u * i);
  }
  if (dict < LZMA_DICT_MIN) {
    dict = LZMA_DICT_MIN;
  }
  info_out->header_size = 13;
  info_out->window_size = dict;
  if (size != UINT64_MAX) {
    info_out->has_content_size = 1;
    info_out->content_size = size;
  }
  if (needed_out) {
    *needed_out = 13;
  }
  return GCOMP_OK;
}

static const gcomp_method_t g_lzma_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "lzma",
    .capabilities = GCOMP_CAP_DECODE,
    .create_decoder = lzma_create_decoder,
    .destroy_decoder = lzma_destroy_decoder_wrapper,
    .get_schema = lzma_get_schema,
    .peek = lzma_peek,
};

static const gcomp_method_t g_lzma2_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "lzma2",
    .capabilities = GCOMP_CAP_DECODE,
    .create_decoder = lzma2_create_decoder,
    .destroy_decoder = lzma_destroy_decoder_wrapper,
    .get_schema = lzma2_get_schema,
};

gcomp_status_t gcomp_method_lzma_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_lzma_method);
}

gcomp_status_t gcomp_method_lzma2_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_lzma2_method);
}

GCOMP_AUTOREG_METHOD(lzma, gcomp_method_lzma_register)
GCOMP_AUTOREG_METHOD(lzma2, gcomp_method_lzma2_register)
