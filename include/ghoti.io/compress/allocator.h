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
 * @file allocator.h
 *
 * Allocator abstraction for the Ghoti.io Compress library.
 *
 * This is cutil's `GCU_Allocator` under a local name. The two were
 * identical - same four function pointers, same context argument, same
 * semantics - and having one definition means an allocator written for any
 * library in the suite works with all of them, rather than needing a
 * near-identical copy per library.
 *
 * Existing code needs no change: `gcomp_allocator_t` still names the type and
 * gcomp_allocator_default() still returns the stdlib-backed instance.
 */

#ifndef GHOTI_IO_GCOMP_ALLOCATOR_H
#define GHOTI_IO_GCOMP_ALLOCATOR_H

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/compress/macros.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. Each receives the `ctx` pointer
 * from the struct as its first argument.
 *
 * Two requirements beyond the C library equivalents: `calloc_fn` must treat
 * overflow of `nitems * size` as an allocation failure and return NULL rather
 * than allocating a truncated block, and a zero-size request should return a
 * usable non-NULL pointer, so that NULL always means failure.
 */
typedef GCU_Allocator gcomp_allocator_t;

/**
 * @brief Get the default allocator (stdlib-backed).
 *
 * The default allocator treats overflow in calloc(nitems, size) as allocation
 * failure: if nitems * size would overflow, it returns NULL. It never returns
 * NULL for a zero-size request.
 *
 * @return Pointer to a process-global allocator instance.
 */
GCOMP_API const gcomp_allocator_t * gcomp_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ALLOCATOR_H
