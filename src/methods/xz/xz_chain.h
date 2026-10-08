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
 * @file xz_chain.h
 *
 * Encoders, or decoders, run one into the next.
 *
 * An xz block is the output of a chain: delta or a branch converter, then
 * LZMA2, going in, and the same in the other order coming out. Each stage is
 * an ordinary encoder or decoder made through the registry, so a stage is
 * exactly the method of that name, and between two stages there is a small
 * buffer that holds what one has made and the next has not yet taken.
 *
 * The last stage writes straight into the caller's output, so a chain of one
 * (a block with no filter in front of LZMA2) copies nothing extra.
 *
 * Finishing is the part that needs care. A stage can only be finished once
 * everything before it has been, and everything it was given has been
 * accepted; and any stage may stop partway because the buffer after it, or
 * the caller's output, is full. chain_finish() keeps its place across such
 * stops, so it is called again with more room until it returns ::GCOMP_OK.
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_CHAIN_H
#define GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_CHAIN_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_CHAIN_STAGES_MAX 4u

typedef struct xz_chain_stage_s {
  gcomp_encoder_t * enc;
  gcomp_decoder_t * dec;
  uint8_t * buf; /* what this stage made and the next has not taken */
  size_t pos, len;
} xz_chain_stage_t;

typedef struct xz_chain_s {
  const gcomp_allocator_t * alloc;
  int encode;
  unsigned count;
  unsigned fin; /* the first stage not yet finished */
  int fin_started; /* finish() has been called on stage `fin` */
  gcomp_status_t status; /* the first failure, kept */
  char detail[256];
  xz_chain_stage_t st[XZ_CHAIN_STAGES_MAX];
} xz_chain_t;

/** An empty chain. */
void xz_chain_init(xz_chain_t * c, const gcomp_allocator_t * alloc, int encode);

/**
 * @brief Add the next stage, taking ownership of @p codec (a gcomp_encoder_t
 *        or a gcomp_decoder_t to match the chain). The last stage added
 *        writes to the caller; every other stage gets a buffer.
 * @return ::GCOMP_OK, or ::GCOMP_ERR_MEMORY, in which case @p codec is
 *         destroyed.
 */
gcomp_status_t xz_chain_add(xz_chain_t * c, void * codec);

/** Destroy every stage and free the buffers; the chain can be reused. */
void xz_chain_clear(xz_chain_t * c);

/** Start every stage over, as a fresh chain of the same stages. */
gcomp_status_t xz_chain_reset(xz_chain_t * c);

/**
 * @brief Pass @p input through the chain into @p output until one of them is
 *        used up. @p input may be NULL, to drain what is already inside.
 */
gcomp_status_t xz_chain_update(
    xz_chain_t * c, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Finish every stage in turn and drain it into @p output.
 * @return ::GCOMP_OK when all is out; ::GCOMP_ERR_LIMIT when @p output is full
 *         and more is to come, call again; or an error.
 */
gcomp_status_t xz_chain_finish(xz_chain_t * c, gcomp_buffer_t * output);

/** Whether stage 0, a decoder, has read its whole stream. */
int xz_chain_first_done(const xz_chain_t * c);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_XZ_XZ_CHAIN_H */
