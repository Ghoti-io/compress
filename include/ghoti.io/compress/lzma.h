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
 * @file lzma.h
 *
 * LZMA and LZMA2 methods for the Ghoti.io Compress library.
 *
 * `"lzma"` is the `.lzma` format ("LZMA-alone"): a 13-byte header holding the
 * properties byte, the dictionary size and the uncompressed size, then one
 * range-coded stream. With `lzma.raw` the header is left out and the caller
 * supplies what it would have said, which is how zip method 14 and 7z carry
 * LZMA. `"lzma2"` is the chunked framing xz and 7z use, with no header and a
 * single zero byte at the end.
 *
 * See documentation/modules/lzma.md.
 */

#ifndef GHOTI_IO_GCOMP_LZMA_H
#define GHOTI_IO_GCOMP_LZMA_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The most a stream can expand: the output over the input of the cheapest
 * symbol, a rep0 match of 273 bytes. It costs fourteen decisions, and a
 * probability stops at 31/2048 or 2017/2048, so each costs at least 0.0220
 * bits; the coder's truncation adds about one percent. 273 bytes for 0.31
 * bits is 7020:1, so 8192 is a ceiling the format cannot reach.
 */
#define GCOMP_LZMA_MAX_EXPANSION_RATIO 8192ULL

/**
 * @brief Register the `lzma` method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_lzma_register(
    gcomp_registry_t * registry);

/**
 * @brief Register the `lzma2` method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_lzma2_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_LZMA_H */
