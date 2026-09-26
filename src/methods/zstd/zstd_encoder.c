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
 * @file zstd_encoder.c
 *
 * Zstandard encoder implementation for the Ghoti.io Compress library.
 *
 * This file implements the streaming Zstd encoder, which produces
 * Zstandard frame format output. The encoder supports both single-threaded
 * and parallel compression modes.
 *
 * ## State Machine (Single-Threaded Mode)
 *
 * The encoder progresses through these stages:
 * 1. HEADER: Write frame header (magic + header descriptor)
 * 2. BLOCKS: Write compressed data blocks
 * 3. CHECKSUM: Write content checksum (if enabled)
 * 4. DONE: Frame complete
 *
 * ## Parallel Mode (threads.count > 1)
 *
 * When parallel mode is enabled via `threads.count > 1`:
 *
 * ```
 * ┌────────────────────────────────────────────────────────────────────┐
 * │                    Parallel Encoder Flow                           │
 * │                                                                    │
 * │  update() called with input:                                       │
 * │  ┌───────────────────────────────────────────────────────────────┐ │
 * │  │ 1. Drain any buffered parallel output                         │ │
 * │  │ 2. Collect any ready parallel results                         │ │
 * │  │ 3. Accumulate input into current job's buffer                 │ │
 * │  │ 4. When job buffer full (job_size), submit to parallel ctx    │ │
 * │  │ 5. Allocate new job for next input                            │ │
 * │  └───────────────────────────────────────────────────────────────┘ │
 * │                                                                    │
 * │  finish() called:                                                  │
 * │  ┌───────────────────────────────────────────────────────────────┐ │
 * │  │ 1. Submit any partial job with remaining input                │ │
 * │  │ 2. Wait for all parallel jobs to complete                     │ │
 * │  │ 3. Collect all remaining results in order                     │ │
 * │  │ 4. Set stage to DONE                                          │ │
 * │  └───────────────────────────────────────────────────────────────┘ │
 * │                                                                    │
 * │  Output: ONE zstd frame; the jobs are its blocks                  │
 * └────────────────────────────────────────────────────────────────────┘
 * ```
 *
 * Key design decisions:
 * - A job produces a run of BLOCKS, not a frame. The encoder writes one frame
 *   header and one final block, so the whole output is a single frame and
 *   decodes without zstd.concat.
 *
 *   This text used to say the opposite - "each parallel job produces a
 *   complete, independent zstd frame", "decode with zstd.concat=true" - and it
 *   was stale. Measured on 8 MB at threads.count 4: the output holds exactly
 *   one frame magic, and a plain decode with no concat option returns all
 *   8,388,608 bytes. zstd_parallel.c's own comment, "Jobs are blocks of one
 *   shared frame now", is the accurate one, and it is the reason the repeat
 *   offsets there start at zero rather than at 1, 4, 8.
 * - Because it is one frame, a job may match back into the job before it. Each
 *   is seeded with a whole window of the preceding stream, which is also why
 *   zstd.long works in parallel mode: a long match may not exceed the declared
 *   window in any case (RFC 8878 section 3.1.1.1.2), so a job's reach is the
 *   single-threaded encoder's reach.
 * - Jobs are collected in submission order (guaranteed ordering)
 * - Single-threaded mode (threads.count <= 1) produces a single frame too
 * - Both consequences of "one frame" land on GCOMP_FLUSH_FULL, which promises
 *   that nothing after it refers to anything before it. A flush already waits
 *   for every worker, so it is the one point where a frame can be closed, and
 *   it closes one: terminator, checksum, fresh header, and the overlap buffer
 *   cleared so the next job starts cold. Until 2026-09-26 it did none of that
 *   and FULL silently behaved as SYNC above one thread.
 *
 * ## Memory Management
 *
 * In parallel mode, the encoder maintains:
 * - `parallel_ctx`: The parallel compression context
 * - `parallel_job`: Current job being filled with input
 * - `parallel_output_buf`: Buffer for collecting compressed output
 *
 * Memory is tracked via `gcomp_memory_tracker_t` and respects
 * `limits.max_memory_bytes`.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "zstd_internal.h"
// gcomp_encode_buffer/gcomp_encode_bound: seekable mode encodes each frame as a
// complete one-shot stream rather than reimplementing frame construction.
#include <ghoti.io/compress/compress.h>
#include "zstd_ldm.h"
#include "zstd_parallel.h"
#include <string.h>

//
// Helper Functions
//

/**
 * @brief Get window log from compression level.
 */
uint8_t zstd_level_to_window_log(int level) {
  // Simple mapping - higher levels get larger windows
  if (level <= 3) {
    return 17; // 128 KB
  }
  else if (level <= 6) {
    return 19; // 512 KB
  }
  else if (level <= 10) {
    return 21; // 2 MB
  }
  else if (level <= 16) {
    return 23; // 8 MB
  }
  else {
    return 25; // 32 MB
  }
}

/**
 * @brief Compute window size from window log.
 */
uint32_t zstd_window_log_to_size(uint8_t window_log) {
  if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
    return 0;
  }
  return 1U << window_log;
}

//
// Parallel Mode Helpers
//

/**
 * @brief Copy a run of staged bytes into the output buffer.
 *
 * The encoder stages whole things -- a frame header, a compressed block, a
 * finished parallel job -- and hands each out across however many calls it
 * takes to fit.  Every one of those hand-offs is a straight copy of a run,
 * and each was written as a loop moving one byte with its own bounds test.
 *
 * @param src Staged bytes.
 * @param pos In/out: how much of them has already gone to the caller.
 * @param len Total staged.
 * @param output Where they go.
 * @return true once the staged bytes have been handed out in full.
 */
static bool zstd_emit_staged(const uint8_t * src, size_t * pos, size_t len,
    gcomp_buffer_t * output) {
  size_t pending = len - *pos;
  if (pending > 0u) {
    size_t space = output->size - output->used;
    size_t n = (pending < space) ? pending : space;
    if (n > 0u) {
      memcpy((uint8_t *)output->data + output->used, src + *pos, n);
      output->used += n;
      *pos += n;
    }
  }
  return *pos >= len;
}

//
// ─────────────────────────────────────────────────────────────────────────────
// Seekable streaming mode (zstd.seekable)
// ─────────────────────────────────────────────────────────────────────────────
//
// gcomp_seekable_write_buffer() needs the whole input at once. This produces
// the identical file from a stream: independent frames of
// zstd.seekable_frame_size decompressed bytes, each declaring that size in its
// own header, then the seek table as a skippable frame.
//
// Input is accumulated to a frame's worth before anything is encoded, because
// a frame cannot declare a size that is not yet known, and that redundancy
// between the headers and the table is the point of the format - it is what
// makes a file indexable when the table is lost or ignored.
//
// The table itself is built and written by src/core/seek_table.c, the same code
// gcomp_seekable_write_buffer() uses, so the two writers cannot drift.
//

/**
 * @brief Hand out whatever is staged in seek_out.
 *
 * @return true once it has all gone.
 */
static bool zstd_seek_drain(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  return zstd_emit_staged(
      state->seek_out, &state->seek_out_pos, state->seek_out_len, output);
}

/**
 * @brief Make sure seek_out can hold @p need bytes, and is empty.
 */
static gcomp_status_t zstd_seek_out_reserve(
    zstd_encoder_state_t * state, size_t need) {
  if (state->seek_out_cap < need) {
    uint8_t * grown =
        gcomp_realloc(state->allocator, state->seek_out, need ? need : 1u);
    if (!grown) {
      return GCOMP_ERR_MEMORY;
    }
    state->seek_out = grown;
    state->seek_out_cap = need;
  }
  state->seek_out_len = 0;
  state->seek_out_pos = 0;
  return GCOMP_OK;
}

/**
 * @brief Encode what is in seek_in as one complete frame, and record it.
 *
 * Staged in seek_out; the caller drains it. Does nothing when there is no
 * input, so a flush with nothing buffered does not write an empty frame -
 * which would be a table entry describing zero bytes and a reader's problem
 * rather than ours.
 */
static gcomp_status_t zstd_seek_emit_frame(
    gcomp_encoder_t * encoder, zstd_encoder_state_t * state) {
  if (state->seek_in_len == 0) {
    return GCOMP_OK;
  }

  // Each frame declares its own decompressed size.
  gcomp_status_t status = gcomp_options_set_uint64(state->seek_frame_options,
      "zstd.content_size", (uint64_t)state->seek_in_len);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "could not set the frame's declared size");
  }

  size_t bound = 0;
  status = gcomp_encode_bound(encoder->registry, "zstd",
      state->seek_frame_options, state->seek_in_len, &bound);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "could not size a seekable frame");
  }
  status = zstd_seek_out_reserve(state, bound);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "could not stage a seekable frame");
  }

  size_t produced = 0;
  status = gcomp_encode_buffer(encoder->registry, "zstd",
      state->seek_frame_options, state->seek_in, state->seek_in_len,
      state->seek_out, state->seek_out_cap, &produced);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "could not encode a seekable frame");
  }

  status = gcomp_seek_table_append(&state->seek_table, (uint64_t)produced,
      (uint64_t)state->seek_in_len, state->seek_in, state->seek_in_len);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, status,
        "could not record a seekable frame in the seek table");
  }

  state->seek_out_len = produced;
  state->seek_out_pos = 0;
  state->seek_in_len = 0;
  return GCOMP_OK;
}

/**
 * @brief update() in seekable mode.
 *
 * Staged output is always handed out before any more is made, because seek_out
 * holds one frame: encoding another before the first has gone would lose it.
 * That is the defect this library has already shipped once - an encoder that
 * dropped staged output - so the order is not an accident.
 */
static gcomp_status_t zstd_seek_update(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  const uint8_t * in = (const uint8_t *)input->data;

  for (;;) {
    if (!zstd_seek_drain(state, output)) {
      return GCOMP_OK; // Output is full; the caller comes back.
    }

    const size_t available = input->size - input->used;
    if (available == 0) {
      return GCOMP_OK;
    }

    size_t room = state->seek_in_cap - state->seek_in_len;
    size_t take = (available < room) ? available : room;
    if (take > 0) {
      memcpy(state->seek_in + state->seek_in_len, in + input->used, take);
      state->seek_in_len += take;
      input->used += take;
      room -= take;
    }

    if (room > 0) {
      return GCOMP_OK; // Input exhausted, frame not yet full.
    }

    const gcomp_status_t status = zstd_seek_emit_frame(encoder, state);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return status;
    }
  }
}

/**
 * @brief flush() in seekable mode: end the frame here.
 *
 * Both flush modes do the same thing, and there is nothing to choose between
 * them: every frame in a seekable file is already independent, so a sync flush
 * cannot keep history across the boundary even if it wanted to. Ending the
 * frame is what makes everything consumed so far decodable, which is what both
 * modes promise.
 *
 * A flush that lands mid-frame therefore costs ratio, exactly as it does in
 * ordinary streaming, and additionally makes a shorter frame - so a caller that
 * flushes every few bytes gets a table with many tiny entries. That is the
 * caller's trade to make; it is documented rather than prevented.
 */
static gcomp_status_t zstd_seek_flush(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  if (!zstd_seek_drain(state, output)) {
    return GCOMP_ERR_LIMIT;
  }
  const gcomp_status_t status = zstd_seek_emit_frame(encoder, state);
  if (status != GCOMP_OK) {
    state->stage = ZSTD_ENC_STAGE_ERROR;
    return status;
  }
  if (!zstd_seek_drain(state, output)) {
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

/**
 * @brief finish() in seekable mode: last frame, then the seek table.
 */
static gcomp_status_t zstd_seek_finish(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  if (!zstd_seek_drain(state, output)) {
    return GCOMP_ERR_LIMIT;
  }

  if (state->seek_in_len > 0) {
    const gcomp_status_t status = zstd_seek_emit_frame(encoder, state);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return status;
    }
    if (!zstd_seek_drain(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  if (!state->seek_table_staged) {
    const uint64_t need = gcomp_seek_table_bytes(&state->seek_table);
    if (need > (uint64_t)SIZE_MAX) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_LIMIT, "the seek table does not fit in memory");
    }
    gcomp_status_t status = zstd_seek_out_reserve(state, (size_t)need);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return gcomp_encoder_set_error(
          encoder, status, "could not stage the seek table");
    }
    size_t written = 0;
    status = gcomp_seek_table_write(
        &state->seek_table, state->seek_out, state->seek_out_cap, &written);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return gcomp_encoder_set_error(
          encoder, status, "could not write the seek table");
    }
    state->seek_out_len = written;
    state->seek_out_pos = 0;
    state->seek_table_staged = true;
  }

  if (!zstd_seek_drain(state, output)) {
    return GCOMP_ERR_LIMIT;
  }
  state->stage = ZSTD_ENC_STAGE_DONE;
  return GCOMP_OK;
}

/// Release what seekable mode allocated. Safe to call twice.
static void zstd_seek_teardown(zstd_encoder_state_t * state) {
  gcomp_seek_table_free(&state->seek_table);
  gcomp_free(state->allocator, state->seek_in);
  state->seek_in = NULL;
  state->seek_in_cap = 0;
  state->seek_in_len = 0;
  gcomp_free(state->allocator, state->seek_out);
  state->seek_out = NULL;
  state->seek_out_cap = 0;
  state->seek_out_len = 0;
  state->seek_out_pos = 0;
  if (state->seek_frame_options) {
    gcomp_options_destroy(state->seek_frame_options);
    state->seek_frame_options = NULL;
  }
  state->seek_table_staged = false;
}

/**
 * @brief Drop the accumulated frames so a reset starts a new file.
 */
static gcomp_status_t zstd_seek_rearm(zstd_encoder_state_t * state) {
  const int checksum = state->seek_table.checksum;
  gcomp_seek_table_free(&state->seek_table);
  state->seek_in_len = 0;
  state->seek_out_len = 0;
  state->seek_out_pos = 0;
  state->seek_table_staged = false;
  return gcomp_seek_table_init(
      &state->seek_table, state->allocator, checksum, 0);
}

/**
 * @brief Drain pending parallel output to the output buffer.
 *
 * Copies data from the internal parallel output buffer to the caller's output.
 *
 * @param state Encoder state
 * @param output Output buffer
 * @return true if all pending output was drained, false if more space needed
 */
static bool zstd_encoder_drain_parallel_output(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  return zstd_emit_staged(state->parallel_output_buf,
      &state->parallel_output_buf_pos, state->parallel_output_buf_len, output);
}

/**
 * @brief Collect completed parallel jobs and copy to output buffer.
 *
 * Checks for completed jobs and copies their output to either the parallel
 * output buffer (for buffering) or directly to the output.
 *
 * @param state Encoder state
 * @param output Output buffer
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t zstd_encoder_take_parallel_result(
    zstd_encoder_state_t * state, gcomp_buffer_t * output,
    zstd_parallel_job_t * completed, bool * buffered_out) {
  uint8_t * out_ptr = (uint8_t *)output->data;
  *buffered_out = false;

  // Check if job had an error
  if (completed->base.status == GCOMP_JOB_ERROR ||
      completed->base.result != GCOMP_OK) {
    gcomp_status_t status = completed->base.result;
    zstd_parallel_free_job(state->parallel_ctx, completed);
    return status;
  }

  // Copy job output to our buffer or directly to output
  size_t output_len = completed->base.output_size;
  const uint8_t * output_data = completed->base.output;

  // First, try to copy directly to output
  size_t direct_copy = output->size - output->used;
  if (direct_copy > output_len) {
    direct_copy = output_len;
  }
  if (direct_copy > 0) {
    memcpy(out_ptr + output->used, output_data, direct_copy);
    output->used += direct_copy;
  }

  // If there's remaining data, buffer it
  size_t remaining = output_len - direct_copy;
  if (remaining > 0) {
    // Ensure buffer has space
    if (remaining > state->parallel_output_buf_cap) {
      // Buffer too small - this shouldn't happen with proper sizing
      zstd_parallel_free_job(state->parallel_ctx, completed);
      return GCOMP_ERR_INTERNAL;
    }
    memcpy(state->parallel_output_buf, output_data + direct_copy, remaining);
    state->parallel_output_buf_pos = 0;
    state->parallel_output_buf_len = remaining;
    *buffered_out = true;
  }

  zstd_parallel_free_job(state->parallel_ctx, completed);
  return GCOMP_OK;
}

/**
 * @brief Collect every result that is ready, without waiting for any.
 */
/**
 * @brief Is a collected job still part-way through being handed out?
 *
 * There is room for exactly one, so this is also the answer to "may another
 * result be collected" -- collecting a second would write over the first.
 */
static bool zstd_encoder_has_staged_output(
    const zstd_encoder_state_t * state) {
  return state->parallel_output_buf_pos < state->parallel_output_buf_len;
}

static gcomp_status_t zstd_encoder_collect_parallel_results(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  // Nothing may be collected while a job's output is still being handed out:
  // the one staging buffer would be overwritten and those bytes would simply
  // vanish from the stream.  The loop below breaks when it stages something,
  // but that is not enough on its own -- a caller can reach here with a job
  // already staged, because zstd_encoder_submit_parallel_job() collects one
  // to make room and then returns to a caller that collects again.
  //
  // This was a live defect, not a hypothetical: two threads, a 1 KB output
  // buffer and 2 MB of input produced a stream that would not decode at all,
  // and its length varied from run to run.  Found while giving LZ4 the same
  // parallel path, where it showed up as a frame 15% short that still
  // decoded, to the wrong content.
  while (!zstd_encoder_has_staged_output(state) &&
      zstd_parallel_result_ready(state->parallel_ctx)) {
    zstd_parallel_job_t * completed = NULL;
    gcomp_status_t status =
        zstd_parallel_get_result(state->parallel_ctx, &completed);
    if (status != GCOMP_OK) {
      return status;
    }
    bool buffered = false;
    status =
        zstd_encoder_take_parallel_result(state, output, completed, &buffered);
    if (status != GCOMP_OK) {
      return status;
    }
    // Only one job's worth of output can be held back at a time, so once
    // something is buffered there is nowhere to put the next result.
    if (buffered) {
      break;
    }
  }

  return GCOMP_OK;
}

/**
 * @brief Collect one result, waiting for it if it is not finished yet.
 *
 * Used when the context has no room for another job: the only thing that
 * frees a slot is taking a result back, so this is what unblocks a submit.
 */
static gcomp_status_t zstd_encoder_collect_one_parallel_result(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  zstd_parallel_job_t * completed = NULL;
  gcomp_status_t status =
      zstd_parallel_get_result(state->parallel_ctx, &completed);
  if (status != GCOMP_OK) {
    return status;
  }
  bool buffered = false;
  return zstd_encoder_take_parallel_result(
      state, output, completed, &buffered);
}

/**
 * @brief Hand the current job to the parallel context.
 *
 * WAITING FOR YOURSELF
 * ====================
 *
 * The context holds at most max_in_flight jobs, and the only thing that
 * frees a slot is collecting a result.  The thread that collects is this
 * one.  So when the context is full, the thing to do is collect -- never
 * wait, because there is nobody else to wait for.
 *
 * Waiting is what this used to do, one level down, and it deadlocked every
 * stream longer than max_in_flight * job_size: four workers idle with
 * nothing left to do, and the encoder asleep waiting for room that only it
 * could make.  At the defaults that was any input over 4 MB compressed with
 * threads.count of 4.
 *
 * Collecting needs somewhere to put the result, and only one job's output
 * can be held back at a time.  If the output buffer is full and something is
 * already held back, there is nothing useful to do here: @p submitted_out is
 * set false and the job stays filled and unsubmitted, to be offered again
 * once the caller has drained.
 *
 * @param state Encoder state.
 * @param output Where collected results go.
 * @param submitted_out Receives whether the job was handed over.
 * @return GCOMP_OK on success, error code on failure.
 */
/**
 * @brief Put the previous job's tail in front of a freshly allocated job.
 *
 * Jobs are compressed independently, so a job with nothing before it starts
 * cold: its first bytes have no history to match against.  Copying the tail of
 * the job just submitted to the front of the next one gives the match finder
 * the same view the single-threaded encoder would have had at that point,
 * without making the jobs depend on each other's RESULTS -- only on their
 * input, which the encoder already holds.
 */
/**
 * @brief Put the dictionary in front of the first job of a frame.
 *
 * A dictionary is history: bytes that came before the content, available to
 * match against and not emitted. That is exactly what the overlap buffer
 * already means to a job - @ref zstd_encoder_seed_parallel_job copies it to the
 * front of the job's input, the worker indexes it into the match finder and the
 * long-distance table, and compresses only what follows it. So a dictionary
 * needs no mechanism of its own here; it needs to be in that buffer when a
 * frame starts.
 *
 * Until 2026-09-26 it was not, and `zstd_parallel.c` had no mention of a
 * dictionary at all. Nothing refused the combination either, so
 * `zstd.dictionary` with `threads.count > 1` was accepted, reported GCOMP_OK,
 * and did nothing: measured on 8,800 bytes whose content the dictionary held,
 * one thread went 61 bytes to 17 and four threads stayed at 64. Worse than the
 * lost ratio, the frame header still declared the Dictionary_ID, so the output
 * hard-required a dictionary it had never used - a decoder without it fails, and
 * a caller had to keep one for no benefit.
 *
 * Only the *content* goes in. A formatted dictionary's entropy tables and
 * repeat offsets are a separate matter, handled per job where the blocks are
 * built; a job that does not use them writes its own tables, which is always
 * valid, so this alone is correct rather than merely better.
 *
 * The tail is what is kept when the dictionary is longer than the window, which
 * is what the single-threaded path does with `mf_window` for the same reason: a
 * match may not reach further back than the window the frame header declares
 * (RFC 8878 section 3.1.1.1.2).
 *
 * @param state Encoder state.
 */
static void zstd_encoder_seed_parallel_dictionary(
    zstd_encoder_state_t * state) {
  state->parallel_overlap_len = 0;
  if (!state->parallel_overlap_buf || !state->dict_parsed.content ||
      state->dict_parsed.content_size == 0) {
    return;
  }
  size_t cap = state->parallel_overlap_cap;
  if (cap > state->mf_window_max) {
    cap = state->mf_window_max;
  }
  if (cap == 0) {
    return;
  }
  size_t take = state->dict_parsed.content_size;
  const uint8_t * from = state->dict_parsed.content;
  if (take > cap) {
    from += take - cap;
    take = cap;
  }
  memcpy(state->parallel_overlap_buf, from, take);
  state->parallel_overlap_len = (uint32_t)take;
}

static void zstd_encoder_seed_parallel_job(zstd_encoder_state_t * state) {
  if (!state->parallel_job) {
    return;
  }
  uint32_t n = state->parallel_overlap_len;
  if (n == 0u || !state->parallel_overlap_buf) {
    state->parallel_job->overlap_len = 0u;
    state->parallel_job->base.input_size = 0u;
    return;
  }
  memcpy((void *)state->parallel_job->base.input, state->parallel_overlap_buf, n);
  state->parallel_job->overlap_len = n;
  state->parallel_job->base.input_size = n;
}

/**
 * @brief Remember the tail of the job about to be handed over.
 *
 * Must run BEFORE the job is submitted: once it is, the context owns it and
 * may free its buffer as soon as the result is taken.
 */
static void zstd_encoder_save_parallel_overlap(zstd_encoder_state_t * state) {
  if (!state->parallel_job || !state->parallel_overlap_buf) {
    return;
  }
  const uint8_t * in = (const uint8_t *)state->parallel_job->base.input;
  size_t total = state->parallel_job->base.input_size;
  uint32_t cap = state->parallel_overlap_cap;
  // The whole buffer is fair game as context, the job's own overlap included:
  // it is simply the data that came before, wherever it came from.
  size_t n = (total < (size_t)cap) ? total : (size_t)cap;
  if (n > 0u) {
    memcpy(state->parallel_overlap_buf, in + total - n, n);
  }
  state->parallel_overlap_len = (uint32_t)n;
}

static gcomp_status_t zstd_encoder_submit_parallel_job(
    zstd_encoder_state_t * state, gcomp_buffer_t * output,
    bool * submitted_out) {
  *submitted_out = false;
  if (!state->parallel_job) {
    return GCOMP_ERR_INTERNAL;
  }

  zstd_encoder_save_parallel_overlap(state);

  for (;;) {
    gcomp_status_t status =
        zstd_parallel_try_submit(state->parallel_ctx, state->parallel_job);
    if (status == GCOMP_OK) {
      break;
    }
    if (status != GCOMP_ERR_LIMIT) {
      return status;
    }
    if (zstd_encoder_has_staged_output(state)) {
      return GCOMP_OK; // Nowhere to put a result; the caller must drain.
    }
    status = zstd_encoder_collect_one_parallel_result(state, output);
    if (status != GCOMP_OK) {
      return status;
    }
  }

  state->parallel_job = NULL; // Job is now owned by parallel context
  *submitted_out = true;
  return GCOMP_OK;
}

/**
 * @brief Process input in parallel mode.
 *
 * Accumulates input until job_size, then submits jobs for compression.
 *
 * @param encoder Encoder
 * @param state Encoder state
 * @param input Input buffer
 * @param output Output buffer
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t zstd_encoder_update_parallel(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  (void)encoder;
  gcomp_status_t status;

  // First, drain any pending output from previous results
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Collect any ready results
  status = zstd_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel compression failed");
    return status;
  }

  // Check if we need to drain again after collecting
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Get job size from parallel context
  uint64_t job_size = zstd_parallel_get_job_size(state->parallel_ctx);

  // Accumulate input into current job
  while (input->used < input->size) {
    if (!state->parallel_job) {
      gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INTERNAL, "internal error: no parallel job");
      return GCOMP_ERR_INTERNAL;
    }

    // How much space in current job?  input_size counts the overlap sitting
    // in front of the content, so the room left is the job size less what
    // content is already there.
    const size_t used_content =
        state->parallel_job->base.input_size - state->parallel_job->overlap_len;
    size_t job_space = job_size - used_content;
    size_t input_avail = input->size - input->used;
    size_t to_copy = (input_avail < job_space) ? input_avail : job_space;

    if (to_copy > 0) {
      const uint8_t * in_ptr = (const uint8_t *)input->data;
      // Cast away const - the input buffer was allocated by alloc_job and is
      // mutable during filling. It's const in the job structure because during
      // compression it's read-only.
      uint8_t * job_input = (uint8_t *)state->parallel_job->base.input;
      memcpy(job_input + state->parallel_job->base.input_size,
          in_ptr + input->used, to_copy);
      // The checksum covers the frame's whole content, so it is taken here,
      // where the bytes are still in order, rather than inside a job that
      // only ever sees its own slice.
      if (state->checksum_enabled) {
        gcomp_xxhash64_update(
            &state->content_hash, in_ptr + input->used, to_copy);
      }
      state->parallel_job->base.input_size += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;
    }

    // If job is full, submit it
    if (state->parallel_job->base.input_size - state->parallel_job->overlap_len >=
        job_size) {
      bool submitted = false;
      status = zstd_encoder_submit_parallel_job(state, output, &submitted);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel job submit failed");
        return status;
      }
      if (!submitted) {
        // The context is full and its results have nowhere to go.  The job
        // is still here, still full; it goes next time, once the caller has
        // taken what is already staged.
        return GCOMP_OK;
      }

      // The job that was just handed over needs a successor to fill.
      status =
          zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(
            encoder, status, "failed to allocate parallel job");
        return status;
      }
      zstd_encoder_seed_parallel_job(state);

      // Try to collect results and drain output
      status = zstd_encoder_collect_parallel_results(state, output);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel compression failed");
        return status;
      }

      if (!zstd_encoder_drain_parallel_output(state, output)) {
        return GCOMP_OK; // Need more output space
      }
    }
  }

  return GCOMP_OK;
}

/**
 * @brief Stage the bytes that end a frame in parallel mode.
 *
 * Every job's blocks have Last_Block clear, because no job knows it is the
 * last.  The frame is ended here instead, by an empty Raw block carrying the
 * flag -- three bytes, and the same terminator the reference encoder writes
 * for empty input.  The checksum over the whole content follows it, taken from
 * the hash this encoder kept as it handed the input to the jobs in order.
 *
 * Only stages; the caller drains.  `parallel_epilogue_staged` makes it
 * idempotent, so a caller that ran out of output can come back.
 *
 * Both the end of the stream and a @ref GCOMP_FLUSH_FULL part-way through it
 * need these same bytes, which is why this is not inline in either.
 *
 * @param encoder Encoder, for error reporting.
 * @param state Encoder state.
 * @return GCOMP_OK once staged, error code on failure.
 */
static gcomp_status_t zstd_encoder_stage_parallel_epilogue(
    gcomp_encoder_t * encoder, zstd_encoder_state_t * state) {
  if (state->parallel_epilogue_staged) {
    return GCOMP_OK;
  }
  size_t need = ZSTD_BLOCK_HEADER_SIZE + (state->checksum_enabled ? 4u : 0u);
  if (need > state->parallel_output_buf_cap) {
    gcomp_encoder_set_error(encoder, GCOMP_ERR_INTERNAL,
        "parallel output buffer too small for the frame terminator");
    return GCOMP_ERR_INTERNAL;
  }
  zstd_write_block_header(
      state->parallel_output_buf, true, ZSTD_BLOCK_TYPE_RAW, 0);
  size_t epi = ZSTD_BLOCK_HEADER_SIZE;
  if (state->checksum_enabled) {
    uint64_t hash = gcomp_xxhash64_finalize(&state->content_hash);
    gcomp_write_le32(
        state->parallel_output_buf + epi, (uint32_t)(hash & 0xFFFFFFFF));
    epi += 4;
  }
  state->parallel_output_buf_pos = 0;
  state->parallel_output_buf_len = epi;
  state->parallel_epilogue_staged = true;
  return GCOMP_OK;
}

/**
 * @brief Start a fresh frame in parallel mode, after one has been ended.
 *
 * The mirror of @ref zstd_encoder_rearm_frame for the parallel path, and it
 * has the same job: leave nothing of the frame just closed behind.  Three
 * kinds of history have to go, and the parallel path's own kind is the one
 * that is easy to miss -- `parallel_overlap_buf` holds the tail of the last
 * job submitted and is copied to the front of the next one, so a frame that
 * kept it would open with matches into bytes the decoder has been told to
 * forget.  The reset path makes the same three moves for the same reason.
 *
 * Frame_Content_Size is dropped for the reason @ref zstd_encoder_rearm_frame
 * gives: it describes the frame it appears in (RFC 8878 3.1.1.1.2), and the
 * caller's figure was for the whole content.
 *
 * The new header is staged rather than emitted, exactly as at create time, so
 * it goes out with whatever the caller writes next.
 *
 * @param state Encoder state.
 * @return GCOMP_OK on success, error code if the header cannot be written.
 */
static gcomp_status_t zstd_encoder_rearm_parallel_frame(
    zstd_encoder_state_t * state) {
  state->parallel_epilogue_staged = false;
  state->blocks_finished = false;

  // The content checksum covers one frame's content, so the next frame's
  // starts from nothing.
  if (state->checksum_enabled) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // The history the jobs actually see.  Without this the next job opens with
  // the previous frame's tail in front of it and matches into it freely, which
  // is what GCOMP_FLUSH_FULL exists to prevent.
  //
  // A dictionary survives it, because the new frame declares the same
  // Dictionary_ID and the single-threaded rearm puts it back for the same
  // reason: what a full flush drops is the *stream* before it, not the history
  // the caller supplied.
  zstd_encoder_seed_parallel_dictionary(state);
  zstd_encoder_seed_parallel_job(state);

  state->header.content_size_present = false;
  state->header.content_size = 0;

  gcomp_status_t status = zstd_write_frame_header(&state->header,
      state->parallel_output_buf, state->parallel_output_buf_cap,
      &state->header_len);
  if (status != GCOMP_OK) {
    return status;
  }
  state->parallel_output_buf_pos = 0;
  state->parallel_output_buf_len = state->header_len;
  state->stage = ZSTD_ENC_STAGE_BLOCKS;
  return GCOMP_OK;
}

/**
 * @brief Finish parallel encoding.
 *
 * Submits final partial job and collects all remaining results.
 *
 * @param encoder Encoder
 * @param state Encoder state
 * @param output Output buffer
 * @return GCOMP_OK when complete, error code on failure
 */
static gcomp_status_t zstd_encoder_finish_parallel(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  gcomp_status_t status;

  // First, drain any pending output
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    // GCOMP_ERR_LIMIT, not GCOMP_OK: finish() reports completion with
    // GCOMP_OK, so returning it while output remains staged silently
    // truncated the stream.
    return GCOMP_ERR_LIMIT;
  }

  // Submit final partial job if not already done
  if (!state->blocks_finished && state->parallel_job) {
    // Only submit if there's data in the job
    if (state->parallel_job->base.input_size >
            state->parallel_job->overlap_len) {
      bool submitted = false;
      status = zstd_encoder_submit_parallel_job(state, output, &submitted);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel job submit failed");
        return status;
      }
      if (!submitted) {
        // Output is full with a result already staged.  finish() reports
        // completion with GCOMP_OK, so this must not: the last job has not
        // even been handed over yet.
        return GCOMP_ERR_LIMIT;
      }
    }
    else {
      // No data in job, just free it
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
    state->blocks_finished = true;
  }

  // Wait for all pending jobs and collect results
  status = zstd_parallel_wait(state->parallel_ctx);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel wait failed");
    return status;
  }

  // Collect all remaining results
  status = zstd_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel compression failed");
    return status;
  }

  // Drain any buffered output
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
  }

  // Check if there are still pending results
  if (zstd_parallel_pending_count(state->parallel_ctx) > 0) {
    // Shouldn't happen after wait, but be defensive: the stream is not
    // complete, so do not report GCOMP_OK.
    return GCOMP_ERR_LIMIT;
  }

  gcomp_status_t epi_status =
      zstd_encoder_stage_parallel_epilogue(encoder, state);
  if (epi_status != GCOMP_OK) {
    return epi_status;
  }

  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_ERR_LIMIT; // terminator did not fit; call finish again.
  }

  // All done
  state->stage = ZSTD_ENC_STAGE_DONE;
  return GCOMP_OK;
}

//
// Initialization
//

gcomp_status_t zstd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  (void)registry;

  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  gcomp_status_t status = GCOMP_OK;

  // Allocate state structure
  zstd_encoder_state_t * state = gcomp_calloc(alloc, 1, sizeof(*state));
  if (!state) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "failed to allocate state");
    return GCOMP_ERR_MEMORY;
  }
  state->allocator = alloc;

  // Initialize memory tracker
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(*state));

  // Read options
  int64_t level = ZSTD_LEVEL_DEFAULT;
  int checksum = 0;
  uint64_t window_log = 0;
  uint64_t content_size = 0;
  bool content_size_present = false;
  uint64_t max_memory = ZSTD_DEFAULT_MAX_MEMORY_BYTES;
  uint64_t num_threads = 1;
  uint64_t job_size = 0;
  const void * dict_opt = NULL;
  size_t dict_opt_size = 0;
  uint64_t dict_id_opt = 0;
  bool dict_id_opt_set = false;
  int ldm_enabled = 0;
  uint64_t ldm_min_match = ZSTD_LDM_MIN_MATCH_DEFAULT;
  uint64_t ldm_hash_log = 0;
  uint64_t ldm_hash_rate_log = ZSTD_LDM_HASH_RATE_LOG_DEFAULT;
  int seekable_enabled = 0;
  // The schema's defaults, restated because these are read with
  // gcomp_options_get_*() which leaves the variable alone when the key is absent.
  uint64_t seekable_frame_size = 1048576u;
  int seekable_checksum = 1;

  if (options) {
    gcomp_options_get_int64(options, "zstd.level", &level);
    gcomp_options_get_bool(options, "zstd.checksum", &checksum);
    gcomp_options_get_uint64(options, "zstd.window_log", &window_log);
    if (gcomp_options_get_uint64(options, "zstd.content_size", &content_size) ==
        GCOMP_OK) {
      content_size_present = true;
    }
    gcomp_options_get_uint64(options, "limits.max_memory_bytes", &max_memory);
    gcomp_options_get_uint64(options, "threads.count", &num_threads);
    gcomp_options_get_uint64(options, "zstd.job_size", &job_size);
    gcomp_options_get_bool(options, "zstd.long", &ldm_enabled);
    gcomp_options_get_bool(options, "zstd.seekable", &seekable_enabled);
    gcomp_options_get_uint64(
        options, "zstd.seekable_frame_size", &seekable_frame_size);
    gcomp_options_get_bool(
        options, "zstd.seekable_checksum", &seekable_checksum);
    gcomp_options_get_uint64(options, "zstd.ldm_min_match", &ldm_min_match);
    gcomp_options_get_uint64(options, "zstd.ldm_hash_log", &ldm_hash_log);
    gcomp_options_get_uint64(
        options, "zstd.ldm_hash_rate_log", &ldm_hash_rate_log);
    if (gcomp_options_get_bytes(options, "zstd.dictionary", &dict_opt,
            &dict_opt_size) == GCOMP_OK &&
        dict_opt != NULL && dict_opt_size > 0) {
      if (gcomp_options_get_uint64(
              options, "zstd.dictionary_id", &dict_id_opt) == GCOMP_OK) {
        dict_id_opt_set = true;
      }
    }
  }

  // zstd.long and threads.count > 1 work together, and the reason the two were
  // thought incompatible was a misreading of this encoder rather than a fact
  // about the format.
  //
  // The note that stood here said a job "compresses its own block with its own
  // window, so a match reaching tens of megabytes back is not available to it:
  // the job would have to hold the whole stream", and pointed at libzstd's
  // answer of scanning once in the calling thread and handing matches to the
  // jobs. Two things were wrong with that. A job is already seeded with a
  // **whole window** of the preceding stream - zstd_parallel.c sizes the
  // overlap from window_log and deliberately does not cap it at the job size -
  // and a window is as far as a long match may reach in any case, because RFC
  // 8878 section 3.1.1.1.2 forbids an offset past the declared window. So a
  // job's own scan has exactly the reach the single-threaded encoder's does,
  // and there is nothing to share.
  //
  // What did have to change was zstd_ldm_reset(). The long-distance table
  // holds absolute positions and so does its scan cursor, and jobs reuse
  // pooled match finders; a carried-over cursor sits ahead of the new buffer
  // and the scan skips everything behind it. That is not incorrect output -
  // every candidate is confirmed against the bytes - it is long-distance
  // matching that quietly finds nothing, which is why it needed a test that
  // measures the ratio rather than one that checks the stream decodes.

  // Seekable streaming mode.
  if (seekable_enabled) {
    // Parallel mode also batches input into independent frames, with its own
    // job_size deciding where the boundaries fall. Making the two agree means
    // reconciling job_size with seekable_frame_size and building the table from
    // job results in order; that is a real change to the parallel result path
    // rather than a flag, so the combination is refused rather than silently
    // producing a file whose frame boundaries are not the ones asked for.
    if (num_threads > 1u) {
      status = GCOMP_ERR_UNSUPPORTED;
      gcomp_encoder_set_error(encoder, status,
          "zstd.seekable is not yet supported with threads.count > 1; the "
          "frame boundaries would be decided by zstd.job_size rather than by "
          "zstd.seekable_frame_size");
      goto cleanup;
    }
    // A caller that declares the whole stream's size is describing one frame's
    // content, which is not what this writes. Refusing says so; honouring it
    // would put a wrong size in every frame header.
    if (content_size_present) {
      status = GCOMP_ERR_INVALID_ARG;
      gcomp_encoder_set_error(encoder, status,
          "zstd.content_size cannot be set with zstd.seekable: each frame "
          "declares its own size, and this encoder computes them");
      goto cleanup;
    }

    state->seekable = true;
    state->seek_frame_size = seekable_frame_size;

    if (seekable_frame_size > (uint64_t)SIZE_MAX) {
      status = GCOMP_ERR_LIMIT;
      gcomp_encoder_set_error(encoder, status,
          "zstd.seekable_frame_size does not fit in memory");
      goto cleanup;
    }
    state->seek_in = gcomp_malloc(state->allocator, (size_t)seekable_frame_size);
    if (!state->seek_in) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    state->seek_in_cap = (size_t)seekable_frame_size;

    status = gcomp_seek_table_init(
        &state->seek_table, state->allocator, seekable_checksum ? 1 : 0, 0);
    if (status != GCOMP_OK) {
      goto cleanup;
    }

    // Per-frame options: the caller's, with seekable off so the recursive
    // encode of one frame is an ordinary single-frame encode rather than
    // another seekable writer.
    if (options) {
      status = gcomp_options_clone(options, &state->seek_frame_options);
    }
    else {
      status = gcomp_options_create(&state->seek_frame_options);
    }
    if (status != GCOMP_OK) {
      goto cleanup;
    }
    status = gcomp_options_set_bool(state->seek_frame_options, "zstd.seekable", 0);
    if (status != GCOMP_OK) {
      goto cleanup;
    }
  }

  // Store threading options
  state->num_threads = (uint32_t)num_threads;
  state->job_size = job_size;

  // Validate and set compression level
  if (level < ZSTD_LEVEL_MIN || level > ZSTD_LEVEL_MAX) {
    status = GCOMP_ERR_INVALID_ARG;
    gcomp_encoder_set_error(encoder, status,
        "zstd.level must be between %d and %d", ZSTD_LEVEL_MIN, ZSTD_LEVEL_MAX);
    goto cleanup;
  }
  state->compression_level = (int)level;

  // Determine window log
  uint8_t effective_window_log;
  if (window_log == 0) {
    effective_window_log = zstd_level_to_window_log(state->compression_level);
  }
  else {
    if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
      status = GCOMP_ERR_INVALID_ARG;
      gcomp_encoder_set_error(encoder, status,
          "zstd.window_log must be 0 (auto) or between %d and %d",
          ZSTD_WINDOW_LOG_MIN, ZSTD_WINDOW_LOG_MAX);
      goto cleanup;
    }
    effective_window_log = (uint8_t)window_log;
  }

  // Set up frame header
  state->header.window_log = effective_window_log;
  state->header.window_size = zstd_window_log_to_size(effective_window_log);
  state->header.content_checksum = (checksum != 0);
  state->header.content_size_present = content_size_present;
  state->header.content_size = content_size;
  state->header.single_segment =
      content_size_present && (content_size <= state->header.window_size);
  state->checksum_enabled = (checksum != 0);
  state->max_memory_bytes = max_memory;

  if (dict_opt != NULL && dict_opt_size > 0) {
    state->dict_bytes = gcomp_malloc(alloc, dict_opt_size);
    if (!state->dict_bytes) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for dictionary", dict_opt_size);
      goto cleanup;
    }
    memcpy(state->dict_bytes, dict_opt, dict_opt_size);
    state->dict_size = dict_opt_size;
    gcomp_memory_track_alloc(&state->mem_tracker, dict_opt_size);
    status = zstd_dict_parse(
        state->dict_bytes, state->dict_size, alloc, &state->dict_parsed);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(encoder, status, "dictionary parse failed");
      gcomp_free(alloc, state->dict_bytes);
      state->dict_bytes = NULL;
      state->dict_size = 0;
      goto cleanup;
    }
    state->header.dict_id =
        dict_id_opt_set ? (uint32_t)dict_id_opt : state->dict_parsed.dict_id;
    if (state->dict_parsed.has_entropy_tables) {
      state->rep_offset_1 = state->dict_parsed.rep_offset_1;
      state->rep_offset_2 = state->dict_parsed.rep_offset_2;
      state->rep_offset_3 = state->dict_parsed.rep_offset_3;
    }
    else {
      state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
      state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
      state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
    }
    state->dict_block_buffer_capacity =
        state->dict_parsed.content_size + ZSTD_BLOCK_SIZE_MAX;
    state->dict_block_buffer =
        gcomp_malloc(alloc, state->dict_block_buffer_capacity);
    if (!state->dict_block_buffer) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for dictionary block buffer",
          (size_t)state->dict_block_buffer_capacity);
      goto cleanup;
    }
    gcomp_memory_track_alloc(
        &state->mem_tracker, state->dict_block_buffer_capacity);
  }
  else {
    state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
    state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
    state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
  }

  // Initialize content hash if checksum enabled
  if (checksum) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // No point holding more history than the stream can produce.  When the
  // caller declares the content size, the window buys nothing past it, and
  // sizing to it keeps a large declared window from costing a large buffer -
  // and a much larger position table - for a small stream.
  state->mf_window_max = state->header.window_size;
  if (content_size_present && content_size < state->mf_window_max) {
    state->mf_window_max = (size_t)content_size;
  }

  // RFC 8878 section 3.1.1.2: Block_Maximum_Size is "the smaller of
  // Window_Size and 128 KB".  The block buffer is what decides how much this
  // encoder puts in one block, so the window the frame header declares caps it
  // as well as the format's own ceiling.
  //
  // This used to be ZSTD_BLOCK_SIZE_MAX unconditionally, so any frame asking
  // for a window below 128 KB -- window_log under 17 -- described itself as
  // holding less than the blocks it then wrote.  Our own decoder reads such a
  // frame, because it sizes its output buffer from the format ceiling rather
  // than from the frame; the reference decoder applies the rule and refuses
  // the frame outright.  Nothing caught it because nothing had ever handed a
  // small-window frame of ours to another implementation.
  size_t block_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  if (state->header.window_size < block_buffer_size) {
    block_buffer_size = (size_t)state->header.window_size;
  }
  // May be larger than the input for incompressible data.
  size_t compressed_buffer_size =
      block_buffer_size + ZSTD_BLOCK_HEADER_SIZE + 256;

  // What the rest of this function will allocate, worked out before any of it
  // is allocated.
  //
  // limits.max_memory_bytes was checked at the end, once every buffer below
  // existed. zstd.window_log is the caller's to choose and a 2 GB window is a
  // legal choice, so refusing it meant allocating the 2 GB first and turning
  // it down afterwards - which is not a limit. It cost nothing visible on
  // Linux, where an untouched mapping is backed by no pages, and was a real
  // allocation under a cgroup cap, with overcommit off, on Windows, and under
  // Valgrind, where it was found.
  //
  // Every size here is the one the code below uses, and the match finder's
  // comes from the function that allocates it, so the projection cannot drift
  // from the allocation. The dictionary buffers are already allocated by this
  // point and are already in the tracker: they are bounded by the dictionary
  // the caller handed in, so they cannot be what runs away.
  {
    uint64_t projected = (uint64_t)block_buffer_size +
        (uint64_t)compressed_buffer_size + sizeof(zstd_match_finder_t) +
        (uint64_t)zstd_mf_memory_estimate(
            state->compression_level, state->mf_window_max) +
        (uint64_t)state->mf_window_max + (uint64_t)block_buffer_size +
        (uint64_t)(block_buffer_size / 3) * sizeof(zstd_sequence_t) +
        (uint64_t)block_buffer_size;

    // The long-distance table is sized from the window, so at window_log 31
    // it is the largest single allocation the encoder makes. It has to be in
    // the projection for the same reason the match finder's tables are: a
    // limit answered after the allocation has not limited anything.
    if (ldm_enabled) {
      projected += sizeof(zstd_ldm_t) +
          (uint64_t)zstd_ldm_memory_estimate(state->mf_window_max,
              (unsigned)ldm_hash_log, (unsigned)ldm_hash_rate_log);
    }

    gcomp_memory_tracker_t projection = state->mem_tracker;
    gcomp_memory_track_alloc(&projection, projected);
    if (gcomp_memory_check_limit(&projection, max_memory) != GCOMP_OK) {
      status = GCOMP_ERR_LIMIT;
      gcomp_encoder_set_error(encoder, status,
          "memory limit exceeded during encoder init (%zu bytes, limit %zu)",
          (size_t)projection.current_bytes, (size_t)max_memory);
      goto cleanup;
    }
  }

  // Allocate block buffer (use calloc to satisfy valgrind - hash function
  // may read bytes before they're fully populated during streaming)
  state->block_buffer = gcomp_calloc(alloc, 1, block_buffer_size);
  if (!state->block_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for block buffer", block_buffer_size);
    goto cleanup;
  }
  state->block_buffer_capacity = block_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, block_buffer_size);

  // Allocate compressed buffer (sized above, with the projection)
  state->compressed_buffer = gcomp_malloc(alloc, compressed_buffer_size);
  if (!state->compressed_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for compressed buffer",
        compressed_buffer_size);
    goto cleanup;
  }
  state->compressed_buffer_capacity = compressed_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, compressed_buffer_size);

  // Allocate match finder
  state->match_finder = gcomp_calloc(alloc, 1, sizeof(zstd_match_finder_t));
  if (!state->match_finder) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status, "failed to allocate match finder");
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(zstd_match_finder_t));

  status = zstd_mf_init(state->match_finder, alloc, state->compression_level,
      state->mf_window_max, &state->mem_tracker);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "match finder init failed");
    goto cleanup;
  }

  if (ldm_enabled) {
    status = zstd_mf_enable_ldm(state->match_finder, alloc,
        state->mf_window_max, (unsigned)ldm_min_match, (unsigned)ldm_hash_log,
        (unsigned)ldm_hash_rate_log, &state->mem_tracker);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "long-distance matching could not be enabled");
      goto cleanup;
    }
  }

  // The window the match finder searches: the history carried from block to
  // block, followed by the block being compressed.  Its length is the window
  // size the frame header declares, which is what tells the decoder how far
  // back a sequence may point.
  state->mf_window_capacity = state->mf_window_max + block_buffer_size;
  state->mf_window_len = 0;
  state->mf_window = gcomp_malloc(alloc, state->mf_window_capacity);
  if (!state->mf_window) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for the match finder window",
        state->mf_window_capacity);
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, state->mf_window_capacity);

  // A dictionary is history the stream starts with, so it goes into the
  // window ahead of the first block and is indexed once here rather than
  // re-indexed for every block.
  if (state->dict_parsed.content && state->dict_parsed.content_size > 0) {
    size_t take = state->dict_parsed.content_size;
    if (take > state->mf_window_max) {
      // Only the tail is reachable: a sequence cannot point further back than
      // the declared window.
      memcpy(state->mf_window,
          state->dict_parsed.content + (take - state->mf_window_max),
          state->mf_window_max);
      state->mf_window_len = state->mf_window_max;
    }
    else {
      memcpy(state->mf_window, state->dict_parsed.content, take);
      state->mf_window_len = take;
    }
    zstd_mf_index_range(state->match_finder, state->mf_window, 0,
        state->mf_window_len, state->mf_window_len);
    // Taken now, while every hash head still names a dictionary position:
    // the first block's positions are about to replace them.
    zstd_mf_note_dictionary(state->match_finder, state->allocator,
        state->mf_window_len, &state->mem_tracker);
  }

  // Allocate sequence buffer
  // Maximum sequences = block_size / min_match_length
  state->seq_buffer_capacity = block_buffer_size / 3;
  state->seq_buffer =
      gcomp_malloc(alloc, state->seq_buffer_capacity * sizeof(zstd_sequence_t));
  if (!state->seq_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for sequence buffer",
        state->seq_buffer_capacity * sizeof(zstd_sequence_t));
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker,
      state->seq_buffer_capacity * sizeof(zstd_sequence_t));

  // Allocate literals buffer
  state->literals_buffer_capacity = block_buffer_size;
  state->literals_buffer = gcomp_malloc(alloc, state->literals_buffer_capacity);
  if (!state->literals_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for literals buffer",
        state->literals_buffer_capacity);
    goto cleanup;
  }
  gcomp_memory_track_alloc(
      &state->mem_tracker, state->literals_buffer_capacity);

  // The same check again, against what was actually allocated rather than
  // against what was projected. The projection above is what refuses a
  // request this large; this catches anything the projection did not account
  // for - the dictionary buffers, or a future allocation added below it
  // without being added to the projection.
  //
  // The core helper treats 0 as unlimited as limits.h documents. Comparing
  // directly, as this did, made a zero limit mean a budget of zero and
  // refused to start.
  if (gcomp_memory_check_limit(&state->mem_tracker, max_memory) != GCOMP_OK) {
    status = GCOMP_ERR_LIMIT;
    gcomp_encoder_set_error(encoder, status,
        "memory limit exceeded during encoder init (%zu bytes, limit %zu)",
        (size_t)state->mem_tracker.current_bytes, (size_t)max_memory);
    goto cleanup;
  }

  // Initialize parallel context if threads > 1
  if (state->num_threads > 1) {
    zstd_parallel_config_t pconfig = {
        .num_threads = state->num_threads,
        .max_in_flight = 0, // Auto
        .job_size = state->job_size,
        .checksum_enabled = state->checksum_enabled,
        .compression_level = state->compression_level,
        .window_log = effective_window_log,
        .max_memory_bytes = max_memory,
        .ldm_enabled = ldm_enabled ? true : false,
        .ldm_min_match = (unsigned)ldm_min_match,
        .ldm_hash_log = (unsigned)ldm_hash_log,
        .ldm_hash_rate_log = (unsigned)ldm_hash_rate_log,
        .allocator = alloc,
        .mem_tracker = &state->mem_tracker,
    };
    status = zstd_parallel_create(&pconfig, &state->parallel_ctx);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "parallel context creation failed");
      goto cleanup;
    }

    // Allocate first job for parallel mode
    status = zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "failed to allocate parallel job");
      goto cleanup;
    }

    // Allocate parallel output buffer for collecting results
    // Size to hold at least one job's output with some margin
    uint64_t actual_job_size = zstd_parallel_get_job_size(state->parallel_ctx);
    state->parallel_output_buf_cap =
        actual_job_size + 4096; // Extra for frame overhead
    state->parallel_output_buf =
        gcomp_malloc(alloc, state->parallel_output_buf_cap);
    if (!state->parallel_output_buf) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for parallel output buffer",
          (size_t)state->parallel_output_buf_cap);
      goto cleanup;
    }
    gcomp_memory_track_alloc(
        &state->mem_tracker, state->parallel_output_buf_cap);

    // Somewhere to keep the tail of each job for the next one to look back
    // into.  It is filled just before a job is handed over, which is the last
    // moment its buffer is still ours.
    state->parallel_overlap_cap =
        zstd_parallel_get_overlap_size(state->parallel_ctx);
    state->parallel_overlap_len = 0;
    if (state->parallel_overlap_cap > 0) {
      state->parallel_overlap_buf =
          gcomp_malloc(alloc, state->parallel_overlap_cap);
      if (!state->parallel_overlap_buf) {
        status = GCOMP_ERR_MEMORY;
        gcomp_encoder_set_error(encoder, status,
            "failed to allocate %u bytes for the job overlap",
            state->parallel_overlap_cap);
        goto cleanup;
      }
      gcomp_memory_track_alloc(
          &state->mem_tracker, state->parallel_overlap_cap);
    }

    // The dictionary is the history the first job has, and the job was
    // allocated above this - before the buffer existed - so it is seeded here
    // rather than there.
    zstd_encoder_seed_parallel_dictionary(state);
    zstd_encoder_seed_parallel_job(state);

    // Parallel jobs emit blocks only, so the frame header is this encoder's
    // to write, exactly as in single-threaded mode.  Staging it as the first
    // thing in the parallel output buffer puts it ahead of every job's blocks
    // without giving the drain path a special case.
    status = zstd_write_frame_header(&state->header, state->parallel_output_buf,
        state->parallel_output_buf_cap, &state->header_len);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(encoder, status, "frame header write failed");
      goto cleanup;
    }
    state->parallel_output_buf_pos = 0;
    state->parallel_output_buf_len = state->header_len;

    state->stage = ZSTD_ENC_STAGE_BLOCKS;
  }
  else {
    // Single-threaded mode: Set initial stage and build header
    state->stage = ZSTD_ENC_STAGE_HEADER;

    // Build frame header
    status = zstd_write_frame_header(&state->header, state->header_buf,
        sizeof(state->header_buf), &state->header_len);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(encoder, status, "frame header write failed");
      goto cleanup;
    }
  }

  encoder->method_state = state;
  return GCOMP_OK;

cleanup:
  if (state) {
    zstd_dict_destroy(&state->dict_parsed);
    if (state->dict_bytes) {
      gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
      gcomp_free(alloc, state->dict_bytes);
    }
    if (state->dict_block_buffer) {
      gcomp_memory_track_free(
          &state->mem_tracker, state->dict_block_buffer_capacity);
      gcomp_free(alloc, state->dict_block_buffer);
    }
    if (state->mf_window) {
      gcomp_free(alloc, state->mf_window);
    }
    if (state->block_buffer) {
      gcomp_free(alloc, state->block_buffer);
    }
    if (state->compressed_buffer) {
      gcomp_free(alloc, state->compressed_buffer);
    }
    if (state->match_finder) {
      zstd_mf_destroy(state->match_finder, alloc, NULL);
      gcomp_free(alloc, state->match_finder);
    }
    if (state->seq_buffer) {
      gcomp_free(alloc, state->seq_buffer);
    }
    if (state->literals_buffer) {
      gcomp_free(alloc, state->literals_buffer);
    }
    if (state->parallel_output_buf) {
      gcomp_free(alloc, state->parallel_output_buf);
    }
    if (state->parallel_overlap_buf) {
      gcomp_free(alloc, state->parallel_overlap_buf);
    }
    if (state->parallel_job) {
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
    }
    if (state->parallel_ctx) {
      zstd_parallel_destroy(state->parallel_ctx);
    }
    gcomp_free(alloc, state);
  }
  return status;
}

//
// Destroy
//

const gcomp_stepdown_tally_t * gcomp_zstd_encoder_stepdowns(
    const gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return NULL;
  }
  const zstd_encoder_state_t * state = (const zstd_encoder_state_t *)encoder->method_state;
  return &state->stepdowns;
}

void zstd_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  zstd_seek_teardown(state);

  if (state->mf_window) {
    gcomp_memory_track_free(&state->mem_tracker, state->mf_window_capacity);
    gcomp_free(alloc, state->mf_window);
    state->mf_window = NULL;
  }
  if (state->block_buffer) {
    gcomp_memory_track_free(&state->mem_tracker, state->block_buffer_capacity);
    gcomp_free(alloc, state->block_buffer);
  }
  if (state->compressed_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->compressed_buffer_capacity);
    gcomp_free(alloc, state->compressed_buffer);
  }
  if (state->match_finder) {
    zstd_mf_destroy(state->match_finder, alloc, &state->mem_tracker);
    gcomp_memory_track_free(&state->mem_tracker, sizeof(zstd_match_finder_t));
    gcomp_free(alloc, state->match_finder);
  }
  if (state->dict_block_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->dict_block_buffer_capacity);
    gcomp_free(alloc, state->dict_block_buffer);
  }
  zstd_dict_destroy(&state->dict_parsed);
  if (state->dict_bytes) {
    gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
    gcomp_free(alloc, state->dict_bytes);
  }
  if (state->seq_buffer) {
    gcomp_memory_track_free(&state->mem_tracker,
        state->seq_buffer_capacity * sizeof(zstd_sequence_t));
    gcomp_free(alloc, state->seq_buffer);
  }
  if (state->literals_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->literals_buffer_capacity);
    gcomp_free(alloc, state->literals_buffer);
  }
  if (state->parallel_output_buf) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->parallel_output_buf_cap);
    gcomp_free(alloc, state->parallel_output_buf);
  }
  if (state->parallel_overlap_buf) {
    gcomp_memory_track_free(&state->mem_tracker, state->parallel_overlap_cap);
    gcomp_free(alloc, state->parallel_overlap_buf);
  }
  if (state->parallel_job) {
    zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
  }
  if (state->parallel_ctx) {
    zstd_parallel_destroy(state->parallel_ctx);
  }
  gcomp_memory_track_free(&state->mem_tracker, sizeof(*state));
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

//
// Update
//

gcomp_status_t zstd_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (input->size > 0 && !input->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "input data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const uint8_t * in_ptr = (const uint8_t *)input->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "encoder in error state");
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Seekable mode batches into whole frames of its own, like parallel mode.
  if (state->seekable) {
    return zstd_seek_update(encoder, state, input, output);
  }

  // Parallel mode: use dedicated parallel update path
  if (state->parallel_ctx) {
    return zstd_encoder_update_parallel(encoder, state, input, output);
  }

  // Single-threaded mode: write header if needed
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    (void)zstd_emit_staged(
        state->header_buf, &state->header_pos, state->header_len, output);
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      return GCOMP_OK; // Need more output space
    }
  }

  // Process input in BLOCKS stage
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // First, drain any pending compressed output
    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK; // Need more output space
    }

    // Buffer input data
    while (input->used < input->size) {
      size_t to_copy = input->size - input->used;
      size_t space = state->block_buffer_capacity - state->block_buffer_pos;
      if (to_copy > space) {
        to_copy = space;
      }
      memcpy(state->block_buffer + state->block_buffer_pos,
          in_ptr + input->used, to_copy);
      state->block_buffer_pos += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;

      // Update content hash if enabled
      if (state->checksum_enabled) {
        gcomp_xxhash64_update(&state->content_hash,
            state->block_buffer + state->block_buffer_pos - to_copy, to_copy);
      }

      // If block buffer is full, compress and output
      if (state->block_buffer_pos >= state->block_buffer_capacity) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(encoder, status, "block compression failed");
          return status;
        }

        // Write block header
        // For RLE blocks, the header size field is the regenerated size
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, false, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;

        // Output compressed block
        (void)zstd_emit_staged(state->compressed_buffer,
            &state->compressed_buffer_pos, state->compressed_buffer_len,
            output);

        state->block_buffer_pos = 0;

        // If we couldn't output everything, return and continue later
        if (state->compressed_buffer_pos < state->compressed_buffer_len) {
          return GCOMP_OK;
        }
      }
    }
  }

  return GCOMP_OK;
}

//
// Flush
//

/**
 * @brief Forget the match history, so nothing after this point reaches back.
 *
 * What GCOMP_FLUSH_FULL adds to a sync flush: the match finder's tables and
 * the window they index are emptied, and the repeat offsets go back to their
 * starting values (RFC 8878 3.1.1.3.2.1.1), so no sequence written afterwards
 * can name a distance into what came before.
 *
 * The dictionary is history the *next* block is still entitled to, so it goes
 * back in, exactly as zstd_encoder_reset() puts it back for a new stream.
 *
 * Nothing needs to be done about entropy tables: this encoder never writes
 * Repeat_Mode, so no block ever depends on the tables of the one before it.
 */
static void zstd_encoder_drop_history(zstd_encoder_state_t * state) {
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // There are two spellings of history in this encoder and dropping only one
  // of them is how GCOMP_FLUSH_FULL came to be a no-op in parallel mode: the
  // match finder below is the single-threaded path's, and the overlap buffer
  // is the parallel path's.  This function is named for all of it.  The
  // parallel flush calls zstd_encoder_rearm_parallel_frame(), which reseeds
  // the job in hand from the cleared buffer; here there is no job to reseed.
  state->parallel_overlap_len = 0;

  if (state->match_finder) {
    zstd_mf_reset(state->match_finder);
  }
  state->mf_window_len = 0;
  if (state->mf_window && state->dict_parsed.content &&
      state->dict_parsed.content_size > 0) {
    size_t take = state->dict_parsed.content_size;
    const uint8_t * from = state->dict_parsed.content;
    if (take > state->mf_window_max) {
      from += take - state->mf_window_max;
      take = state->mf_window_max;
    }
    memcpy(state->mf_window, from, take);
    state->mf_window_len = take;
    zstd_mf_index_range(state->match_finder, state->mf_window, 0,
        state->mf_window_len, state->mf_window_len);
    // Taken now, while every hash head still names a dictionary position:
    // the first block's positions are about to replace them.
    zstd_mf_note_dictionary(state->match_finder, state->allocator,
        state->mf_window_len, &state->mem_tracker);
  }
}

/**
 * @brief Start a fresh frame after a full flush has closed the previous one.
 *
 * A new frame is the only place zstd's frame-level state legitimately starts
 * over, which is the whole reason GCOMP_FLUSH_FULL ends the frame.
 */
static gcomp_status_t zstd_encoder_rearm_frame(zstd_encoder_state_t * state) {
  state->blocks_finished = false;
  state->compressed_buffer_len = 0;
  state->compressed_buffer_pos = 0;
  state->block_buffer_pos = 0;

  // The content checksum covers one frame's content, so the next frame's
  // starts from nothing.
  if (state->checksum_enabled) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  zstd_encoder_drop_history(state);

  // Frame_Content_Size describes the frame it appears in (RFC 8878 3.1.1.1.2).
  // The value the caller gave was for the whole content, which the first
  // frame no longer holds on its own, so later frames declare nothing rather
  // than repeat a number that is now wrong.
  state->header.content_size_present = false;
  state->header.content_size = 0;

  gcomp_status_t status = zstd_write_frame_header(&state->header,
      state->header_buf, sizeof(state->header_buf), &state->header_len);
  state->header_pos = 0;
  state->stage = ZSTD_ENC_STAGE_HEADER;
  return status;
}

/**
 * @brief Close the frame, so that a full flush really does start over.
 *
 * WHY A FULL FLUSH ENDS THE FRAME
 * ===============================
 * The other methods here can forget their history in place.  Zstd cannot:
 * the repeat offsets are frame-level state that the decoder tracks in step
 * with the encoder (RFC 8878 3.1.1.3.2.1.1), so an encoder that quietly reset
 * them mid-frame would be describing distances the decoder computes
 * differently.  That is not theoretical -- it is what the first version of
 * this did, and the flush-invariant test caught it six flushes in, with the
 * decoder producing the wrong bytes.
 *
 * Nor is there a cheaper fix.  Keeping the repeat offsets and clearing only
 * the match finder still leaves a sequence free to name a repeat code whose
 * value came from before the flush, which is exactly what a full flush
 * promises will not happen.
 *
 * So the unit of recovery in zstd is the frame, and a full flush ends one.
 * libzstd draws the same line: it offers ZSTD_e_flush and ZSTD_e_end, and no
 * mid-frame equivalent of Z_FULL_FLUSH.
 *
 * The consequence for callers is that the output becomes more than one frame,
 * and reading it needs `zstd.concat` on the decoder. This is a requirement a
 * full flush introduces, and not one parallel mode carries: an earlier version
 * of this sentence said parallel mode "already carries" it, which was the same
 * stale belief that it writes a frame per job.
 */
static gcomp_status_t zstd_encoder_flush_end_frame(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT;
    }

    if (!state->blocks_finished) {
      if (state->block_buffer_pos > 0) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          return gcomp_encoder_set_error(
              encoder, status, "final block compression failed");
        }
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, true, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;
        state->block_buffer_pos = 0;
      }
      else {
        // A frame must end with a block marked last, even an empty one.
        zstd_write_block_header(
            state->compressed_buffer, true, ZSTD_BLOCK_TYPE_RAW, 0);
        state->compressed_buffer_len = 3;
        state->compressed_buffer_pos = 0;
      }
      state->blocks_finished = true;
    }

    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT;
    }

    if (!state->checksum_enabled) {
      return zstd_encoder_rearm_frame(state);
    }
    uint64_t hash = gcomp_xxhash64_finalize(&state->content_hash);
    gcomp_write_le32(state->checksum_buf, (uint32_t)(hash & 0xFFFFFFFFu));
    state->checksum_pos = 0;
    state->stage = ZSTD_ENC_STAGE_CHECKSUM;
  }

  if (state->stage == ZSTD_ENC_STAGE_CHECKSUM) {
    while (state->checksum_pos < ZSTD_CONTENT_CHECKSUM_SIZE &&
        output->used < output->size) {
      out_ptr[output->used++] = state->checksum_buf[state->checksum_pos++];
    }
    if (state->checksum_pos < ZSTD_CONTENT_CHECKSUM_SIZE) {
      return GCOMP_ERR_LIMIT;
    }
    return zstd_encoder_rearm_frame(state);
  }

  return GCOMP_ERR_INTERNAL;
}

gcomp_status_t zstd_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "encoder in error state");
  }
  // blocks_finished is set part-way through ending a frame as well as by
  // finish(), so it cannot stand on its own here: a full flush that ran out
  // of output mid-epilogue has to be allowed to come back and complete it.
  if (state->stage == ZSTD_ENC_STAGE_DONE || state->finish_called) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "zstd encoder cannot flush after finish");
  }

  if (state->seekable) {
    (void)mode; // Both modes end the frame; see zstd_seek_flush().
    return zstd_seek_flush(encoder, state, output);
  }

  if (state->parallel_ctx) {
    // A flush submits the job in hand so its bytes go out, and the next input
    // starts a new job. The jobs are blocks of one frame, so this moves a block
    // boundary and not a frame boundary, and the result still decodes without
    // zstd.concat. The previous version of this comment said a flush ended a
    // frame and that a reader "needs zstd.concat either way" -- the same stale
    // belief corrected in the file header above.
    if (!zstd_encoder_drain_parallel_output(state, output)) {
      return GCOMP_ERR_LIMIT;
    }

    if (state->parallel_job && state->parallel_job->base.input_size >
            state->parallel_job->overlap_len) {
      bool submitted = false;
      gcomp_status_t status =
          zstd_encoder_submit_parallel_job(state, output, &submitted);
      if (status != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, status, "parallel job submit failed");
      }
      if (!submitted) {
        return GCOMP_ERR_LIMIT; // Output full with a result staged; drain.
      }
      status =
          zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
      if (status != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, status, "failed to allocate parallel job");
      }

      // The job the flush just handed over saved its tail; the new one has to
      // be given it, or the bytes after the flush start cold.  update() seeds
      // every job it allocates and this path did not, which made a SYNC flush
      // lose the history it exists to keep: measured on 100 KB written twice
      // with a flush between, threads.count 4 cost 100% of 2x where one thread
      // cost 50%.  A GCOMP_FLUSH_FULL below clears the overlap again, on
      // purpose; the two are not the same call any more.
      zstd_encoder_seed_parallel_job(state);
    }

    // A flush has to wait: its whole point is that nothing the caller handed
    // over is still sitting with a worker.
    gcomp_status_t status = zstd_parallel_wait(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, status, "parallel wait failed");
    }
    status = zstd_encoder_collect_parallel_results(state, output);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "parallel compression failed");
    }
    if (!zstd_encoder_drain_parallel_output(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
    if (zstd_parallel_pending_count(state->parallel_ctx) > 0) {
      return GCOMP_ERR_LIMIT; // Jobs remain; call again once drained.
    }

    if (mode != GCOMP_FLUSH_FULL) {
      return GCOMP_OK;
    }

    // Everything above has drained, submitted, waited and collected, so no
    // worker holds anything and this is the one point in the parallel path
    // where a frame can be closed.  It has to be closed, for the same reason
    // the single-threaded path closes one: the jobs are blocks of a single
    // frame -- measured, see the note on parallel mode in the file header --
    // so without this the next job's blocks continue the frame the caller
    // asked to be able to recover from, and the overlap hands them the bytes
    // before the flush to match into.
    //
    // This comment used to say the opposite, that each job was its own frame
    // with its own window and there was therefore no history for
    // GCOMP_FLUSH_FULL to drop, and the code trusted it: FULL did exactly
    // what SYNC does whenever threads.count was above one. That belief is the
    // same one the file header records as measured and wrong; the correction
    // reached the header and not this branch.
    gcomp_status_t epi_status =
        zstd_encoder_stage_parallel_epilogue(encoder, state);
    if (epi_status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, epi_status, "parallel frame terminator failed");
    }
    if (!zstd_encoder_drain_parallel_output(state, output)) {
      return GCOMP_ERR_LIMIT; // Terminator did not fit; call flush again.
    }

    // Only now is the frame really over, so only now may the next one be set
    // up.  Staging the new header before the terminator has left would put
    // the two in the buffer in the wrong order.
    status = zstd_encoder_rearm_parallel_frame(state);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "failed to start a new parallel frame");
    }
    return GCOMP_OK;
  }

  // The frame header comes before any block, so a flush before the first byte
  // of input still has this much to hand over.
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    (void)zstd_emit_staged(
        state->header_buf, &state->header_pos, state->header_len, output);
    if (state->header_pos < state->header_len) {
      return GCOMP_ERR_LIMIT;
    }
    state->stage = ZSTD_ENC_STAGE_BLOCKS;
  }

  if (mode == GCOMP_FLUSH_FULL) {
    // Ends the frame and starts another; see zstd_encoder_flush_end_frame().
    return zstd_encoder_flush_end_frame(encoder, state, output);
  }

  // A block staged by an earlier call -- by update(), or by a flush that ran
  // out of output -- goes first.  block_buffer_pos was zeroed when it was
  // compressed, so the branch below will not compress it a second time.
  if (state->compressed_buffer_pos < state->compressed_buffer_len) {
    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT;
    }
  }
  else if (state->block_buffer_pos > 0) {
    // An ordinary block, not the last one: the frame stays open.  This is
    // the same call update() makes when the block buffer fills, on a block
    // that happens to be short of full.
    uint8_t block_type;
    size_t compressed_len;
    size_t input_len = state->block_buffer_pos;
    gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
        input_len, state->compressed_buffer + 3,
        state->compressed_buffer_capacity - 3, &compressed_len, &block_type);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_ENC_STAGE_ERROR;
      return gcomp_encoder_set_error(
          encoder, status, "block compression failed");
    }

    uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
        ? (uint32_t)input_len
        : (uint32_t)compressed_len;
    zstd_write_block_header(
        state->compressed_buffer, false, block_type, header_size);
    state->compressed_buffer_len = 3 + compressed_len;
    state->compressed_buffer_pos = 0;
    state->block_buffer_pos = 0;

    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT;
    }
  }

  return GCOMP_OK;
}

//
// Finish
//

gcomp_status_t zstd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "encoder in error state");
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  state->finish_called = true;

  if (state->seekable) {
    return zstd_seek_finish(encoder, state, output);
  }

  // Parallel mode: use dedicated parallel finish path
  if (state->parallel_ctx) {
    return zstd_encoder_finish_parallel(encoder, state, output);
  }

  // Single-threaded mode: drain any remaining header
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    (void)zstd_emit_staged(
        state->header_buf, &state->header_pos, state->header_len, output);
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      // Need more output space. GCOMP_ERR_LIMIT, not GCOMP_OK:
      // gcomp_encoder_finish() documents GCOMP_OK as meaning the stream is
      // complete, so returning it here made a truncated stream
      // indistinguishable from a finished one.
      return GCOMP_ERR_LIMIT;
    }
  }

  // Flush remaining buffered data as final block
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // Drain pending compressed output
    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
    }

    if (!state->blocks_finished) {
      // Compress remaining data as final block
      if (state->block_buffer_pos > 0) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(
              encoder, status, "final block compression failed");
          return status;
        }

        // Write block header (last block)
        // For RLE blocks, the header size field is the regenerated size
        // (input_len) For RAW blocks, it's the compressed size (same as
        // input_len)
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, true, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;
        state->block_buffer_pos = 0;
      }
      else {
        // No remaining data - write empty last block
        zstd_write_block_header(
            state->compressed_buffer, true, ZSTD_BLOCK_TYPE_RAW, 0);
        state->compressed_buffer_len = 3;
        state->compressed_buffer_pos = 0;
      }

      state->blocks_finished = true;
    }

    // Output final block
    (void)zstd_emit_staged(state->compressed_buffer,
        &state->compressed_buffer_pos, state->compressed_buffer_len, output);
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
    }

    // Prepare checksum if enabled
    if (state->checksum_enabled) {
      uint64_t hash = gcomp_xxhash64_finalize(&state->content_hash);
      gcomp_write_le32(state->checksum_buf, (uint32_t)(hash & 0xFFFFFFFF));
      state->checksum_pos = 0;
      state->stage = ZSTD_ENC_STAGE_CHECKSUM;
    }
    else {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
  }

  // Output checksum
  if (state->stage == ZSTD_ENC_STAGE_CHECKSUM) {
    while (state->checksum_pos < ZSTD_CONTENT_CHECKSUM_SIZE &&
        output->used < output->size) {
      out_ptr[output->used++] = state->checksum_buf[state->checksum_pos++];
    }
    if (state->checksum_pos >= ZSTD_CONTENT_CHECKSUM_SIZE) {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
    return GCOMP_ERR_LIMIT; // Checksum partially written; call finish again.
  }

  if (state->stage != ZSTD_ENC_STAGE_DONE) {
    // Something above still has bytes to emit.
    return GCOMP_ERR_LIMIT;
  }
  return GCOMP_OK;
}

//
// Reset
//

gcomp_status_t zstd_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  gcomp_status_t status = GCOMP_OK;

  // Reset counters
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  // Seekable mode: the next stream is a new file, so the frames recorded for
  // the last one must go. Keeping them would append this stream's frames to the
  // previous table and describe a file that does not exist -- the same shape of
  // bug a726438 fixed in the parallel encoder, where reset left the held job's
  // overlap behind.
  if (state->seekable) {
    status = zstd_seek_rearm(state);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "could not re-arm the seek table");
    }
  }

  // Reset repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Reset content hash if enabled
  if (state->checksum_enabled) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Parallel mode reset
  if (state->parallel_ctx) {
    // Outstanding jobs have to go before the context can be reset, and
    // discarding them is exactly what a reset means: the stream they belong to
    // is being abandoned.
    //
    // Without this, `gcomp_encoder_reset()` returned GCOMP_ERR_INVALID_ARG
    // whenever any job was still with a worker - so a caller who had handed
    // over more than one job's worth of input and then changed their mind could
    // not reset at all, while the same call single-threaded succeeded. Two
    // things were wrong with that. It contradicts the documented contract, that
    // after a reset "the encoder behaves as if it was just created"; and
    // INVALID_ARG was the wrong code for it, because the argument was fine and
    // the state was busy. `gcomp_parallel_block_reset()` still refuses a
    // context with work outstanding, which is right - it is this layer's job to
    // make sure none is.
    //
    // The results are collected and freed rather than left for the sequencer,
    // because a job holds a match finder and buffers from the pool.
    status = zstd_parallel_wait(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "parallel wait failed during reset");
    }
    while (zstd_parallel_pending_count(state->parallel_ctx) > 0) {
      zstd_parallel_job_t * completed = NULL;
      status = zstd_parallel_get_result(state->parallel_ctx, &completed);
      if (status != GCOMP_OK || !completed) {
        return gcomp_encoder_set_error(encoder,
            status != GCOMP_OK ? status : GCOMP_ERR_INTERNAL,
            "could not drain a parallel job during reset");
      }
      zstd_parallel_free_job(state->parallel_ctx, completed);
    }
    // Anything those jobs had already staged belongs to the abandoned stream.
    state->parallel_output_buf_pos = 0;
    state->parallel_output_buf_len = 0;

    // Reset parallel context
    status = zstd_parallel_reset(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return status;
    }

    // Free and reallocate current job if needed
    if (state->parallel_job) {
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
    status = zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      return status;
    }
    zstd_encoder_seed_parallel_job(state);

    // A reset starts a new frame, so the header goes back at the front of the
    // output buffer exactly as it did at create time, and the terminator this
    // encoder appends after the last job is owed again.  Leaving either out
    // produced a second stream with no frame header and no final block --
    // headerless bytes that decoded to nothing.
    state->parallel_epilogue_staged = false;
    // A reset starts a new frame, so nothing precedes its first job.
    //
    // Both halves are needed. Clearing the saved overlap stops the *next* job
    // being seeded from the old stream; re-seeding the job that is still held
    // clears the overlap it was already given. Without the second, a job left
    // over from the previous stream kept that stream's bytes and its
    // overlap_len, and the first block of the new frame matched against data
    // the decoder had never seen - "Second parallel stream after reset"
    // encoded the words "parallel stream " as a match into the frame before
    // it, and decoded to nothing.
    //
    // That was invisible until short blocks with history started being
    // compressed at all: every input below MIN_COMPRESSION_SIZE used to be
    // stored raw, which hid the stale history rather than fixing it.
    zstd_encoder_seed_parallel_dictionary(state);
    zstd_encoder_seed_parallel_job(state);
    status = zstd_write_frame_header(&state->header, state->parallel_output_buf,
        state->parallel_output_buf_cap, &state->header_len);
    if (status != GCOMP_OK) {
      return status;
    }
    state->parallel_output_buf_pos = 0;
    state->parallel_output_buf_len = state->header_len;

    // Set stage to BLOCKS (parallel mode doesn't use HEADER stage)
    state->stage = ZSTD_ENC_STAGE_BLOCKS;
  }
  else {
    // Single-threaded mode reset
    state->stage = ZSTD_ENC_STAGE_HEADER;

    // A reset starts a new stream, so the record of what the previous one had
    // to settle for does not carry into it.
    memset(&state->stepdowns, 0, sizeof(state->stepdowns));

    // Reset positions (retain buffers)
    state->header_pos = 0;
    state->checksum_pos = 0;
    state->block_buffer_pos = 0;
    state->compressed_buffer_pos = 0;
    state->compressed_buffer_len = 0;

    // Reset match finder state (clear hash tables, not free), and with it the
    // window it indexes.  A reused encoder starts a new stream, so the
    // previous stream's bytes are not history for it - but a dictionary is,
    // so it goes back in just as it did at create time.
    if (state->match_finder) {
      zstd_mf_reset(state->match_finder);
    }
    state->mf_window_len = 0;
    if (state->mf_window && state->dict_parsed.content &&
        state->dict_parsed.content_size > 0) {
      size_t take = state->dict_parsed.content_size;
      const uint8_t * from = state->dict_parsed.content;
      if (take > state->mf_window_max) {
        from += take - state->mf_window_max;
        take = state->mf_window_max;
      }
      memcpy(state->mf_window, from, take);
      state->mf_window_len = take;
      zstd_mf_index_range(state->match_finder, state->mf_window, 0,
          state->mf_window_len, state->mf_window_len);
      zstd_mf_note_dictionary(state->match_finder, state->allocator,
          state->mf_window_len, &state->mem_tracker);
    }

    // Rebuild frame header
    status = zstd_write_frame_header(&state->header, state->header_buf,
        sizeof(state->header_buf), &state->header_len);
  }

  return status;
}
