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
 * @file zstd_parallel.c
 *
 * Zstd-specific parallel block compression implementation.
 *
 * This module provides parallel block compression for Zstd by integrating
 * the generic thread pool and job queue infrastructure with Zstd-specific
 * compression logic.
 *
 * ## Architecture
 *
 * The parallel compression system has three layers:
 *
 * ```
 * ┌─────────────────────────────────────────────────────────────┐
 * │                    zstd_parallel_ctx_t                      │
 * │        (Zstd-specific parallel compression context)         │
 * └───────────────────────────┬─────────────────────────────────┘
 *                             │
 *           ┌─────────────────┼─────────────────┐
 *           │                 │                 │
 *           v                 v                 v
 * ┌─────────────────┐ ┌───────────────┐ ┌────────────────────┐
 * │ gcomp_thread_   │ │ gcomp_job_    │ │ Per-Job Resources  │
 * │ pool_t          │ │ queue_t       │ │ (match finders,    │
 * │ (worker threads)│ │ (ordering)    │ │  output buffers)   │
 * └─────────────────┘ └───────────────┘ └────────────────────┘
 * ```
 *
 * ## Modes of Operation
 *
 * ### Inline Mode (is_inline = true)
 *
 * When `threads.count <= 1`, compression runs in the calling thread without
 * any threading overhead:
 *
 * - `submit()` compresses the block immediately and returns
 * - `get_result()` returns the already-completed job
 * - No thread pool or job queue is created
 * - Single match finder and output buffer are reused
 *
 * ### Threaded Mode (is_inline = false)
 *
 * When `threads.count > 1`:
 *
 * - Worker threads compress blocks in parallel
 * - Each job produces a complete zstd frame
 * - Job queue ensures results are returned in submission order
 * - Each in-flight job has its own match finder and output buffer
 * - Output is valid concatenation of frames
 *
 * ## Memory Management
 *
 * Memory is tracked to enforce `limits.max_memory_bytes`:
 *
 * | Resource | Per-Job | Total |
 * |----------|---------|-------|
 * | Context struct | - | 1 |
 * | Thread pool | - | 1 |
 * | Job queue | - | 1 |
 * | Match finders | max_in_flight | hash + chain tables |
 * | Output buffers | max_in_flight | job_size + frame overhead |
 *
 * In inline mode, only one match finder and output buffer are allocated.
 *
 * ## Frame Output
 *
 * Each job produces a complete zstd frame:
 * - Magic number (4 bytes)
 * - Frame header (variable)
 * - One or more blocks
 * - Content checksum (4 bytes, if enabled)
 *
 * The concatenation of all job outputs forms a valid multi-frame zstd stream.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "zstd_parallel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zstd_internal.h"

#include "../../core/alloc_internal.h"
#include "../../core/parallel_block.h"

//
// Constants
//

// Default max in-flight jobs based on thread count
#define ZSTD_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER 2

// Frame overhead (magic + max header + checksum)
#define ZSTD_FRAME_OVERHEAD                                                    \
  (4 + ZSTD_HEADER_MAX_SIZE + 4 + ZSTD_BLOCK_HEADER_SIZE)

//
// Internal Types
//

/**
 * @brief Zstd parallel context structure.
 */
struct zstd_parallel_ctx_s {
  const gcomp_allocator_t * allocator;
  gcomp_parallel_block_ctx_t * block_ctx;

  uint64_t job_size;
  uint32_t overlap_size;
  bool checksum_enabled;
  int compression_level;
  uint8_t window_log;
  uint64_t max_memory_bytes;
  bool ldm_enabled;
  unsigned ldm_min_match;
  unsigned ldm_hash_log;
  unsigned ldm_hash_rate_log;
  gcomp_memory_tracker_t * mem_tracker;
};

//
// Helper: Compress a job to a complete zstd frame
//

/**
 * @brief Compress one job's input into a run of zstd blocks.
 *
 * A job is a piece of ONE frame, not a frame of its own.  It emits blocks and
 * nothing else: the frame header, the block that ends the frame, and the
 * checksum over the whole content all belong to the encoder, which is the only
 * party that sees the jobs in order and knows where the content stops.
 *
 * This used to write a complete frame per job, so compressing with
 * threads.count > 1 produced a multi-frame stream.  That is legal -- RFC 8878
 * section 3.1 defines the content of concatenated frames as the concatenation
 * of their contents -- but it costs ratio: a frame boundary throws away the
 * repeat offsets and forces a fresh header, and it made our output differ in
 * shape from every other zstd encoder for no benefit.
 *
 * Every block here is written with Last_Block clear.  The encoder appends an
 * empty raw block with the flag set once the input really has ended, which is
 * also how it terminates a frame whose content size it never knew.
 *
 * Jobs stay independent: a job's matches never reach behind its own first
 * byte, so any declared window at least one job wide is honest, and the
 * decoder needs no knowledge of the split.
 *
 * @param job The job containing input data and configuration.
 * @param allocator Allocator for temporary buffers.
 * @return GCOMP_OK on success, error code on failure.
 */
static gcomp_status_t zstd_parallel_compress_blocks(
    zstd_parallel_job_t * job, const gcomp_allocator_t * allocator) {
  const uint8_t * input = job->base.input;
  size_t input_len = job->base.input_size;
  uint8_t * output = job->base.output;
  size_t output_cap = job->base.output_capacity;

  gcomp_status_t status = GCOMP_OK;
  size_t pos = 0;

  // RFC 8878 section 3.1.1.2.4: Block_Maximum_Size is the smaller of
  // Window_Size and 128 KB, and the window the frame header declares is the
  // one every block in it has to fit.
  size_t block_max = ZSTD_BLOCK_SIZE_MAX;
  {
    uint64_t window_size = zstd_window_log_to_size(job->window_log);
    if (window_size < (uint64_t)block_max) {
      block_max = (size_t)window_size;
    }
  }

  // Create a temporary encoder state for block compression
  // We need match finder and sequence buffers from the job
  zstd_encoder_state_t temp_state = {
      .allocator = allocator,
      .match_finder = (zstd_match_finder_t *)job->match_finder,
      .seq_buffer = (zstd_sequence_t *)job->seq_buffer,
      .seq_buffer_capacity = job->seq_buffer_capacity / sizeof(zstd_sequence_t),
      .literals_buffer = job->literals_buffer,
      .literals_buffer_capacity = job->literals_buffer_capacity,
      // Deliberately zero, not the frame-start values.
      //
      // RFC 8878 section 3.1.1.3.2.1.1 starts a FRAME's offset history at
      // 1, 4, 8 and carries it from one block to the next.  Jobs are blocks
      // of one shared frame now, so the decoder reaching this job's first
      // block holds whatever the previous job left -- which this job cannot
      // know, because they are compressed at the same time.  Starting from
      // 1, 4, 8 here made the encoder name distances the decoder had never
      // seen, and the stream decoded to the wrong bytes while remaining
      // perfectly well-formed.
      //
      // Zero is not a distance, so both parses decline every repeat code
      // while the history holds one (they already skip a zero candidate) and
      // zstd_opt_encode_offset falls through to writing the distance in
      // full.  The history then fills with distances this job has itself
      // emitted, and those the decoder does have, in the same places: after
      // an explicit distance X ours is {X,0,0} and the decoder's is {X,...},
      // and every later rotation moves both the same way.  So a non-zero
      // entry here always equals the decoder's entry at that index, and a
      // zero one is never chosen -- which is the whole invariant.
      //
      // The cost is a handful of full distances at the start of each job,
      // before the history has filled.
      .rep_offset_1 = 0u,
      .rep_offset_2 = 0u,
      .rep_offset_3 = 0u,
  };

  // Give the job the same sliding window the single-threaded encoder uses:
  // the block compressor's stream path appends each block to it, slides it
  // when it outgrows one window, and keeps the finder's tables describing
  // exactly what is in it.  That makes every block of a job history for the
  // next one, which per-block calls on their own could never do.
  temp_state.mf_window = job->mf_window;
  temp_state.mf_window_capacity = job->mf_window_capacity;
  temp_state.mf_window_max = job->mf_window_max;
  temp_state.mf_window_len = 0;
  temp_state.mf_window_start = 0;

  // The finder is reused between jobs, so it starts clean.  Then the overlap
  // -- the tail of the job before this one -- goes in as history and is
  // indexed, which is what stops each job starting cold.
  if (temp_state.match_finder) {
    zstd_mf_reset(temp_state.match_finder);
    uint32_t ov = job->overlap_len;
    if (ov > job->mf_window_max) {
      ov = job->mf_window_max; // never more history than a window
    }
    if (ov > 0u) {
      memcpy(temp_state.mf_window,
          input + job->overlap_len - ov, ov);
      temp_state.mf_window_len = ov;
      zstd_mf_index_range(temp_state.match_finder, temp_state.mf_window, 0, ov, ov);
      // And into the long-distance table, which zstd_mf_index_range() does not
      // touch. Without this a job's long scan starts from an empty table and
      // can only match inside the job's own content, so zstd.long with threads
      // was on and found nothing.
      status = zstd_mf_index_ldm_range(
          temp_state.match_finder, temp_state.mf_window, 0, ov, ov);
      if (status != GCOMP_OK) {
        return status;
      }
    }
  }

  // Compress the content, which begins after the overlap.
  size_t remaining = input_len - job->overlap_len;
  const uint8_t * inp = input + job->overlap_len;

  while (remaining > 0) {
    size_t block_input_len = (remaining > block_max) ? block_max : remaining;
    // Never the last block of the frame: only the encoder knows where the
    // content ends, and it marks that with a block of its own.
    const bool is_last = false;

    // The repeat offsets are NOT reset here, and resetting them was a bug
    // that corrupted every job longer than one block.  RFC 8878 section
    // 3.1.1.3.2.1.1 resets the three at the start of a FRAME; within one,
    // every block continues from where the last left off, and a decoder
    // does exactly that.  Starting each block from 1, 4 and 8 meant the
    // encoder wrote code 1 meaning one distance while the decoder read it
    // as another, from the second block of each job onwards.

    // Compress block
    uint8_t block_type;
    size_t compressed_len;
    status = zstd_block_compress(&temp_state, inp, block_input_len,
        output + pos + ZSTD_BLOCK_HEADER_SIZE,
        output_cap - pos - ZSTD_BLOCK_HEADER_SIZE, &compressed_len,
        &block_type);
    if (status != GCOMP_OK) {
      return status;
    }

    // Write block header
    // For RLE blocks, the header size field is the regenerated size
    uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
        ? (uint32_t)block_input_len
        : (uint32_t)compressed_len;
    zstd_write_block_header(output + pos, is_last, block_type, header_size);
    pos += ZSTD_BLOCK_HEADER_SIZE + compressed_len;

    inp += block_input_len;
    remaining -= block_input_len;
  }

  // No checksum here: it covers the frame's whole content, which no single
  // job has.  The encoder hashes the input as it hands it over and writes the
  // result once, after the last block.
  job->base.output_size = pos;
  return GCOMP_OK;
}

//
// Job Processing Function
//

/**
 * @brief Process a frame compression job.
 *
 * Called by thread pool workers to compress a single input into a frame.
 */
static int zstd_parallel_process_job(void * ctx) {
  zstd_parallel_job_t * job = (zstd_parallel_job_t *)ctx;

  // Get allocator from match finder (stored in the context that created job)
  // For now, use default allocator in worker threads
  const gcomp_allocator_t * allocator = gcomp_allocator_default();

  return zstd_parallel_compress_blocks(job, allocator);
}

//
// Public API
//

gcomp_status_t zstd_parallel_create(
    const zstd_parallel_config_t * config, zstd_parallel_ctx_t ** ctx_out) {
  if (!ctx_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *ctx_out = NULL;

  const gcomp_allocator_t * allocator = config && config->allocator
      ? config->allocator
      : gcomp_allocator_default();

  uint32_t num_threads = config ? config->num_threads : 0;

  // The automatic job size follows the window, and that is the whole of what
  // decides whether threads help.
  //
  // Every job is seeded with a window of the preceding stream and indexes it
  // into the hash, chain and long-distance tables before compressing its own
  // job_size bytes (see the overlap block in zstd_parallel_compress_blocks).
  // The job before it indexed the same bytes, so the wasted work per useful byte
  // is window_size / job_size - and a fixed 512 KiB job made that ratio the
  // window size in disguise.  Four threads against one, 64 MiB of highly
  // compressible input, fixed job against scaled:
  //
  //     window_log 17 (128 KiB)    0.58x -> 0.74x
  //     window_log 19 (512 KiB)    0.29x -> 0.74x
  //     window_log 21 (2 MiB)      0.19x -> 0.81x
  //     window_log 25 (32 MiB)     0.09x -> 0.80x
  //
  // **Four windows, and the multiplier was measured rather than guessed**: it is
  // a trade that runs in opposite directions for the two input shapes, so no
  // value wins everywhere.  Against eight windows:
  //
  //                         compressible        prose-like
  //     window_log 17     4x 0.68  8x 0.98    4x 2.91  8x 2.78
  //     window_log 19     4x 0.74  8x 0.98    4x 2.60  8x 2.31
  //     window_log 21     4x 0.95  8x 1.03    4x 1.90  8x 1.56
  //     window_log 23     4x 0.70  8x 0.70    4x 1.06  8x 0.98
  //
  // Eight buys parity on input that compresses so fast there is nothing left to
  // parallelise; four keeps up to 18% more of the speedup on the shape callers
  // actually have.  Both remove the pathology equally - worst case 0.70x either
  // way, against 0.09x - so the tie-break is the realistic shape.
  //
  // notes/compress/PERFORMANCE.md has the sweep, the shapes and the machine load.
  // Two things this does not fix: input below about two jobs cannot be split at
  // all, and above window_log 21 the clamp bites so the ratio cannot come all the
  // way down.  Larger jobs also compress very slightly BETTER, there being fewer
  // job seams: 14,479 bytes became 13,859 on one 64 MiB case.
  uint32_t effective_window_log =
      (config && config->window_log) ? (uint32_t)config->window_log : 0u;
  if (effective_window_log == 0u) {
    effective_window_log = (uint32_t)zstd_level_to_window_log(
        config ? config->compression_level : ZSTD_LEVEL_DEFAULT);
  }
  if (effective_window_log > 31u) {
    effective_window_log = 31u;
  }
  uint64_t job_size = ZSTD_DEFAULT_JOB_SIZE;
  if (config && config->job_size > 0) {
    job_size = config->job_size;
  }
  else {
    // A caller who names no job size gets one scaled to the window, never
    // smaller than the figure this defaulted to before.
    const uint64_t window_bytes = (uint64_t)1u << effective_window_log;
    if (window_bytes <= (UINT64_MAX / 4u)) {
      const uint64_t scaled = window_bytes * 4u;
      if (scaled > job_size) {
        job_size = scaled;
      }
    }
    else {
      job_size = ZSTD_MAX_JOB_SIZE;
    }
  }
  if (job_size < ZSTD_MIN_JOB_SIZE) {
    job_size = ZSTD_MIN_JOB_SIZE;
  }
  if (job_size > ZSTD_MAX_JOB_SIZE) {
    job_size = ZSTD_MAX_JOB_SIZE;
  }

  zstd_parallel_ctx_t * ctx =
      gcomp_calloc(allocator, 1, sizeof(zstd_parallel_ctx_t));
  if (!ctx) {
    return GCOMP_ERR_MEMORY;
  }

  ctx->allocator = allocator;
  ctx->job_size = job_size;
  ctx->checksum_enabled = config ? config->checksum_enabled : false;
  ctx->compression_level =
      config ? config->compression_level : ZSTD_LEVEL_DEFAULT;
  ctx->window_log = config ? config->window_log : 0;
  ctx->ldm_enabled = config ? config->ldm_enabled : false;
  ctx->ldm_min_match = config ? config->ldm_min_match : 0u;
  ctx->ldm_hash_log = config ? config->ldm_hash_log : 0u;
  ctx->ldm_hash_rate_log = config ? config->ldm_hash_rate_log : 0u;
  ctx->max_memory_bytes = config ? config->max_memory_bytes : 0;
  ctx->mem_tracker = config && config->mem_tracker ? config->mem_tracker : NULL;

  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_parallel_ctx_t));
  }

  if (ctx->window_log == 0) {
    ctx->window_log = zstd_level_to_window_log(ctx->compression_level);
  }

  // How much of the previous job each job gets to look back into.
  //
  // A job with no context starts cold: its first bytes have nothing to match
  // and its entropy tables are built from a short sample.  Over 2 MB of
  // synthetic input that cost 2.4x the output of the single-threaded encoder,
  // while the reference encoder pays nothing for threading -- it gives each
  // job a slice of what came before, and so does this now.
  //
  // A whole window, and NOT capped by the job size: the encoder keeps the
  // overlap in a buffer of its own, so how much history it can offer has
  // nothing to do with how big a job is.  Capping it at the job size left
  // 64 KB jobs with half a window of context and 2.7% worse output, while
  // every job size at or above the window was already at parity.
  ctx->overlap_size = zstd_window_log_to_size(ctx->window_log);

  uint32_t max_in_flight = 1;
  if (num_threads > 1) {
    max_in_flight = config && config->max_in_flight > 0
        ? config->max_in_flight
        : num_threads * ZSTD_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER;
    if (ctx->max_memory_bytes > 0) {
      uint32_t window_size = zstd_window_log_to_size(ctx->window_log);
      // Each job now carries a copy of the preceding overlap as well.
      size_t overlap_estimate = (size_t)ctx->overlap_size;
      size_t per_job_memory = job_size + overlap_estimate +
          (size_t)window_size +
          zstd_window_slack(window_size, ZSTD_BLOCK_SIZE_MAX) +
          (job_size + ZSTD_FRAME_OVERHEAD) +
          (sizeof(uint32_t) * (1UL << 14)) + (sizeof(uint32_t) * window_size) +
          (job_size / 3 * sizeof(zstd_sequence_t)) + job_size;
      uint32_t mem_limited_jobs =
          (uint32_t)(ctx->max_memory_bytes / per_job_memory);
      if (mem_limited_jobs < max_in_flight) {
        max_in_flight = mem_limited_jobs;
      }
      if (max_in_flight < 1) {
        max_in_flight = 1;
      }
    }
  }

  gcomp_parallel_block_config_t block_config = {
      .allocator = allocator,
      .num_threads = num_threads,
      .max_in_flight = max_in_flight,
  };
  gcomp_status_t status =
      gcomp_parallel_block_create(&block_config, &ctx->block_ctx);
  if (status != GCOMP_OK) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_ctx_t));
    }
    gcomp_free(allocator, ctx);
    return status;
  }

  *ctx_out = ctx;
  return GCOMP_OK;
}

void zstd_parallel_destroy(zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return;
  }
  gcomp_parallel_block_destroy(ctx->block_ctx);
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_ctx_t));
  }
  gcomp_free(ctx->allocator, ctx);
}

gcomp_status_t zstd_parallel_alloc_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;
  gcomp_status_t status = GCOMP_OK;

  // Allocate job structure
  zstd_parallel_job_t * job =
      gcomp_calloc(ctx->allocator, 1, sizeof(zstd_parallel_job_t));
  if (!job) {
    return GCOMP_ERR_MEMORY;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }

  // Allocate input buffer
  const size_t input_cap = (size_t)ctx->job_size + (size_t)ctx->overlap_size;
  uint8_t * input_buf = gcomp_malloc(ctx->allocator, input_cap);
  if (!input_buf) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, input_cap);
  }

  // Allocate output buffer (worst case: input size + frame overhead)
  size_t output_cap = ctx->job_size + ZSTD_FRAME_OVERHEAD +
      (ctx->job_size / ZSTD_BLOCK_SIZE_MAX + 1) * ZSTD_BLOCK_HEADER_SIZE;
  uint8_t * output_buf = gcomp_malloc(ctx->allocator, output_cap);
  if (!output_buf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, output_cap);
  }

  // Allocate match finder
  zstd_match_finder_t * mf =
      gcomp_calloc(ctx->allocator, 1, sizeof(zstd_match_finder_t));
  if (!mf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }

  uint32_t window_size = zstd_window_log_to_size(ctx->window_log);
  status = zstd_mf_init(mf, ctx->allocator, ctx->compression_level, window_size,
      ctx->mem_tracker);
  if (status == GCOMP_OK && ctx->ldm_enabled) {
    // Per job, not shared. A job is seeded with a whole window of the
    // preceding stream and a long match may not reach past the declared window
    // anyway, so a job's own scan has the same reach the single-threaded
    // encoder's does - there is nothing for the jobs to share, and sharing a
    // table they all scan into would need locking for no gain.
    status = zstd_mf_enable_ldm(mf, ctx->allocator, window_size,
        ctx->ldm_min_match, ctx->ldm_hash_log, ctx->ldm_hash_rate_log,
        ctx->mem_tracker);
  }
  if (status != GCOMP_OK) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_match_finder_t));
  }

  // A sliding match window, sized exactly as the single-threaded encoder
  // sizes its own: one window of history plus room for the block being added.
  //
  // Pointing the match finder at the job's input buffer instead looks
  // tempting -- the overlap and the content are already contiguous there --
  // but the finder's tables are built for window_size, so handing it a job
  // several windows long makes its positions alias and it silently loses
  // matches.  Measured on 2 MB: a 512 KB job came out 47% larger than the
  // same data compressed serially, and the loss tracked job size rather than
  // anything about the data.
  uint32_t mf_win_max = zstd_window_log_to_size(ctx->window_log);
  size_t mf_block_max = ZSTD_BLOCK_SIZE_MAX;
  if ((uint64_t)mf_win_max < (uint64_t)mf_block_max) {
    mf_block_max = mf_win_max;
  }
  // Room past one window, so that dropping history inside a job is a pointer
  // move rather than a copy of the whole window.  A job slides exactly as the
  // single-threaded encoder does and paid the same for it: four threads at
  // window_log 25 went from 109 to 400 MiB/s.  zstd_window_slack() has the
  // measurement and the divisor sweep behind it.
  size_t mf_win_cap =
      (size_t)mf_win_max + zstd_window_slack(mf_win_max, mf_block_max);
  uint8_t * mf_window_buf = gcomp_malloc(ctx->allocator, mf_win_cap);
  if (!mf_window_buf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    zstd_mf_destroy(mf, ctx->allocator, ctx->mem_tracker);
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, mf_win_cap);
  }

  // Allocate sequence buffer
  size_t seq_cap = ctx->job_size / 3;
  size_t seq_alloc_size = seq_cap * sizeof(zstd_sequence_t);
  uint8_t * seq_buf = gcomp_malloc(ctx->allocator, seq_alloc_size);
  if (!seq_buf) {
    zstd_mf_destroy(mf, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, seq_alloc_size);
  }

  // Allocate literals buffer
  uint8_t * lit_buf = gcomp_malloc(ctx->allocator, ctx->job_size);
  if (!lit_buf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, seq_alloc_size);
    }
    gcomp_free(ctx->allocator, seq_buf);
    zstd_mf_destroy(mf, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, input_cap);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, (size_t)ctx->job_size);
  }

  // Initialize job
  job->base.input = input_buf;
  job->overlap_len = 0;
  job->mf_window = mf_window_buf;
  job->mf_window_capacity = mf_win_cap;
  job->mf_window_max = mf_win_max;
  job->base.input_size = 0;
  job->base.output = output_buf;
  job->base.output_capacity = output_cap;
  job->base.output_size = 0;
  job->base.user_data = input_buf; // Store for freeing
  job->match_finder = mf;
  job->seq_buffer = seq_buf;
  job->seq_buffer_capacity = seq_cap * sizeof(zstd_sequence_t);
  job->literals_buffer = lit_buf;
  job->literals_buffer_capacity = ctx->job_size;
  job->checksum_enabled = ctx->checksum_enabled;
  job->compression_level = ctx->compression_level;
  job->window_log = ctx->window_log;
  job->next_inline = NULL;

  *job_out = job;
  return GCOMP_OK;

cleanup:
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }
  gcomp_free(ctx->allocator, job);
  return status;
}

void zstd_parallel_free_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job) {
  if (!ctx || !job) {
    return;
  }

  if (ctx->mem_tracker) {
    if (job->base.user_data) {
      gcomp_memory_track_free(
          ctx->mem_tracker, (size_t)ctx->job_size + (size_t)ctx->overlap_size);
    }
    if (job->base.output) {
      gcomp_memory_track_free(ctx->mem_tracker, job->base.output_capacity);
    }
  }

  if (job->mf_window) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, job->mf_window_capacity);
    }
    gcomp_free(ctx->allocator, job->mf_window);
    job->mf_window = NULL;
  }

  // Free buffers (input pointer stored in user_data)
  if (job->base.user_data) {
    gcomp_free(ctx->allocator, job->base.user_data);
  }
  if (job->base.output) {
    gcomp_free(ctx->allocator, job->base.output);
  }
  if (job->match_finder) {
    zstd_mf_destroy(job->match_finder, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
    }
    gcomp_free(ctx->allocator, job->match_finder);
  }
  if (job->seq_buffer) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, job->seq_buffer_capacity);
    }
    gcomp_free(ctx->allocator, job->seq_buffer);
  }
  if (job->literals_buffer) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, job->literals_buffer_capacity);
    }
    gcomp_free(ctx->allocator, job->literals_buffer);
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }
  gcomp_free(ctx->allocator, job);
}

gcomp_status_t zstd_parallel_try_submit(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job) {
  if (!ctx || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // A job is its own frame, so it starts from nothing: no positions, and no
  // statistics carried over from whatever the finder last looked at.  Doing
  // it before the try means a refused submit has still reset it, which is
  // harmless -- nothing has looked at it in between and the caller tries
  // again with the same job.
  zstd_mf_reset(job->match_finder);
  return gcomp_parallel_block_try_submit(
      ctx->block_ctx, job, zstd_parallel_process_job);
}

gcomp_status_t zstd_parallel_get_result(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *job_out = NULL;
  gcomp_block_job_t * base = NULL;
  gcomp_status_t status =
      gcomp_parallel_block_get_result(ctx->block_ctx, &base);
  if (!base) {
    return status;
  }
  *job_out = (zstd_parallel_job_t *)base;
  return (*job_out)->base.result;
}

bool zstd_parallel_result_ready(const zstd_parallel_ctx_t * ctx) {
  return ctx && gcomp_parallel_block_result_ready(ctx->block_ctx);
}

gcomp_status_t zstd_parallel_wait(zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_wait(ctx->block_ctx);
}

bool zstd_parallel_is_inline(const zstd_parallel_ctx_t * ctx) {
  return !ctx || gcomp_parallel_block_is_inline(ctx->block_ctx);
}

uint32_t zstd_parallel_pending_count(const zstd_parallel_ctx_t * ctx) {
  return ctx ? gcomp_parallel_block_pending_count(ctx->block_ctx) : 0;
}

gcomp_status_t zstd_parallel_reset(zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_reset(ctx->block_ctx);
}

uint32_t zstd_parallel_get_overlap_size(const zstd_parallel_ctx_t * ctx) {
  return ctx ? ctx->overlap_size : 0u;
}

uint64_t zstd_parallel_get_job_size(const zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return ZSTD_DEFAULT_JOB_SIZE;
  }
  return ctx->job_size;
}
