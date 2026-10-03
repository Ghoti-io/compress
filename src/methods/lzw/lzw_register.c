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
 * @file lzw_register.c
 *
 * LZW method registration for the Ghoti.io Compress library.
 *
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema: lzw.format, lzw.lit_width, lzw.max_code_bits, limits.*
 * - Public registration and auto-registration (GCOMP_AUTOREG_METHOD)
 *
 * SCHEMA / OPTIONS
 * ================
 *
 * Public options are exposed through the registry schema so callers can:
 * - discover available keys (introspection)
 * - validate user-provided options at create time
 * - reject unknown keys (policy: GCOMP_UNKNOWN_KEY_ERROR)
 *
 * LZW is profile-driven: `lzw.format` selects the on-the-wire variant:
 * - `"gif"`  : LSB-first bit packing and GIF code-width growth rule
 * - `"tiff"` : MSB-first bit packing and TIFF code-width growth rule
 *
 * The library does not define a container or embed the format identifier in
 * the stream. Callers must supply matching options on encode and decode.
 */

#include <ghoti.io/compress/macros.h>
#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "lzw_core.h"
#include "lzw_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include "../../core/bound_internal.h"
#include <ghoti.io/compress/compress.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

//
// Option schema (LW1.1): lzw.format, lzw.lit_width, lzw.max_code_bits, limits.*
//

static const char LZW_DEFAULT_FORMAT[] = "gif";
static const char LZW_DEFAULT_ENCODER_LOOKUP[] = "hash";

/// The profiles lzw_profile.c implements; see LZW_FORMAT_* in lzw_internal.h.
static const char * const g_lzw_format_values[] = {
    LZW_FORMAT_GIF, LZW_FORMAT_TIFF, NULL};

/**
 * @brief The two lookup modes the encoder implements.
 *
 * Declaring them matters more here than it looks. The encoder switches to the
 * linear scan only on an exact "linear" and treats everything else as "hash",
 * so before this a typo silently selected a mode instead of being refused -
 * and since both produce identical bytes, nothing downstream would ever have
 * said so.
 */
static const char * const g_lzw_encoder_lookup_values[] = {
    "linear", "hash", NULL};

static const gcomp_option_schema_t g_lzw_option_schemas[] = {
    {
        "lzw.format",                // key
        GCOMP_OPT_STRING,            // type
        1,                           // has_default
        {.str = LZW_DEFAULT_FORMAT}, // default_value
        0,                           // has_min
        0,                           // has_max
        0,                           // min_int
        0,                           // max_int
        0,                           // min_uint
        0,                           // max_uint
        "LZW format: gif or tiff",   // help
        g_lzw_format_values, // allowed
    },
    {
        "lzw.lit_width",  // key
        GCOMP_OPT_UINT64, // type
        0,                // has_default (optional; format default used)
        {.ui64 = 0},      // default_value
        0,                // has_min
        0,                // has_max
        0,                // min_int
        0,                // max_int
        0,                // min_uint
        0,                // max_uint
        "Literal code width in bits, 2..11: GIF's \"LZW minimum code size\", "
        "fixed at 8 by TIFF. The stream opens one bit wider",
        NULL, // allowed
    },
    {
        "lzw.max_code_bits", // key
        GCOMP_OPT_UINT64,    // type
        1,                   // has_default
        {.ui64 = 12},        // default_value
        // The range the decoder and lzw_core_encoder_init() already enforce,
        // declared so that gcomp_options_validate() - which the create calls
        // run over a caller's options - can refuse it by name, and so that
        // introspection can report it.  Undeclared, a 13 reached the method
        // and failed there against a limit the schema had never mentioned.
        // TIFF 6.0 section 13 caps a code at twelve bits; below nine there is
        // no room for 256 literals plus Clear and End_of_Information.
        1,                      // has_min
        1,                      // has_max
        0,                      // min_int
        0,                      // max_int
        9,                      // min_uint
        LZW_CORE_MAX_CODE_BITS, // max_uint
        "Maximum code width in bits, 9 to 12 (12 gives 4096 entries)",
        NULL, // allowed
    },
    {
        "lzw.encoder_lookup",                // key
        GCOMP_OPT_STRING,                    // type
        1,                                   // has_default
        {.str = LZW_DEFAULT_ENCODER_LOOKUP}, // default_value
        0,
        0,
        0,
        0,
        0,
        0,
        "Encoder lookup mode: linear or hash (hash is faster)",
        g_lzw_encoder_lookup_values, // allowed
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
        NULL, // allowed
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
        NULL, // allowed
    },
    {
        "limits.max_expansion_ratio",
        GCOMP_OPT_UINT64,
        1,
        {.ui64 = GCOMP_LZW_MAX_EXPANSION_RATIO},
        0,
        0,
        0,
        0,
        0,
        0,
        "Maximum output/input ratio; default is this format's own ceiling",
        NULL, // allowed
    },
};

static const char * const g_lzw_option_keys[] = {
    "lzw.format",
    "lzw.lit_width",
    "lzw.max_code_bits",
    "lzw.encoder_lookup",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_lzw_schema = {
    g_lzw_option_schemas,
    sizeof(g_lzw_option_schemas) / sizeof(g_lzw_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_lzw_option_keys,
};

static const gcomp_method_schema_t * lzw_get_schema(void) {
  return &g_lzw_schema;
}

//
// Encoder/decoder wrappers
//

static gcomp_status_t lzw_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lzw_encoder_update(encoder, input, output);
}

static gcomp_status_t lzw_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return lzw_encoder_finish(encoder, output);
}

static gcomp_status_t lzw_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return lzw_decoder_update(decoder, input, output);
}

static gcomp_status_t lzw_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return lzw_decoder_finish(decoder, output);
}

//
// Factory functions
//

static gcomp_status_t lzw_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = lzw_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = lzw_encoder_update_wrapper;
  (*encoder_out)->finish_fn = lzw_encoder_finish_wrapper;
  (*encoder_out)->flush_fn = lzw_encoder_flush;
  (*encoder_out)->reset_fn = lzw_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t lzw_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = lzw_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = lzw_decoder_update_wrapper;
  (*decoder_out)->finish_fn = lzw_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = lzw_decoder_reset;
  return GCOMP_OK;
}

static void lzw_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  lzw_encoder_destroy(encoder);
}

static void lzw_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  lzw_decoder_destroy(decoder);
}

//
// Method descriptor
//

//
// Worst-case encoded size
//

/**
 * @brief Largest LZW stream this encoder can produce for @p input_size.
 *
 * LZW can expand, and the worst case is the input that never lets a dictionary
 * entry be reused: every byte is emitted as its own code, at the widest code
 * the stream ever reaches.
 *
 * The codes counted are one per input byte, plus the Clear that opens the
 * stream and the End_of_Information that closes it (TIFF 6.0 section 13; GIF89a
 * appendix F), plus one Clear for each time the table fills.  A table of
 * 2^max_code_bits entries holds (1 << lit_width) + 2 reserved codes before any
 * string is added, so that many new entries fit between Clears.
 *
 * One byte is added at the end because the last code need not finish on a byte
 * boundary and the encoder pads.
 */
static gcomp_status_t lzw_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint64_t max_code_bits = 12u;
  uint64_t lit_width = 8u;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "lzw.max_code_bits", &v) ==
            GCOMP_OK &&
        v > 0) {
      max_code_bits = v;
    }
    if (gcomp_options_get_uint64(options, "lzw.lit_width", &v) == GCOMP_OK &&
        v > 0) {
      lit_width = v;
    }
  }
  // The same range lzw_core_encoder_init() enforces.  A bound that accepted a
  // width the encoder rejects would answer a question about a stream that
  // cannot exist.
  if (max_code_bits == 0u || max_code_bits > LZW_CORE_MAX_CODE_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (max_code_bits < 9u) {
    max_code_bits = 9u;
  }
  if (lit_width >= max_code_bits) {
    lit_width = max_code_bits - 1u;
  }

  // Entries available between one Clear and the next.
  uint64_t table = ((uint64_t)1u << max_code_bits);
  uint64_t reserved = ((uint64_t)1u << lit_width) + 2u;
  uint64_t span = (table > reserved) ? table - reserved : 1u;

  // Through the checked helpers, every step.  This used to do its own uint64
  // arithmetic and range-check only the final byte count, which the wrap
  // defeated: `codes * max_code_bits` overflows for an input a little under
  // SIZE_MAX, so the check saw a small number and the function answered
  // GCOMP_OK with a bound 2560 times smaller than the input it was asked
  // about.  bound_internal.h: a bound that silently wrapped is worse than no
  // bound at all, because the caller allocates to it and believes it.
  size_t codes = 0;
  gcomp_status_t s = gcomp_bound_add(&codes, input_size);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&codes, 3u); // opening Clear, closing EOI, and the
                                     // partial span the division rounds off
  }
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&codes, (size_t)((uint64_t)input_size / span));
  }
  if (s != GCOMP_OK) {
    return s;
  }

  size_t bits = 0;
  s = gcomp_bound_add_mul(&bits, codes, (size_t)max_code_bits);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bits, 7u);
  }
  if (s != GCOMP_OK) {
    return s;
  }

  size_t bytes = bits / 8u;
  s = gcomp_bound_add(&bytes, 1u);
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bytes;
  return GCOMP_OK;
}


//
// Peeking
//

/**
 * @brief LZW has no header: the stream opens with a Clear code.
 *
 * Neither the TIFF nor the GIF framing puts anything in front of the codes -
 * what surrounds them belongs to the container, not to this stream - so there
 * is nothing to report.  There is no window either: LZW refers to its
 * dictionary, not to a distance behind the current position.
 */
static gcomp_status_t lzw_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  (void)options;
  (void)input;
  (void)input_size;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(info_out, 0, sizeof(*info_out));
  if (needed_out) {
    *needed_out = 0;
  }
  return GCOMP_OK;
}


static const gcomp_method_t g_lzw_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "lzw",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = lzw_create_encoder,
    .create_decoder = lzw_create_decoder,
    .destroy_encoder = lzw_destroy_encoder_wrapper,
    .destroy_decoder = lzw_destroy_decoder_wrapper,
    .get_schema = lzw_get_schema,
    .encode_bound = lzw_encode_bound,
    .peek = lzw_peek,
};

gcomp_status_t gcomp_method_lzw_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_lzw_method);
}

GCOMP_AUTOREG_METHOD(lzw, gcomp_method_lzw_register)
