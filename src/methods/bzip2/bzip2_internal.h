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
 * @file bzip2_internal.h
 *
 * What the bzip2 decoder and encoder share. Nothing here is part of the public
 * API.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_BZIP2_BZIP2_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_BZIP2_BZIP2_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/bzip2.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The block magic, 0x314159265359: the digits of pi. */
#define BZIP2_BLOCK_MAGIC 0x314159265359ull
/** The end-of-stream magic, 0x177245385090: the digits of the square root of pi. */
#define BZIP2_END_MAGIC 0x177245385090ull

#define BZIP2_MAX_GROUPS 6
#define BZIP2_MIN_GROUPS 2
#define BZIP2_GROUP_SIZE 50
#define BZIP2_MAX_ALPHA 258
#define BZIP2_MAX_CODE_LEN 20
#define BZIP2_MAX_SELECTORS 18002
#define BZIP2_BLOCK_UNIT 100000u
#define BZIP2_RUNA 0
#define BZIP2_RUNB 1

extern const uint32_t bzip2_crc_table[256];

static inline uint32_t bzip2_crc_update(uint32_t crc, uint8_t byte) {
  return (crc << 8) ^ bzip2_crc_table[(crc >> 24) ^ byte];
}

static inline uint32_t bzip2_combine(uint32_t combined, uint32_t block_crc) {
  return ((combined << 1) | (combined >> 31)) ^ block_crc;
}

/** Scratch for the transform: 2n + 1 symbols of text and of suffix array. */
typedef struct bzip2_bwt_scratch_s {
  int32_t * s;
  int32_t * sa;
} bzip2_bwt_scratch_t;

/**
 * The Burrows-Wheeler transform of a block of n >= 1 bytes: the last column of
 * its sorted rotations into `last`, and the row the unrotated block is in. The
 * scratch holds at least 2n + 1 entries of each. Returns 0, or -1 if memory
 * ran out.
 */
int bzip2_bwt(const gcomp_allocator_t * alloc, bzip2_bwt_scratch_t * scratch,
    const uint8_t * block, uint32_t n, uint8_t * last, uint32_t * orig_ptr);

gcomp_status_t bzip2_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);
void bzip2_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t bzip2_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t bzip2_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);
gcomp_status_t bzip2_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);
gcomp_status_t bzip2_encoder_reset(gcomp_encoder_t * encoder);

gcomp_status_t bzip2_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);
void bzip2_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t bzip2_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t bzip2_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);
gcomp_status_t bzip2_decoder_reset(gcomp_decoder_t * decoder);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_BZIP2_BZIP2_INTERNAL_H */
