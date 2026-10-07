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
 * @file bzip2_decode.c
 *
 * The bzip2 decoder.
 *
 * ## Shape
 *
 * bzip2 cannot produce a byte of a block until it has read all of it: the
 * Burrows-Wheeler transform is undone from the last column, and the last
 * column is the whole block's symbols. So decoding has two halves with a wall
 * between them. Before it, the stream is bits: a header, two tables, and the
 * block's Huffman-coded symbols, which this decoder reads as they come and
 * parks in an array of 32-bit words. After it, the block is bytes: the array
 * is turned into a linked list through the sorted first column, and walking
 * the list yields the block in order, which then has its first run-length pass
 * undone on the way out.
 *
 * ## Resumable by state, not by retrying
 *
 * The bit half is a state machine. Every field that has to be read whole is
 * read only when the bit buffer holds all of it, and every loop over fields
 * (the symbol map, the selectors, the code lengths, the symbols) keeps its
 * counters in the decoder, so a call that runs out of input stops where it is
 * and the next call carries on. Nothing is read twice and nothing is rewound.
 * A symbol is only decoded when 20 bits, the longest a code can be, are in
 * hand; a valid stream always has at least the 80 bits of its end marker after
 * its last symbol, so that never refuses one.
 *
 * ## What is not read
 *
 * A block can carry a bit saying its bytes were randomised before sorting. It
 * is a defence against a worst case of the 1990s sort that bzip2 0.9.5 removed
 * and no encoder has set since; it needs a 512-entry table of the original's
 * and is refused with GCOMP_ERR_UNSUPPORTED rather than guessed at.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "bzip2_internal.h"

#include <ghoti.io/compress/limits.h>

#include <string.h>

typedef enum {
  ST_STREAM_HEADER,
  ST_BLOCK_MAGIC,
  ST_STREAM_CRC,
  ST_BLOCK_CRC,
  ST_BLOCK_HEAD,
  ST_MAP_GROUPS,
  ST_MAP_BYTES,
  ST_NGROUPS,
  ST_NSEL,
  ST_SEL,
  ST_TABLE_START,
  ST_TABLE_DELTA,
  ST_DATA,
  ST_OUTPUT
} bz_state_t;

typedef enum { BLOCKED_INPUT = 1, BLOCKED_OUTPUT = 2 } bz_blocked_t;

typedef struct bz_dec_s {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  bz_state_t state;
  int streams_done;
  int failed;
  gcomp_status_t err;

  /* bits, most significant first, left-aligned in buf */
  uint64_t buf;
  unsigned cnt;

  unsigned level;
  uint32_t block_max;
  uint32_t * tt;
  size_t tt_cap; /* in words */

  uint32_t block_crc_stored;
  uint32_t block_crc;
  uint32_t combined;
  uint32_t orig_ptr;

  uint16_t used_groups;
  unsigned map_i;
  uint8_t seq_to_unseq[256];
  unsigned n_in_use;
  unsigned alpha;

  unsigned n_groups;
  unsigned n_sel;       /* as the stream states it */
  unsigned n_selectors; /* as many as are kept */
  uint8_t selector_mtf[BZIP2_MAX_SELECTORS];
  uint8_t selector[BZIP2_MAX_SELECTORS];
  unsigned sel_i, sel_j;

  uint8_t len[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  unsigned tab_t, tab_i;
  int curr;
  int32_t limit[BZIP2_MAX_GROUPS][BZIP2_MAX_CODE_LEN + 3];
  int32_t base[BZIP2_MAX_GROUPS][BZIP2_MAX_CODE_LEN + 3];
  uint16_t perm[BZIP2_MAX_GROUPS][BZIP2_MAX_ALPHA];
  unsigned min_len[BZIP2_MAX_GROUPS];
  unsigned max_len[BZIP2_MAX_GROUPS];

  int group_no;
  unsigned group_pos;
  unsigned cur_table;
  uint8_t mtf[256];
  uint32_t unzftab[256];
  uint32_t nblock;
  int in_run;
  uint32_t run_es;
  uint32_t run_n;

  uint32_t cftab[257];
  uint32_t t_pos;
  uint32_t used;
  uint8_t prev;
  unsigned run;
  uint32_t pend_n;
  uint8_t pend_b;

  uint64_t max_out, max_mem, max_ratio;
  uint64_t produced, in_total;
} bz_dec_t;

/* A pointer to nothing that is still a pointer: arithmetic on a null one, even
 * by zero, is undefined, and finish() has no input. */
static const uint8_t g_no_input[1] = {0};

static void refill(bz_dec_t * d, const uint8_t ** in, const uint8_t * end) {
  while (d->cnt <= 56 && *in < end) {
    d->buf |= (uint64_t)*(*in)++ << (56 - d->cnt);
    d->cnt += 8;
    d->in_total++;
  }
}

static inline uint32_t peek(const bz_dec_t * d, unsigned n) {
  return (uint32_t)(d->buf >> (64 - n));
}

static inline void drop(bz_dec_t * d, unsigned n) {
  d->buf <<= n;
  d->cnt -= n;
}

static gcomp_status_t dec_fail(
    bz_dec_t * d, gcomp_status_t status, const char * msg) {
  d->failed = 1;
  d->err = gcomp_decoder_set_error(d->pub, status, "%s", msg);
  return d->err;
}

static gcomp_status_t corrupt(bz_dec_t * d, const char * msg) {
  return dec_fail(d, GCOMP_ERR_CORRUPT, msg);
}

/* Canonical decode tables, as libbz2 builds them: for each code length the
 * last code of that length, the offset from a code to its place in perm, and
 * perm itself, the symbols in code order. */
static void make_tables(bz_dec_t * d, unsigned t) {
  const uint8_t * length = d->len[t];
  unsigned alpha = d->alpha;
  unsigned min_len = 32, max_len = 0;
  unsigned i, j, pp = 0;
  int32_t vec;
  int32_t * limit = d->limit[t];
  int32_t * base = d->base[t];
  for (i = 0; i < alpha; i++) {
    if (length[i] > max_len) {
      max_len = length[i];
    }
    if (length[i] < min_len) {
      min_len = length[i];
    }
  }
  d->min_len[t] = min_len;
  d->max_len[t] = max_len;
  memset(d->perm[t], 0, sizeof(d->perm[t]));
  for (i = min_len; i <= max_len; i++) {
    for (j = 0; j < alpha; j++) {
      if (length[j] == i) {
        d->perm[t][pp++] = (uint16_t)j;
      }
    }
  }
  for (i = 0; i < BZIP2_MAX_CODE_LEN + 3; i++) {
    base[i] = 0;
    limit[i] = 0;
  }
  for (i = 0; i < alpha; i++) {
    base[length[i] + 1]++;
  }
  for (i = 1; i < BZIP2_MAX_CODE_LEN + 3; i++) {
    base[i] += base[i - 1];
  }
  vec = 0;
  for (i = min_len; i <= max_len; i++) {
    vec += base[i + 1] - base[i];
    limit[i] = vec - 1;
    vec <<= 1;
  }
  for (i = min_len + 1; i <= max_len; i++) {
    base[i] = ((limit[i - 1] + 1) << 1) - base[i];
  }
}

static gcomp_status_t ensure_tt(bz_dec_t * d, uint32_t block_max) {
  uint32_t * nt;
  if (d->tt_cap >= block_max) {
    return GCOMP_OK;
  }
  if (gcomp_limits_check_memory((size_t)block_max * sizeof(uint32_t),
          d->max_mem) != GCOMP_OK) {
    return dec_fail(d, GCOMP_ERR_LIMIT,
        "bzip2: block memory exceeds limits.max_memory_bytes");
  }
  nt = gcomp_realloc(d->alloc, d->tt, (size_t)block_max * sizeof(uint32_t));
  if (!nt) {
    return dec_fail(d, GCOMP_ERR_MEMORY, "bzip2: out of memory for a block");
  }
  d->tt = nt;
  d->tt_cap = block_max;
  return GCOMP_OK;
}

/* The symbols of a block, up to its end symbol. */
static gcomp_status_t decode_symbols(
    bz_dec_t * d, const uint8_t ** in, const uint8_t * end, int * blocked) {
  const unsigned eob = d->n_in_use + 1;
  for (;;) {
    unsigned t, zn;
    uint32_t w;
    int32_t code, idx;
    unsigned sym;
    refill(d, in, end);
    if (d->cnt < BZIP2_MAX_CODE_LEN) {
      *blocked = BLOCKED_INPUT;
      return GCOMP_OK;
    }
    if (d->group_pos == 0) {
      d->group_no++;
      if ((unsigned)d->group_no >= d->n_selectors) {
        return corrupt(d, "bzip2: more symbols than selectors");
      }
      d->group_pos = BZIP2_GROUP_SIZE;
      d->cur_table = d->selector[d->group_no];
    }
    d->group_pos--;
    t = d->cur_table;
    w = peek(d, BZIP2_MAX_CODE_LEN);
    for (zn = d->min_len[t]; zn <= d->max_len[t]; zn++) {
      code = (int32_t)(w >> (BZIP2_MAX_CODE_LEN - zn));
      if (code <= d->limit[t][zn]) {
        break;
      }
    }
    if (zn > d->max_len[t]) {
      return corrupt(d, "bzip2: a code is longer than any in its table");
    }
    /* Defensive: libbz2 makes this check, and it can only fire if the tables'
     * arithmetic is wrong; the index is into a fixed array either way. */
    idx = code - d->base[t][zn];
    if (idx < 0 || idx >= BZIP2_MAX_ALPHA) {
      return corrupt(d, "bzip2: a code is outside its table");
    }
    /* perm is zeroed before it is filled, and only alpha entries are filled,
     * so every entry names a symbol inside the alphabet. */
    sym = d->perm[t][idx];
    drop(d, zn);

    if (sym == BZIP2_RUNA || sym == BZIP2_RUNB) {
      if (!d->in_run) {
        d->in_run = 1;
        d->run_es = 0;
        d->run_n = 1;
      }
      if (d->run_n >= 2u * 1024u * 1024u) {
        return corrupt(d, "bzip2: a run of zeros is longer than any block");
      }
      d->run_es += (sym + 1u) * d->run_n;
      d->run_n *= 2u;
      continue;
    }
    if (d->in_run) {
      uint8_t uc = d->seq_to_unseq[d->mtf[0]];
      if (d->run_es > d->block_max - d->nblock) {
        return corrupt(d, "bzip2: a block is larger than its level allows");
      }
      d->unzftab[uc] += d->run_es;
      while (d->run_es-- > 0) {
        d->tt[d->nblock++] = uc;
      }
      d->in_run = 0;
    }
    if (sym == eob) {
      return GCOMP_OK; /* *blocked is still zero: the block's symbols are all in */
    }
    if (d->nblock >= d->block_max) {
      return corrupt(d, "bzip2: a block is larger than its level allows");
    }
    {
      unsigned nn = sym - 1u;
      uint8_t ui = d->mtf[nn];
      uint8_t byte = d->seq_to_unseq[ui];
      memmove(d->mtf + 1, d->mtf, nn);
      d->mtf[0] = ui;
      d->unzftab[byte]++;
      d->tt[d->nblock++] = byte;
    }
  }
}

/* The block's symbols are in: turn them into the order they were sorted from. */
static gcomp_status_t begin_output(bz_dec_t * d) {
  uint32_t i, sum = 0;
  if (d->orig_ptr >= d->nblock) {
    return corrupt(d, "bzip2: the origin pointer is outside its block");
  }
  for (i = 0; i < 256; i++) {
    d->cftab[i] = sum;
    sum += d->unzftab[i];
  }
  d->cftab[256] = sum;
  for (i = 0; i < d->nblock; i++) {
    uint8_t uc = (uint8_t)(d->tt[i] & 0xFFu);
    d->tt[d->cftab[uc]] |= i << 8;
    d->cftab[uc]++;
  }
  d->t_pos = d->tt[d->orig_ptr] >> 8;
  d->used = 0;
  d->run = 0;
  d->prev = 0;
  d->pend_n = 0;
  d->block_crc = 0xFFFFFFFFu;
  d->state = ST_OUTPUT;
  return GCOMP_OK;
}

static inline void emit(bz_dec_t * d, gcomp_buffer_t * out, uint8_t b) {
  ((uint8_t *)out->data)[out->used++] = b;
  d->block_crc = bzip2_crc_update(d->block_crc, b);
  d->produced++;
}

/* Walk the sorted list, undo the first run-length pass, and write. */
static gcomp_status_t write_block(
    bz_dec_t * d, gcomp_buffer_t * out, int * blocked) {
  size_t room = out->size - out->used;
  size_t limit_room = room;
  if (d->max_out != 0) {
    /* One byte more than the limit, so that exceeding it is seen and meeting
     * it is not. */
    uint64_t allow = d->max_out > d->produced ? d->max_out - d->produced : 0;
    if (allow != UINT64_MAX) {
      allow += 1;
    }
    if (allow < limit_room) {
      limit_room = (size_t)allow;
    }
  }
  for (;;) {
    while (d->pend_n != 0 && limit_room != 0) {
      emit(d, out, d->pend_b);
      d->pend_n--;
      limit_room--;
    }
    if (d->pend_n != 0) {
      break;
    }
    if (d->used == d->nblock) {
      uint32_t crc = ~d->block_crc;
      if (d->run == 4) {
        return corrupt(d, "bzip2: a block ends inside a run");
      }
      if (crc != d->block_crc_stored) {
        return corrupt(d, "bzip2: block CRC mismatch");
      }
      d->combined = bzip2_combine(d->combined, crc);
      d->state = ST_BLOCK_MAGIC;
      return GCOMP_OK;
    }
    if (limit_room == 0) {
      break;
    }
    {
      uint8_t b;
      d->t_pos = d->tt[d->t_pos];
      b = (uint8_t)(d->t_pos & 0xFFu);
      d->t_pos >>= 8;
      d->used++;
      if (d->run == 4) {
        d->pend_b = d->prev;
        d->pend_n = b;
        d->run = 0;
        continue;
      }
      emit(d, out, b);
      limit_room--;
      if (d->run > 0 && b == d->prev) {
        d->run++;
      }
      else {
        d->run = 1;
        d->prev = b;
      }
    }
  }
  *blocked = BLOCKED_OUTPUT;
  return GCOMP_OK;
}

/* Run the state machine until it wants input or output. */
static gcomp_status_t drive(bz_dec_t * d, const uint8_t ** in,
    const uint8_t * end, gcomp_buffer_t * out, int * why) {
  int blocked = 0;
  uint64_t checked = d->produced;
  gcomp_status_t s;
  while (!blocked) {
    refill(d, in, end);
    switch (d->state) {
    case ST_STREAM_HEADER: {
      uint32_t h;
      unsigned digit;
      if (d->cnt < 32) {
        blocked = BLOCKED_INPUT;
        break;
      }
      h = peek(d, 32);
      digit = h & 0xFFu;
      if ((h >> 8) != 0x425A68u || digit < '1' || digit > '9') {
        return corrupt(d, d->streams_done == 0
                ? "bzip2: not a bzip2 stream"
                : "bzip2: data after the last stream is not another stream");
      }
      drop(d, 32);
      d->level = digit - '0';
      d->block_max = d->level * BZIP2_BLOCK_UNIT;
      s = ensure_tt(d, d->block_max);
      if (s != GCOMP_OK) {
        return s;
      }
      d->combined = 0;
      d->state = ST_BLOCK_MAGIC;
      break;
    }
    case ST_BLOCK_MAGIC: {
      uint64_t m;
      if (d->cnt < 48) {
        blocked = BLOCKED_INPUT;
        break;
      }
      m = d->buf >> 16;
      drop(d, 48);
      if (m == BZIP2_BLOCK_MAGIC) {
        d->state = ST_BLOCK_CRC;
      }
      else if (m == BZIP2_END_MAGIC) {
        d->state = ST_STREAM_CRC;
      }
      else {
        return corrupt(d, "bzip2: neither a block nor the end of the stream");
      }
      break;
    }
    case ST_STREAM_CRC: {
      uint32_t crc;
      if (d->cnt < 32) {
        blocked = BLOCKED_INPUT;
        break;
      }
      crc = peek(d, 32);
      drop(d, 32);
      if (crc != d->combined) {
        return corrupt(d, "bzip2: stream CRC mismatch");
      }
      d->streams_done++;
      drop(d, d->cnt & 7u); /* the stream ends on a byte */
      d->state = ST_STREAM_HEADER;
      break;
    }
    case ST_BLOCK_CRC:
      if (d->cnt < 32) {
        blocked = BLOCKED_INPUT;
        break;
      }
      d->block_crc_stored = peek(d, 32);
      drop(d, 32);
      d->state = ST_BLOCK_HEAD;
      break;
    case ST_BLOCK_HEAD:
      if (d->cnt < 25) {
        blocked = BLOCKED_INPUT;
        break;
      }
      if (peek(d, 1) != 0) {
        return dec_fail(d, GCOMP_ERR_UNSUPPORTED,
            "bzip2: randomised blocks (bzip2 0.9.0) are not supported");
      }
      d->orig_ptr = (peek(d, 25)) & 0xFFFFFFu;
      drop(d, 25);
      d->state = ST_MAP_GROUPS;
      break;
    case ST_MAP_GROUPS:
      if (d->cnt < 16) {
        blocked = BLOCKED_INPUT;
        break;
      }
      d->used_groups = (uint16_t)peek(d, 16);
      drop(d, 16);
      d->map_i = 0;
      d->n_in_use = 0;
      d->state = ST_MAP_BYTES;
      break;
    case ST_MAP_BYTES:
      while (d->map_i < 16 && !(d->used_groups & (0x8000u >> d->map_i))) {
        d->map_i++;
      }
      if (d->map_i == 16) {
        if (d->n_in_use == 0) {
          return corrupt(d, "bzip2: a block uses no byte values");
        }
        d->alpha = d->n_in_use + 2u;
        d->state = ST_NGROUPS;
        break;
      }
      if (d->cnt < 16) {
        blocked = BLOCKED_INPUT;
        break;
      }
      {
        uint32_t bits = peek(d, 16);
        unsigned j;
        for (j = 0; j < 16; j++) {
          if (bits & (0x8000u >> j)) {
            d->seq_to_unseq[d->n_in_use++] = (uint8_t)(d->map_i * 16u + j);
          }
        }
      }
      drop(d, 16);
      d->map_i++;
      break;
    case ST_NGROUPS:
      if (d->cnt < 3) {
        blocked = BLOCKED_INPUT;
        break;
      }
      d->n_groups = peek(d, 3);
      drop(d, 3);
      if (d->n_groups < BZIP2_MIN_GROUPS || d->n_groups > BZIP2_MAX_GROUPS) {
        return corrupt(d, "bzip2: a block has an impossible number of tables");
      }
      d->state = ST_NSEL;
      break;
    case ST_NSEL:
      if (d->cnt < 15) {
        blocked = BLOCKED_INPUT;
        break;
      }
      d->n_sel = peek(d, 15);
      drop(d, 15);
      if (d->n_sel < 1) {
        return corrupt(d, "bzip2: a block has no selectors");
      }
      d->sel_i = 0;
      d->sel_j = 0;
      d->n_selectors = 0;
      d->state = ST_SEL;
      break;
    case ST_SEL:
      /* Each selector is a table number in move-to-front form, in unary. */
      while (d->sel_i < d->n_sel) {
        refill(d, in, end);
        if (d->cnt < 1) {
          blocked = BLOCKED_INPUT;
          break;
        }
        if (peek(d, 1)) {
          d->sel_j++;
          if (d->sel_j >= d->n_groups) {
            return corrupt(d, "bzip2: a selector names a table that is not there");
          }
        }
        else {
          if (d->sel_i < BZIP2_MAX_SELECTORS) {
            d->selector_mtf[d->sel_i] = (uint8_t)d->sel_j;
            d->n_selectors++;
          }
          d->sel_i++;
          d->sel_j = 0;
        }
        drop(d, 1);
      }
      if (blocked) {
        break;
      }
      {
        uint8_t pos[BZIP2_MAX_GROUPS];
        unsigned i, v;
        /* All of them: GCC at -O3 under TSan cannot see n_groups is at most
         * six and takes the loop for an overflow. */
        for (i = 0; i < BZIP2_MAX_GROUPS; i++) {
          pos[i] = (uint8_t)i;
        }
        for (i = 0; i < d->n_selectors; i++) {
          uint8_t tmp;
          v = d->selector_mtf[i];
          tmp = pos[v];
          while (v > 0) {
            pos[v] = pos[v - 1];
            v--;
          }
          pos[0] = tmp;
          d->selector[i] = tmp;
        }
      }
      d->tab_t = 0;
      d->state = ST_TABLE_START;
      break;
    case ST_TABLE_START:
      if (d->cnt < 5) {
        blocked = BLOCKED_INPUT;
        break;
      }
      d->curr = (int)peek(d, 5);
      drop(d, 5);
      d->tab_i = 0;
      d->state = ST_TABLE_DELTA;
      break;
    case ST_TABLE_DELTA:
      while (d->tab_i < d->alpha) {
        if (d->curr < 1 || d->curr > BZIP2_MAX_CODE_LEN) {
          return corrupt(d, "bzip2: a code length is out of range");
        }
        refill(d, in, end);
        if (d->cnt < 1) {
          blocked = BLOCKED_INPUT;
          break;
        }
        if (peek(d, 1) == 0) {
          d->len[d->tab_t][d->tab_i++] = (uint8_t)d->curr;
          drop(d, 1);
          continue;
        }
        refill(d, in, end);
        if (d->cnt < 2) {
          blocked = BLOCKED_INPUT;
          break;
        }
        d->curr += (peek(d, 2) & 1u) ? -1 : 1;
        drop(d, 2);
      }
      if (blocked) {
        break;
      }
      make_tables(d, d->tab_t);
      d->tab_t++;
      if (d->tab_t < d->n_groups) {
        d->state = ST_TABLE_START;
        break;
      }
      {
        unsigned i;
        for (i = 0; i < d->n_in_use; i++) {
          d->mtf[i] = (uint8_t)i;
        }
        memset(d->unzftab, 0, sizeof(d->unzftab));
        d->group_no = -1;
        d->group_pos = 0;
        d->nblock = 0;
        d->in_run = 0;
        d->state = ST_DATA;
      }
      break;
    case ST_DATA:
      s = decode_symbols(d, in, end, &blocked);
      if (s != GCOMP_OK) {
        return s;
      }
      if (!blocked) {
        s = begin_output(d);
        if (s != GCOMP_OK) {
          return s;
        }
      }
      break;
    case ST_OUTPUT:
      s = write_block(d, out, &blocked);
      if (s != GCOMP_OK) {
        return s;
      }
      break;
    }
    if (d->max_out != 0 && d->produced > d->max_out) {
      /* The byte that went over was written to find out; take it back. */
      out->used -= (size_t)(d->produced - d->max_out);
      d->produced = d->max_out;
      return dec_fail(
          d, GCOMP_ERR_LIMIT, "bzip2: decompressed output exceeds the limit");
    }
    /* After every step that wrote, and not only while a block is still being
     * written: a block that fits the caller's buffer is written and finished in
     * one step, which leaves the state at the next block's magic. */
    if (d->produced != checked) {
      checked = d->produced;
      if (gcomp_limits_check_expansion_ratio(
              d->in_total, d->produced, d->max_ratio) != GCOMP_OK) {
        return dec_fail(
            d, GCOMP_ERR_LIMIT, "bzip2: expansion ratio exceeds the limit");
      }
    }
  }
  *why = blocked;
  return GCOMP_OK;
}

gcomp_status_t bzip2_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  const gcomp_allocator_t * alloc;
  bz_dec_t * d;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  alloc = gcomp_registry_get_allocator(registry);
  d = gcomp_calloc(alloc, 1, sizeof(*d));
  if (!d) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "bzip2: out of memory");
  }
  d->pub = decoder;
  d->alloc = alloc;
  d->state = ST_STREAM_HEADER;
  d->max_out = gcomp_limits_read_output_max(options, 0);
  d->max_mem = gcomp_limits_read_memory_max(options, 0);
  d->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_BZIP2_MAX_EXPANSION_RATIO);
  decoder->method_state = d;
  return GCOMP_OK;
}

void bzip2_decoder_destroy(gcomp_decoder_t * decoder) {
  bz_dec_t * d;
  if (!decoder || !decoder->method_state) {
    return;
  }
  d = decoder->method_state;
  gcomp_free(d->alloc, d->tt);
  gcomp_free(d->alloc, d);
  decoder->method_state = NULL;
}

gcomp_status_t bzip2_decoder_reset(gcomp_decoder_t * decoder) {
  bz_dec_t * d;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  d = decoder->method_state;
  d->state = ST_STREAM_HEADER;
  d->streams_done = 0;
  d->failed = 0;
  d->err = GCOMP_OK;
  d->buf = 0;
  d->cnt = 0;
  d->produced = 0;
  d->in_total = 0;
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

gcomp_status_t bzip2_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  bz_dec_t * d = decoder->method_state;
  const uint8_t * in;
  const uint8_t * end;
  gcomp_status_t s;
  int why = 0;
  if (!d) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (d->failed) {
    return d->err;
  }
  if (input->data) {
    in = (const uint8_t *)input->data + input->used;
    end = in + (input->size - input->used);
  }
  else {
    in = end = g_no_input;
  }
  s = drive(d, &in, end, output, &why);
  if (input->data) {
    input->used = (size_t)(in - (const uint8_t *)input->data);
  }
  return s;
}

gcomp_status_t bzip2_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  bz_dec_t * d = decoder->method_state;
  const uint8_t * none = g_no_input;
  gcomp_status_t s;
  int why = 0;
  if (!d) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (d->failed) {
    return d->err;
  }
  s = drive(d, &none, none, output, &why);
  if (s != GCOMP_OK) {
    return s;
  }
  if (why == BLOCKED_OUTPUT) {
    return GCOMP_ERR_LIMIT;
  }
  if (d->state == ST_STREAM_HEADER && d->cnt == 0 && d->streams_done > 0) {
    return GCOMP_OK;
  }
  return corrupt(d, "bzip2: truncated stream");
}
