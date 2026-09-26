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
 * @file zstd_register.c
 *
 * Zstandard method registration for the Ghoti.io Compress library.
 *
 * This file provides:
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema defining Zstd-specific options
 * - Public registration function `gcomp_method_zstd_register()`
 * - Auto-registration hook for the default registry
 *
 * ## Architecture
 *
 * The Zstd method is a standalone compression method (not a wrapper).
 * It implements the Zstandard frame format specification, which provides:
 * - Frame header with configuration flags
 * - Multiple data blocks (Raw, RLE, or Compressed)
 * - Optional content checksum (xxHash64 low 32 bits)
 *
 * ## Options
 *
 * Zstd-specific options use the `zstd.*` prefix:
 * - `zstd.level`: Compression level (0-22; 0 is the fast strategy)
 * - `zstd.checksum`: Enable content checksum
 * - `zstd.window_log`: Window log (10-31, or 0 for auto)
 * - `zstd.content_size`: Content size for header (optional)
 * - `zstd.concat`: Decoder: support concatenated frames
 *
 * Limit options use the `limits.*` prefix (shared with core):
 * - `limits.max_output_bytes`: Maximum decompressed output
 * - `limits.max_window_bytes`: Maximum window size
 * - `limits.max_memory_bytes`: Maximum memory usage
 * - `limits.max_expansion_ratio`: Decompression bomb protection
 */

#include <ghoti.io/compress/macros.h>
#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "../../core/bound_internal.h"
#include "../../core/seek_table.h"
#include <ghoti.io/compress/compress.h>
#include <string.h>
#include "zstd_internal.h"
#include "zstd_ldm.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <stddef.h>
#include <stdint.h>

//
// Option Schema
//

static const gcomp_option_schema_t g_zstd_option_schemas[] = {
    // zstd.level - Compression level (0-22; 0 is the fast strategy)
    {
        "zstd.level",                // key
        GCOMP_OPT_INT64,             // type
        1,                           // has_default
        {.i64 = ZSTD_LEVEL_DEFAULT}, // default_value
        1,                           // has_min
        1,                           // has_max
        ZSTD_LEVEL_MIN,              // min_int
        ZSTD_LEVEL_MAX,              // max_int
        0,                           // min_uint
        0,                           // max_uint
        "Compression level (0-22; 0 favours speed, higher = better compression)", // help
        NULL, // allowed
    },
    // zstd.checksum - Enable content checksum
    {
        "zstd.checksum",                    // key
        GCOMP_OPT_BOOL,                     // type
        1,                                  // has_default
        {.b = false},                       // default_value
        0,                                  // has_min
        0,                                  // has_max
        0,                                  // min_int
        0,                                  // max_int
        0,                                  // min_uint
        0,                                  // max_uint
        "Enable xxHash64 content checksum", // help
        NULL, // allowed
    },
    // zstd.seekable - Write a seekable file from the streaming encoder
    {
        "zstd.seekable",  // key
        GCOMP_OPT_BOOL,   // type
        1,                // has_default
        {.b = false},     // default_value
        0,                // has_min
        0,                // has_max
        0,                // min_int
        0,                // max_int
        0,                // min_uint
        0,                // max_uint
        // Only meaningful on an encoder. gcomp_seekable_write_buffer() writes
        // the same file when the whole input is in hand; this is for a stream.
        "Write independent frames and a seek table; flush ends a frame", // help
        NULL, // allowed
    },
    // zstd.seekable_frame_size - Frame length when writing a seekable file
    {
        "zstd.seekable_frame_size",  // key
        GCOMP_OPT_UINT64,            // type
        1,                           // has_default
        {.ui64 = 1048576},           // default_value
        1,                           // has_min
        1,                           // has_max
        0,                           // min_int
        0,                           // max_int
        1024,                        // min_uint
        // The seek table records each frame's sizes in 32 bits, so a frame
        // decompressing to more than 4 GiB could not be described by one. Half
        // of that is the practical ceiling and keeps the arithmetic plainly in
        // range.
        2147483647u,                 // max_uint
        "Decompressed bytes per frame when writing a seekable file", // help
        NULL, // allowed
    },
    // zstd.seekable_checksum - Per-frame checksums in the seek table
    {
        "zstd.seekable_checksum", // key
        GCOMP_OPT_BOOL,           // type
        1,                        // has_default
        {.b = true},              // default_value
        0,                        // has_min
        0,                        // has_max
        0,                        // min_int
        0,                        // max_int
        0,                        // min_uint
        0,                        // max_uint
        "Record a checksum per frame in the seek table", // help
        NULL, // allowed
    },
    // zstd.long - Long-distance matching (see zstd_ldm.h)
    {
        "zstd.long",   // key
        GCOMP_OPT_BOOL, // type
        1,              // has_default
        {.b = false},   // default_value
        0,              // has_min
        0,              // has_max
        0,              // min_int
        0,              // max_int
        0,              // min_uint
        0,              // max_uint
        "Find matches across the whole window, not just the nearest 8 MB", // help
        NULL, // allowed
    },
    // zstd.ldm_min_match - Shortest match long-distance matching will take
    {
        "zstd.ldm_min_match",        // key
        GCOMP_OPT_UINT64,            // type
        1,                           // has_default
        {.ui64 = ZSTD_LDM_MIN_MATCH_DEFAULT}, // default_value
        1,                           // has_min
        1,                           // has_max
        0,                           // min_int
        0,                           // max_int
        ZSTD_LDM_MIN_MATCH_MIN,      // min_uint
        ZSTD_LDM_MIN_MATCH_MAX,      // max_uint
        "Bytes a long-distance match must have", // help
        NULL, // allowed
    },
    // zstd.ldm_hash_log - Size of the long-distance table
    {
        "zstd.ldm_hash_log",     // key
        GCOMP_OPT_UINT64,        // type
        1,                       // has_default
        {.ui64 = 0},             // default_value (0 = size from the window)
        1,                       // has_min
        1,                       // has_max
        0,                       // min_int
        0,                       // max_int
        0,                       // min_uint (0 = auto allowed)
        ZSTD_LDM_HASH_LOG_MAX,   // max_uint
        "Log2 of the long-distance table, 0 = from the window", // help
        NULL, // allowed
    },
    // zstd.ldm_hash_rate_log - How sparsely the window is indexed
    {
        "zstd.ldm_hash_rate_log",       // key
        GCOMP_OPT_UINT64,               // type
        1,                              // has_default
        {.ui64 = ZSTD_LDM_HASH_RATE_LOG_DEFAULT}, // default_value
        1,                              // has_min
        1,                              // has_max
        0,                              // min_int
        0,                              // max_int
        0,                              // min_uint
        ZSTD_LDM_HASH_RATE_LOG_MAX,     // max_uint
        "Index one position in 2^this for long-distance matching", // help
        NULL, // allowed
    },
    // zstd.window_log - Window log (10-31, or 0 for auto)
    {
        "zstd.window_log",                       // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = 0},                             // default_value (0 = auto)
        1,                                       // has_min
        1,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint (0 = auto allowed)
        ZSTD_WINDOW_LOG_MAX,                     // max_uint
        "Window log (10-31, 0=auto from level)", // help
        NULL, // allowed
    },
    // zstd.content_size - Content size for header (optional)
    {
        "zstd.content_size",                       // key
        GCOMP_OPT_UINT64,                          // type
        0,                                         // has_default (optional)
        {.ui64 = 0},                               // default_value
        0,                                         // has_min
        0,                                         // has_max
        0,                                         // min_int
        0,                                         // max_int
        0,                                         // min_uint
        0,                                         // max_uint
        "Content size (written to header if set)", // help
        NULL, // allowed
    },
    // zstd.concat - Decoder: support concatenated frames
    {
        "zstd.concat",                               // key
        GCOMP_OPT_BOOL,                              // type
        1,                                           // has_default
        {.b = true},                                 // default_value
        0,                                           // has_min
        0,                                           // has_max
        0,                                           // min_int
        0,                                           // max_int
        0,                                           // min_uint
        0,                                           // max_uint
        "Decoder: decode every frame in the input (RFC 8878 section 3.1); "
        "set false to stop after the first",                              // help
        NULL, // allowed
    },
    // zstd.dictionary - Dictionary data (RFC 8878 §5; raw or formatted)
    {
        "zstd.dictionary",    // key
        GCOMP_OPT_BYTES,      // type
        0,                    // has_default (optional)
        {.bytes = {NULL, 0}}, // default_value
        0,                    // has_min
        0,                    // has_max
        0,                    // min_int
        0,                    // max_int
        0,                    // min_uint
        0,                    // max_uint
        "Dictionary data (raw content or zstd --train format)", // help
        NULL, // allowed
    },
    // zstd.dictionary_id - Dictionary ID to write/validate (optional)
    {
        "zstd.dictionary_id", // key
        GCOMP_OPT_UINT64,     // type
        0,                    // has_default (optional)
        {.ui64 = 0},          // default_value
        0,                    // has_min
        0,                    // has_max
        0,                    // min_int
        0,                    // max_int
        0,                    // min_uint
        0,                    // max_uint
        "Dictionary ID (encoder: write to header; decoder: validate if "
        "present)", // help
        NULL, // allowed
    },
    // limits.max_output_bytes - Maximum decompressed output
    {
        "limits.max_output_bytes",               // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_OUTPUT_BYTES}, // default_value
        0,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint (0 means unlimited)
        0,                                       // max_uint
        "Maximum decompressed output bytes",     // help
        NULL, // allowed
    },
    // limits.max_window_bytes - Maximum window size
    {
        "limits.max_window_bytes",               // key
        GCOMP_OPT_UINT64,                        // type
        1,                                       // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_WINDOW_BYTES}, // default_value
        0,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint
        0,                                       // max_uint
        "Maximum window size in bytes",          // help
        NULL, // allowed
    },
    // limits.max_memory_bytes - Maximum memory usage
    {
        "limits.max_memory_bytes",                // key
        GCOMP_OPT_UINT64,                         // type
        1,                                        // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_MEMORY_BYTES},  // default_value
        0,                                        // has_min
        0,                                        // has_max
        0,                                        // min_int
        0,                                        // max_int
        0,                                        // min_uint
        0,                                        // max_uint
        "Maximum memory usage (buffers, tables)", // help
        NULL, // allowed
    },
    // limits.max_expansion_ratio - Decompression bomb protection
    {
        "limits.max_expansion_ratio",               // key
        GCOMP_OPT_UINT64,                           // type
        1,                                          // has_default
        {.ui64 = ZSTD_DEFAULT_MAX_EXPANSION_RATIO}, // default_value
        0,                                          // has_min
        0,                                          // has_max
        0,                                          // min_int
        0,                                          // max_int
        0,                                          // min_uint
        0,                                          // max_uint
        "Maximum output/input ratio; default is this format's own ceiling", // help
        NULL, // allowed
    },
    // threads.count - Number of worker threads for parallel compression
    {
        "threads.count",                                       // key
        GCOMP_OPT_UINT64,                                      // type
        1,                                                     // has_default
        {.ui64 = 1},                                           // default_value
        1,                                                     // has_min
        0,                                                     // has_max
        0,                                                     // min_int
        0,                                                     // max_int
        0,                                                     // min_uint
        0,                                                     // max_uint
        "Number of worker threads (0 or 1 = single-threaded)", // help
        NULL, // allowed
    },
    // zstd.job_size - Size of each compression job in parallel mode
    {
        "zstd.job_size",   // key
        GCOMP_OPT_UINT64,  // type
        1,                 // has_default
        {.ui64 = 0},       // default_value (0 = auto)
        1,                 // has_min
        1,                 // has_max
        0,                 // min_int
        0,                 // max_int
        0,                 // min_uint (0 = auto)
        ZSTD_MAX_JOB_SIZE, // max_uint
        "Job size for parallel compression (0=auto, min 64KB)", // help
        NULL, // allowed
    },
};

static const char * const g_zstd_option_keys[] = {
    "zstd.level",
    "zstd.checksum",
    "zstd.seekable",
    "zstd.seekable_frame_size",
    "zstd.seekable_checksum",
    "zstd.window_log",
    "zstd.long",
    "zstd.ldm_min_match",
    "zstd.ldm_hash_log",
    "zstd.ldm_hash_rate_log",
    "zstd.content_size",
    "zstd.concat",
    "zstd.dictionary",
    "zstd.dictionary_id",
    "zstd.job_size",
    "threads.count",
    "limits.max_output_bytes",
    "limits.max_window_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_zstd_schema = {
    g_zstd_option_schemas,
    sizeof(g_zstd_option_schemas) / sizeof(g_zstd_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR, // Zstd doesn't wrap another method
    g_zstd_option_keys,
};

static const gcomp_method_schema_t * zstd_get_schema(void) {
  return &g_zstd_schema;
}

//
// Option Validation
//

/**
 * @brief Validate window_log option value.
 *
 * Window log must be 0 (auto) or in range [10, 31].
 */
static bool zstd_validate_window_log(uint64_t window_log) {
  return window_log == 0 ||
      (window_log >= ZSTD_WINDOW_LOG_MIN && window_log <= ZSTD_WINDOW_LOG_MAX);
}

//
// Encoder/Decoder Wrappers
//

static gcomp_status_t zstd_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zstd_encoder_update(encoder, input, output);
}

static gcomp_status_t zstd_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return zstd_encoder_finish(encoder, output);
}

static gcomp_status_t zstd_encoder_flush_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  return zstd_encoder_flush(encoder, output, mode);
}

static gcomp_status_t zstd_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return zstd_decoder_update(decoder, input, output);
}

static gcomp_status_t zstd_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return zstd_decoder_finish(decoder, output);
}

//
// Encoder/Decoder Factory Functions
//

static gcomp_status_t zstd_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (options) {
    // Validate window_log option if provided
    uint64_t window_log_val;
    if (gcomp_options_get_uint64(options, "zstd.window_log", &window_log_val) ==
        GCOMP_OK) {
      if (!zstd_validate_window_log(window_log_val)) {
        gcomp_encoder_set_error(*encoder_out, GCOMP_ERR_INVALID_ARG,
            "zstd.window_log must be 0 (auto) or between 10 and 31");
        return GCOMP_ERR_INVALID_ARG;
      }
    }

    // Validate the long-distance options if provided.  They are checked here
    // rather than only in zstd_ldm_init() so that the caller is told which
    // option was wrong and what its range is, the way window_log above is.
    uint64_t ldm_val;
    if (gcomp_options_get_uint64(options, "zstd.ldm_min_match", &ldm_val) ==
        GCOMP_OK) {
      if (ldm_val < ZSTD_LDM_MIN_MATCH_MIN ||
          ldm_val > ZSTD_LDM_MIN_MATCH_MAX) {
        gcomp_encoder_set_error(*encoder_out, GCOMP_ERR_INVALID_ARG,
            "zstd.ldm_min_match must be between %u and %u",
            ZSTD_LDM_MIN_MATCH_MIN, ZSTD_LDM_MIN_MATCH_MAX);
        return GCOMP_ERR_INVALID_ARG;
      }
    }
    if (gcomp_options_get_uint64(options, "zstd.ldm_hash_log", &ldm_val) ==
        GCOMP_OK) {
      if (ldm_val != 0u && (ldm_val < ZSTD_LDM_HASH_LOG_MIN ||
                               ldm_val > ZSTD_LDM_HASH_LOG_MAX)) {
        gcomp_encoder_set_error(*encoder_out, GCOMP_ERR_INVALID_ARG,
            "zstd.ldm_hash_log must be 0 (auto) or between %u and %u",
            ZSTD_LDM_HASH_LOG_MIN, ZSTD_LDM_HASH_LOG_MAX);
        return GCOMP_ERR_INVALID_ARG;
      }
    }
    if (gcomp_options_get_uint64(options, "zstd.ldm_hash_rate_log",
            &ldm_val) == GCOMP_OK) {
      if (ldm_val > ZSTD_LDM_HASH_RATE_LOG_MAX) {
        gcomp_encoder_set_error(*encoder_out, GCOMP_ERR_INVALID_ARG,
            "zstd.ldm_hash_rate_log must be at most %u",
            ZSTD_LDM_HASH_RATE_LOG_MAX);
        return GCOMP_ERR_INVALID_ARG;
      }
    }

    // Validate job_size option if provided (must be 0 or within bounds)
    uint64_t job_size_val;
    if (gcomp_options_get_uint64(options, "zstd.job_size", &job_size_val) ==
        GCOMP_OK) {
      if (job_size_val != 0 &&
          (job_size_val < ZSTD_MIN_JOB_SIZE ||
              job_size_val > ZSTD_MAX_JOB_SIZE)) {
        gcomp_encoder_set_error(*encoder_out, GCOMP_ERR_INVALID_ARG,
            "zstd.job_size must be 0 (auto) or between %zu and %zu bytes",
            (size_t)ZSTD_MIN_JOB_SIZE, (size_t)ZSTD_MAX_JOB_SIZE);
        return GCOMP_ERR_INVALID_ARG;
      }
    }
  }

  gcomp_status_t status = zstd_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = zstd_encoder_update_wrapper;
  (*encoder_out)->finish_fn = zstd_encoder_finish_wrapper;
  (*encoder_out)->flush_fn = zstd_encoder_flush_wrapper;
  (*encoder_out)->reset_fn = zstd_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t zstd_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = zstd_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = zstd_decoder_update_wrapper;
  (*decoder_out)->finish_fn = zstd_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = zstd_decoder_reset;
  return GCOMP_OK;
}

static void zstd_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  zstd_encoder_destroy(encoder);
}

static void zstd_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  zstd_decoder_destroy(decoder);
}

//
// Method Descriptor
//

//
// Worst-case encoded size
//

/**
 * @brief Largest Zstandard frame this encoder can produce for @p input_size.
 *
 * RFC 8878 section 3.1.1.1 puts the Frame_Header at no more than fourteen
 * bytes, which with the four-byte Magic_Number makes eighteen: the maximum is
 * charged rather than working out which optional fields these options select.
 *
 * Every block costs a three-byte Block_Header (section 3.1.1.2), and a block
 * that does not compress is written as a Raw_Block carrying its input
 * verbatim, so the blocks together carry no more than @p input_size.  The
 * number of blocks follows from Block_Maximum_Size, which section 3.1.1.2.4
 * gives as the smaller of Window_Size and 128 KB.  One extra block is charged
 * for the empty last block the parallel encoder writes to close the frame.
 *
 * Four more bytes when zstd.checksum asks for a Content_Checksum.
 */
static gcomp_status_t zstd_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  int64_t level = ZSTD_LEVEL_DEFAULT;
  uint64_t window_log = 0;
  int checksum = 0;
  if (options) {
    gcomp_options_get_int64(options, "zstd.level", &level);
    gcomp_options_get_uint64(options, "zstd.window_log", &window_log);
    gcomp_options_get_bool(options, "zstd.checksum", &checksum);
  }
  if (window_log == 0) {
    window_log = zstd_level_to_window_log((int)level);
  }
  if (window_log < ZSTD_WINDOW_LOG_MIN) {
    window_log = ZSTD_WINDOW_LOG_MIN;
  }
  if (window_log > ZSTD_WINDOW_LOG_MAX) {
    window_log = ZSTD_WINDOW_LOG_MAX;
  }

  size_t block_max = ZSTD_BLOCK_SIZE_MAX;
  if (window_log < 31 && ((uint64_t)1u << window_log) < (uint64_t)block_max) {
    block_max = (size_t)((uint64_t)1u << window_log);
  }

  size_t blocks = 0;
  gcomp_status_t s = gcomp_bound_block_count(input_size, block_max, &blocks);
  if (s != GCOMP_OK) {
    return s;
  }
  if (!gcu_safe_add_size(blocks, 1u, &blocks)) {
    return GCOMP_ERR_LIMIT;
  }

  // Seekable mode writes many frames and a seek table, so the single-frame
  // arithmetic below is not a bound for it.  This used to be missing entirely -
  // the option was never read here - and the figure came out byte-identical
  // whether it was set or not.  With compressible input the slack hid it; with
  // incompressible input the documented pattern (bound, malloc, encode into
  // exactly that) returned GCOMP_ERR_LIMIT at every size measured: 300,000
  // bytes wanted 300,048 against a bound of 300,030, and 2,160,000 wanted
  // 2,160,137 against 2,160,072.
  //
  // Each frame is a complete one-shot stream, so each owes its own header and
  // its own content checksum; the table owes one entry per frame plus eight
  // bytes of skippable-frame header and the footer.
  uint64_t seekable_frame_size = 1048576u;
  int seekable = 0;
  int seekable_checksum = 1;
  if (options) {
    gcomp_options_get_bool(options, "zstd.seekable", &seekable);
    gcomp_options_get_uint64(
        options, "zstd.seekable_frame_size", &seekable_frame_size);
    gcomp_options_get_bool(
        options, "zstd.seekable_checksum", &seekable_checksum);
  }
  size_t frames = 1u;
  if (seekable) {
    if (seekable_frame_size == 0u) {
      return GCOMP_ERR_INVALID_ARG;
    }
    size_t frame_span = (seekable_frame_size > (uint64_t)SIZE_MAX)
        ? SIZE_MAX
        : (size_t)seekable_frame_size;
    s = gcomp_bound_block_count(input_size, frame_span, &frames);
    if (s != GCOMP_OK) {
      return s;
    }
    if (frames == 0u) {
      frames = 1u; // Empty input still writes one frame and a table.
    }
    // Every frame's last block is its own, so the per-frame "+1" the
    // single-frame path adds has to be paid once per frame rather than once.
    if (!gcu_safe_add_size(blocks, frames, &blocks)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  size_t bound = input_size;
  // Magic_Number + Frame_Header, maximum, once per frame.
  s = gcomp_bound_add_mul(&bound, frames, 18u);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add_mul(&bound, blocks, ZSTD_BLOCK_HEADER_SIZE);
  }
  if (s == GCOMP_OK && checksum) {
    s = gcomp_bound_add_mul(&bound, frames, 4u);
  }
  if (s == GCOMP_OK && seekable) {
    s = gcomp_bound_add_mul(&bound, frames, seekable_checksum ? 12u : 8u);
    if (s == GCOMP_OK) {
      s = gcomp_bound_add(&bound, 8u + GCOMP_SEEK_FOOTER_SIZE);
    }
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}


//
// Peeking
//

/**
 * @brief The Zstandard frame header, through the parser the decoder uses.
 *
 * zstd_frame_header_parse() is shared with zstd_decoder.c so that the answer a
 * caller is given here and the reading the decoder acts on are the same one.
 * Skippable frames (RFC 8878 section 3.1.2) are reported as themselves rather
 * than being stepped over: the magic range belongs to LZ4 as well, so what
 * follows one is not this function's to assume.
 */
static gcomp_status_t zstd_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(info_out, 0, sizeof(*info_out));

  const uint8_t * p = (const uint8_t *)input;
  if (input_size < 4) {
    if (needed_out) {
      *needed_out = 4;
    }
    return GCOMP_ERR_LIMIT;
  }

  uint32_t magic = gcomp_read_le32(p);
  if (magic >= ZSTD_MAGIC_SKIPPABLE_MIN && magic <= ZSTD_MAGIC_SKIPPABLE_MAX) {
    if (input_size < 8) {
      if (needed_out) {
        *needed_out = 8;
      }
      return GCOMP_ERR_LIMIT;
    }
    info_out->is_skippable = 1;
    info_out->skippable_variant = (unsigned)(magic & 0x0Fu);
    info_out->header_size = 8;
    info_out->skippable_size = 8u + (uint64_t)gcomp_read_le32(p + 4);
    if (needed_out) {
      *needed_out = 8;
    }
    return GCOMP_OK;
  }

  zstd_frame_header_t h;
  uint64_t window_size = 0;
  size_t needed = 0;
  gcomp_status_t s =
      zstd_frame_header_parse(p, input_size, &h, &window_size, &needed);
  if (s != GCOMP_OK) {
    if (needed_out) {
      *needed_out = needed;
    }
    return s;
  }

  info_out->header_size = needed;
  info_out->has_content_size = h.content_size_present ? 1 : 0;
  info_out->content_size = h.content_size;
  info_out->window_size = window_size;
  info_out->has_checksum = h.content_checksum ? 1 : 0;
  info_out->has_dictionary = (h.dict_id_flag != 0) ? 1 : 0;
  info_out->dictionary_id = h.dict_id;
  if (needed_out) {
    *needed_out = needed;
  }
  return GCOMP_OK;
}


static const gcomp_method_t g_zstd_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "zstd",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = zstd_create_encoder,
    .create_decoder = zstd_create_decoder,
    .destroy_encoder = zstd_destroy_encoder_wrapper,
    .destroy_decoder = zstd_destroy_decoder_wrapper,
    .get_schema = zstd_get_schema,
    .encode_bound = zstd_encode_bound,
    .peek = zstd_peek,
    .decode_parallel = zstd_decode_parallel,
};

//
// Public Registration Function
//

gcomp_status_t gcomp_method_zstd_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_zstd_method);
}

//
// Auto-Registration Hook
//
GCOMP_AUTOREG_METHOD(zstd, gcomp_method_zstd_register)
