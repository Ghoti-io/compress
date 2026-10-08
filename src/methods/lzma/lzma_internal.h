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
 * @file lzma_internal.h
 *
 * What the LZMA decoder and encoder share: the probability model's layout and
 * the constants that size it, the properties byte, and the entry points the
 * two methods register. Nothing here is part of the public API.
 *
 * The model is the one in the LZMA SDK's `lzma-specification.txt`. A symbol is
 * a literal, a match with a new distance, a one-byte match at the last
 * distance ("short rep"), or a match at one of the four most recent distances.
 * Every decision is one adaptive bit: an 11-bit probability, moved by a shift
 * of 5 toward whichever value just occurred.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_LZMA_LZMA_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_LZMA_LZMA_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzma.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZMA_PROB_BITS 11
#define LZMA_PROB_ONE (1u << LZMA_PROB_BITS)
#define LZMA_PROB_INIT (LZMA_PROB_ONE / 2u)
#define LZMA_MOVE_BITS 5
#define LZMA_TOP (1u << 24)

#define LZMA_STATES 12
#define LZMA_LIT_STATES 7
#define LZMA_POS_STATES_MAX 16
#define LZMA_MATCH_MIN 2u
#define LZMA_MATCH_MAX 273u
#define LZMA_LEN_LOW_BITS 3
#define LZMA_LEN_MID_BITS 3
#define LZMA_LEN_HIGH_BITS 8
#define LZMA_LEN_LOW_SYMBOLS (1u << LZMA_LEN_LOW_BITS)
#define LZMA_LEN_MID_SYMBOLS (1u << LZMA_LEN_MID_BITS)
#define LZMA_DIST_STATES 4
#define LZMA_DIST_SLOT_BITS 6
#define LZMA_DIST_SLOTS (1u << LZMA_DIST_SLOT_BITS)
#define LZMA_END_POS_MODEL 14
#define LZMA_FULL_DISTANCES (1u << (LZMA_END_POS_MODEL / 2))
#define LZMA_ALIGN_BITS 4
#define LZMA_ALIGN_SIZE (1u << LZMA_ALIGN_BITS)
#define LZMA_LIT_CODER 0x300u

/** The most input one symbol can need; the SDK's LZMA_REQUIRED_INPUT_MAX. */
#define LZMA_REQUIRED_INPUT_MAX 20u

/** The distance value the end marker carries. */
#define LZMA_EOS_DISTANCE 0xFFFFFFFFu

/** A stream's smallest legal dictionary; the SDK and liblzma both clamp to it. */
#define LZMA_DICT_MIN 4096u

typedef struct lzma_len_probs_s {
  uint16_t choice;
  uint16_t choice2;
  uint16_t low[LZMA_POS_STATES_MAX][LZMA_LEN_LOW_SYMBOLS];
  uint16_t mid[LZMA_POS_STATES_MAX][LZMA_LEN_MID_SYMBOLS];
  uint16_t high[1u << LZMA_LEN_HIGH_BITS];
} lzma_len_probs_t;

/** Every probability except the literal coders, which depend on lc and lp. */
typedef struct lzma_probs_s {
  uint16_t is_match[LZMA_STATES][LZMA_POS_STATES_MAX];
  uint16_t is_rep[LZMA_STATES];
  uint16_t is_rep_g0[LZMA_STATES];
  uint16_t is_rep_g1[LZMA_STATES];
  uint16_t is_rep_g2[LZMA_STATES];
  uint16_t is_rep0_long[LZMA_STATES][LZMA_POS_STATES_MAX];
  uint16_t pos_slot[LZMA_DIST_STATES][LZMA_DIST_SLOTS];
  uint16_t pos_spec[1u + LZMA_FULL_DISTANCES - LZMA_END_POS_MODEL];
  uint16_t align[LZMA_ALIGN_SIZE];
  lzma_len_probs_t len;
  lzma_len_probs_t rep_len;
} lzma_probs_t;

/** Fill every probability, literal coders included, with one half. */
void lzma_probs_init(lzma_probs_t * probs, uint16_t * lit, size_t lit_count);

/** Literal coders needed for a given lc and lp. */
static inline size_t lzma_lit_count(unsigned lc, unsigned lp) {
  return (size_t)LZMA_LIT_CODER << (lc + lp);
}

/** The state after a literal, a match, a rep match, and a short rep. */
static inline unsigned lzma_state_literal(unsigned s) {
  return s < 4 ? 0 : (s < 10 ? s - 3 : s - 6);
}
static inline unsigned lzma_state_match(unsigned s) {
  return s < LZMA_LIT_STATES ? 7 : 10;
}
static inline unsigned lzma_state_rep(unsigned s) {
  return s < LZMA_LIT_STATES ? 8 : 11;
}
static inline unsigned lzma_state_short_rep(unsigned s) {
  return s < LZMA_LIT_STATES ? 9 : 11;
}

/** Which of the four length-states a match length selects distance slots from. */
static inline unsigned lzma_dist_state(unsigned len) {
  return len - LZMA_MATCH_MIN < LZMA_DIST_STATES - 1 ? len - LZMA_MATCH_MIN
                                                      : LZMA_DIST_STATES - 1;
}

/** The properties byte, (pb * 5 + lp) * 9 + lc; false if it is out of range. */
int lzma_props_decode(uint8_t byte, unsigned * lc, unsigned * lp, unsigned * pb);
uint8_t lzma_props_encode(unsigned lc, unsigned lp, unsigned pb);

/** The registered entry points. */
gcomp_status_t lzma_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder, int lzma2);
void lzma_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t lzma_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t lzma_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);
gcomp_status_t lzma_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);
gcomp_status_t lzma_encoder_reset(gcomp_encoder_t * encoder);

gcomp_status_t lzma_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder, int lzma2);
void lzma_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t lzma_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t lzma_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);
gcomp_status_t lzma_decoder_reset(gcomp_decoder_t * decoder);

/** Whether an LZMA2 decoder has read its end marker, which xz needs in order
 * to know where a block's data stops. */
int lzma_decoder_done(const gcomp_decoder_t * decoder);

/** The dictionary an encoder was configured with, in bytes. */
uint32_t lzma_encoder_dict_size(const gcomp_encoder_t * encoder);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_LZMA_LZMA_INTERNAL_H */
