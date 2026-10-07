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
 * @file bzip2_register.c
 *
 * bzip2 method registration. The stream has a magic number, so peek reads the
 * level from it and gcomp_detect() recognises it.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/bound_internal.h"
#include "../../core/stream_internal.h"
#include "bzip2_internal.h"

#include <ghoti.io/compress/bzip2.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include <stdint.h>
#include <string.h>

static const gcomp_option_schema_t g_bzip2_option_schemas[] = {
    {
        "bzip2.level",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 9},
        1,
        1,
        1,
        9,
        0,
        0,
        "Block size in hundreds of thousands of bytes, 1..9. The encoder holds "
        "about 21 times a block while it sorts one.",
        NULL,
    },
    {
        "limits.max_output_bytes",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum decompressed output bytes",
        NULL,
    },
    {
        "limits.max_memory_bytes",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum memory usage",
        NULL,
    },
    {
        "limits.max_expansion_ratio",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = GCOMP_BZIP2_MAX_EXPANSION_RATIO},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL,
    },
};

/* One entry per g_bzip2_option_schemas entry, in the same order: see the note
 * on g_brotli_option_keys. */
static const char * const g_bzip2_option_keys[] = {
    "bzip2.level",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_bzip2_schema = {
    g_bzip2_option_schemas,
    sizeof(g_bzip2_option_schemas) / sizeof(g_bzip2_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_bzip2_option_keys,
};

static const gcomp_method_schema_t * bzip2_get_schema(void) {
  return &g_bzip2_schema;
}

static gcomp_status_t bzip2_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = bzip2_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = bzip2_encoder_update;
  (*encoder_out)->finish_fn = bzip2_encoder_finish;
  (*encoder_out)->flush_fn = bzip2_encoder_flush;
  (*encoder_out)->reset_fn = bzip2_encoder_reset;
  return GCOMP_OK;
}

static void bzip2_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  bzip2_encoder_destroy(encoder);
}

/**
 * @brief Bytes for the worst case of a bzip2 stream.
 *
 * libbz2 documents 1% and 600 bytes. A stream here is the same coding, and the
 * margin is doubled and the constant raised so that a difference in how the
 * tables are chosen cannot make a block a few bytes larger than libbz2's and
 * the bound a lie: the input, 2% of it, and 1024 bytes, which covers the
 * header, the end marker and the tables of an empty or one-block stream.
 */
static gcomp_status_t bzip2_encode_bound(gcomp_options_t * options,
    size_t input_size, size_t * bound_out) {
  size_t bound = 1024;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = gcomp_bound_add(&bound, input_size);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, input_size / 50u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static gcomp_status_t bzip2_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = bzip2_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = bzip2_decoder_update;
  (*decoder_out)->finish_fn = bzip2_decoder_finish;
  (*decoder_out)->reset_fn = bzip2_decoder_reset;
  return GCOMP_OK;
}

static void bzip2_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  bzip2_decoder_destroy(decoder);
}

/**
 * @brief What the four bytes `BZh1` to `BZh9` say.
 *
 * The level is the block size in hundreds of thousands of bytes, and a decoder
 * holds one block, so it is the history the decoder keeps. The stream carries
 * CRCs, and does not say its own length.
 */
static gcomp_status_t bzip2_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  const uint8_t * p = (const uint8_t *)input;
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (input_size < 4 || p == NULL) {
    if (needed_out) {
      *needed_out = 4;
    }
    return GCOMP_ERR_LIMIT;
  }
  if (p[0] != 'B' || p[1] != 'Z' || p[2] != 'h' || p[3] < '1' || p[3] > '9') {
    return GCOMP_ERR_CORRUPT;
  }
  info_out->header_size = 4;
  info_out->window_size = (uint64_t)(p[3] - '0') * BZIP2_BLOCK_UNIT;
  info_out->has_checksum = 1;
  if (needed_out) {
    *needed_out = 4;
  }
  return GCOMP_OK;
}

static const gcomp_method_t g_bzip2_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "bzip2",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = bzip2_create_encoder,
    .create_decoder = bzip2_create_decoder,
    .destroy_encoder = bzip2_destroy_encoder_wrapper,
    .destroy_decoder = bzip2_destroy_decoder_wrapper,
    .get_schema = bzip2_get_schema,
    .encode_bound = bzip2_encode_bound,
    .peek = bzip2_peek,
};

gcomp_status_t gcomp_method_bzip2_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_bzip2_method);
}

GCOMP_AUTOREG_METHOD(bzip2, gcomp_method_bzip2_register)
