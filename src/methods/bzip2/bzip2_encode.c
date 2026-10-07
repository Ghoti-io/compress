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
 * @file bzip2_encode.c
 *
 * The bzip2 encoder.
 *
 * ## The pipeline, per block
 *
 * Input is run-length coded as it arrives (four or more equal bytes become
 * four and a count) into a block of up to `level * 100000 - 19` bytes, with a
 * CRC kept over the bytes as they came in. A full block is sorted by the
 * Burrows-Wheeler transform (bzip2_bwt.c), move-to-front coded with runs of
 * zeros spelled in a two-symbol bijective code, and Huffman coded with two to
 * six tables chosen per group of 50 symbols. The tables are found the way
 * libbz2 finds them: start from a partition of the alphabet by frequency,
 * assign each group to the table that codes it cheapest, rebuild the tables
 * from what they were assigned, and do that four times.
 *
 * ## Threads
 *
 * Everything from the sort on is one function of a block and nothing else, so
 * blocks are coded in parallel: the caller's thread does the run-length pass
 * and the CRC, which is a cheap pass over the input and has to be in order, and
 * hands each full block to a worker as a job. A job owns every byte it touches,
 * including the scratch the sort allocates, which comes from an arena the job
 * carries, so a worker never calls the allocator the caller supplied (which
 * the caller may not have made thread-safe). Each job codes its block into a
 * bit string of its own; the caller's thread splices the strings into the
 * stream in order, which is the only place the blocks meet, since a block ends
 * in the middle of a byte and the next starts there. The output does not
 * depend on the number of threads: a block's bits are a function of the
 * block.
 *
 * ## Flush
 *
 * A bzip2 block ends in the middle of a byte and the next one starts there, so
 * there is no point inside a stream where its bytes stand alone. A flush
 * therefore ends the stream: the block in hand, then the end marker and the
 * stream CRC, then padding to a byte. The next byte fed in begins a new
 * stream, and concatenated streams are one stream to `bzip2 -d` and to this
 * library's decoder. A flush costs 14 bytes of header and footer, and a full
 * flush is the same as a sync flush because no block refers to another.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/huffman_lengths.h"
#include "../../core/parallel_block.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "bzip2_internal.h"

#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define ENC_ITERS 4
#define ENC_MAX_CODE_LEN 17

/* ---- bits, most significant first --------------------------------------- */

typedef struct bz_bits_s {
  uint8_t * buf;
  size_t len;
  uint64_t acc;
  unsigned nacc;
} bz_bits_t;

static inline void put(bz_bits_t * w, uint32_t v, unsigned n) {
  w->acc = (w->acc << n) | (v & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)));
  w->nacc += n;
  while (w->nacc >= 8) {
    w->nacc -= 8;
    w->buf[w->len++] = (uint8_t)(w->acc >> w->nacc);
  }
}

static void put48(bz_bits_t * w, uint64_t v) {
  put(w, (uint32_t)(v >> 24), 24);
  put(w, (uint32_t)(v & 0xFFFFFFu), 24);
}

static void put_pad(bz_bits_t * w) {
  if (w->nacc != 0) {
    put(w, 0, 8 - w->nacc);
  }
}

/* Append a bit string made elsewhere: its whole bytes, then the bits that did
 * not fill the last one. When this string ends mid-byte they are shifted in a
 * byte at a time, which is a small cost beside coding the block. */
static void put_bits(bz_bits_t * w, const bz_bits_t * from) {
  if (w->nacc == 0) {
    memcpy(w->buf + w->len, from->buf, from->len);
    w->len += from->len;
  }
  else {
    size_t i;
    for (i = 0; i < from->len; i++) {
      put(w, from->buf[i], 8);
    }
  }
  if (from->nacc != 0) {
    put(w, (uint32_t)(from->acc & ((1u << from->nacc) - 1u)), from->nacc);
  }
}

/* ---- memory a job owns --------------------------------------------------- */

/* A bump allocator over one block of memory, so that a worker's allocations
 * (the sort's scratch, the Huffman builder's) are the job's and not the
 * caller's. Each allocation carries a 16-byte header saying where the arena
 * stood before it and where it ended, and a free that finds the allocation at
 * the top gives the space back, which is the order the sort and the builder
 * release it in. Nothing else is ever freed, so nothing else is needed. */
typedef struct bz_arena_s {
  uint8_t * base;
  size_t cap;
  size_t off;
} bz_arena_t;

#define BZ_ARENA_HEADER 16u

static void * arena_malloc(void * ctx, size_t size) {
  bz_arena_t * a = (bz_arena_t *)ctx;
  size_t start = (a->off + 15u) & ~(size_t)15u;
  size_t p = start + BZ_ARENA_HEADER;
  size_t end;
  size_t * h;
  if (size > a->cap) {
    return NULL;
  }
  end = (p + size + 15u) & ~(size_t)15u;
  if (end > a->cap) {
    return NULL;
  }
  h = (size_t *)(void *)(a->base + start);
  h[0] = a->off;
  h[1] = end;
  a->off = end;
  return a->base + p;
}

static void * arena_calloc(void * ctx, size_t n, size_t size) {
  void * p;
  if (size != 0 && n > (size_t)-1 / size) {
    return NULL;
  }
  p = arena_malloc(ctx, n * size);
  if (p) {
    memset(p, 0, n * size);
  }
  return p;
}

static void * arena_realloc(void * ctx, void * ptr, size_t size) {
  (void)ctx;
  (void)ptr;
  (void)size;
  return NULL; /* nothing the encoder calls through the arena resizes */
}

static void arena_free(void * ctx, void * ptr) {
  bz_arena_t * a = (bz_arena_t *)ctx;
  size_t * h;
  if (!ptr) {
    return;
  }
  h = (size_t *)(void *)((uint8_t *)ptr - BZ_ARENA_HEADER);
  if (h[1] == a->off) {
    a->off = h[0];
  }
}

/* ---- one block ----------------------------------------------------------- */

typedef struct block_coding_s {
  unsigned n_in_use;
  uint8_t in_use[256];
  uint8_t unseq_to_seq[256];
  unsigned alpha;
  unsigned n_mtf;
  uint32_t freq[BZIP2_MAX_ALPHA];
  unsigned n_groups;
  uint8_t len[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  uint32_t code[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  unsigned n_selectors;
  uint8_t selector[BZIP2_MAX_SELECTORS];
} block_coding_t;

/* A block on its way through: the bytes the caller's thread filled, the
 * workspace a worker needs to code them, and the bits that come out. */
typedef struct bz_job_s {
  gcomp_block_job_t base; /* first, so a pointer to the job is one to this */
  struct bz_job_s * next_free;
  uint32_t nblock;
  uint32_t crc; /* the block CRC, finished */
  uint8_t * blk;
  uint8_t * last;
  uint16_t * mtfv;
  bzip2_bwt_scratch_t bwt;
  bz_arena_t arena;
  gcomp_allocator_t arena_alloc;
  bz_bits_t w;
  block_coding_t coding;
} bz_job_t;

/* Move-to-front the last column and spell the runs of zeros. */
static void make_mtf(bz_job_t * j) {
  block_coding_t * c = &j->coding;
  uint8_t yy[256];
  unsigned i, k, wr = 0;
  uint32_t z_pend = 0;
  const unsigned eob = c->n_in_use + 1;
  memset(c->freq, 0, sizeof(c->freq));
  for (i = 0; i < c->n_in_use; i++) {
    yy[i] = (uint8_t)i;
  }
  for (i = 0; i < j->nblock; i++) {
    uint8_t ll = c->unseq_to_seq[j->last[i]];
    if (yy[0] == ll) {
      z_pend++;
      continue;
    }
    if (z_pend > 0) {
      z_pend--;
      for (;;) {
        unsigned sym = (z_pend & 1u) ? BZIP2_RUNB : BZIP2_RUNA;
        j->mtfv[wr++] = (uint16_t)sym;
        c->freq[sym]++;
        if (z_pend < 2) {
          break;
        }
        z_pend = (z_pend - 2u) / 2u;
      }
      z_pend = 0;
    }
    for (k = 1; yy[k] != ll; k++) {
    }
    memmove(yy + 1, yy, k);
    yy[0] = ll;
    j->mtfv[wr++] = (uint16_t)(k + 1);
    c->freq[k + 1]++;
  }
  if (z_pend > 0) {
    z_pend--;
    for (;;) {
      unsigned sym = (z_pend & 1u) ? BZIP2_RUNB : BZIP2_RUNA;
      j->mtfv[wr++] = (uint16_t)sym;
      c->freq[sym]++;
      if (z_pend < 2) {
        break;
      }
      z_pend = (z_pend - 2u) / 2u;
    }
  }
  j->mtfv[wr++] = (uint16_t)eob;
  c->freq[eob]++;
  c->n_mtf = wr;
}

/* Code lengths of at most ENC_MAX_CODE_LEN for every symbol of the alphabet,
 * whether it occurs or not: a decoder reads a length for each. */
static int lengths_for(bz_job_t * j, const uint32_t * freq, unsigned alpha,
    uint8_t * len_out) {
  uint32_t f[BZIP2_MAX_ALPHA];
  unsigned i;
  for (i = 0; i < alpha; i++) {
    f[i] = freq[i] ? freq[i] : 1u;
  }
  return gcomp_huffman_code_lengths(
             &j->arena_alloc, f, alpha, ENC_MAX_CODE_LEN, len_out) == GCOMP_OK
      ? 0
      : -1;
}

/* What each of the (at most six) tables would spend on the group of symbols in
 * [lo, hi), into cost[0..5]. The lengths are held by symbol, six to a row, so
 * that one load gives a symbol's length in every table and the sums for all
 * the tables run together: with SSE2, eight at a time in 16-bit lanes (a
 * group is 50 symbols of at most 17 bits, 850, far inside a lane). */
static void group_costs(const uint8_t (*by_sym)[8], const uint16_t * mtfv,
    unsigned lo, unsigned hi, uint16_t * cost) {
#if defined(__SSE2__)
  __m128i acc = _mm_setzero_si128();
  const __m128i zero = _mm_setzero_si128();
  unsigned i;
  for (i = lo; i < hi; i++) {
    __m128i v = _mm_loadl_epi64((const __m128i *)(const void *)by_sym[mtfv[i]]);
    acc = _mm_add_epi16(acc, _mm_unpacklo_epi8(v, zero));
  }
  {
    uint16_t out[8];
    unsigned t;
    _mm_storeu_si128((__m128i *)(void *)out, acc);
    for (t = 0; t < BZIP2_MAX_GROUPS; t++) {
      cost[t] = out[t];
    }
  }
#else
  unsigned i, t;
  for (t = 0; t < BZIP2_MAX_GROUPS; t++) {
    cost[t] = 0;
  }
  for (i = lo; i < hi; i++) {
    for (t = 0; t < BZIP2_MAX_GROUPS; t++) {
      cost[t] = (uint16_t)(cost[t] + by_sym[mtfv[i]][t]);
    }
  }
#endif
}

/* The tables and the choice of table for each group of 50 symbols. */
static int make_tables(bz_job_t * j) {
  block_coding_t * c = &j->coding;
  const unsigned alpha = c->alpha;
  unsigned t, v, iter, g;
  uint32_t rfreq[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  uint8_t by_sym[BZIP2_MAX_ALPHA][8];
  c->n_groups = c->n_mtf < 200 ? 2 : c->n_mtf < 600 ? 3 : c->n_mtf < 1200 ? 4
      : c->n_mtf < 2400                                ? 5
                                                       : 6;
  /* A start: split the alphabet into n_groups ranges of about equal
   * frequency, and give each table short codes for its own range. */
  {
    uint32_t rem = c->n_mtf;
    unsigned part = c->n_groups, gs = 0;
    while (part > 0) {
      uint32_t target = rem / part, acc = 0;
      int ge = (int)gs - 1;
      while (acc < target && ge < (int)alpha - 1) {
        ge++;
        acc += c->freq[ge];
      }
      for (v = 0; v < alpha; v++) {
        c->len[part - 1][v] = (v >= gs && (int)v <= ge) ? 1 : 15;
      }
      gs = (unsigned)ge + 1;
      rem -= acc;
      part--;
    }
  }
  c->n_selectors = (c->n_mtf + BZIP2_GROUP_SIZE - 1) / BZIP2_GROUP_SIZE;
  memset(by_sym, 0, sizeof(by_sym));
  for (iter = 0; iter < ENC_ITERS; iter++) {
    for (v = 0; v < alpha; v++) {
      for (t = 0; t < c->n_groups; t++) {
        by_sym[v][t] = c->len[t][v];
      }
    }
    memset(rfreq, 0, sizeof(rfreq));
    for (g = 0; g < c->n_selectors; g++) {
      unsigned lo = g * BZIP2_GROUP_SIZE;
      unsigned hi = lo + BZIP2_GROUP_SIZE;
      unsigned best = 0, i;
      uint16_t cost[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      if (hi > c->n_mtf) {
        hi = c->n_mtf;
      }
      group_costs((const uint8_t(*)[8])by_sym, j->mtfv, lo, hi, cost);
      for (t = 1; t < c->n_groups; t++) {
        if (cost[t] < cost[best]) {
          best = t;
        }
      }
      c->selector[g] = (uint8_t)best;
      for (i = lo; i < hi; i++) {
        rfreq[best][j->mtfv[i]]++;
      }
    }
    for (t = 0; t < c->n_groups; t++) {
      if (lengths_for(j, rfreq[t], alpha, c->len[t]) != 0) {
        return -1;
      }
    }
  }
  /* Canonical codes: by length, then by symbol, as the decoder assigns them. */
  for (t = 0; t < c->n_groups; t++) {
    unsigned min_len = 32, max_len = 0, n;
    uint32_t vec = 0;
    for (v = 0; v < alpha; v++) {
      if (c->len[t][v] > max_len) {
        max_len = c->len[t][v];
      }
      if (c->len[t][v] < min_len) {
        min_len = c->len[t][v];
      }
    }
    for (n = min_len; n <= max_len; n++) {
      for (v = 0; v < alpha; v++) {
        if (c->len[t][v] == n) {
          c->code[t][v] = vec++;
        }
      }
      vec <<= 1;
    }
  }
  return 0;
}

/* Everything between the block's magic and its first symbol, and the symbols. */
static void write_block_body(bz_job_t * j, uint32_t orig) {
  block_coding_t * c = &j->coding;
  bz_bits_t * w = &j->w;
  unsigned i, t, g, v;
  uint16_t groups = 0;
  put(w, 0, 1); /* not randomised */
  put(w, orig, 24);
  for (i = 0; i < 16; i++) {
    for (v = 0; v < 16; v++) {
      if (c->in_use[i * 16 + v]) {
        groups |= (uint16_t)(0x8000u >> i);
        break;
      }
    }
  }
  put(w, groups, 16);
  for (i = 0; i < 16; i++) {
    if (groups & (0x8000u >> i)) {
      uint32_t bits = 0;
      for (v = 0; v < 16; v++) {
        if (c->in_use[i * 16 + v]) {
          bits |= 0x8000u >> v;
        }
      }
      put(w, bits, 16);
    }
  }
  put(w, c->n_groups, 3);
  put(w, c->n_selectors, 15);
  {
    uint8_t pos[BZIP2_MAX_GROUPS];
    /* All of them, not just n_groups: GCC at -O3 under TSan cannot see the
     * bound and takes the loop for an overflow. */
    for (i = 0; i < BZIP2_MAX_GROUPS; i++) {
      pos[i] = (uint8_t)i;
    }
    for (g = 0; g < c->n_selectors; g++) {
      uint8_t sel = c->selector[g];
      unsigned k = 0;
      while (pos[k] != sel) {
        k++;
      }
      for (i = 0; i < k; i++) {
        put(w, 1, 1);
      }
      put(w, 0, 1);
      memmove(pos + 1, pos, k);
      pos[0] = sel;
    }
  }
  for (t = 0; t < c->n_groups; t++) {
    unsigned curr = c->len[t][0];
    put(w, curr, 5);
    for (v = 0; v < c->alpha; v++) {
      while (curr < c->len[t][v]) {
        put(w, 2, 2);
        curr++;
      }
      while (curr > c->len[t][v]) {
        put(w, 3, 2);
        curr--;
      }
      put(w, 0, 1);
    }
  }
  for (i = 0; i < c->n_mtf; i++) {
    unsigned tbl = c->selector[i / BZIP2_GROUP_SIZE];
    uint16_t sym = j->mtfv[i];
    put(w, c->code[tbl][sym], c->len[tbl][sym]);
  }
}

/* Code the block a job holds into its bit string: the magic, the CRC, and the
 * body. This is the whole of the work that runs on a worker, and it touches
 * nothing but the job. */
static gcomp_status_t code_block(bz_job_t * j) {
  block_coding_t * c = &j->coding;
  uint32_t orig = 0;
  unsigned i;
  memset(c, 0, sizeof(*c));
  j->w.len = 0;
  j->w.acc = 0;
  j->w.nacc = 0;
  j->arena.off = 0;
  if (bzip2_bwt(&j->arena_alloc, &j->bwt, j->blk, j->nblock, j->last, &orig) != 0) {
    return GCOMP_ERR_MEMORY;
  }
  for (i = 0; i < j->nblock; i++) {
    c->in_use[j->blk[i]] = 1;
  }
  for (i = 0; i < 256; i++) {
    if (c->in_use[i]) {
      c->unseq_to_seq[i] = (uint8_t)c->n_in_use++;
    }
  }
  c->alpha = c->n_in_use + 2;
  make_mtf(j);
  if (make_tables(j) != 0) {
    return GCOMP_ERR_MEMORY;
  }
  put48(&j->w, BZIP2_BLOCK_MAGIC);
  put(&j->w, j->crc, 32);
  write_block_body(j, orig);
  return GCOMP_OK;
}

static int process_job(void * job_ctx) {
  bz_job_t * j = (bz_job_t *)job_ctx;
  gcomp_status_t s = code_block(j);
  j->base.result = s;
  return (int)s;
}

/* ---- the encoder --------------------------------------------------------- */

typedef struct bz_enc_s {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  unsigned level;
  uint32_t block_max; /* bytes of block, after the first run-length pass */

  /* the first run-length pass, and the job whose block it is filling */
  bz_job_t * cur;
  uint8_t * blk;
  uint32_t nblock;
  uint32_t block_crc;
  int cur_full; /* the block is full and has not been handed on */
  int run_ch;
  unsigned run_len;

  uint32_t combined;
  int in_stream; /* the header has been written and the end has not */
  int closing;   /* the last block of the stream has been handed on */
  unsigned streams; /* streams ended so far: a flush may have ended some */
  int finished;

  /* the jobs: all of them, those free to fill, and those out being coded */
  gcomp_parallel_block_ctx_t * pb; /* NULL: coded on this thread */
  bz_job_t ** jobs;
  unsigned jobs_made;
  unsigned jobs_max;
  bz_job_t * free_list;
  unsigned inflight;
  unsigned max_inflight;

  /* the bits */
  bz_bits_t w;
  uint8_t * stage;
  size_t stage_cap;
  size_t stage_pos;
} bz_enc_t;

/* ---- jobs ---------------------------------------------------------------- */

/* The most the sort's scratch and the Huffman builder's can come to for a block
 * of n bytes: the sort holds a byte per symbol of each level of its recursion
 * (2n + 1, halving) and a pair of bucket arrays per level of the size of that
 * level's alphabet, which is at most its length, so under 8 bytes a symbol;
 * and the builder a few hundred kilobytes. Most of it is never touched, and
 * untouched pages cost nothing. */
static size_t arena_bytes(uint32_t n) {
  return (size_t)n * 20u + 2048u + 786432u;
}

static void job_destroy(const gcomp_allocator_t * alloc, bz_job_t * j) {
  if (!j) {
    return;
  }
  gcomp_free(alloc, j->blk);
  gcomp_free(alloc, j->last);
  gcomp_free(alloc, j->mtfv);
  gcomp_free(alloc, j->bwt.s);
  gcomp_free(alloc, j->bwt.sa);
  gcomp_free(alloc, j->w.buf);
  gcomp_free(alloc, j->arena.base);
  gcomp_free(alloc, j);
}

static bz_job_t * job_create(bz_enc_t * e) {
  const size_t n = e->block_max;
  bz_job_t * j = gcomp_calloc(e->alloc, 1, sizeof(*j));
  if (!j) {
    return NULL;
  }
  j->blk = gcomp_malloc(e->alloc, n + 8u);
  j->last = gcomp_malloc(e->alloc, n + 8u);
  j->mtfv = gcomp_malloc(e->alloc, (n + 8u) * sizeof(uint16_t));
  j->bwt.s = gcomp_malloc(e->alloc, (2u * n + 8u) * sizeof(uint16_t));
  j->bwt.sa = gcomp_malloc(e->alloc, (2u * n + 8u) * sizeof(int32_t));
  /* One block's worst case: at most n + 1 symbols of at most 17 bits, and the
   * tables and selectors, which together are under 25 KB. */
  j->w.buf = gcomp_malloc(e->alloc, e->stage_cap);
  j->arena.cap = arena_bytes((uint32_t)n);
  j->arena.base = gcomp_malloc(e->alloc, j->arena.cap);
  if (!j->blk || !j->last || !j->mtfv || !j->bwt.s || !j->bwt.sa || !j->w.buf ||
      !j->arena.base) {
    job_destroy(e->alloc, j);
    return NULL;
  }
  j->arena_alloc.ctx = &j->arena;
  j->arena_alloc.malloc_fn = arena_malloc;
  j->arena_alloc.calloc_fn = arena_calloc;
  j->arena_alloc.realloc_fn = arena_realloc;
  j->arena_alloc.free_fn = arena_free;
  return j;
}

/* A job to fill. Threaded, there is always one: the pool is one larger than the
 * number that may be out being coded. */
static bz_job_t * take_job(bz_enc_t * e) {
  bz_job_t * j = e->free_list;
  if (j) {
    e->free_list = j->next_free;
    j->next_free = NULL;
    return j;
  }
  if (e->jobs_made < e->jobs_max) {
    j = job_create(e);
    if (j) {
      e->jobs[e->jobs_made++] = j;
    }
  }
  return j;
}

static void give_job(bz_enc_t * e, bz_job_t * j) {
  j->next_free = e->free_list;
  e->free_list = j;
}

/* ---- the first run-length pass ------------------------------------------- */

/* The run in hand becomes bytes of the block: up to three as they are, four or
 * more as four and a count of the rest. */
static void flush_run(bz_enc_t * e) {
  unsigned i;
  if (e->run_len == 0) {
    return;
  }
  for (i = 0; i < e->run_len; i++) {
    e->block_crc = bzip2_crc_update(e->block_crc, (uint8_t)e->run_ch);
  }
  if (e->run_len >= 4) {
    for (i = 0; i < 4; i++) {
      e->blk[e->nblock++] = (uint8_t)e->run_ch;
    }
    e->blk[e->nblock++] = (uint8_t)(e->run_len - 4u);
  }
  else {
    for (i = 0; i < e->run_len; i++) {
      e->blk[e->nblock++] = (uint8_t)e->run_ch;
    }
  }
  e->run_len = 0;
}

static inline void add_byte(bz_enc_t * e, uint8_t b) {
  if (e->run_len > 0 && b == (uint8_t)e->run_ch && e->run_len < 255) {
    e->run_len++;
    return;
  }
  flush_run(e);
  e->run_ch = b;
  e->run_len = 1;
}

/* ---- the stream ---------------------------------------------------------- */

static void begin_stream(bz_enc_t * e) {
  put(&e->w, 'B', 8);
  put(&e->w, 'Z', 8);
  put(&e->w, 'h', 8);
  put(&e->w, '0' + e->level, 8);
  e->combined = 0;
  e->in_stream = 1;
}

static void end_stream(bz_enc_t * e) {
  put48(&e->w, BZIP2_END_MAGIC);
  put(&e->w, e->combined, 32);
  put_pad(&e->w);
  e->in_stream = 0;
  e->streams++;
}

static void drain(bz_enc_t * e, gcomp_buffer_t * out) {
  size_t avail = e->w.len - e->stage_pos;
  size_t room = out->size - out->used;
  size_t n = avail < room ? avail : room;
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->stage + e->stage_pos, n);
    out->used += n;
    e->stage_pos += n;
  }
  if (e->stage_pos == e->w.len) {
    e->stage_pos = 0;
    e->w.len = 0;
  }
}

/* The block a job has coded joins the stream. The stage must be empty: it is
 * sized for one block. */
static gcomp_status_t land(bz_enc_t * e, bz_job_t * j) {
  if (j->base.result != GCOMP_OK) {
    return gcomp_encoder_set_error(e->pub, (gcomp_status_t)j->base.result,
        "bzip2: a block could not be coded");
  }
  put_bits(&e->w, &j->w);
  e->combined = bzip2_combine(e->combined, j->crc);
  return GCOMP_OK;
}

/* Take the next finished block, waiting for it, and put it in the stream. The
 * stage is empty. */
static gcomp_status_t collect_one(bz_enc_t * e) {
  gcomp_block_job_t * base = NULL;
  bz_job_t * j;
  gcomp_status_t s;
  s = gcomp_parallel_block_get_result(e->pb, &base);
  if (!base) {
    return s != GCOMP_OK ? gcomp_encoder_set_error(e->pub, s,
                               "bzip2: the block queue failed")
                         : GCOMP_ERR_INTERNAL;
  }
  j = (bz_job_t *)base;
  e->inflight--;
  s = land(e, j);
  give_job(e, j);
  return s;
}

/* Hand the block in hand on to be coded; in line, code it now and land it. The
 * stage is empty when this is called, and a threaded caller has seen to it that
 * there is room for one more job in flight. */
static gcomp_status_t submit_cur(bz_enc_t * e) {
  bz_job_t * j = e->cur;
  gcomp_status_t s;
  j->nblock = e->nblock;
  j->crc = ~e->block_crc;
  e->nblock = 0;
  e->block_crc = 0xFFFFFFFFu;
  e->cur_full = 0;
  if (!e->pb) {
    s = (gcomp_status_t)process_job(j);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          e->pub, s, "bzip2: a block could not be coded");
    }
    return land(e, j);
  }
  e->cur = NULL;
  e->blk = NULL;
  j->base.input = j->blk;
  j->base.input_size = j->nblock;
  s = gcomp_parallel_block_try_submit(e->pb, j, process_job);
  if (s != GCOMP_OK) {
    give_job(e, j);
    return gcomp_encoder_set_error(e->pub, s, "bzip2: could not queue a block");
  }
  e->inflight++;
  return GCOMP_OK;
}

/* Make sure a job is there to fill. */
static gcomp_status_t ensure_cur(bz_enc_t * e) {
  if (e->cur) {
    return GCOMP_OK;
  }
  e->cur = take_job(e);
  if (!e->cur) {
    return gcomp_encoder_set_error(
        e->pub, GCOMP_ERR_MEMORY, "bzip2: out of memory for a block");
  }
  e->blk = e->cur->blk;
  return GCOMP_OK;
}

/* ---- the API ------------------------------------------------------------- */

static void encoder_free(bz_enc_t * e) {
  unsigned i;
  if (!e) {
    return;
  }
  if (e->pb) {
    gcomp_parallel_block_destroy(e->pb); /* waits for the jobs out first */
  }
  for (i = 0; i < e->jobs_made; i++) {
    job_destroy(e->alloc, e->jobs[i]);
  }
  gcomp_free(e->alloc, e->jobs);
  gcomp_free(e->alloc, e->stage);
  gcomp_free(e->alloc, e);
}

gcomp_status_t bzip2_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  bz_enc_t * e;
  int64_t level = 9;
  uint64_t threads = 1, mem = 0;
  /* The ranges are the schema's: gcomp_encoder_create() has validated them. */
  if (options) {
    (void)gcomp_options_get_int64(options, "bzip2.level", &level);
    (void)gcomp_options_get_uint64(options, "threads.count", &threads);
    (void)gcomp_options_get_uint64(options, "limits.max_memory_bytes", &mem);
  }
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "bzip2: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->level = (unsigned)level;
  e->block_max = e->level * BZIP2_BLOCK_UNIT - 19u;
  e->stage_cap = (size_t)e->block_max * 9u / 4u + 65536u;
  if (threads > 64) {
    threads = 64;
  }
  if (threads > 1 && mem != 0) {
    /* A job holds about 29 bytes for each byte of block. Fewer threads than
     * asked for if the budget will not stretch, but never fewer than one. */
    uint64_t per_job = (uint64_t)e->block_max * 38u;
    uint64_t fit = mem / per_job;
    if (fit < 2) {
      threads = 1;
    }
    else if (threads + 1 > fit) {
      threads = fit - 1;
    }
  }
  e->stage = gcomp_malloc(alloc, e->stage_cap);
  e->jobs_max = threads > 1 ? (unsigned)threads + 1u : 1u;
  e->max_inflight = threads > 1 ? (unsigned)threads : 0u;
  e->jobs = gcomp_calloc(alloc, e->jobs_max, sizeof(*e->jobs));
  if (!e->stage || !e->jobs) {
    goto fail;
  }
  e->w.buf = e->stage;
  e->block_crc = 0xFFFFFFFFu;
  if (threads > 1) {
    gcomp_parallel_block_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = alloc;
    cfg.num_threads = (uint32_t)threads;
    cfg.max_in_flight = (uint32_t)threads;
    if (gcomp_parallel_block_create(&cfg, &e->pb) != GCOMP_OK) {
      goto fail;
    }
  }
  /* The first job is made now, so that a one-thread encoder fails here, where
   * the caller expects a memory failure, and not on its first byte. */
  if (ensure_cur(e) != GCOMP_OK) {
    goto fail;
  }
  encoder->method_state = e;
  return GCOMP_OK;
fail:
  encoder_free(e);
  return gcomp_encoder_set_error(
      encoder, GCOMP_ERR_MEMORY, "bzip2: out of memory for the encoder");
}

void bzip2_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }
  encoder_free(encoder->method_state);
  encoder->method_state = NULL;
}

/* A job to fill, waiting for one to come back from a worker if every one is
 * out. LIMIT: the stage holds bits, and a block cannot be collected into it
 * until the caller has taken them; nothing has changed, so call again. */
static gcomp_status_t need_cur(bz_enc_t * e) {
  while (!e->cur) {
    gcomp_status_t s;
    if (e->pb && !e->free_list && e->jobs_made >= e->jobs_max) {
      if (e->w.len != 0) {
        return GCOMP_ERR_LIMIT;
      }
      s = collect_one(e);
    }
    else {
      s = ensure_cur(e);
    }
    if (s != GCOMP_OK) {
      return s;
    }
  }
  return GCOMP_OK;
}

gcomp_status_t bzip2_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  bz_enc_t * e = encoder->method_state;
  const uint8_t * src;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "bzip2: encoder update after finish");
  }
  src = (const uint8_t *)input->data;
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->w.len != 0) {
      return GCOMP_OK;
    }
    /* The stage is empty, so a finished block can go into it; take what is
     * ready, in order, before doing anything else. */
    if (e->pb && e->inflight > 0 && gcomp_parallel_block_result_ready(e->pb)) {
      s = collect_one(e);
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (e->cur_full) {
      if (e->pb && e->inflight >= e->max_inflight) {
        s = collect_one(e);
      }
      else {
        s = submit_cur(e);
      }
      if (s != GCOMP_OK) {
        return s;
      }
      continue;
    }
    if (input->used >= input->size) {
      return GCOMP_OK;
    }
    if (!e->in_stream) {
      begin_stream(e);
      continue;
    }
    s = need_cur(e);
    if (s != GCOMP_OK) {
      return s;
    }
    /* Fill the block; a run adds at most five bytes, and the block keeps 19
     * back for that. */
    while (input->used < input->size && e->nblock < e->block_max) {
      add_byte(e, src[input->used++]);
    }
    if (e->nblock >= e->block_max) {
      e->cur_full = 1;
    }
  }
}

/* End the current stream, if one is open, with whatever is pending. GCOMP_OK:
 * the stream is ended and its last bytes are in the stage. GCOMP_ERR_LIMIT:
 * the stage has to be emptied before this can go on, so empty it and call
 * again; each step is done once however often it is called. */
static gcomp_status_t close_stream(bz_enc_t * e) {
  gcomp_status_t s;
  if (!e->in_stream) {
    return GCOMP_OK;
  }
  if (!e->closing) {
    if (e->run_len > 0) {
      s = need_cur(e);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    flush_run(e);
    if (e->nblock > 0) {
      if (e->pb ? e->inflight >= e->max_inflight : e->w.len != 0) {
        if (e->w.len != 0) {
          return GCOMP_ERR_LIMIT;
        }
        s = collect_one(e);
        if (s != GCOMP_OK) {
          return s;
        }
      }
      s = submit_cur(e);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    e->closing = 1;
  }
  while (e->inflight > 0) {
    if (e->w.len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    s = collect_one(e);
    if (s != GCOMP_OK) {
      return s;
    }
  }
  end_stream(e);
  e->closing = 0;
  return GCOMP_OK;
}

gcomp_status_t bzip2_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  bz_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->w.len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (e->finished) {
      return GCOMP_OK;
    }
    if (!e->in_stream && e->streams == 0) {
      /* No input at all is still a stream: the header and the end. Input that
       * a flush has already written out is not owed another. */
      begin_stream(e);
      continue;
    }
    s = close_stream(e);
    if (s == GCOMP_OK) {
      e->finished = 1;
    }
    else if (s != GCOMP_ERR_LIMIT) {
      return s;
    }
  }
}

gcomp_status_t bzip2_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  bz_enc_t * e = encoder->method_state;
  (void)mode;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "bzip2: encoder cannot flush after finish");
  }
  for (;;) {
    gcomp_status_t s;
    drain(e, output);
    if (e->w.len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (!e->in_stream) {
      return GCOMP_OK;
    }
    s = close_stream(e);
    if (s != GCOMP_OK && s != GCOMP_ERR_LIMIT) {
      return s;
    }
  }
}

gcomp_status_t bzip2_encoder_reset(gcomp_encoder_t * encoder) {
  bz_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  e = encoder->method_state;
  while (e->pb && e->inflight > 0) {
    gcomp_block_job_t * base = NULL;
    (void)gcomp_parallel_block_get_result(e->pb, &base);
    if (!base) {
      break;
    }
    e->inflight--;
    give_job(e, (bz_job_t *)base);
  }
  e->inflight = 0;
  e->nblock = 0;
  e->block_crc = 0xFFFFFFFFu;
  e->cur_full = 0;
  e->closing = 0;
  e->run_len = 0;
  e->combined = 0;
  e->in_stream = 0;
  e->streams = 0;
  e->finished = 0;
  e->w.len = 0;
  e->w.acc = 0;
  e->w.nacc = 0;
  e->stage_pos = 0;
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
