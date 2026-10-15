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
 * @file deflate64_register.c
 *
 * deflate64 method registration: its option schema and its bound.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/bound_internal.h"
#include "../../core/stream_internal.h"
#include "deflate64_internal.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate64.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

static const gcomp_option_schema_t g_d64_option_schemas[] = {
    {
        "deflate64.level",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 6},
        1,
        1,
        0,
        9,
        0,
        0,
        "Compression level 0 to 9; 0 stores, 1 to 3 take the first match, 4 and up try the next position too.",
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
        "Maximum decompressed output",
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
        {.ui64 = GCOMP_DEFLATE64_MAX_EXPANSION_RATIO},
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

/* Kept in step with the schema above by SchemaAllMethodsTest. */
static const char * const g_d64_option_keys[] = {
    "deflate64.level",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_d64_schema = {
    g_d64_option_schemas,
    sizeof(g_d64_option_schemas) / sizeof(g_d64_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_d64_option_keys,
};

static const gcomp_method_schema_t * d64_get_schema(void) {
  return &g_d64_schema;
}

static gcomp_status_t d64_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = d64_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = d64_encoder_update;
  (*encoder_out)->finish_fn = d64_encoder_finish;
  (*encoder_out)->flush_fn = d64_encoder_flush;
  (*encoder_out)->reset_fn = d64_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t d64_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = d64_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = d64_decoder_update;
  (*decoder_out)->finish_fn = d64_decoder_finish;
  (*decoder_out)->reset_fn = d64_decoder_reset;
  return GCOMP_OK;
}

static void d64_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  d64_encoder_destroy(encoder);
}

static void d64_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  d64_decoder_destroy(decoder);
}

/**
 * @brief Bytes for the worst case: every block stored.
 *
 * The encoder takes 64 KiB of input a block and codes it as the smallest of
 * stored, fixed and dynamic, so no block is bigger than stored, which costs
 * five bytes for each 65535 (two for a 65536-byte block). One more block for
 * the empty final one, and a margin.
 */
static gcomp_status_t d64_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  size_t blocks, bound = input_size;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  s = gcomp_bound_block_count(input_size, 32768u, &blocks);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add_mul(&bound, blocks, 8u);
  }
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 32u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static const gcomp_method_t g_d64_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "deflate64",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = d64_create_encoder,
    .create_decoder = d64_create_decoder,
    .destroy_encoder = d64_destroy_encoder_wrapper,
    .destroy_decoder = d64_destroy_decoder_wrapper,
    .get_schema = d64_get_schema,
    .encode_bound = d64_encode_bound,
};

gcomp_status_t gcomp_method_deflate64_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_d64_method);
}

GCOMP_AUTOREG_METHOD(deflate64, gcomp_method_deflate64_register)
