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
 * @file deflate64.h
 *
 * Deflate64 method for the Ghoti.io Compress library.
 *
 * Deflate64 ("enhanced deflate", ZIP method 9, a 7z coder) is DEFLATE
 * (RFC 1951) with three changes, which PKWARE's APPNOTE section 4.4.5 gives:
 * the window is 64 KiB, so a distance reaches 65536 back and distance codes 30
 * and 31 exist, with fourteen extra bits each; and length code 285 means a
 * length of 3 plus sixteen extra bits, so a match may be as long as 65538. A
 * raw stream, like `"deflate"`: the container supplies the framing and the
 * check.
 *
 * Options: `deflate64.level` (int64, 0 to 9, default 6; 0 stores), and the
 * `limits.*` options. See documentation/modules/deflate64.md.
 */

#ifndef GHOTI_IO_GCOMP_DEFLATE64_H
#define GHOTI_IO_GCOMP_DEFLATE64_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most a deflate64 stream can expand, output over input.
 *
 * The longest match, 65538 bytes, is length code 285 with sixteen extra bits;
 * with a one-bit length code and a one-bit distance code (distance 1, no extra
 * bits) that is 18 bits for 65538 bytes, 65538 * 8 / 18 = 29127.3. No shorter
 * length does better, so a `limits.max_expansion_ratio` at this value cannot
 * refuse a stream the format allows.
 */
#define GCOMP_DEFLATE64_MAX_EXPANSION_RATIO 29128ULL

/**
 * @brief Register the deflate64 method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_deflate64_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_DEFLATE64_H
