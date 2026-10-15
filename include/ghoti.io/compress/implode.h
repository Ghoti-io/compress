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
 * @file implode.h
 *
 * Implode method for the Ghoti.io Compress library.
 *
 * Imploding is ZIP method 6: a sliding dictionary of 4 or 8 KiB, with the
 * literals, the match lengths and the high bits of the match distances coded
 * by Shannon-Fano trees stored at the start of the stream, as PKWARE's APPNOTE
 * section 5.3 describes. A raw stream: the container supplies the framing and
 * the check, and says (in two bits of the member's general purpose flags) how
 * big the dictionary is and whether there is a literal tree; those, and the
 * size the member has when unpacked, are options here. There is no end marker.
 *
 * Options: `implode.dictionary_size` (4096 or 8192, default 8192),
 * `implode.literal_tree` (default true), `implode.uncompressed_size` (where
 * the decoder stops; 0 for unknown, which decodes to the end of the input), and
 * the `limits.*` options. See documentation/modules/implode.md.
 */

#ifndef GHOTI_IO_GCOMP_IMPLODE_H
#define GHOTI_IO_GCOMP_IMPLODE_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The most an implode stream can expand, output over input.
 *
 * The longest match is 321 bytes (63 plus the minimum of 3 plus 255), and
 * costs at least a flag bit, six bits of distance, a bit each for the distance
 * and length codes and the eight bits of extra length: 17 bits. 321 * 8 / 17 is
 * 151.1.
 */
#define GCOMP_IMPLODE_MAX_EXPANSION_RATIO 152ULL

/**
 * @brief Register the implode method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_implode_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_IMPLODE_H
