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
 * @file xz_internal.h
 *
 * What the xz encoder and decoder share: the container's constants, the
 * integrity checks, the variable-length integers, the filter chain specs, and
 * the entry points the method registers. Nothing here is public API.
 *
 * An xz file is one or more streams. A stream is a 12-byte header, blocks, an
 * index of the blocks, and a 12-byte footer. A block is a header naming a
 * chain of filters (the last one LZMA2), the chain's output, padding to a
 * multiple of four, and a check of the uncompressed bytes. The index repeats
 * what each block's size was, so that a reader can find a block without
 * reading the ones before it.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/security/sha256.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_STREAM_HEADER_SIZE 12u
#define XZ_STREAM_FOOTER_SIZE 12u
#define XZ_BLOCK_HEADER_MAX 1024u
#define XZ_BLOCK_HEADER_MIN 8u
#define XZ_FILTERS_MAX 4u
#define XZ_VLI_MAX_BYTES 9u
#define XZ_VLI_MAX 0x7FFFFFFFFFFFFFFFull

/** The largest dictionary a block may name by default: xz's own ceiling. */
#define GCOMP_XZ_DEFAULT_WINDOW (3ull << 29)

/** Filter ids, as the format numbers them. LZMA2 is always the last. */
#define XZ_FILTER_DELTA 0x03u
#define XZ_FILTER_LZMA2 0x21u

/** Integrity checks. The format reserves sixteen; four are implemented. */
#define XZ_CHECK_NONE 0u
#define XZ_CHECK_CRC32 1u
#define XZ_CHECK_CRC64 4u
#define XZ_CHECK_SHA256 10u

/** The largest an LZMA2 dictionary size property can be. */
#define XZ_DICT_PROP_MAX 40u

/** The bytes a check of this id occupies; the format fixes it for all sixteen. */
unsigned xz_check_size(unsigned type);

/** Whether this build can compute and verify a check of this id. */
int xz_check_known(unsigned type);

typedef struct {
  unsigned type;
  uint32_t crc32;
  uint64_t crc64;
  GSEC_Sha256 sha;
} xz_check_t;

void xz_check_init(xz_check_t * c, unsigned type);
void xz_check_update(xz_check_t * c, const void * data, size_t n);
/** Writes xz_check_size(type) bytes, in the order the format stores them. */
void xz_check_final(xz_check_t * c, uint8_t * out);

/** CRC-64/XZ (ECMA-182 polynomial, reflected), running value in and out. */
uint64_t xz_crc64_update(uint64_t crc, const uint8_t * data, size_t n);

/** Little-endian stores and loads. */
static inline void xz_put_le32(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t xz_get_le32(const uint8_t * p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
      (uint32_t)p[3] << 24;
}

/** CRC-32 over a buffer, finished, as the format's headers store it. */
uint32_t xz_crc32(const uint8_t * data, size_t n);

/** Append a variable-length integer; returns the bytes written, 1 to 9. */
size_t xz_vli_put(uint8_t * out, uint64_t v);

/** Bytes a value occupies as a variable-length integer. */
size_t xz_vli_size(uint64_t v);

/** Reads a variable-length integer a byte at a time. */
typedef struct {
  uint64_t value;
  unsigned count;
} xz_vli_t;

/**
 * @brief Feed one byte. Returns 1 when the integer is complete, 0 when it
 *        wants another byte, and -1 when it is not a valid one: more than nine
 *        bytes, or ending in a zero byte that a shorter spelling would have
 *        done without.
 */
int xz_vli_feed(xz_vli_t * v, uint8_t byte);

/** The property byte for the smallest LZMA2 dictionary holding @p dict. */
uint8_t xz_dict_prop(uint64_t dict);

/** The dictionary size a property byte means; 0 when the byte is above 40. */
uint64_t xz_dict_size(uint8_t prop);

/** One filter in front of LZMA2: delta, or a branch converter. */
typedef struct {
  unsigned id;
  uint32_t arg; /* delta: the distance; a converter: the start offset */
} xz_filter_t;

/**
 * @brief Parse an `xz.filters` string: filters separated by commas, each
 *        `delta` or `delta:N`, or an architecture name with an optional
 *        `:OFFSET`, in the order they are applied when encoding.
 *
 * @return 1 on success; 0 with a message in @p err otherwise.
 */
int xz_filters_parse(const char * text, xz_filter_t * out, unsigned * count,
    char * err, size_t err_cap);

/** The registered entry points. */
gcomp_status_t xz_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);
void xz_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t xz_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t xz_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);
gcomp_status_t xz_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);
gcomp_status_t xz_encoder_reset(gcomp_encoder_t * encoder);

gcomp_status_t xz_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);
void xz_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t xz_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t xz_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);
gcomp_status_t xz_decoder_reset(gcomp_decoder_t * decoder);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_INTERNAL_H */
