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
 * @file bzip2.h
 *
 * bzip2 method for the Ghoti.io Compress library.
 *
 * The format is the one `bzip2` and libbz2 read and write: `BZh` and a level
 * digit, then blocks of up to 900000 bytes, each run-length coded, sorted with
 * the Burrows-Wheeler transform, move-to-front coded and Huffman coded, each
 * with a CRC-32 of its contents, and a stream end that carries a CRC of the
 * whole. Concatenated streams are read as one, as `bzip2 -d` does.
 *
 * See documentation/modules/bzip2.md.
 */

#ifndef GHOTI_IO_GCOMP_BZIP2_H
#define GHOTI_IO_GCOMP_BZIP2_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most a bzip2 stream can expand, output over input.
 *
 * A block holds at most 899981 bytes after the first run-length pass, and a
 * pass turns five bytes (four equal and a count of 255) into 259, so a block
 * can decode to at most 179996 * 259 = 46,618,964 bytes plus a few. The
 * smallest block is its 48-bit magic, 32-bit CRC, randomisation bit and 24-bit
 * origin pointer, a 16-bit symbol map, the group count, selector count and at
 * least one selector, and two code tables, which is over 165 bits: 21 bytes.
 * 46,618,964 over 21 is 2.22 million to one, so 2,500,000 is a ceiling the
 * format cannot reach. The best a real encoder does on zeros is about 1.3
 * million to one.
 */
#define GCOMP_BZIP2_MAX_EXPANSION_RATIO 2500000ULL

/**
 * @brief Register the bzip2 method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_bzip2_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_BZIP2_H */
