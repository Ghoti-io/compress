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
 * @file brotli_register.c
 *
 * Brotli method registration. The stream has no magic, so peek reports the
 * window from the first byte and detect does not claim the format.
 */

#include <ghoti.io/compress/macros.h>

#include "../../autoreg/autoreg_platform.h"
#include "../../core/bound_internal.h"
#include "../../core/stream_internal.h"
#include "brotli_bits.h"
#include "brotli_internal.h"

#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include <stdint.h>
#include <string.h>

#define BROTLI_DEFAULT_WINDOW (1ull << 24)

static const gcomp_option_schema_t g_brotli_option_schemas[] = {
    {
        "brotli.lgwin",
        GCOMP_OPT_INT64,
        1,
        {.i64 = 16},
        1,
        1,
        10,
        24,
        0,
        0,
        "Encoder window bits, 10..24. The decoder reads WBITS from the stream.",
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
        {.ui64 = GCOMP_BROTLI_MAX_EXPANSION_RATIO},
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
        {.ui64 = BROTLI_DEFAULT_WINDOW},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum window size in bytes. Default accepts every RFC 7932 window.",
        NULL,
    },
};

static const char * const g_brotli_option_keys[] = {
    "brotli.lgwin",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
    "limits.max_window_bytes",
};

static const gcomp_method_schema_t g_brotli_schema = {
    g_brotli_option_schemas,
    sizeof(g_brotli_option_schemas) / sizeof(g_brotli_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_brotli_option_keys,
};

static const gcomp_method_schema_t * brotli_get_schema(void) {
  return &g_brotli_schema;
}

static gcomp_status_t brotli_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  gcomp_status_t status;
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = brotli_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*encoder_out)->update_fn = brotli_encoder_update;
  (*encoder_out)->finish_fn = brotli_encoder_finish;
  (*encoder_out)->flush_fn = brotli_encoder_flush;
  (*encoder_out)->reset_fn = brotli_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t brotli_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  gcomp_status_t status;
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  status = brotli_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }
  (*decoder_out)->update_fn = brotli_decoder_update;
  (*decoder_out)->finish_fn = brotli_decoder_finish;
  (*decoder_out)->reset_fn = brotli_decoder_reset;
  return GCOMP_OK;
}

static void brotli_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  brotli_encoder_destroy(encoder);
}

static void brotli_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  brotli_decoder_destroy(decoder);
}

/**
 * @brief Bytes for the trivial compressor, including every window size.
 *
 * Two bytes cover the window field and the empty metadata block that aligns
 * the stream. Each stored chunk then costs a 3-byte header and at most 65536
 * payload bytes, and one byte ends the stream. Empty input is the window
 * bits plus the empty-last marker, which is at most two bytes.
 */
static gcomp_status_t brotli_encode_bound(gcomp_options_t * options,
    size_t input_size, size_t * bound_out) {
  size_t blocks;
  size_t bound;
  gcomp_status_t s;
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (input_size == 0) {
    *bound_out = 2;
    return GCOMP_OK;
  }
  blocks = (input_size + 65535u) / 65536u;
  bound = 2;
  s = gcomp_bound_add(&bound, input_size);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add_mul(&bound, blocks, 3u);
  }
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 1u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static int peek_wbits(const uint8_t * byte, int * wbits_out) {
  brotli_bits_t br;
  uint32_t n = 0;
  int r;
  memset(&br, 0, sizeof(br));
  br.data = byte;
  br.size = 1;
  r = brotli_bits_take(&br, 1, &n);
  if (r != BR_OK) {
    return -1;
  }
  if (n == 0) {
    *wbits_out = 16;
    return 0;
  }
  r = brotli_bits_take(&br, 3, &n);
  if (r != BR_OK) {
    return -1;
  }
  if (n != 0) {
    *wbits_out = 17 + (int)n;
    return 0;
  }
  r = brotli_bits_take(&br, 3, &n);
  if (r != BR_OK) {
    return -1;
  }
  if (n == 1) {
    return -1;
  }
  if (n != 0) {
    *wbits_out = 8 + (int)n;
    return 0;
  }
  *wbits_out = 17;
  return 0;
}

static gcomp_status_t brotli_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  int wbits = 0;
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (input_size == 0 || input == NULL) {
    if (needed_out) {
      *needed_out = 1;
    }
    return GCOMP_ERR_LIMIT;
  }
  if (peek_wbits((const uint8_t *)input, &wbits) != 0 || wbits < 10 ||
      wbits > 24) {
    return GCOMP_ERR_CORRUPT;
  }
  info_out->header_size = 1;
  info_out->window_size = (1ull << wbits) - 16ull;
  if (needed_out) {
    *needed_out = 1;
  }
  return GCOMP_OK;
}

static const gcomp_method_t g_brotli_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "brotli",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = brotli_create_encoder,
    .create_decoder = brotli_create_decoder,
    .destroy_encoder = brotli_destroy_encoder_wrapper,
    .destroy_decoder = brotli_destroy_decoder_wrapper,
    .get_schema = brotli_get_schema,
    .encode_bound = brotli_encode_bound,
    .peek = brotli_peek,
};

gcomp_status_t gcomp_method_brotli_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_brotli_method);
}

GCOMP_AUTOREG_METHOD(brotli, gcomp_method_brotli_register)
