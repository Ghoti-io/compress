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
 * @file method.h
 *
 * Compression method interface and metadata for the Ghoti.io Compress library.
 */

#ifndef GHOTI_IO_GCOMP_METHOD_H
#define GHOTI_IO_GCOMP_METHOD_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declarations
 */
typedef struct gcomp_registry_s gcomp_registry_t;
typedef struct gcomp_stream_info_s gcomp_stream_info_t;
typedef struct gcomp_options_s gcomp_options_t;
typedef struct gcomp_encoder_s gcomp_encoder_t;
typedef struct gcomp_decoder_s gcomp_decoder_t;

/**
 * @brief Compression method capabilities
 */
typedef enum {
  GCOMP_CAP_NONE = 0,
  GCOMP_CAP_ENCODE = 1 << 0, ///< Method supports encoding
  GCOMP_CAP_DECODE = 1 << 1, ///< Method supports decoding
} gcomp_capabilities_t;

/**
 * @brief Option schema descriptor for a single key.
 *
 * Methods expose their supported options via arrays of this structure.
 */
typedef struct gcomp_option_schema_s {
  /**
   * @brief Option key name (e.g., "deflate.level").
   */
  const char * key;

  /**
   * @brief Option value type.
   */
  gcomp_option_type_t type;

  /**
   * @brief Whether a default value is provided.
   */
  int has_default;

  /**
   * @brief Default value (valid only if @ref has_default is non-zero).
   */
  union {
    int64_t i64;
    uint64_t ui64;
    int b;
    const char * str;
    struct {
      const void * data;
      size_t size;
    } bytes;
    double f;
  } default_value;

  /**
   * @brief Whether minimum/maximum constraints are present.
   *
   * These currently apply only to integer and unsigned integer types.
   */
  int has_min;

  /// Nonzero when the maximum constraint applies (see @c has_min).
  int has_max;

  /**
   * @brief Integer constraints (for ::GCOMP_OPT_INT64).
   */
  int64_t min_int;

  /// Largest accepted value for a ::GCOMP_OPT_INT64 option.
  int64_t max_int;

  /**
   * @brief Unsigned integer constraints (for ::GCOMP_OPT_UINT64).
   */
  uint64_t min_uint;

  /// Largest accepted value for a ::GCOMP_OPT_UINT64 option.
  uint64_t max_uint;

  /**
   * @brief Optional help text (may be NULL).
   */
  const char * help;

  /**
   * @brief Permitted values for a ::GCOMP_OPT_STRING option, or NULL.
   *
   * A NULL-terminated array of the strings this option accepts. NULL means
   * the option takes any string, which is the right answer for a filename or
   * a comment but the wrong one for a mode selector.
   *
   * Declaring them is what lets `gcomp_options_validate()` refuse a bad
   * value where the caller set it, naming the key, rather than leaving the
   * method to fail later with a message about something further downstream.
   * It is also the only way a caller can discover the set by introspection
   * instead of by reading prose in @ref help.
   *
   * Ignored for every other type.
   */
  const char * const * allowed;
} gcomp_option_schema_t;

/**
 * @brief Schema descriptor for all options supported by a method.
 */
typedef struct gcomp_method_schema_s {
  /**
   * @brief Array of option schemas.
   */
  const gcomp_option_schema_t * options;

  /**
   * @brief Number of entries in @ref options.
   */
  size_t num_options;

  /**
   * @brief Policy for handling unknown keys.
   */
  gcomp_unknown_key_policy_t unknown_key_policy;

  /**
   * @brief Optional array of option key strings.
   *
   * If non-NULL, this must be an array of @ref num_options pointers, each
   * matching the corresponding @ref gcomp_option_schema_s::key. If NULL,
   * callers can instead iterate @ref options directly.
   */
  const char * const * keys;
} gcomp_method_schema_t;

/**
 * @brief Forward declaration and typedef for the method vtable structure.
 */
typedef struct gcomp_method_s gcomp_method_t;

/**
 * @brief Compression method vtable
 *
 * Each compression method provides an implementation of this vtable
 * to register with the registry.
 */
struct gcomp_method_s {
  /**
   * @brief ABI version for forward compatibility
   */
  uint32_t abi_version;

  /**
   * @brief Size of this structure for forward compatibility
   */
  size_t size;

  /**
   * @brief Method name (e.g., "deflate", "gzip", "zstd")
   */
  const char * name;

  /**
   * @brief Method capabilities
   */
  gcomp_capabilities_t capabilities;

  /**
   * @brief Create an encoder instance
   *
   * @param registry The registry (for accessing wrapped methods)
   * @param options Configuration options
   * @param encoder_out Output parameter for the created encoder
   * @return Status code
   */
  gcomp_status_t (*create_encoder)(gcomp_registry_t * registry,
      gcomp_options_t * options, gcomp_encoder_t ** encoder_out);

  /**
   * @brief Create a decoder instance
   *
   * @param registry The registry (for accessing wrapped methods)
   * @param options Configuration options
   * @param decoder_out Output parameter for the created decoder
   * @return Status code
   */
  gcomp_status_t (*create_decoder)(gcomp_registry_t * registry,
      gcomp_options_t * options, gcomp_decoder_t ** decoder_out);

  /**
   * @brief Destroy an encoder instance
   *
   * @param encoder The encoder to destroy
   */
  void (*destroy_encoder)(gcomp_encoder_t * encoder);

  /**
   * @brief Destroy a decoder instance
   *
   * @param decoder The decoder to destroy
   */
  void (*destroy_decoder)(gcomp_decoder_t * decoder);

  /**
   * @brief Retrieve the option schema for this method.
   *
   * Methods that support option introspection must implement this hook
   * and return a pointer to a static `gcomp_method_schema_t` instance.
   * Methods that do not support introspection may leave this as @c NULL.
   *
   * @return Pointer to the method's schema, or NULL if not available.
   */
  const gcomp_method_schema_t * (*get_schema)(void);

  /**
   * @brief Worst-case encoded size for @p input_size bytes.
   *
   * Added in method ABI version 2.  A method that does not implement it
   * leaves it @c NULL, and gcomp_encode_bound() then reports
   * ::GCOMP_ERR_UNSUPPORTED for that method rather than guessing.
   *
   * The value must be large enough that gcomp_encode_buffer() with these
   * options cannot return ::GCOMP_ERR_LIMIT for any input of that length -
   * including an input the method cannot compress at all, which is what
   * makes the stored or raw block fallback part of the contract rather than
   * an optimisation.  It covers one whole stream and not the extra output a
   * gcomp_encoder_flush() forces; see gcomp_encode_bound().
   *
   * @param options Configuration options (may be NULL for defaults)
   * @param input_size Number of input bytes to bound
   * @param bound_out Receives the worst-case encoded size
   * @return ::GCOMP_OK, or ::GCOMP_ERR_LIMIT when the bound cannot be
   *         represented in a @c size_t, or ::GCOMP_ERR_INVALID_ARG
   */
  gcomp_status_t (*encode_bound)(
      gcomp_options_t * options, size_t input_size, size_t * bound_out);

  /**
   * @brief Read what this stream's header says, without decoding it.
   *
   * Added in method ABI version 2.  A method that does not implement it
   * leaves it @c NULL, and gcomp_peek() then reports ::GCOMP_ERR_UNSUPPORTED
   * rather than passing an empty answer off as a real one.
   *
   * @p info_out has already been cleared when this is called, so a field the
   * format does not carry can simply be left alone.  A format with no header
   * at all succeeds and reports nothing, which is the truth about it.
   *
   * @param options Configuration options (may be NULL for defaults)
   * @param input Start of the compressed stream
   * @param input_size How much of it is available
   * @param info_out Receives what the header says
   * @param needed_out Receives how many bytes are needed when there are not
   *        enough yet; may be NULL
   * @return ::GCOMP_OK; ::GCOMP_ERR_LIMIT when more input is needed;
   *         ::GCOMP_ERR_CORRUPT for a malformed header
   */
  gcomp_status_t (*peek)(gcomp_options_t * options, const void * input,
      size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out);

  /**
   * @brief Decode a whole buffer using several threads, or decline to.
   *
   * Added in method ABI version 3. Optional: a method that leaves it @c NULL
   * simply never decodes in parallel, and gcomp_decode_buffer() uses the
   * ordinary path.
   *
   * ## Declining is normal, and is not an error
   *
   * Returning ::GCOMP_ERR_UNSUPPORTED means "not this stream", and the caller
   * then decodes it single-threaded. That is the answer for most streams:
   * whether a stream can be split at all is a property of how it was written,
   * not of the format. Blocks inside a Zstandard frame share a window (RFC 8878
   * section 3.1.1.1.2) and cannot be split; an LZ4 frame that clears B.Indep
   * links its blocks to the ones before them; a stream with one block has
   * nothing to divide. A method must decline all of those rather than produce
   * a plausible wrong answer.
   *
   * It must also decline anything it is not certain of. The single-threaded
   * path is always correct, so the cost of declining is speed, and the cost of
   * guessing is a wrong decode.
   *
   * ## What it must not change
   *
   * The bytes. Decoding with threads must produce exactly what decoding
   * without them produces, including every limit and checksum the ordinary
   * path enforces - otherwise `threads.count` becomes a correctness setting
   * rather than a speed one, which is the sort of option nobody can use
   * safely.
   *
   * @param registry Registry the method was found in
   * @param options Configuration options (may be NULL for defaults)
   * @param input Whole compressed stream
   * @param input_size How many bytes of it
   * @param output Where the decoded bytes go
   * @param output_capacity How much room there is
   * @param output_size_out Receives how much was written
   * @return ::GCOMP_OK; ::GCOMP_ERR_UNSUPPORTED to decline, leaving
   *         @p output untouched; any other error as the ordinary path would
   *         report it
   */
  gcomp_status_t (*decode_parallel)(gcomp_registry_t * registry,
      gcomp_options_t * options, const void * input, size_t input_size,
      void * output, size_t output_capacity, size_t * output_size_out);
};

/**
 * @brief Method ABI version this build of the library defines.
 *
 * Version 1 ends at @ref gcomp_method_s::get_schema.  Version 2 adds
 * @ref gcomp_method_s::encode_bound and @ref gcomp_method_s::peek.  Version 3
 * adds @ref gcomp_method_s::decode_parallel.  A descriptor is registered by
 * pointer and read through @ref gcomp_method_s::size, so a version-1 method
 * still registers and works; only the hooks it does not define are
 * unavailable.
 */
#define GCOMP_METHOD_ABI_VERSION 3u

/**
 * @brief List all option keys supported by a method.
 *
 * The returned @p keys_out pointer (if non-NULL) is owned by the method and
 * remains valid for the lifetime of the method descriptor; it must not be
 * freed by the caller.
 *
 * @param method The method whose option keys should be listed.
 * @param keys_out Output parameter for the array of key strings (may be NULL).
 * @param count_out Output parameter for the number of keys.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_option_keys(
    const gcomp_method_t * method, const char * const ** keys_out,
    size_t * count_out);

/**
 * @brief Get the schema descriptor for a specific option key.
 *
 * @param method The method whose schema should be queried.
 * @param key The option key to look up.
 * @param schema_out Output parameter for the schema descriptor.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_option_schema(
    const gcomp_method_t * method, const char * key,
    const gcomp_option_schema_t ** schema_out);

/**
 * @brief Get the full option schema for a method.
 *
 * @param method The method whose schema should be retrieved.
 * @param schema_out Output parameter for the schema descriptor.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_method_get_all_schemas(
    const gcomp_method_t * method, const gcomp_method_schema_t ** schema_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_METHOD_H
