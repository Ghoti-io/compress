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
 * @file filter_transform.h
 *
 * The transforms behind the `delta` and `bcj` methods, as plain functions on a
 * buffer. Each is size-preserving and exactly reversible, and each exists to
 * make data compress better rather than to compress it: delta turns a signal
 * that changes slowly into one that is mostly small numbers, and the branch
 * converters turn the relative targets of calls and jumps into absolute ones,
 * so that the same function called from many places becomes the same bytes.
 *
 * A branch converter cannot always finish what it is given: an x86 call
 * carries four bytes after its opcode, so the last four bytes of a buffer may
 * be the start of one. Each converter says how many bytes it settled and the
 * caller keeps the rest for the next call, or lets them through unchanged at
 * the end of the stream, which is what xz does.
 *
 * The format is xz's: filter ids, and for the branch converters the exact
 * rewriting, since a stream written here is meant to be read by xz and the
 * other way round.
 */

#ifndef GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_TRANSFORM_H
#define GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_TRANSFORM_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The architectures a branch converter knows, numbered as xz's filter ids.
typedef enum {
  FILTER_ARCH_X86 = 0x04,
  FILTER_ARCH_POWERPC = 0x05,
  FILTER_ARCH_IA64 = 0x06,
  FILTER_ARCH_ARM = 0x07,
  FILTER_ARCH_ARMTHUMB = 0x08,
  FILTER_ARCH_SPARC = 0x09,
  FILTER_ARCH_ARM64 = 0x0A,
  FILTER_ARCH_RISCV = 0x0B
} filter_arch_t;

/// The most bytes a converter ever leaves unsettled: IA-64's bundle is 16.
#define FILTER_BCJ_MAX_HOLD 16u

/// What a branch converter remembers between calls. Only x86 has any.
typedef struct {
  uint32_t prev_mask;
  uint32_t prev_pos;
} filter_bcj_state_t;

/// The state of a converter before the first byte.
void filter_bcj_init(filter_bcj_state_t * st);

/// Whether this build converts @p arch.
int filter_bcj_known(unsigned arch);

/**
 * @brief Convert the branches in @p buf.
 *
 * @param st State carried between calls; start it with filter_bcj_init().
 * @param arch One of ::filter_arch_t.
 * @param now_pos Where @p buf starts in the stream, counting from the
 *        stream's start offset, modulo 2^32.
 * @param encoding Nonzero to make targets absolute, zero to make them
 *        relative again.
 * @param buf The bytes, converted in place.
 * @param size How many.
 * @return How many bytes at the start of @p buf are settled; the caller
 *         passes the rest again, with more after it, as the start of the
 *         next call.
 */
size_t filter_bcj_code(filter_bcj_state_t * st, unsigned arch,
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size);

/// Delta's history: the last 256 bytes, which is as far back as its distance
/// can reach.
typedef struct {
  uint8_t hist[256];
  uint8_t pos;
} filter_delta_state_t;

void filter_delta_init(filter_delta_state_t * st);

/// Delta in place: each byte less, or plus, the byte @p dist before it.
void filter_delta_code(filter_delta_state_t * st, unsigned dist, int encoding,
    uint8_t * buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_TRANSFORM_H */
