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
 * @file xz_decode_par.c
 *
 * Decoding an xz stream with several threads, when it has several blocks.
 *
 * ## What is split, and how it is found
 *
 * Blocks are independent: each starts a fresh LZMA2 stream with a fresh
 * dictionary. Where they are, and how big, is in the index at the end, which
 * a decoder holding the whole stream can read first. So this reads the
 * footer, the index, and checks that the blocks it lists lie end to end
 * between the stream header and the index, and then decodes each block on a
 * worker.
 *
 * ## How a block is decoded
 *
 * A worker does not take the block apart. It is given a stream of one block
 * (the real stream header, the block, an index naming just that block, a
 * footer) and decodes that with the ordinary xz decoder, so the block header,
 * its filters, its padding, its check, and its sizes against the index are
 * all checked by the same code that checks them one thread at a time.
 *
 * ## Declining
 *
 * Anything this does not handle, and any failure of any kind, is declined
 * with ::GCOMP_ERR_UNSUPPORTED and the ordinary decoder is left to decode the
 * stream, so the error a bad stream gets, and the limit it hits, are the
 * ordinary ones. The cost of that is a corrupt stream being decoded twice.
 * Declined are: one thread, a stream of fewer than two blocks, several
 * streams, anything whose index does not agree with the footer, and output
 * that would not fit the caller's buffer or limits.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/block_job.h"
#include "../../core/parallel_block.h"
#include "../../core/registry_internal.h"
#include "xz_internal.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/xz.h>
#include <ghoti.io/cutil/safemath.h>

#include <string.h>

/// More blocks than this and the bookkeeping outweighs the threads.
#define XZ_DECODE_PARALLEL_MAX_BLOCKS (1u << 20)

typedef struct {
  gcomp_block_job_t base; /* first */
  gcomp_registry_t * registry;
  gcomp_options_t * options;
  uint8_t * stream; /* the stream of one block */
  size_t stream_size;
  uint8_t * out;
  size_t out_capacity, out_size;
  gcomp_status_t status;
} xz_dec_job_t;

static int xz_dec_job_run(void * ctx) {
  xz_dec_job_t * j = ctx;
  size_t produced = 0;
  gcomp_status_t s = gcomp_decode_buffer(j->registry, "xz", j->options,
      j->stream, j->stream_size, j->out, j->out_capacity, &produced);
  if (s == GCOMP_OK && produced != j->out_capacity) {
    s = GCOMP_ERR_CORRUPT;
  }
  j->out_size = s == GCOMP_OK ? produced : 0;
  j->status = s;
  return (int)s;
}

static int read_vli(const uint8_t * b, size_t n, size_t * pos, uint64_t * out) {
  xz_vli_t v = {0, 0};
  while (*pos < n) {
    const int r = xz_vli_feed(&v, b[(*pos)++]);
    if (r < 0) {
      return 0;
    }
    if (r > 0) {
      *out = v.value;
      return 1;
    }
  }
  return 0;
}

/* A stream of the one block at `block`: the real header, the block, an index
 * of one record, a footer. Returns its size; `out` has room for
 * header + padded + 64. */
static size_t make_stream(uint8_t * out, const uint8_t * hdr,
    const uint8_t * block, uint64_t unpadded, uint64_t unc) {
  const size_t padded = (size_t)((unpadded + 3u) & ~(uint64_t)3u);
  uint8_t * idx;
  size_t n = 1, idx_len;
  memcpy(out, hdr, XZ_STREAM_HEADER_SIZE);
  memcpy(out + XZ_STREAM_HEADER_SIZE, block, padded);
  idx = out + XZ_STREAM_HEADER_SIZE + padded;
  idx[0] = 0;
  n += xz_vli_put(idx + n, 1);
  n += xz_vli_put(idx + n, unpadded);
  n += xz_vli_put(idx + n, unc);
  while (n & 3u) {
    idx[n++] = 0;
  }
  xz_put_le32(idx + n, xz_crc32(idx, n));
  idx_len = n + 4u;
  idx += idx_len;
  xz_put_le32(idx + 4u, (uint32_t)(idx_len / 4u - 1u));
  idx[8] = hdr[6];
  idx[9] = hdr[7];
  xz_put_le32(idx, xz_crc32(idx + 4u, 6));
  idx[10] = 'Y';
  idx[11] = 'Z';
  return XZ_STREAM_HEADER_SIZE + padded + idx_len + XZ_STREAM_FOOTER_SIZE;
}

static void job_free(const gcomp_allocator_t * alloc, xz_dec_job_t * j) {
  gcomp_free(alloc, j->stream);
  gcomp_free(alloc, j->out);
  gcomp_free(alloc, j);
}

gcomp_status_t xz_decode_parallel(gcomp_registry_t * registry,
    gcomp_options_t * options, const void * input, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out) {
  const gcomp_allocator_t * alloc;
  const uint8_t * in = input;
  uint64_t threads = 1, total = 0, largest = 0, max_output, max_memory, max_ratio;
  uint64_t * rec = NULL;
  uint64_t count = 0, offset;
  size_t m, zeros = 0, backward, idx_off, at = 1, i;
  const uint8_t * hdr;
  const uint8_t * foot;
  gcomp_options_t * job_options = NULL;
  gcomp_parallel_block_ctx_t * ctx = NULL;
  gcomp_parallel_block_config_t cfg;
  gcomp_status_t result = GCOMP_OK;
  size_t submitted = 0, collected = 0, produced = 0;

  if (!registry || !input || !output || !output_size_out) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (options &&
      gcomp_options_get_uint64(options, "threads.count", &threads) != GCOMP_OK) {
    threads = 1;
  }
  if (threads <= 1) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  alloc = gcomp_registry_get_allocator(registry);

  /* One stream, possibly followed by padding of zeros in fours. */
  while (zeros < input_size && in[input_size - 1u - zeros] == 0) {
    zeros++;
  }
  if ((zeros & 3u) != 0 || input_size < XZ_STREAM_HEADER_SIZE +
          XZ_STREAM_FOOTER_SIZE + 8u + zeros) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  m = input_size - zeros;
  hdr = in;
  foot = in + m - XZ_STREAM_FOOTER_SIZE;
  if (memcmp(hdr, "\xFD" "7zXZ\0", 6) != 0 || hdr[6] != 0 ||
      xz_crc32(hdr + 6, 2) != xz_get_le32(hdr + 8) ||
      !xz_check_known(hdr[7]) || foot[10] != 'Y' || foot[11] != 'Z' ||
      xz_crc32(foot + 4, 6) != xz_get_le32(foot) || foot[8] != hdr[6] ||
      foot[9] != hdr[7]) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  backward = ((size_t)xz_get_le32(foot + 4) + 1u) * 4u;
  if (backward < 8u ||
      backward > m - XZ_STREAM_HEADER_SIZE - XZ_STREAM_FOOTER_SIZE) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  idx_off = m - XZ_STREAM_FOOTER_SIZE - backward;
  if (in[idx_off] != 0 || xz_crc32(in + idx_off, backward - 4u) !=
          xz_get_le32(in + idx_off + backward - 4u) ||
      !read_vli(in + idx_off, backward - 4u, &at, &count) || count < 2 ||
      count > XZ_DECODE_PARALLEL_MAX_BLOCKS) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  rec = gcomp_malloc(alloc, (size_t)count * 2u * sizeof(*rec));
  if (!rec) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  offset = XZ_STREAM_HEADER_SIZE;
  for (i = 0; i < count; i++) {
    uint64_t up, unc;
    if (!read_vli(in + idx_off, backward - 4u, &at, &up) ||
        !read_vli(in + idx_off, backward - 4u, &at, &unc) || up < 5 ||
        !gcu_safe_add_u64(total, unc, &total) ||
        !gcu_safe_add_u64(offset, (up + 3u) & ~(uint64_t)3u, &offset)) {
      goto decline;
    }
    /* The ordinary decoder checks the ratio as it goes; the workers are not
     * asked to, so look at every block boundary here. */
    max_ratio = gcomp_limits_read_expansion_ratio_max(
        options, GCOMP_XZ_MAX_EXPANSION_RATIO);
    if (gcomp_limits_check_expansion_ratio(offset, total, max_ratio) != GCOMP_OK) {
      goto decline;
    }
    rec[2u * i] = up;
    rec[2u * i + 1u] = unc;
    if (unc > largest) {
      largest = unc;
    }
  }
  /* What is left of the index is its padding, and it is zero. */
  for (i = at; i < backward - 4u; i++) {
    if (in[idx_off + i] != 0) {
      goto decline;
    }
  }
  max_output = gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  max_memory = gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);
  if (offset != idx_off || total > (uint64_t)output_capacity ||
      (max_output != 0 && total > max_output)) {
    goto decline;
  }

  if (options) {
    if (gcomp_options_clone(options, &job_options) != GCOMP_OK) {
      goto decline;
    }
  }
  else if (gcomp_options_create(&job_options) != GCOMP_OK) {
    goto decline;
  }
  if (gcomp_options_set_uint64(job_options, "threads.count", 1) != GCOMP_OK ||
      gcomp_options_set_uint64(job_options, "limits.max_expansion_ratio", 0) !=
          GCOMP_OK) {
    goto decline;
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.allocator = alloc;
  cfg.num_threads = (uint32_t)(threads > 64 ? 64 : threads);
  cfg.max_in_flight = cfg.num_threads * 2u;
  if (max_memory != 0 && largest > 0) {
    uint64_t fit = max_memory / largest;
    if (fit < 1) {
      fit = 1;
    }
    if (fit < cfg.max_in_flight) {
      cfg.max_in_flight = (uint32_t)fit;
    }
  }
  if (gcomp_parallel_block_create(&cfg, &ctx) != GCOMP_OK) {
    goto decline;
  }

  {
    uint8_t * out = output;
    uint64_t at_block = XZ_STREAM_HEADER_SIZE;
    size_t in_flight = 0;
    while (collected < count && result == GCOMP_OK) {
      while (submitted < count) {
        const uint64_t up = rec[2u * submitted], unc = rec[2u * submitted + 1u];
        xz_dec_job_t * j = gcomp_calloc(alloc, 1, sizeof(*j));
        gcomp_status_t sub;
        if (!j) {
          result = GCOMP_ERR_MEMORY;
          break;
        }
        j->stream = gcomp_malloc(
            alloc, XZ_STREAM_HEADER_SIZE + (size_t)up + 3u + 64u);
        j->out = gcomp_malloc(alloc, unc ? (size_t)unc : 1u);
        if (!j->stream || !j->out) {
          job_free(alloc, j);
          result = GCOMP_ERR_MEMORY;
          break;
        }
        j->stream_size = make_stream(j->stream, hdr, in + at_block, up, unc);
        j->out_capacity = (size_t)unc;
        j->registry = registry;
        j->options = job_options;
        sub = gcomp_parallel_block_try_submit(ctx, j, xz_dec_job_run);
        if (sub == GCOMP_ERR_LIMIT) {
          job_free(alloc, j);
          break; /* full: collect one first */
        }
        if (sub != GCOMP_OK) {
          job_free(alloc, j);
          result = sub;
          break;
        }
        at_block += (up + 3u) & ~(uint64_t)3u;
        submitted++;
        in_flight++;
      }
      if (result != GCOMP_OK) {
        break;
      }
      {
        gcomp_block_job_t * base = NULL;
        xz_dec_job_t * j;
        const gcomp_status_t got = gcomp_parallel_block_get_result(ctx, &base);
        if (!base) {
          result = got == GCOMP_OK ? GCOMP_ERR_INTERNAL : got;
          break;
        }
        j = (xz_dec_job_t *)base;
        collected++;
        in_flight--;
        if (j->status != GCOMP_OK) {
          result = j->status;
        }
        else {
          memcpy(out + produced, j->out, j->out_size);
          produced += j->out_size;
        }
        job_free(alloc, j);
      }
    }
    while (in_flight > 0) {
      gcomp_block_job_t * base = NULL;
      (void)gcomp_parallel_block_get_result(ctx, &base);
      if (!base) {
        break;
      }
      job_free(alloc, (xz_dec_job_t *)base);
      in_flight--;
    }
  }
  gcomp_parallel_block_wait(ctx);
  gcomp_parallel_block_destroy(ctx);
  gcomp_options_destroy(job_options);
  gcomp_free(alloc, rec);
  if (result != GCOMP_OK) {
    /* Whatever went wrong, the ordinary decoder gives the verdict. */
    *output_size_out = 0;
    return GCOMP_ERR_UNSUPPORTED;
  }
  *output_size_out = produced;
  return GCOMP_OK;

decline:
  gcomp_options_destroy(job_options);
  gcomp_free(alloc, rec);
  return GCOMP_ERR_UNSUPPORTED;
}
