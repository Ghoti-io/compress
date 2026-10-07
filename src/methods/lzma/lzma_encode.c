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
 * @file lzma_encode.c
 *
 * The LZMA and LZMA2 encoders.
 *
 * ## Shape
 *
 * One symbol coder serves both methods. `lzma` runs it as one range-coded
 * stream behind a 13-byte header (or none, with `lzma.raw`); `lzma2` runs it a
 * chunk at a time, restarting the range coder for each and falling back to a
 * stored chunk when coding does not shrink the bytes.
 *
 * Input is held in a window until a batch is worth coding. The window is the
 * dictionary plus room for the next batch, and slides when it fills: the
 * match finder keeps positions as running 32-bit counts, so sliding moves the
 * bytes and changes one base address and touches no table.
 *
 * ## The parser
 *
 * A fast parser, after the LZMA SDK's fast mode: take the longest match the
 * hash chains find, prefer a repeat of a recent distance when it is nearly as
 * long, and look one byte ahead before committing, so that a literal now can
 * buy a longer match next. It does not price symbols. An optimal parser, which
 * does, is the larger gain still to take and is recorded in the plan.
 *
 * ## What the match finder may be wrong about
 *
 * Candidates come from hash tables whose entries may be stale or aliased, and
 * every one is checked against the bytes themselves before it is used, so a
 * stale entry costs time and never correctness.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "lzma_internal.h"

#include <stdio.h>
#include <string.h>

#define ENC_STAGE_CAP (65536u + 4096u)
#define ENC_BATCH (128u * 1024u)
#define ENC_LZMA2_HEADER 6u
#define ENC_LZMA2_UNCOMPRESSED_MAX (1u << 21)
#define ENC_LZMA2_COMPRESSED_MAX 65536u
/* The most one symbol can add to the output before the coder next checks. */
#define ENC_SYMBOL_MARGIN 128u

/* ---- range encoder ------------------------------------------------------- */

typedef struct lzma_renc_s {
  uint64_t low;
  uint32_t range;
  uint8_t cache;
  uint64_t cache_size;
  uint8_t * out;
  size_t out_len;
} lzma_renc_t;

static void renc_init(lzma_renc_t * r, uint8_t * out, size_t out_len) {
  r->low = 0;
  r->range = 0xFFFFFFFFu;
  r->cache = 0;
  r->cache_size = 1;
  r->out = out;
  r->out_len = out_len;
}

static inline void renc_shift_low(lzma_renc_t * r) {
  if ((uint32_t)r->low < 0xFF000000u || (r->low >> 32) != 0) {
    uint8_t temp = r->cache;
    do {
      r->out[r->out_len++] = (uint8_t)(temp + (uint8_t)(r->low >> 32));
      temp = 0xFF;
    } while (--r->cache_size != 0);
    r->cache = (uint8_t)((uint32_t)r->low >> 24);
  }
  r->cache_size++;
  r->low = (uint32_t)r->low << 8;
}

static inline void renc_norm(lzma_renc_t * r) {
  while (r->range < LZMA_TOP) {
    r->range <<= 8;
    renc_shift_low(r);
  }
}

static inline void renc_bit(lzma_renc_t * r, uint16_t * prob, unsigned bit) {
  uint32_t p = *prob;
  uint32_t bound = (r->range >> LZMA_PROB_BITS) * p;
  if (!bit) {
    r->range = bound;
    *prob = (uint16_t)(p + ((LZMA_PROB_ONE - p) >> LZMA_MOVE_BITS));
  }
  else {
    r->low += bound;
    r->range -= bound;
    *prob = (uint16_t)(p - (p >> LZMA_MOVE_BITS));
  }
  renc_norm(r);
}

static inline void renc_direct(lzma_renc_t * r, uint32_t value, unsigned count) {
  while (count--) {
    r->range >>= 1;
    if ((value >> count) & 1u) {
      r->low += r->range;
    }
    renc_norm(r);
  }
}

static void renc_flush(lzma_renc_t * r) {
  unsigned i;
  for (i = 0; i < 5; i++) {
    renc_shift_low(r);
  }
}

/* What the coder holds that has not yet been written: bytes the carry may
 * still change, and the four bytes of `low`. */
static inline size_t renc_pending(const lzma_renc_t * r) {
  return r->out_len + (size_t)r->cache_size + 4u;
}

static void enc_tree(
    lzma_renc_t * r, uint16_t * probs, unsigned bits, unsigned sym) {
  unsigned m = 1;
  unsigned i;
  for (i = bits; i-- > 0;) {
    unsigned b = (sym >> i) & 1u;
    renc_bit(r, probs + m, b);
    m = (m << 1) | b;
  }
}

static void enc_tree_reverse(
    lzma_renc_t * r, uint16_t * probs, unsigned bits, unsigned sym) {
  unsigned m = 1;
  unsigned i;
  for (i = 0; i < bits; i++) {
    unsigned b = sym & 1u;
    sym >>= 1;
    renc_bit(r, probs + m, b);
    m = (m << 1) | b;
  }
}

/* ---- the encoder --------------------------------------------------------- */

typedef struct lzma_enc_s {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  int lzma2;
  int raw;
  unsigned lc, lp, pb;
  uint32_t dict_size;
  unsigned nice_len;
  unsigned depth;
  uint64_t known_size; /* UINT64_MAX: not stated, so an end marker */
  uint64_t in_total;

  lzma_probs_t probs;
  uint16_t * lit;
  size_t lit_count;
  unsigned state;
  uint32_t rep[4];
  uint64_t total; /* bytes coded since the last dictionary reset */

  uint8_t * win;
  size_t win_cap;
  size_t win_len;
  size_t pos;
  size_t hist_start; /* a match may not reach before this index */
  uint32_t base;     /* the running position of win[0] */

  uint32_t * head;
  unsigned head_bits;
  uint32_t * head3;
  uint32_t * head2;
  uint32_t * prev;
  uint32_t prev_mask;
  size_t ins; /* the next window index to enter in the tables */

  lzma_renc_t rc;
  uint8_t * stage;
  size_t stage_len;
  size_t stage_pos;

  /* lzma2 */
  int need_dict_reset, need_props, need_state_reset;
  /* snapshot taken before a chunk, restored when the chunk is stored */
  lzma_probs_t snap_probs;
  uint16_t * snap_lit;

  int header_done;
  int marker_done;
  int finished;
} lzma_enc_t;

/* ---- match finder -------------------------------------------------------- */

static inline uint32_t hash4(const uint8_t * p, unsigned bits) {
  uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
  return (v * 2654435761u) >> (32u - bits);
}

static inline uint32_t hash3(const uint8_t * p) {
  uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  return (v * 2654435761u) >> 16;
}

static inline uint32_t hash2(const uint8_t * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t abs_pos(const lzma_enc_t * e, size_t idx) {
  return e->base + (uint32_t)idx;
}

/* Enter every position below `upto` that has four bytes after it. */
static void mf_insert_to(lzma_enc_t * e, size_t upto) {
  while (e->ins < upto && e->ins + 4u <= e->win_len) {
    const uint8_t * p = e->win + e->ins;
    uint32_t a = abs_pos(e, e->ins) + 1u; /* +1: zero means nothing */
    uint32_t h = hash4(p, e->head_bits);
    e->prev[(a - 1u) & e->prev_mask] = e->head[h];
    e->head[h] = a;
    e->head3[hash3(p)] = a;
    e->head2[hash2(p)] = a;
    e->ins++;
  }
}

static inline unsigned match_len(
    const uint8_t * a, const uint8_t * b, unsigned limit) {
  unsigned n = 0;
  while (n < limit && a[n] == b[n]) {
    n++;
  }
  return n;
}

typedef struct enc_match_s {
  unsigned len;
  uint32_t dist; /* the distance less one, as the format spells it */
} enc_match_t;

static void consider(const lzma_enc_t * e, size_t idx, unsigned avail,
    uint32_t cand_abs_plus1, enc_match_t * best) {
  uint32_t d;
  unsigned len;
  size_t hist = idx - e->hist_start;
  if (cand_abs_plus1 == 0) {
    return;
  }
  d = abs_pos(e, idx) + 1u - cand_abs_plus1;
  if (d == 0 || d > e->dict_size || d > hist) {
    return;
  }
  len = match_len(e->win + idx, e->win + idx - d, avail);
  if (len > best->len || (len == best->len && len != 0 && d - 1u < best->dist)) {
    best->len = len;
    best->dist = d - 1u;
  }
}

/* The longest match at idx among the tables' candidates; len 0 if none. */
static void mf_find(
    lzma_enc_t * e, size_t idx, unsigned avail, enc_match_t * best) {
  best->len = 0;
  best->dist = 0;
  if (avail < 2 || idx + 4u > e->win_len) {
    return;
  }
  {
    const uint8_t * p = e->win + idx;
    uint32_t cur = abs_pos(e, idx) + 1u;
    uint32_t cand = e->head[hash4(p, e->head_bits)];
    unsigned depth = e->depth;
    consider(e, idx, avail, e->head2[hash2(p)], best);
    consider(e, idx, avail, e->head3[hash3(p)], best);
    while (cand != 0 && depth-- > 0) {
      uint32_t d = cur - cand;
      uint32_t next;
      if (d == 0 || d > e->dict_size) {
        break;
      }
      consider(e, idx, avail, cand, best);
      if (best->len >= avail || best->len >= e->nice_len) {
        break;
      }
      next = e->prev[(cand - 1u) & e->prev_mask];
      if (next == 0 || cur - next <= d) {
        break; /* the chain does not go back: stale, or wrapped */
      }
      cand = next;
    }
  }
}

/* ---- symbols ------------------------------------------------------------- */

typedef enum { ES_LIT, ES_MATCH, ES_REP } enc_kind_t;

typedef struct enc_sym_s {
  enc_kind_t kind;
  unsigned len;
  uint32_t dist;
  unsigned rep;
} enc_sym_t;

static unsigned pos_slot_of(uint32_t dist) {
  unsigned n = 0;
  uint32_t t = dist;
  if (dist < 4) {
    return dist;
  }
  while (t >>= 1) {
    n++;
  }
  return (n << 1) | ((dist >> (n - 1)) & 1u);
}

static void enc_len(lzma_renc_t * r, lzma_len_probs_t * l, unsigned len,
    unsigned pos_state) {
  len -= LZMA_MATCH_MIN;
  if (len < LZMA_LEN_LOW_SYMBOLS) {
    renc_bit(r, &l->choice, 0);
    enc_tree(r, l->low[pos_state], LZMA_LEN_LOW_BITS, len);
  }
  else if (len < LZMA_LEN_LOW_SYMBOLS + LZMA_LEN_MID_SYMBOLS) {
    renc_bit(r, &l->choice, 1);
    renc_bit(r, &l->choice2, 0);
    enc_tree(r, l->mid[pos_state], LZMA_LEN_MID_BITS, len - LZMA_LEN_LOW_SYMBOLS);
  }
  else {
    renc_bit(r, &l->choice, 1);
    renc_bit(r, &l->choice2, 1);
    enc_tree(r, l->high, LZMA_LEN_HIGH_BITS,
        len - LZMA_LEN_LOW_SYMBOLS - LZMA_LEN_MID_SYMBOLS);
  }
}

static void enc_dist(lzma_renc_t * r, lzma_probs_t * P, unsigned len, uint32_t dist) {
  unsigned dist_state = lzma_dist_state(len);
  unsigned slot = pos_slot_of(dist);
  unsigned direct_bits;
  uint32_t base, reduced;
  enc_tree(r, P->pos_slot[dist_state], LZMA_DIST_SLOT_BITS, slot);
  if (slot < 4) {
    return;
  }
  direct_bits = (slot >> 1) - 1;
  base = (2u | (slot & 1u)) << direct_bits;
  reduced = dist - base;
  if (slot < LZMA_END_POS_MODEL) {
    enc_tree_reverse(r, P->pos_spec + base - slot, direct_bits, reduced);
  }
  else {
    renc_direct(r, reduced >> LZMA_ALIGN_BITS, direct_bits - LZMA_ALIGN_BITS);
    enc_tree_reverse(r, P->align, LZMA_ALIGN_BITS, reduced & (LZMA_ALIGN_SIZE - 1u));
  }
}

static inline unsigned pos_state_of(const lzma_enc_t * e) {
  return (unsigned)e->total & ((1u << e->pb) - 1u);
}

static void put_literal(lzma_enc_t * e, uint8_t byte) {
  lzma_renc_t * r = &e->rc;
  unsigned pos_state = pos_state_of(e);
  unsigned prev = e->pos > e->hist_start ? e->win[e->pos - 1] : 0;
  size_t ctx = (((size_t)e->total & (((size_t)1 << e->lp) - 1u)) << e->lc) +
      (prev >> (8u - e->lc));
  uint16_t * probs = e->lit + (size_t)LZMA_LIT_CODER * ctx;
  unsigned symbol = 1;
  int i;
  renc_bit(r, &e->probs.is_match[e->state][pos_state], 0);
  if (e->state >= LZMA_LIT_STATES) {
    unsigned match_byte = e->win[e->pos - (size_t)e->rep[0] - 1u];
    int same = 1;
    for (i = 7; i >= 0; i--) {
      unsigned bit = (byte >> i) & 1u;
      if (same) {
        unsigned match_bit = (match_byte >> i) & 1u;
        renc_bit(r, &probs[((1u + match_bit) << 8) + symbol], bit);
        same = match_bit == bit;
      }
      else {
        renc_bit(r, &probs[symbol], bit);
      }
      symbol = (symbol << 1) | bit;
    }
  }
  else {
    for (i = 7; i >= 0; i--) {
      unsigned bit = (byte >> i) & 1u;
      renc_bit(r, &probs[symbol], bit);
      symbol = (symbol << 1) | bit;
    }
  }
  e->state = lzma_state_literal(e->state);
}

static void put_match(lzma_enc_t * e, unsigned len, uint32_t dist) {
  lzma_renc_t * r = &e->rc;
  unsigned pos_state = pos_state_of(e);
  renc_bit(r, &e->probs.is_match[e->state][pos_state], 1);
  renc_bit(r, &e->probs.is_rep[e->state], 0);
  enc_len(r, &e->probs.len, len, pos_state);
  enc_dist(r, &e->probs, len, dist);
  e->rep[3] = e->rep[2];
  e->rep[2] = e->rep[1];
  e->rep[1] = e->rep[0];
  e->rep[0] = dist;
  e->state = lzma_state_match(e->state);
}

static void put_rep(lzma_enc_t * e, unsigned rep, unsigned len) {
  lzma_renc_t * r = &e->rc;
  unsigned pos_state = pos_state_of(e);
  renc_bit(r, &e->probs.is_match[e->state][pos_state], 1);
  renc_bit(r, &e->probs.is_rep[e->state], 1);
  if (rep == 0) {
    renc_bit(r, &e->probs.is_rep_g0[e->state], 0);
    renc_bit(r, &e->probs.is_rep0_long[e->state][pos_state], len != 1);
  }
  else {
    uint32_t dist = e->rep[rep];
    renc_bit(r, &e->probs.is_rep_g0[e->state], 1);
    if (rep == 1) {
      renc_bit(r, &e->probs.is_rep_g1[e->state], 0);
    }
    else {
      renc_bit(r, &e->probs.is_rep_g1[e->state], 1);
      renc_bit(r, &e->probs.is_rep_g2[e->state], rep - 2u);
    }
    while (rep > 0) {
      e->rep[rep] = e->rep[rep - 1];
      rep--;
    }
    e->rep[0] = dist;
  }
  if (len == 1) {
    e->state = lzma_state_short_rep(e->state);
  }
  else {
    enc_len(r, &e->probs.rep_len, len, pos_state);
    e->state = lzma_state_rep(e->state);
  }
}

/* The end marker: a match of length 2 at the distance no match can have. */
static void put_end_marker(lzma_enc_t * e) {
  lzma_renc_t * r = &e->rc;
  unsigned pos_state = pos_state_of(e);
  renc_bit(r, &e->probs.is_match[e->state][pos_state], 1);
  renc_bit(r, &e->probs.is_rep[e->state], 0);
  enc_len(r, &e->probs.len, LZMA_MATCH_MIN, pos_state);
  enc_dist(r, &e->probs, LZMA_MATCH_MIN, LZMA_EOS_DISTANCE);
}

/* ---- the parser ---------------------------------------------------------- */

static inline int change_pair(uint32_t small_dist, uint32_t big_dist) {
  return (big_dist >> 7) > small_dist;
}

static unsigned best_rep(const lzma_enc_t * e, size_t idx, unsigned avail,
    unsigned * which) {
  unsigned best = 0;
  unsigned i;
  size_t hist = idx - e->hist_start;
  *which = 0;
  if (avail < 2) {
    return 0;
  }
  for (i = 0; i < 4; i++) {
    size_t d = (size_t)e->rep[i] + 1u;
    unsigned len;
    if (d > hist) {
      continue;
    }
    len = match_len(e->win + idx, e->win + idx - d, avail);
    if (len >= 2 && len > best) {
      best = len;
      *which = i;
    }
  }
  return best;
}

/* The fast parser: one symbol at e->pos, looking one byte ahead. */
static void parse_fast(lzma_enc_t * e, size_t end, enc_sym_t * s) {
  size_t idx = e->pos;
  unsigned avail = end - idx > LZMA_MATCH_MAX ? LZMA_MATCH_MAX : (unsigned)(end - idx);
  enc_match_t main_m, next_m;
  unsigned rep_len, rep_i;

  s->kind = ES_LIT;
  s->len = 1;
  if (avail < 2) {
    /* A single byte left: it can still be a repeat of the last distance. */
    if (avail == 1 && idx - e->hist_start > e->rep[0] &&
        e->win[idx] == e->win[idx - (size_t)e->rep[0] - 1u] &&
        e->state >= LZMA_LIT_STATES) {
      s->kind = ES_REP;
      s->rep = 0;
    }
    return;
  }
  mf_insert_to(e, idx);
  mf_find(e, idx, avail, &main_m);
  rep_len = best_rep(e, idx, avail, &rep_i);

  if (rep_len >= e->nice_len) {
    s->kind = ES_REP;
    s->rep = rep_i;
    s->len = rep_len;
    return;
  }
  if (main_m.len >= e->nice_len) {
    s->kind = ES_MATCH;
    s->len = main_m.len;
    s->dist = main_m.dist;
    return;
  }
  if (main_m.len == 2 && main_m.dist >= 0x80) {
    main_m.len = 1;
  }
  if (rep_len >= 2 &&
      (rep_len + 1 >= main_m.len ||
          (rep_len + 2 >= main_m.len && main_m.dist >= (1u << 9)) ||
          (rep_len + 3 >= main_m.len && main_m.dist >= (1u << 15)))) {
    s->kind = ES_REP;
    s->rep = rep_i;
    s->len = rep_len;
    return;
  }
  if (main_m.len < 2) {
    /* Nothing worth a match; a short rep of one byte is still cheaper than a
     * literal when the byte at the last distance is the byte here. */
    if (idx - e->hist_start > e->rep[0] && e->state >= LZMA_LIT_STATES &&
        e->win[idx] == e->win[idx - (size_t)e->rep[0] - 1u]) {
      s->kind = ES_REP;
      s->rep = 0;
    }
    return;
  }
  if (avail > 2) {
    unsigned avail1 = avail - 1u;
    unsigned i;
    mf_insert_to(e, idx + 1u);
    mf_find(e, idx + 1u, avail1, &next_m);
    if ((next_m.len >= main_m.len && next_m.dist < main_m.dist) ||
        (next_m.len == main_m.len + 1 && !change_pair(main_m.dist, next_m.dist)) ||
        next_m.len > main_m.len + 1 ||
        (next_m.len + 1 >= main_m.len && main_m.len >= 3 &&
            change_pair(next_m.dist, main_m.dist))) {
      return; /* a literal now buys a better match next */
    }
    for (i = 0; i < 4; i++) {
      size_t d = (size_t)e->rep[i] + 1u;
      unsigned lim = main_m.len - 1u < avail1 ? main_m.len - 1u : avail1;
      if (d <= idx + 1u - e->hist_start && lim >= 2 &&
          match_len(e->win + idx + 1u, e->win + idx + 1u - d, lim) >= main_m.len - 1u) {
        return;
      }
    }
  }
  s->kind = ES_MATCH;
  s->len = main_m.len;
  s->dist = main_m.dist;
}

/* Code symbols over [pos, end), stopping early if the coder's output would
 * pass `out_limit` bytes. Returns the bytes covered. */
static size_t enc_symbols(lzma_enc_t * e, size_t end, size_t out_limit) {
  size_t start = e->pos;
  while (e->pos < end && renc_pending(&e->rc) + ENC_SYMBOL_MARGIN < out_limit) {
    enc_sym_t s;
    unsigned n;
    parse_fast(e, end, &s);
    switch (s.kind) {
    case ES_MATCH:
      put_match(e, s.len, s.dist);
      n = s.len;
      break;
    case ES_REP:
      put_rep(e, s.rep, s.len);
      n = s.len;
      break;
    default:
      put_literal(e, e->win[e->pos]);
      n = 1;
      break;
    }
    e->pos += n;
    e->total += n;
    mf_insert_to(e, e->pos);
  }
  return e->pos - start;
}

static void lzma_encoder_destroy_state(lzma_enc_t * e);

/* ---- the stream ---------------------------------------------------------- */

typedef struct preset_s {
  uint32_t dict;
  unsigned nice;
  unsigned depth;
} preset_t;

/* The dictionaries are xz's, which is what a container advertising a preset
 * expects; the search effort is this encoder's own. */
static const preset_t g_presets[10] = {
    {1u << 18, 32, 4},
    {1u << 20, 32, 8},
    {1u << 21, 48, 16},
    {1u << 22, 64, 24},
    {1u << 22, 64, 32},
    {1u << 23, 64, 48},
    {1u << 23, 96, 64},
    {1u << 24, 128, 96},
    {1u << 25, 192, 128},
    {1u << 26, 273, 192},
};

static void enc_model_reset(lzma_enc_t * e) {
  lzma_probs_init(&e->probs, e->lit, e->lit_count);
  e->state = 0;
  e->rep[0] = e->rep[1] = e->rep[2] = e->rep[3] = 0;
}

static void enc_tables_clear(lzma_enc_t * e) {
  memset(e->head, 0, ((size_t)1 << e->head_bits) * sizeof(uint32_t));
  memset(e->head3, 0, 65536u * sizeof(uint32_t));
  memset(e->head2, 0, 65536u * sizeof(uint32_t));
  e->ins = 0;
}

static void enc_stream_reset(lzma_enc_t * e) {
  enc_model_reset(e);
  enc_tables_clear(e);
  e->total = 0;
  e->in_total = 0;
  e->win_len = 0;
  e->pos = 0;
  e->hist_start = 0;
  e->base = 0;
  e->stage_len = 0;
  e->stage_pos = 0;
  e->need_dict_reset = e->need_props = e->need_state_reset = 1;
  e->header_done = 0;
  e->marker_done = 0;
  e->finished = 0;
}

static unsigned ceil_log2_u32(uint32_t v) {
  unsigned n = 0;
  while (n < 32 && ((uint64_t)1 << n) < v) {
    n++;
  }
  return n;
}

gcomp_status_t lzma_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder, int lzma2) {
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  lzma_enc_t * e;
  int64_t preset = 6, lc = 3, lp = 0, pb = 2;
  uint64_t dict = 0, size = UINT64_MAX;
  int raw = 0;
  size_t slack, prev_n;

  /* The ranges are the schema's: gcomp_encoder_create() has validated every
   * option against it before this runs. What the schema cannot say is that
   * lzma2 takes lc + lp up to 4 and lzma up to 12. */
  if (options) {
    const char * pre = lzma2 ? "lzma2." : "lzma.";
    char key[32];
#define ENC_KEY(name) (snprintf(key, sizeof key, "%s%s", pre, name), key)
    (void)gcomp_options_get_int64(options, ENC_KEY("preset"), &preset);
    (void)gcomp_options_get_int64(options, ENC_KEY("lc"), &lc);
    (void)gcomp_options_get_int64(options, ENC_KEY("lp"), &lp);
    (void)gcomp_options_get_int64(options, ENC_KEY("pb"), &pb);
    (void)gcomp_options_get_uint64(options, ENC_KEY("dict_size"), &dict);
#undef ENC_KEY
    if (!lzma2) {
      (void)gcomp_options_get_bool(options, "lzma.raw", &raw);
      (void)gcomp_options_get_uint64(options, "lzma.uncompressed_size", &size);
    }
  }
  if (lzma2 && lc + lp > 4) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "lzma2: lc + lp may not exceed 4");
  }
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "lzma: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->lzma2 = lzma2;
  e->raw = raw;
  e->lc = (unsigned)lc;
  e->lp = (unsigned)lp;
  e->pb = (unsigned)pb;
  e->dict_size = dict != 0 ? (uint32_t)dict : g_presets[preset].dict;
  e->nice_len = g_presets[preset].nice;
  e->depth = g_presets[preset].depth;
  e->known_size = size;
  e->lit_count = lzma_lit_count(e->lc, e->lp);
  e->head_bits = ceil_log2_u32(e->dict_size);
  if (e->head_bits < 16) {
    e->head_bits = 16;
  }
  if (e->head_bits > 22) {
    e->head_bits = 22;
  }
  prev_n = (size_t)1 << ceil_log2_u32(e->dict_size);
  e->prev_mask = (uint32_t)(prev_n - 1u);
  slack = e->dict_size > (1u << 20) ? e->dict_size : (1u << 20);
  e->win_cap = (size_t)e->dict_size + slack;

  e->win = gcomp_malloc(alloc, e->win_cap);
  e->head = gcomp_calloc(alloc, (size_t)1 << e->head_bits, sizeof(uint32_t));
  e->head3 = gcomp_calloc(alloc, 65536u, sizeof(uint32_t));
  e->head2 = gcomp_calloc(alloc, 65536u, sizeof(uint32_t));
  e->prev = gcomp_calloc(alloc, prev_n, sizeof(uint32_t));
  e->lit = gcomp_malloc(alloc, e->lit_count * sizeof(uint16_t));
  e->stage = gcomp_malloc(alloc, ENC_STAGE_CAP);
  if (lzma2) {
    e->snap_lit = gcomp_malloc(alloc, e->lit_count * sizeof(uint16_t));
  }
  if (!e->win || !e->head || !e->head3 || !e->head2 || !e->prev || !e->lit ||
      !e->stage || (lzma2 && !e->snap_lit)) {
    lzma_encoder_destroy_state(e);
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "lzma: out of memory for the encoder");
  }
  enc_stream_reset(e);
  encoder->method_state = e;
  return GCOMP_OK;
}

static void lzma_encoder_destroy_state(lzma_enc_t * e) {
  if (!e) {
    return;
  }
  gcomp_free(e->alloc, e->win);
  gcomp_free(e->alloc, e->head);
  gcomp_free(e->alloc, e->head3);
  gcomp_free(e->alloc, e->head2);
  gcomp_free(e->alloc, e->prev);
  gcomp_free(e->alloc, e->lit);
  gcomp_free(e->alloc, e->snap_lit);
  gcomp_free(e->alloc, e->stage);
  gcomp_free(e->alloc, e);
}

void lzma_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }
  lzma_encoder_destroy_state(encoder->method_state);
  encoder->method_state = NULL;
}

/* Move what the stage holds into the caller's buffer, as far as it fits. */
static void enc_drain(lzma_enc_t * e, gcomp_buffer_t * out) {
  size_t avail = e->stage_len - e->stage_pos;
  size_t room = out->size - out->used;
  size_t n = avail < room ? avail : room;
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->stage + e->stage_pos, n);
    out->used += n;
    e->stage_pos += n;
  }
  if (e->stage_pos == e->stage_len) {
    e->stage_pos = 0;
    e->stage_len = 0;
    e->rc.out_len = 0;
  }
}

/* Keep one dictionary of history behind pos and move the rest of the window
 * down. Tables hold running positions, so only the base address changes. */
static void enc_slide(lzma_enc_t * e) {
  size_t keep = e->pos < e->dict_size ? e->pos : e->dict_size;
  size_t shift = e->pos - keep;
  if (shift == 0) {
    return;
  }
  memmove(e->win, e->win + shift, e->win_len - shift);
  e->win_len -= shift;
  e->pos -= shift;
  e->ins = e->ins > shift ? e->ins - shift : 0;
  e->hist_start = e->hist_start > shift ? e->hist_start - shift : 0;
  e->base += (uint32_t)shift;
}

/* ---- lzma (alone and raw) ------------------------------------------------ */

static void alone_begin(lzma_enc_t * e) {
  size_t hdr = 0;
  unsigned i;
  if (!e->raw) {
    uint64_t size = e->known_size;
    e->stage[0] = lzma_props_encode(e->lc, e->lp, e->pb);
    for (i = 0; i < 4; i++) {
      e->stage[1 + i] = (uint8_t)(e->dict_size >> (8u * i));
    }
    for (i = 0; i < 8; i++) {
      e->stage[5 + i] = (uint8_t)(size >> (8u * i));
    }
    hdr = 13;
  }
  renc_init(&e->rc, e->stage, hdr);
  e->stage_len = hdr;
  e->header_done = 1;
}

/* Code what the window holds, up to the stage filling. The stage is empty on
 * entry; whatever this adds is the caller's to drain. */
static void alone_run(lzma_enc_t * e, int final) {
  size_t end = final ? e->win_len
                     : (e->win_len > LZMA_MATCH_MAX ? e->win_len - LZMA_MATCH_MAX : 0);
  if (!e->header_done) {
    alone_begin(e);
  }
  if (e->pos < end) {
    enc_symbols(e, end, ENC_STAGE_CAP);
  }
  e->stage_len = e->rc.out_len;
}

static void alone_close(lzma_enc_t * e) {
  if (!e->header_done) {
    alone_begin(e);
  }
  if (e->known_size == UINT64_MAX) {
    put_end_marker(e);
  }
  renc_flush(&e->rc);
  e->stage_len = e->rc.out_len;
  e->marker_done = 1;
}

/* ---- lzma2 --------------------------------------------------------------- */

/* One chunk over [pos, end): coded if that is smaller than the bytes, stored
 * if not. The stage is empty on entry and holds the whole chunk on exit. */
static void lz2_chunk(lzma_enc_t * e, size_t end) {
  size_t start = e->pos;
  size_t target = end - start;
  size_t usize, csize;
  unsigned state_save = e->state;
  uint32_t rep_save[4];
  uint8_t control;
  size_t lit_bytes = e->lit_count * sizeof(uint16_t);

  if (target > ENC_LZMA2_UNCOMPRESSED_MAX) {
    target = ENC_LZMA2_UNCOMPRESSED_MAX;
  }
  memcpy(rep_save, e->rep, sizeof(rep_save));
  memcpy(&e->snap_probs, &e->probs, sizeof(e->probs));
  memcpy(e->snap_lit, e->lit, lit_bytes);

  renc_init(&e->rc, e->stage + ENC_LZMA2_HEADER, 0);
  enc_symbols(e, start + target, ENC_LZMA2_COMPRESSED_MAX - 8u);
  usize = e->pos - start;
  renc_flush(&e->rc);
  csize = e->rc.out_len;

  if (csize >= usize) {
    /* Coding did not pay. The decoder never sees the symbols just coded, so
     * the model goes back to where it was, and then to its initial state,
     * which is what the next LZMA chunk's state reset will make of it. */
    memcpy(&e->probs, &e->snap_probs, sizeof(e->probs));
    memcpy(e->lit, e->snap_lit, lit_bytes);
    e->state = state_save;
    memcpy(e->rep, rep_save, sizeof(rep_save));
    enc_model_reset(e);
    control = e->need_dict_reset ? 1 : 2;
    e->stage[0] = control;
    e->stage[1] = (uint8_t)((usize - 1u) >> 8);
    e->stage[2] = (uint8_t)(usize - 1u);
    memcpy(e->stage + 3, e->win + start, usize);
    e->stage_pos = 0;
    e->stage_len = 3 + usize;
    /* A dictionary reset is only ever pending together with a properties
     * reset (they are set and cleared as a pair), so a stored chunk that
     * performs the first leaves the second still owed, and the next coded
     * chunk carries the properties. */
    e->need_dict_reset = 0;
    e->need_state_reset = 1;
    return;
  }

  {
    size_t h = 5;
    int with_props = e->need_dict_reset || e->need_props;
    uint8_t * hp;
    control = e->need_dict_reset ? 0xE0
        : e->need_props          ? 0xC0
        : e->need_state_reset    ? 0xA0
                                 : 0x80;
    if (with_props) {
      h = 6;
    }
    hp = e->stage + ENC_LZMA2_HEADER - h;
    hp[0] = (uint8_t)(control | (((usize - 1u) >> 16) & 0x1Fu));
    hp[1] = (uint8_t)((usize - 1u) >> 8);
    hp[2] = (uint8_t)(usize - 1u);
    hp[3] = (uint8_t)((csize - 1u) >> 8);
    hp[4] = (uint8_t)(csize - 1u);
    if (with_props) {
      hp[5] = lzma_props_encode(e->lc, e->lp, e->pb);
    }
    e->stage_pos = ENC_LZMA2_HEADER - h;
    e->stage_len = ENC_LZMA2_HEADER + csize;
    e->need_dict_reset = e->need_props = e->need_state_reset = 0;
  }
}

/* Chunks until the window is coded or the stage holds one. */
static void lz2_run(lzma_enc_t * e, int final) {
  size_t end = final ? e->win_len
                     : (e->win_len > LZMA_MATCH_MAX ? e->win_len - LZMA_MATCH_MAX : 0);
  if (e->pos < end && e->stage_len == 0) {
    lz2_chunk(e, end);
  }
}

/* ---- the API ------------------------------------------------------------- */

static int enc_has_pending(const lzma_enc_t * e) {
  return e->pos < e->win_len;
}

static void enc_run(lzma_enc_t * e, int final) {
  if (e->lzma2) {
    lz2_run(e, final);
  }
  else {
    alone_run(e, final);
  }
}

/*
 * Input waits in the window until a batch is there, and no more than a batch:
 * the window is filled to ENC_BATCH + one maximum match past what is coded,
 * coded, and filled again. Two things follow. The window slides when the
 * dictionary has left room for several batches, so the bytes moved are a
 * fraction of the bytes coded; letting it fill with input first made every
 * slide move a megabyte to gain one chunk. And where the batches fall depends
 * only on how many bytes have arrived, not on how they were handed over, so
 * the same input gives the same stream however it is split.
 */
gcomp_status_t lzma_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  lzma_enc_t * e = encoder->method_state;
  const size_t thresh = (size_t)ENC_BATCH + LZMA_MATCH_MAX;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "lzma: encoder update after finish");
  }
  for (;;) {
    size_t pending, want, take, avail;
    enc_drain(e, output);
    if (e->stage_len != 0) {
      return GCOMP_OK;
    }
    pending = e->win_len - e->pos;
    if (pending >= thresh) {
      enc_run(e, 0);
      continue;
    }
    want = thresh - pending;
    if (e->win_len + want > e->win_cap) {
      enc_slide(e);
    }
    avail = input->size - input->used;
    take = want < avail ? want : avail;
    if (take == 0) {
      return GCOMP_OK;
    }
    if (e->known_size != UINT64_MAX && take > e->known_size - e->in_total) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
          "lzma: more input than lzma.uncompressed_size");
    }
    memcpy(e->win + e->win_len, (const uint8_t *)input->data + input->used, take);
    e->win_len += take;
    e->in_total += take;
    input->used += take;
  }
}

gcomp_status_t lzma_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  lzma_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  for (;;) {
    enc_drain(e, output);
    if (e->stage_len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (e->finished) {
      return GCOMP_OK;
    }
    if (e->known_size != UINT64_MAX && e->in_total != e->known_size) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
          "lzma: input length is not lzma.uncompressed_size");
    }
    if (enc_has_pending(e)) {
      enc_run(e, 1);
      continue;
    }
    if (e->lzma2) {
      e->stage[0] = 0;
      e->stage_pos = 0;
      e->stage_len = 1;
    }
    else {
      alone_close(e);
    }
    e->finished = 1;
  }
}

/* Only lzma2 can flush: an LZMA stream has one range coder from first byte to
 * last, so there is no point at which its bytes stand alone. */
gcomp_status_t lzma_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  lzma_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "lzma2: encoder cannot flush after finish");
  }
  for (;;) {
    enc_drain(e, output);
    if (e->stage_len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (enc_has_pending(e)) {
      enc_run(e, 1);
      continue;
    }
    break;
  }
  if (mode == GCOMP_FLUSH_FULL) {
    /* Forget everything before this point: the next chunk resets the
     * dictionary, and a match may not reach behind it. */
    enc_model_reset(e);
    e->total = 0;
    e->hist_start = e->pos;
    e->need_dict_reset = e->need_props = e->need_state_reset = 1;
  }
  return GCOMP_OK;
}

gcomp_status_t lzma_encoder_reset(gcomp_encoder_t * encoder) {
  lzma_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  e = encoder->method_state;
  enc_stream_reset(e);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}
