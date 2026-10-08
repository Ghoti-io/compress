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
 * @file xz_register.c
 *
 * xz method registration, its option schema, its bound and its peek.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/bound_internal.h"
#include "../../core/stream_internal.h"
#include "xz_internal.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/xz.h>

#include <stdint.h>
#include <string.h>

static const char * const g_check_names[] = {
    "none", "crc32", "crc64", "sha256", NULL};

static const gcomp_option_schema_t g_xz_option_schemas[] = {
    {
        "xz.preset",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 6},
        1,
        1,
        0,
        9,
        0,
        0,
        "Encoder effort and dictionary, 0..9, as xz's presets.",
        NULL,
    },
    {
        "xz.dict_size",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Dictionary in bytes; 0 takes the preset's. The stream names the next size up that xz can spell.",
        NULL,
    },
    {
        "xz.lc",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 3},
        1,
        1,
        0,
        4,
        0,
        0,
        "Literal context bits, 0..4. With lp, at most 4.",
        NULL,
    },
    {
        "xz.lp",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 0},
        1,
        1,
        0,
        4,
        0,
        0,
        "Literal position bits, 0..4. With lc, at most 4.",
        NULL,
    },
    {
        "xz.pb",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 2},
        1,
        1,
        0,
        4,
        0,
        0,
        "Position bits, 0..4.",
        NULL,
    },
    {
        "xz.check",
        GCOMP_OPT_STRING,
        1,
        {.str = "crc64"},
        0,
        0,
        0,
        0,
        0,
        0,
        "The integrity check of each block: none, crc32, crc64 or sha256.",
        g_check_names,
    },
    {
        "xz.block_size",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = 0},
        0,
        0,
        0,
        0,
        0,
        0,
        "Uncompressed bytes per block; 0 puts the whole stream in one. Smaller blocks cost ratio and let a reader decode one without the rest.",
        NULL,
    },
    {
        "xz.filters",
        GCOMP_OPT_STRING,
        1,
        {.str = ""},
        0,
        0,
        0,
        0,
        0,
        0,
        "Filters applied before LZMA2, comma separated, in the order they are applied when encoding: delta, delta:N, or x86, powerpc, ia64, arm, armthumb, sparc, arm64, each with an optional :OFFSET. At most three.",
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
        {.ui64 = GCOMP_XZ_MAX_EXPANSION_RATIO},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL,
    },
    {
        "limits.max_window_bytes",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = GCOMP_XZ_DEFAULT_WINDOW},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum dictionary a block may name. Default is 1.5 GiB, xz's largest.",
        NULL,
    },
};

/* Kept in step with the schema above by SchemaAllMethodsTest. */
static const char * const g_xz_option_keys[] = {
    "xz.preset",
    "xz.dict_size",
    "xz.lc",
    "xz.lp",
    "xz.pb",
    "xz.check",
    "xz.block_size",
    "xz.filters",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
    "limits.max_window_bytes",
};

static const gcomp_method_schema_t g_xz_schema = {
    g_xz_option_schemas,
    sizeof(g_xz_option_schemas) / sizeof(g_xz_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_xz_option_keys,
};

static const gcomp_method_schema_t * xz_get_schema(void) {
  return &g_xz_schema;
}

static gcomp_status_t xz_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = xz_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = xz_encoder_update;
  (*encoder_out)->finish_fn = xz_encoder_finish;
  (*encoder_out)->flush_fn = xz_encoder_flush;
  (*encoder_out)->reset_fn = xz_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t xz_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = xz_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = xz_decoder_update;
  (*decoder_out)->finish_fn = xz_decoder_finish;
  (*decoder_out)->reset_fn = xz_decoder_reset;
  return GCOMP_OK;
}

static void xz_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  xz_encoder_destroy(encoder);
}

static void xz_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  xz_decoder_destroy(decoder);
}

/**
 * @brief Bytes for the worst case of an xz stream.
 *
 * The two ends of the stream are 24 bytes. Each block adds a header (at most
 * 28 bytes with three filters, 32 allowed), up to three bytes of padding, a
 * check of at most 32, and an index record of at most 18; and its LZMA2 data
 * is the bytes themselves plus six for each 16 KiB chunk, at most, with 64 to
 * spare for the stream's end. The index itself has a count, padding and a
 * CRC, 20 more.
 */
static gcomp_status_t xz_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  uint64_t block_size = 0;
  size_t blocks = 0, chunks;
  size_t bound = 24u + 20u;
  gcomp_status_t s;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_uint64(options, "xz.block_size", &block_size);
  }
  if (input_size != 0) {
    if (block_size == 0 || block_size >= (uint64_t)input_size) {
      blocks = 1;
    }
    else {
      s = gcomp_bound_block_count(input_size, (size_t)block_size, &blocks);
      if (s != GCOMP_OK) {
        return s;
      }
    }
  }
  s = gcomp_bound_block_count(input_size, 16384u, &chunks);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, input_size);
  }
  if (s == GCOMP_OK) {
    s = gcomp_bound_add_mul(&bound, chunks, 6u);
  }
  if (s == GCOMP_OK) {
    s = gcomp_bound_add_mul(&bound, blocks, 32u + 3u + 32u + 18u + 64u + 6u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

/** What the 12-byte stream header says: that it is one, and which check. */
static gcomp_status_t xz_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  static const uint8_t magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
  const uint8_t * p = (const uint8_t *)input;
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (needed_out) {
    *needed_out = XZ_STREAM_HEADER_SIZE;
  }
  if (input_size < XZ_STREAM_HEADER_SIZE || !p) {
    return GCOMP_ERR_LIMIT;
  }
  if (memcmp(p, magic, 6) != 0 || p[6] != 0 || (p[7] & 0xF0u) != 0 ||
      xz_crc32(p + 6, 2) != xz_get_le32(p + 8)) {
    return GCOMP_ERR_CORRUPT;
  }
  info_out->header_size = XZ_STREAM_HEADER_SIZE;
  info_out->has_checksum = (p[7] & 0x0Fu) != 0;
  return GCOMP_OK;
}

static const gcomp_method_t g_xz_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "xz",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = xz_create_encoder,
    .create_decoder = xz_create_decoder,
    .destroy_encoder = xz_destroy_encoder_wrapper,
    .destroy_decoder = xz_destroy_decoder_wrapper,
    .get_schema = xz_get_schema,
    .encode_bound = xz_encode_bound,
    .peek = xz_peek,
};

gcomp_status_t gcomp_method_xz_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_xz_method);
}

GCOMP_AUTOREG_METHOD(xz, gcomp_method_xz_register)
