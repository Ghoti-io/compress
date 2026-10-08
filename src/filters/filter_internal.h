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
 * @file filter_internal.h
 *
 * What the filters share with the methods that chain them: the names of the
 * architectures a branch converter knows, and their numbers.
 */

#ifndef GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The number of an architecture by name; zero when there is none.
int filter_arch_id(const char * name, unsigned * id_out);

/// The name of an architecture by number; NULL when there is none.
const char * filter_arch_name(unsigned id);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_SRC_FILTERS_FILTER_INTERNAL_H */
