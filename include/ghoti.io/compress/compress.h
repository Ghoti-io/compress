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
 * @file compress.h
 *
 * Main header for the Ghoti.io Compress library.
 *
 * Cross-platform C library implementing streaming compression with no
 * external dependencies.
 */

#ifndef GHOTI_IO_GCOMP_COMPRESS_H
#define GHOTI_IO_GCOMP_COMPRESS_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>


/*
 * Library version information comes from libver.h, which takes it from the
 * generated libver_gen.h: GCOMP_VERSION_MAJOR / _MINOR / _PATCH, plus
 * GCOMP_VERSION_STRING and the packed GCOMP_VERSION_NUMBER. It used to be
 * written out here as three zeros that no build step ever updated.
 */

/**
 * @brief Get the major version number
 * @return The major version number
 */
GCOMP_API uint32_t gcomp_version_major(void);

/**
 * @brief Get the minor version number
 * @return The minor version number
 */
GCOMP_API uint32_t gcomp_version_minor(void);

/**
 * @brief Get the patch version number
 * @return The patch version number
 */
GCOMP_API uint32_t gcomp_version_patch(void);

/**
 * @brief Get the version string
 * @return A string representation of the version (e.g., "0.0.0")
 */
GCOMP_API const char * gcomp_version_string(void);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode data from a buffer to a buffer
 *
 * Convenience function that encodes input data using the specified compression
 * method. This function handles encoder creation, multiple update calls, and
 * finish internally.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to input data; may be NULL only when input_size is
 * 0
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer (must be non-NULL)
 * @param output_capacity Capacity of output buffer in bytes; must be > 0
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. GCOMP_ERR_INVALID_ARG if output_capacity is 0, or
 *         input_data is NULL with input_size > 0; GCOMP_ERR_LIMIT if output
 *         buffer is too small. On error when output_capacity is 0,
 *         *output_size_out is set to 0.
 */
GCOMP_API gcomp_status_t gcomp_encode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out);

/**
 * @brief Decode data from a buffer to a buffer
 *
 * Convenience function that decodes compressed data using the specified
 * compression method. This function handles decoder creation, multiple update
 * calls, and finish internally.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to compressed input data; may be NULL only when
 *        input_size is 0
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer (must be non-NULL)
 * @param output_capacity Capacity of output buffer in bytes; must be > 0
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. GCOMP_ERR_INVALID_ARG if output_capacity is 0, or
 *         input_data is NULL with input_size > 0; GCOMP_ERR_LIMIT if output
 *         buffer is too small. On error when output_capacity is 0,
 *         *output_size_out is set to 0.
 */
GCOMP_API gcomp_status_t gcomp_decode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out);

/**
 * @brief Largest output gcomp_encode_buffer() can produce for this input size
 *
 * An output buffer of at least this size is always enough: encoding
 * @p input_size bytes with @p method_name and @p options into a buffer this
 * large cannot fail for want of room, whatever those bytes turn out to be.
 * Data that does not compress is the case that matters - every method here
 * falls back to storing such data verbatim, and the bound is what that costs,
 * which is the input plus the framing around it.
 *
 * Use it to size a buffer before a one-shot encode:
 *
 * @code
 * size_t cap = 0;
 * if (gcomp_encode_bound(NULL, "zstd", opts, len, &cap) != GCOMP_OK) { ... }
 * void * buf = malloc(cap);
 * size_t written = 0;
 * gcomp_encode_buffer(NULL, "zstd", opts, data, len, buf, cap, &written);
 * @endcode
 *
 * ## What it does not cover
 *
 * One whole stream: create, update as often as you like, finish. It does
 * **not** account for gcomp_encoder_flush(). A flush ends a block early and
 * pads to a byte boundary, so it adds output every time it is called and the
 * total depends on how often a caller chooses to call it - which is not
 * knowable from the input size. A caller that flushes must size its own
 * buffer, or stream into one it can refill.
 *
 * The options matter and are read: a gzip name and comment, an LZ4 block size
 * and its checksums, a zstd window that lowers the maximum block size all move
 * the answer. Pass the same options you will pass to the encoder.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_size Number of input bytes that will be encoded
 * @param bound_out Receives the worst-case encoded size
 * @return GCOMP_OK; GCOMP_ERR_INVALID_ARG for a NULL name or output pointer;
 *         GCOMP_ERR_UNSUPPORTED if the method is not registered, cannot
 *         encode, or does not implement a bound; GCOMP_ERR_LIMIT if the bound
 *         is too large to represent in a size_t
 */
GCOMP_API gcomp_status_t gcomp_encode_bound(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, size_t input_size,
    size_t * bound_out);

/**
 * @brief What a stream's header says about it, whatever the format.
 *
 * Filled by gcomp_peek(). Every field is read out of the header; nothing is
 * inferred and nothing is decoded. A format that does not carry a field leaves
 * it zero - which is why @ref has_content_size and @ref has_dictionary exist
 * separately from the values they guard, since zero is a legitimate content
 * size and a legitimate dictionary ID.
 *
 * Three formats have no header at all (deflate, LZW, RLE). For
 * those, @ref header_size is zero and only @ref window_size is meaningful.
 */
typedef struct gcomp_stream_info_s {
  /**
   * @brief Non-zero when the stream opens with a skippable frame.
   *
   * LZ4 and Zstandard both define these, on the same magic range and with the
   * same layout, so a skippable frame does not say which format follows it.
   * The other fields describe the skippable frame rather than any data frame;
   * step over it using @ref skippable_size and peek again.
   */
  int is_skippable;
  /// Whole skippable frame, its eight-byte header included.
  uint64_t skippable_size;
  /// The magic number's low nibble, 0 through 15, which is the caller's to use.
  unsigned skippable_variant;

  /// Bytes of header before the first block. Zero for a headerless format.
  size_t header_size;

  /// Non-zero when the header states the decompressed size.
  int has_content_size;
  /// That size, when it is stated.
  uint64_t content_size;

  /**
   * @brief History the decoder must keep, in bytes.
   *
   * The window a back-reference may reach into. For deflate and the formats
   * that wrap it this comes from the header where there is one and is 32768
   * otherwise, that being the largest RFC 1951 allows.
   */
  uint64_t window_size;

  /// Non-zero when the stream carries a checksum of the content.
  int has_checksum;

  /// Non-zero when the header says a dictionary is needed to decode.
  int has_dictionary;
  /// Which dictionary, when the format names one.
  uint32_t dictionary_id;
} gcomp_stream_info_t;

/**
 * @brief Read what a stream's header says, without decoding it
 *
 * Answers the questions a caller has to settle before it can decode safely:
 * how large the output will be, how much history the decoder will hold, and
 * whether a dictionary it does not have is required.
 *
 * The most useful of those is the content size, where a format states it. A
 * caller that knows it can size an output buffer exactly and set
 * `limits.max_output_bytes` to the same number, which is a tighter and more
 * honest bound than the expansion-ratio heuristic that otherwise has to stand
 * in for it.
 *
 * @code
 * gcomp_stream_info_t info;
 * size_t needed = 0;
 * gcomp_status_t s = gcomp_peek(NULL, "zstd", NULL, buf, len, &info, &needed);
 * if (s == GCOMP_ERR_LIMIT) {
 *   // Read `needed` bytes and try again.
 * }
 * @endcode
 *
 * ## Not every format has an answer
 *
 * deflate, LZW and RLE begin with data, so there is no header to read: the
 * call succeeds with @ref gcomp_stream_info_t::header_size zero and nothing
 * stated. That is not an error - it is the honest answer for those formats,
 * and it is why the fields are guarded by their own flags.
 *
 * A stream that opens with a skippable frame reports that frame
 * (@ref gcomp_stream_info_t::is_skippable), because the frame belongs to both
 * LZ4 and Zstandard and says nothing about what follows it.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "zstd")
 * @param options Configuration options (can be NULL for defaults)
 * @param input Start of the compressed stream
 * @param input_size How much of it is available
 * @param info_out Receives what the header says
 * @param needed_out Receives how many bytes are needed, when there are not
 *        enough yet; may be NULL
 * @return GCOMP_OK; GCOMP_ERR_LIMIT when more input is needed, with
 *         @p needed_out set; GCOMP_ERR_CORRUPT when the header is malformed;
 *         GCOMP_ERR_UNSUPPORTED if the method is not registered, cannot
 *         decode, or does not implement a peek; GCOMP_ERR_INVALID_ARG for a
 *         NULL name or output
 */
GCOMP_API gcomp_status_t gcomp_peek(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out);

/**
 * @brief Work out which method wrote these bytes
 *
 * Recognises the three formats that carry a magic number - gzip (RFC 1952
 * section 2.3.1), Zstandard (RFC 8878 section 3.1.1) and LZ4 (LZ4 Frame
 * Format) - with near certainty, because four bytes of magic do not occur by
 * accident.
 *
 * zlib is different and the difference matters. RFC 1950 gives it no magic
 * number, only two header bytes with enough structure to test - a compression
 * method of 8, a window of at most 32 KB, and a value divisible by 31 - which
 * about one random byte pair in 992 satisfies. zlib is therefore reported only
 * when nothing else matches, and a caller that cannot afford a wrong answer
 * should treat it as a hint and confirm by decoding.
 *
 * deflate, LZW and RLE begin with data. They are never reported, and a stream
 * in one of them comes back as ::GCOMP_ERR_UNSUPPORTED - which is the honest
 * answer, and better than a guess the caller would act on.
 *
 * A stream that opens with skippable frames is stepped over until a frame that
 * identifies itself is reached, because LZ4 and Zstandard share that magic
 * range and a skippable frame belongs to both.
 *
 * There is no registry parameter: which format wrote a stream is a property of
 * the bytes, not of what happens to be registered. Check the name against a
 * registry yourself if that matters.
 *
 * @param input Start of the compressed stream
 * @param input_size How much of it is available
 * @param method_name_out Receives a static string naming the method; never
 *        freed, valid for the life of the program
 * @param needed_out Receives how many bytes are needed, when there are not
 *        enough yet; may be NULL
 * @return GCOMP_OK with @p method_name_out set; GCOMP_ERR_LIMIT when more
 *         input is needed; GCOMP_ERR_UNSUPPORTED when nothing matches;
 *         GCOMP_ERR_INVALID_ARG for a NULL @p method_name_out
 */
GCOMP_API gcomp_status_t gcomp_detect(const void * input, size_t input_size,
    const char ** method_name_out, size_t * needed_out);

/**
 * @brief Encode into a buffer the library allocates
 *
 * The same as gcomp_encode_buffer(), without having to size the output first:
 * it takes gcomp_encode_bound()'s answer, allocates that, encodes, and gives
 * back a buffer trimmed to what was actually written.
 *
 * The buffer belongs to the caller and must be released with
 * gcomp_buffer_free(), passing the same registry - it is allocated with that
 * registry's allocator, and freeing it with anything else is wrong.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to input data; may be NULL only when input_size is 0
 * @param input_size Size of input data in bytes
 * @param data_out Receives the allocated buffer; NULL on failure
 * @param size_out Receives the number of bytes written
 * @return Status code. Nothing is allocated for the caller to free on failure.
 */
GCOMP_API gcomp_status_t gcomp_encode_alloc(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void ** data_out,
    size_t * size_out);

/**
 * @brief Decode into a buffer the library allocates
 *
 * For the common case of decompressing something whose size you do not know.
 * Where the format states a decompressed size in its header, that is used as a
 * starting point - a hint, not a promise, since the number comes from the
 * stream. Otherwise the buffer grows as the decoder fills it.
 *
 * **Every enlargement is checked against `limits.max_output_bytes` first**, so
 * a decompression bomb is refused by the documented ceiling rather than by
 * exhausting memory. The default is 512 MiB; setting that option to zero asks
 * for no ceiling and gets none.
 *
 * The buffer belongs to the caller and must be released with
 * gcomp_buffer_free(), passing the same registry.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to compressed input; may be NULL only when
 *        input_size is 0
 * @param input_size Size of compressed input in bytes
 * @param data_out Receives the allocated buffer; NULL on failure
 * @param size_out Receives the number of bytes decoded
 * @return Status code; GCOMP_ERR_LIMIT if the output would exceed
 *         `limits.max_output_bytes`. Nothing is allocated for the caller to
 *         free on failure.
 */
GCOMP_API gcomp_status_t gcomp_decode_alloc(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void ** data_out,
    size_t * size_out);

/**
 * @brief Release a buffer from gcomp_encode_alloc() or gcomp_decode_alloc()
 *
 * Pass the registry the buffer was allocated through, or NULL if that was
 * NULL. Freeing with anything else - including plain free() - is wrong
 * whenever the registry carries an allocator of its own.
 *
 * Does nothing when @p data is NULL.
 *
 * @param registry The registry the buffer came from (NULL for the default)
 * @param data The buffer to release
 */
GCOMP_API void gcomp_buffer_free(gcomp_registry_t * registry, void * data);


#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_COMPRESS_H
