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
 * @file zstd_parallel.h
 *
 * Zstd-specific parallel block compression interface.
 *
 * This module provides parallel block compression for Zstd encoding when
 * `threads.count > 1`. It integrates the generic thread pool and job queue
 * with Zstd block compression.
 *
 * ## Architecture
 *
 * When parallel compression is enabled:
 * 1. Input blocks are submitted to the parallel context
 * 2. Worker threads compress blocks in parallel using Zstd block compression
 * 3. Results are retrieved in submission order
 * 4. Output is valid concatenation of frames (each job produces one frame)
 *
 * When disabled (threads.count <= 1):
 * - Compression is performed inline, avoiding threading overhead
 *
 * ## Memory Management
 *
 * Each parallel block requires:
 * - Input buffer (job_size bytes)
 * - Output buffer (worst-case: job_size + frame overhead)
 * - Match finder hash/chain tables
 *
 * Total memory for N in-flight jobs:
 *   N * (job_size * 2 + hash_table_size + chain_table_size)
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_PARALLEL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_PARALLEL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../core/block_job.h"


#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct zstd_parallel_ctx_s zstd_parallel_ctx_t;
typedef struct zstd_parallel_job_s zstd_parallel_job_t;

/**
 * @brief Zstd parallel block job.
 *
 * Extends gcomp_block_job_t with Zstd-specific fields.
 */
struct zstd_parallel_job_s {
  gcomp_block_job_t base;          ///< Base job structure
  void * match_finder;             ///< Per-job match finder context
  uint8_t * seq_buffer;            ///< Sequence buffer for encoding
  size_t seq_buffer_capacity;      ///< Sequence buffer capacity
  uint8_t * literals_buffer;       ///< Literals buffer for encoding
  size_t literals_buffer_capacity; ///< Literals buffer capacity
  bool checksum_enabled;     ///< Whether checksum was requested
  int compression_level;     ///< Compression level for this job
  uint8_t window_log; ///< Window log for this job
  /// Leading bytes of `base.input` that are context from the previous job,
  /// not content of this one.  The job compresses input[overlap_len ..
  /// input_size) and may match back into the bytes before it.
  uint32_t overlap_len;
  uint8_t * mf_window;          ///< Sliding match window for this job
  size_t mf_window_capacity;    ///< window_size + zstd_window_slack()
  uint32_t mf_window_max;       ///< How much history the window may hold
  zstd_parallel_job_t * next_inline; ///< Next job in inline result queue
};

/**
 * @brief Zstd parallel compression configuration.
 */
typedef struct {
  uint32_t num_threads;                ///< Number of worker threads
  uint32_t max_in_flight;              ///< Max in-flight jobs (0 = default)
  uint64_t job_size;                   ///< Job size in bytes (0 = auto)
  bool checksum_enabled;               ///< Compute content checksums
  int compression_level;               ///< Compression level (0-22)
  uint8_t window_log;                  ///< Window log (10-31)
  uint64_t max_memory_bytes;           ///< Memory limit
  /// Long-distance matching, per job.
  ///
  /// Each job is seeded with a whole window of the preceding stream (see
  /// overlap_size in zstd_parallel.c), and a window is as far as a long match
  /// may reach anyway - RFC 8878 section 3.1.1.1.2 - so a job's own scan finds
  /// what the single-threaded encoder finds.
  bool ldm_enabled;
  unsigned ldm_min_match;     ///< 0 for the default.
  unsigned ldm_hash_log;      ///< 0 to size it from the window.
  unsigned ldm_hash_rate_log;
  const gcomp_allocator_t * allocator; ///< Allocator (NULL = default)
  gcomp_memory_tracker_t *
      mem_tracker; ///< Optional: track allocations (NULL = do not track)
} zstd_parallel_config_t;

/**
 * @brief Create a Zstd parallel compression context.
 *
 * If num_threads <= 1, creates an inline context (no actual threading).
 *
 * @param config Configuration.
 * @param ctx_out Pointer to receive context.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_create(
    const zstd_parallel_config_t * config, zstd_parallel_ctx_t ** ctx_out);

/**
 * @brief Destroy a Zstd parallel compression context.
 *
 * Waits for any pending jobs to complete before destroying.
 *
 * @param ctx Context to destroy.
 */
GCOMP_INTERNAL_API void zstd_parallel_destroy(zstd_parallel_ctx_t * ctx);

/**
 * @brief Allocate a job structure for parallel compression.
 *
 * The returned job has pre-allocated buffers for input, output, and
 * match finder tables. Call zstd_parallel_free_job() when done.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive allocated job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_alloc_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out);

/**
 * @brief Free a job structure.
 *
 * @param ctx Parallel context.
 * @param job Job to free.
 */
GCOMP_INTERNAL_API void zstd_parallel_free_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job);

/**
 * @brief Submit a block for compression, if the context has room for it.
 *
 * The job's input buffer must be filled before calling this function.
 * The job remains owned by the caller but must not be modified until
 * retrieved via zstd_parallel_get_result().
 *
 * In parallel mode, each job produces a complete zstd frame. The output
 * of multiple jobs can be concatenated to form valid zstd stream.
 *
 * There is deliberately no blocking form.  The only thing that makes room
 * for another job is collecting a result, and the thread that collects is
 * the thread that submits, so waiting for room here would be waiting for
 * itself -- which is exactly what used to happen: compressing more than
 * max_in_flight * job_size bytes with threads.count > 1 hung the encoder.
 *
 * @param ctx Parallel context.
 * @param job Job to submit (input buffer must be filled).
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT when the context is full and
 *         a result must be collected first, other error codes on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_try_submit(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job);

/**
 * @brief Get the next compressed frame in order.
 *
 * Blocks until the next job in sequence order is complete.
 * The job's output buffer contains the compressed frame.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive completed job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_get_result(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out);

/**
 * @brief Check if a result is available without blocking.
 *
 * @param ctx Parallel context.
 * @return true if a result is ready, false otherwise.
 */
GCOMP_INTERNAL_API bool zstd_parallel_result_ready(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Wait for all pending jobs to complete.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, or first error from any job.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_wait(zstd_parallel_ctx_t * ctx);

/**
 * @brief Check if context is in inline mode.
 *
 * @param ctx Parallel context.
 * @return true if inline mode (no threading), false otherwise.
 */
GCOMP_INTERNAL_API bool zstd_parallel_is_inline(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Get number of pending jobs.
 *
 * @param ctx Parallel context.
 * @return Number of jobs submitted but not yet retrieved.
 */
GCOMP_INTERNAL_API uint32_t zstd_parallel_pending_count(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Reset context for reuse.
 *
 * All jobs must be retrieved before calling reset.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_reset(
    zstd_parallel_ctx_t * ctx);

/**
 * @brief Get the job size configured for this context.
 *
 * @param ctx Parallel context.
 * @return Job size in bytes.
 */
/**
 * @brief How many bytes of the previous job each job wants in front of it.
 *
 * Jobs are compressed independently, so without this a job's first bytes have
 * nothing to match against and every job pays a cold start.  The encoder
 * copies this many trailing bytes of the previous job's content to the front
 * of the next job's buffer and sets job->overlap_len to match.
 *
 * @param ctx The parallel context.
 * @return Overlap in bytes (0 if the context is NULL).
 */
GCOMP_INTERNAL_API uint32_t zstd_parallel_get_overlap_size(
    const zstd_parallel_ctx_t * ctx);

GCOMP_INTERNAL_API uint64_t zstd_parallel_get_job_size(
    const zstd_parallel_ctx_t * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_PARALLEL_H
