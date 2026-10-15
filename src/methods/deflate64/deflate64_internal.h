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
 * @file deflate64_internal.h
 *
 * Declarations shared by the deflate64 sources: the tables the format changes,
 * and the decoder and encoder entry points the registration wires up.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_DEFLATE64_DEFLATE64_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_DEFLATE64_DEFLATE64_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

/** How far back a match may reach. */
#define D64_WINDOW 65536u
#define D64_WINDOW_MASK (D64_WINDOW - 1u)
/** The longest match: length code 285 is 3 plus sixteen extra bits. */
#define D64_MAX_MATCH 65538u
#define D64_MIN_MATCH 3u

/** Length codes 257..285 (index 0..28): the base length and extra bits. */
extern const uint16_t g_d64_len_base[29];
extern const uint8_t g_d64_len_extra[29];
/** Distance codes 0..31: the base distance and extra bits. */
extern const uint32_t g_d64_dist_base[32];
extern const uint8_t g_d64_dist_extra[32];
/** The order code length code lengths are sent in. */
extern const uint8_t g_d64_cl_order[19];

gcomp_status_t d64_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);
void d64_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t d64_decoder_reset(gcomp_decoder_t * decoder);
gcomp_status_t d64_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t d64_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

gcomp_status_t d64_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);
void d64_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t d64_encoder_reset(gcomp_encoder_t * encoder);
gcomp_status_t d64_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t d64_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);
gcomp_status_t d64_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode);

#endif
