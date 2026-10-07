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
    .capabilities = GCOMP_CAP_DECODE,
    .create_decoder = bzip2_create_decoder,
    .destroy_decoder = bzip2_destroy_decoder_wrapper,
    .get_schema = bzip2_get_schema,
    .peek = bzip2_peek,
};

gcomp_status_t gcomp_method_bzip2_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_bzip2_method);
}

GCOMP_AUTOREG_METHOD(bzip2, gcomp_method_bzip2_register)
