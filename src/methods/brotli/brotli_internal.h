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
 * @file brotli_internal.h
 *
 * Shared types for the brotli method. Nothing here is part of the public API.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include "brotli_bits.h"

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#define BROTLI_DICT_SIZE 122784
#define BROTLI_MAX_ALPHABET 704
#define BROTLI_WINDOW_MAX_BITS 24

typedef struct brotli_huff_s {
  int simple;
  int max_bits;
  uint16_t count[16];
  uint16_t first[16];
  uint16_t offset[16];
  uint16_t * syms;
} brotli_huff_t;

typedef struct brotli_sym_s {
  uint32_t code;
  int n;
} brotli_sym_t;

/**
 * @brief One word transformation from RFC 7932 Appendix B.
 *
 * prefix and suffix are offsets into the transform blob, which stores each
 * string with a terminating NUL that is not part of the length.
 */
typedef struct brotli_xform_s {
  uint16_t prefix_off;
  uint8_t prefix_len;
  uint8_t kind;
  uint16_t suffix_off;
  uint8_t suffix_len;
} brotli_xform_t;

typedef struct brotli_tree_s {
  int step;
  int alphabet;
  brotli_huff_t * dest;
  int idx;
  int space;
  int nnz;
  int only;
  uint8_t clen[18];
  brotli_huff_t cl_huff;
  int cl_step;
  int cl_b0;
  uint8_t lens[BROTLI_MAX_ALPHABET];
  int prev_len;
  int repeat;
  int repeat_len;
  int rep_sym;
  int nsym;
  int abits;
  int tree_select;
  uint16_t got[4];
  gcomp_status_t err_status;
  char err[96];
} brotli_tree_t;

const uint8_t * brotli_dict_data(void);
const uint8_t * brotli_dict_ndbits(void);
const uint32_t * brotli_dict_offset(void);
const uint8_t * brotli_lut0(void);
const uint8_t * brotli_lut1(void);
const uint8_t * brotli_lut2(void);
const uint8_t * brotli_xform_blob(void);
const brotli_xform_t * brotli_xform(int id);

void brotli_huff_free(const gcomp_allocator_t * alloc, brotli_huff_t * h);
void brotli_tree_begin(brotli_tree_t * t, brotli_huff_t * dest, int alphabet,
    const gcomp_allocator_t * alloc);
int brotli_tree_read(brotli_tree_t * t, brotli_bits_t * bits, brotli_sym_t * sym,
    const gcomp_allocator_t * alloc);
int brotli_read_sym(brotli_bits_t * bits, const brotli_huff_t * h,
    brotli_sym_t * sym, int * out);

int brotli_dict_word(int length, uint64_t word_id, uint8_t * dst, int dst_cap,
    int * out_len);

/**
 * @brief Write one compressed meta-block, or ask the caller to store it.
 *
 * Returns 0 when dst holds a compressed meta-block followed by an empty
 * metadata block (so the stream is back on a byte boundary) and dist_rb is
 * updated. Returns 1 when the compressed form is not smaller than a stored
 * block, or the block cannot be represented; dist_rb is unchanged and the
 * caller writes the section 11.1 block. Returns -1 on allocation failure.
 */
int brotli_compress_chunk(const gcomp_allocator_t * alloc, uint8_t * dst,
    size_t dst_cap, size_t * out_n, const uint8_t * data, size_t len,
    uint32_t window, uint32_t dist_rb[4]);

gcomp_status_t brotli_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);
void brotli_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t brotli_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t brotli_encoder_finish(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output);
gcomp_status_t brotli_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode);
gcomp_status_t brotli_encoder_reset(gcomp_encoder_t * encoder);

gcomp_status_t brotli_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);
void brotli_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t brotli_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t brotli_decoder_finish(gcomp_decoder_t * decoder,
    gcomp_buffer_t * output);
gcomp_status_t brotli_decoder_reset(gcomp_decoder_t * decoder);

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_BROTLI_BROTLI_INTERNAL_H */
