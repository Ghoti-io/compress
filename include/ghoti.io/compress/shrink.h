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
 * @file shrink.h
 *
 * Shrink method for the Ghoti.io Compress library.
 *
 * Shrinking is ZIP method 1: Lempel-Ziv-Welch with codes of 9 to 13 bits,
 * where the compressor says when the code size grows (the pair 256, 1) and
 * when the table frees its leaves (256, 2), as PKWARE's APPNOTE section 5.1
 * describes. A raw stream: the container supplies the framing and the check,
 * and there is no end marker, so a stream ends where its input does.
 *
 * Options: only the `limits.*` options. See documentation/modules/shrink.md.
 */

#ifndef GHOTI_IO_GCOMP_SHRINK_H
#define GHOTI_IO_GCOMP_SHRINK_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most a shrink stream can expand, output over input.
 *
 * A code names a string of at most 8192 bytes and takes at least nine bits:
 * 8192 * 8 / 9 = 7281.8.
 */
#define GCOMP_SHRINK_MAX_EXPANSION_RATIO 7282ULL

/**
 * @brief Register the shrink method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_shrink_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SHRINK_H
