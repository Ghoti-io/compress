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
 * @file xz.h
 *
 * xz method for the Ghoti.io Compress library.
 *
 * The format is the one `xz` and liblzma read and write: a stream of a 12-byte
 * header, blocks, an index of the blocks and a 12-byte footer. Each block is a
 * chain of filters ending in LZMA2 (see filter.h for delta and bcj, which may
 * come before it), padded to four bytes, with a CRC-32, CRC-64 or SHA-256 of
 * its contents. Several streams in a row, with zero padding between, are read
 * as one, as `xz -d` does.
 *
 * `"xz"` needs `"lzma2"`, `"delta"` and `"bcj"` registered in the same
 * registry; the default registry has them.
 *
 * See documentation/modules/xz.md.
 */

#ifndef GHOTI_IO_GCOMP_XZ_H
#define GHOTI_IO_GCOMP_XZ_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most an xz stream can expand, output over input.
 *
 * An xz stream's bytes are mostly LZMA2's, and LZMA2 cannot exceed
 * ::GCOMP_LZMA_MAX_EXPANSION_RATIO (8192 to one); the container only adds to
 * the input, so the same figure is a ceiling here.
 */
#define GCOMP_XZ_MAX_EXPANSION_RATIO 8192ULL

/**
 * @brief Register the xz method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_xz_register(gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_XZ_H */
