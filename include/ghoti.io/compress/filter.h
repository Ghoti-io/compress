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
 * @file filter.h
 *
 * The `delta` and `bcj` filters for the Ghoti.io Compress library.
 *
 * A filter is a method that does not compress. It rewrites data so that
 * whatever compresses it next does better, and it can be undone exactly. Each
 * is the same size in and out and has no header, so the decoder has to be
 * told what the encoder was: the filter and its options are part of the
 * format of whatever carries the stream, which is how xz and 7z hold them.
 *
 * `"delta"` replaces each byte with its difference from the byte
 * `delta.distance` before it. `"bcj"` rewrites the relative targets of
 * branch and call instructions as absolute ones, for the instruction set in
 * `bcj.arch`, so that the same function called from many places is the same
 * bytes each time. A filter chain is built by feeding one into the next; xz
 * does it for the filters named in `xz.filters`.
 *
 * See documentation/modules/filters.md.
 */

#ifndef GHOTI_IO_GCOMP_FILTER_H
#define GHOTI_IO_GCOMP_FILTER_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the `delta` method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_delta_register(
    gcomp_registry_t * registry);

/**
 * @brief Register the `bcj` method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_bcj_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_FILTER_H */
