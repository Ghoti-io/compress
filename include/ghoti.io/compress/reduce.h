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
 * @file reduce.h
 *
 * Reduce method for the Ghoti.io Compress library.
 *
 * Reducing is ZIP methods 2 to 5: a byte-repeat scheme (a marker byte, then a
 * length and a distance packed into two or three bytes) whose output is coded
 * with "follower sets", up to 32 bytes that may come after each byte, so that
 * a likely byte costs a flag bit and a few bits of index. The method number is
 * the compression factor plus one, and the factor decides how the length and
 * the distance share a byte. PKWARE's APPNOTE section 5.2 describes it. A raw
 * stream: the container supplies the framing and the check, and there is no
 * end marker, so the size the member has when unpacked is an option.
 *
 * Options: `reduce.factor` (1 to 4, default 4), `reduce.uncompressed_size`
 * (where the decoder stops; 0 for unknown, which decodes to the end of the
 * input), and the `limits.*` options. See documentation/modules/reduce.md.
 */

#ifndef GHOTI_IO_GCOMP_REDUCE_H
#define GHOTI_IO_GCOMP_REDUCE_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most a reduce stream can expand, output over input.
 *
 * A repeat is a marker, a byte of length and distance, an extra length byte
 * and a distance byte, each at least one bit when it is a likely follower:
 * four bits for up to 385 + 3 bytes at factor 1, 388 * 8 / 4 = 776. The
 * figure is rounded up generously.
 */
#define GCOMP_REDUCE_MAX_EXPANSION_RATIO 1024ULL

/**
 * @brief Register the reduce method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_reduce_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_REDUCE_H
