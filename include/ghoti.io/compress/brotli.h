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
 * @file brotli.h
 *
 * Brotli (RFC 7932) method for the Ghoti.io Compress library.
 *
 * The decoder reads the format in full. The encoder writes the trivial
 * compressor from section 11.1: uncompressed meta-blocks, byte aligned by
 * an empty metadata block. `brotli.lgwin` is the window the encoder puts in
 * the stream header (10..24, default 16). The decoder takes the window from
 * the stream.
 *
 * See documentation/modules/brotli.md.
 */

#ifndef GHOTI_IO_GCOMP_BROTLI_H
#define GHOTI_IO_GCOMP_BROTLI_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Largest expansion a brotli stream can force, output over input.
 *
 * A last compressed meta-block whose prefix codes are single 0-bit symbols
 * can carry one insert-and-copy command with insert code 23. That code is
 * 24 extra bits and fills MLEN, which is at most 16777216, and the copy is
 * then ignored so no distance is read. The window, the meta-block header and
 * that command are 102 bits, 13 bytes. 1290555 * 13 is 16777215, which would
 * refuse this stream; 1290556 * 13 is 16777228, which accepts it.
 */
#define GCOMP_BROTLI_MAX_EXPANSION_RATIO 1290556ULL

/**
 * @brief Register the brotli method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_brotli_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_BROTLI_H */
