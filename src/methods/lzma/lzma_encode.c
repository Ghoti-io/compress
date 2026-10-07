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
 * Presets 3 and up use an optimal parser instead, after xz's normal mode: a
 * dynamic program over the next stretch of input in which every position holds
 * the cheapest known way to reach it, priced in 1/16 bits from the live
 * probabilities, with the coder state and the four repeat distances carried in
 * each node so that a repeat is priced as the repeat it would be on that path.
 * Prices are cached in tables that are rebuilt every 128 symbols, which is
 * stale by design: a price only steers the choice, and the coder codes what
 * the choice was with the real probabilities, so a stale price costs a little
 * ratio and can never cost correctness.
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

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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

/* Prices are in 1/16 bit, looked up by the probability's top seven bits. */
#define ENC_PRICE_SHIFT 4u
#define ENC_PRICE_ENTRIES (LZMA_PROB_ONE >> 4)
/* A length of 2 to 273 is 272 symbols: 8 low, 8 middle and 256 high. */
#define ENC_LEN_PRICES (LZMA_LEN_LOW_SYMBOLS + LZMA_LEN_MID_SYMBOLS + 256u)
/* How far ahead one run of the optimal parser looks, in positions. */
#define ENC_OPT_MAX 1024u
#define ENC_OPT_INF 0xFFFFFFFFu
#define ENC_MAX_CAND 260u

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

  /* the optimal parser */
  int optimal;
  unsigned bt_depth;
  uint16_t prob_price[ENC_PRICE_ENTRIES];
  int price_dirty;
  unsigned price_age;
  uint16_t slot_price[LZMA_DIST_STATES][LZMA_DIST_SLOTS];
  uint16_t dist_price[LZMA_DIST_STATES][LZMA_FULL_DISTANCES];
  uint16_t align_price[LZMA_ALIGN_SIZE];
  uint16_t len_price[2][LZMA_POS_STATES_MAX][ENC_LEN_PRICES];
  uint8_t len_valid[2][LZMA_POS_STATES_MAX];
  struct enc_opt_s * opt;
  size_t opt_len; /* the highest node holding a price */
  struct enc_sym_s * queue;
  size_t queue_len, queue_pos;
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
typedef struct enc_match_s {
  unsigned len;
  uint32_t dist; /* the distance less one, as the format spells it */
} enc_match_t;

static unsigned bt_step(lzma_enc_t * e, size_t idx, unsigned lim, unsigned avail,
    enc_match_t * pairs);

static void mf_insert_to(lzma_enc_t * e, size_t upto) {
  if (e->optimal) {
    while (e->ins < upto && e->ins + 4u <= e->win_len) {
      size_t room = e->win_len - e->ins;
      (void)bt_step(e, e->ins, room > LZMA_MATCH_MAX ? LZMA_MATCH_MAX : (unsigned)room, 0, NULL);
      e->ins++;
    }
    return;
  }
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

/* How many bytes from the start of a and of b agree, up to limit. The two may
 * overlap (a distance shorter than the length), so this only reads. 16 bytes at
 * a time where SSE2 is there, which is every x86-64, and 8 otherwise; the
 * loads stop short of `limit`, so nothing past the data is read. */
static inline unsigned match_len(
    const uint8_t * a, const uint8_t * b, unsigned limit) {
  unsigned n = 0;
#if defined(__SSE2__)
  while (n + 16u <= limit) {
    __m128i x = _mm_loadu_si128((const __m128i *)(const void *)(a + n));
    __m128i y = _mm_loadu_si128((const __m128i *)(const void *)(b + n));
    unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, y));
    if (m != 0xFFFFu) {
      return n + (unsigned)__builtin_ctz(~m);
    }
    n += 16u;
  }
#else
  while (n + 8u <= limit) {
    uint64_t x, y;
    memcpy(&x, a + n, 8);
    memcpy(&y, b + n, 8);
    if (x != y) {
      uint64_t d = x ^ y;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      return n + (unsigned)(__builtin_clzll(d) >> 3);
#else
      return n + (unsigned)(__builtin_ctzll(d) >> 3);
#endif
    }
    n += 8u;
  }
#endif
  while (n < limit && a[n] == b[n]) {
    n++;
  }
  return n;
}

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
  unsigned n;
  if (dist < 4) {
    return dist;
  }
  n = 31u - (unsigned)__builtin_clz(dist);
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

/* ---- the optimal parser -------------------------------------------------- */

/* The price of a bit in 1/16 bit: -log2(p) of its probability, from the table
 * built by price_init(). */
static inline uint32_t price0(const lzma_enc_t * e, uint16_t prob) {
  return e->prob_price[prob >> ENC_PRICE_SHIFT];
}

static inline uint32_t price1(const lzma_enc_t * e, uint16_t prob) {
  return e->prob_price[(LZMA_PROB_ONE - prob) >> ENC_PRICE_SHIFT];
}

static inline uint32_t price_bit(const lzma_enc_t * e, uint16_t prob, unsigned bit) {
  return bit ? price1(e, prob) : price0(e, prob);
}

/* The table is -log2((i*16 + 8) / 2048) * 16, found without a logarithm by the
 * method the LZMA SDK uses: square the value four times, counting the shifts
 * that keep it under 2^16, which yields the fractional bits of the log. */
static void price_init(lzma_enc_t * e) {
  unsigned i;
  for (i = 0; i < ENC_PRICE_ENTRIES; i++) {
    uint32_t w = (i << ENC_PRICE_SHIFT) + (1u << (ENC_PRICE_SHIFT - 1u));
    uint32_t count = 0;
    unsigned j;
    for (j = 0; j < ENC_PRICE_SHIFT; j++) {
      w *= w;
      count <<= 1;
      while (w >= (1u << 16)) {
        w >>= 1;
        count++;
      }
    }
    e->prob_price[i] =
        (uint16_t)((LZMA_PROB_BITS << ENC_PRICE_SHIFT) - 15u - count);
  }
}

static uint32_t tree_price_reverse(
    const lzma_enc_t * e, const uint16_t * probs, unsigned bits, unsigned sym) {
  uint32_t price = 0;
  unsigned m = 1, i;
  for (i = 0; i < bits; i++) {
    unsigned b = sym & 1u;
    sym >>= 1;
    price += price_bit(e, probs[m], b);
    m = (m << 1) | b;
  }
  return price;
}

/* The prices of every symbol of a tree of `bits` levels, by one pass down the
 * tree rather than a walk per symbol. */
static void tree_prices_all(const lzma_enc_t * e, const uint16_t * probs,
    unsigned bits, uint16_t * out) {
  uint32_t node[512];
  unsigned m, n = 1u << bits;
  node[1] = 0;
  for (m = 1; m < n; m++) {
    node[2 * m] = node[m] + price0(e, probs[m]);
    node[2 * m + 1] = node[m] + price1(e, probs[m]);
  }
  for (m = 0; m < n; m++) {
    out[m] = (uint16_t)node[n + m];
  }
}

/* The prices that depend on the distance model, rebuilt when it has moved on:
 * the slot of each of the four length states, the whole price of each distance
 * under 128, and the four align bits. */
static void price_rebuild(lzma_enc_t * e) {
  unsigned ds, d, i;
  for (ds = 0; ds < LZMA_DIST_STATES; ds++) {
    tree_prices_all(e, e->probs.pos_slot[ds], LZMA_DIST_SLOT_BITS, e->slot_price[ds]);
    for (d = 0; d < LZMA_FULL_DISTANCES; d++) {
      unsigned slot = pos_slot_of(d);
      uint32_t price = e->slot_price[ds][slot];
      if (slot >= 4) {
        unsigned direct = (slot >> 1) - 1;
        uint32_t base = (2u | (slot & 1u)) << direct;
        price += tree_price_reverse(
            e, e->probs.pos_spec + base - slot, direct, d - base);
      }
      e->dist_price[ds][d] = (uint16_t)price;
    }
  }
  for (i = 0; i < LZMA_ALIGN_SIZE; i++) {
    e->align_price[i] =
        (uint16_t)tree_price_reverse(e, e->probs.align, LZMA_ALIGN_BITS, i);
  }
  memset(e->len_valid, 0, sizeof(e->len_valid));
  e->price_dirty = 0;
  e->price_age = 0;
}

/* The price of coding `dist` (the distance less one) after a match of `len`. */
static inline uint32_t dist_price_of(const lzma_enc_t * e, uint32_t dist, unsigned ds) {
  unsigned slot, direct;
  uint32_t base;
  if (dist < LZMA_FULL_DISTANCES) {
    return e->dist_price[ds][dist];
  }
  slot = pos_slot_of(dist);
  direct = (slot >> 1) - 1;
  base = (2u | (slot & 1u)) << direct;
  return e->slot_price[ds][slot] + ((direct - LZMA_ALIGN_BITS) << ENC_PRICE_SHIFT) +
      e->align_price[(dist - base) & (LZMA_ALIGN_SIZE - 1u)];
}

/* The price of a length for one of the two length coders at one position
 * state, from a table filled the first time it is asked for. */
static inline uint32_t len_price_of(
    lzma_enc_t * e, int rep, unsigned pos_state, unsigned len) {
  uint16_t * tab = e->len_price[rep][pos_state];
  if (!e->len_valid[rep][pos_state]) {
    const lzma_len_probs_t * l = rep ? &e->probs.rep_len : &e->probs.len;
    uint32_t c0 = price0(e, l->choice), c1 = price1(e, l->choice);
    uint32_t c20 = price0(e, l->choice2), c21 = price1(e, l->choice2);
    uint16_t sub[256];
    unsigned i;
    tree_prices_all(e, l->low[pos_state], LZMA_LEN_LOW_BITS, sub);
    for (i = 0; i < LZMA_LEN_LOW_SYMBOLS; i++) {
      tab[i] = (uint16_t)(c0 + sub[i]);
    }
    tree_prices_all(e, l->mid[pos_state], LZMA_LEN_MID_BITS, sub);
    for (i = 0; i < LZMA_LEN_MID_SYMBOLS; i++) {
      tab[LZMA_LEN_LOW_SYMBOLS + i] = (uint16_t)(c1 + c20 + sub[i]);
    }
    tree_prices_all(e, l->high, LZMA_LEN_HIGH_BITS, sub);
    for (i = 0; i < 256; i++) {
      tab[LZMA_LEN_LOW_SYMBOLS + LZMA_LEN_MID_SYMBOLS + i] =
          (uint16_t)(c1 + c21 + sub[i]);
    }
    e->len_valid[rep][pos_state] = 1;
  }
  return tab[len - LZMA_MATCH_MIN];
}

/* What a literal costs in the context it would be coded in. */
static uint32_t literal_price(const lzma_enc_t * e, size_t idx,
    size_t position, unsigned state, uint32_t rep0) {
  unsigned byte = e->win[idx];
  unsigned prev = idx > e->hist_start ? e->win[idx - 1] : 0;
  size_t ctx = ((position & (((size_t)1 << e->lp) - 1u)) << e->lc) +
      (prev >> (8u - e->lc));
  const uint16_t * probs = e->lit + (size_t)LZMA_LIT_CODER * ctx;
  uint32_t price = 0;
  unsigned symbol = 1;
  int i;
  if (state >= LZMA_LIT_STATES) {
    unsigned match_byte = e->win[idx - (size_t)rep0 - 1u];
    int same = 1;
    for (i = 7; i >= 0; i--) {
      unsigned bit = (byte >> i) & 1u;
      if (same) {
        unsigned match_bit = (match_byte >> i) & 1u;
        price += price_bit(e, probs[((1u + match_bit) << 8) + symbol], bit);
        same = match_bit == bit;
      }
      else {
        price += price_bit(e, probs[symbol], bit);
      }
      symbol = (symbol << 1) | bit;
    }
  }
  else {
    for (i = 7; i >= 0; i--) {
      unsigned bit = (byte >> i) & 1u;
      price += price_bit(e, probs[symbol], bit);
      symbol = (symbol << 1) | bit;
    }
  }
  return price;
}

typedef struct enc_opt_s {
  uint32_t price;
  uint32_t prev; /* the node this one was reached from */
  uint32_t dist;
  uint16_t len;
  uint8_t kind;
  uint8_t rep;
  uint8_t state;
  uint32_t reps[4];
} enc_opt_t;

/* Offer a way to reach node `to` from node `from`; keep it if it is cheaper. */
static inline void opt_relax(lzma_enc_t * e, uint32_t to, uint32_t price,
    uint32_t from, enc_kind_t kind, unsigned len, uint32_t dist, unsigned rep) {
  enc_opt_t * o;
  const enc_opt_t * f = &e->opt[from];
  while (e->opt_len < to) {
    e->opt[++e->opt_len].price = ENC_OPT_INF;
  }
  o = &e->opt[to];
  if (price >= o->price) {
    return;
  }
  o->price = price;
  o->prev = from;
  o->kind = (uint8_t)kind;
  o->len = (uint16_t)len;
  o->dist = dist;
  o->rep = (uint8_t)rep;
  if (kind == ES_LIT) {
    o->state = (uint8_t)lzma_state_literal(f->state);
    memcpy(o->reps, f->reps, sizeof(o->reps));
  }
  else if (kind == ES_MATCH) {
    o->state = (uint8_t)lzma_state_match(f->state);
    o->reps[0] = dist;
    o->reps[1] = f->reps[0];
    o->reps[2] = f->reps[1];
    o->reps[3] = f->reps[2];
  }
  else {
    unsigned r;
    o->state = (uint8_t)(len == 1 ? lzma_state_short_rep(f->state)
                                  : lzma_state_rep(f->state));
    memcpy(o->reps, f->reps, sizeof(o->reps));
    for (r = rep; r > 0; r--) {
      o->reps[r] = o->reps[r - 1];
    }
    o->reps[0] = f->reps[rep];
  }
}

/* One step of the binary-tree match finder (the LZMA SDK's BT4): enter idx as
 * the new root of the tree for its four-byte hash while walking down it, and
 * when `pairs` is not NULL record the matches met on the way. The walk is a
 * binary search by the bytes that follow, so what it passes is the candidates
 * that matter, each longer than the last, and it re-hangs the subtrees it
 * passes under the new root as it goes. Positions not searched are still
 * entered, with pairs NULL, because a tree with holes in it finds less.
 *
 * Nothing here is trusted: a match is measured against the bytes, so a tree
 * damaged by a stale position, or by a position left behind when the encoder
 * moved on, costs matches and never correctness. Every walk is bounded by
 * `depth`, so a damaged tree cannot loop.
 *
 * `lim` is how many bytes there truly are from idx, at most 273, and is what
 * the walk compares to; `avail` is how many the caller may use, which is less
 * where a chunk ends, and it only clamps what is reported. A tree built
 * against the shorter limit would be ordered on fewer bytes than the data has.
 *
 * Returns the number of pairs: strictly rising lengths, each at the distance
 * the walk found it. */
static unsigned bt_step(lzma_enc_t * e, size_t idx, unsigned lim, unsigned avail,
    enc_match_t * pairs) {
  const uint8_t * p = e->win + idx;
  const uint32_t cur = abs_pos(e, idx) + 1u;
  const uint32_t h4 = hash4(p, e->head_bits);
  const uint32_t mask = e->prev_mask;
  const size_t hist = idx - e->hist_start;
  uint32_t * const son = e->prev;
  uint32_t cand = e->head[h4];
  uint32_t m3 = e->head3[hash3(p)];
  uint32_t m2 = e->head2[hash2(p)];
  uint32_t * ptr1 = son + 2u * ((cur - 1u) & mask);
  uint32_t * ptr0 = ptr1 + 1;
  unsigned len0 = 0, len1 = 0, best = 1, last = 1, np = 0;
  unsigned depth = e->bt_depth;

  e->head[h4] = cur;
  e->head3[hash3(p)] = cur;
  e->head2[hash2(p)] = cur;

  if (pairs) {
    uint32_t direct[2];
    unsigned i;
    direct[0] = m2;
    direct[1] = m3;
    for (i = 0; i < 2; i++) {
      uint32_t d = cur - direct[i];
      if (direct[i] != 0 && (int32_t)d > 0 && d <= e->dict_size && d <= hist) {
        unsigned len = match_len(p, p - d, lim);
        if (len > best) {
          best = len;
          if ((len < avail ? len : avail) > last) {
            last = len < avail ? len : avail;
            pairs[np].len = last;
            pairs[np].dist = d - 1u;
            np++;
          }
        }
      }
    }
  }
  for (;;) {
    uint32_t d = cur - cand;
    uint32_t * pair;
    const uint8_t * pb;
    unsigned len;
    /* A tree node lives in the slot of its position modulo the ring, so a
     * candidate a whole ring behind would be this position's own node:
     * when the dictionary is exactly the ring (a power of two), the farthest
     * distance is left out. */
    if (cand == 0 || (int32_t)d <= 0 || d > e->dict_size || d > mask ||
        d > hist || depth-- == 0) {
      *ptr0 = 0;
      *ptr1 = 0;
      break;
    }
    pair = son + 2u * ((cand - 1u) & mask);
    pb = p - d;
    len = len0 < len1 ? len0 : len1;
    if (pb[len] == p[len]) {
      len += match_len(p + len + 1, pb + len + 1, lim - len - 1u) + 1u;
      if (pairs && len > best) {
        best = len;
        if ((len < avail ? len : avail) > last) {
          last = len < avail ? len : avail;
          pairs[np].len = last;
          pairs[np].dist = d - 1u;
          np++;
        }
      }
      if (len >= lim) {
        *ptr1 = pair[0];
        *ptr0 = pair[1];
        break;
      }
    }
    if (pb[len] < p[len]) {
      *ptr1 = cand;
      ptr1 = pair + 1;
      cand = *ptr1;
      len1 = len;
    }
    else {
      *ptr0 = cand;
      ptr0 = pair;
      cand = *ptr0;
      len0 = len;
    }
  }
  return np;
}

/* The matches at idx, entering idx in the tree: the cheaper positions behind
 * it must already be in. Where the tree already holds idx (the plan that
 * covered it was dropped), there is nothing to enter and nothing is found. */
static unsigned mf_find_all(
    lzma_enc_t * e, size_t idx, unsigned avail, enc_match_t * pairs) {
  size_t room = e->win_len - idx;
  unsigned lim = room > LZMA_MATCH_MAX ? LZMA_MATCH_MAX : (unsigned)room;
  unsigned np;
  if (avail < 2 || idx + 4u > e->win_len || e->ins != idx) {
    return 0;
  }
  np = bt_step(e, idx, lim, avail, pairs);
  e->ins = idx + 1u;
  return np;
}

static inline void queue_sym(lzma_enc_t * e, const enc_opt_t * o) {
  enc_sym_t * s = &e->queue[e->queue_len++];
  s->kind = (enc_kind_t)o->kind;
  s->len = o->len;
  s->dist = o->dist;
  s->rep = o->rep;
}

/* One run of the optimal parser from e->pos: fills the queue with the symbols
 * of the cheapest path it found over the stretch it looked at. */
static void parse_optimal(lzma_enc_t * e, size_t end) {
  const size_t idx0 = e->pos;
  const size_t pos_mask = ((size_t)1 << e->pb) - 1u;
  enc_match_t pairs[ENC_MAX_CAND];
  uint32_t cur = 0, stop;
  enc_opt_t * opt = e->opt;

  e->queue_len = 0;
  e->queue_pos = 0;
  if (e->price_dirty || e->price_age >= 128u) {
    price_rebuild(e);
  }
  opt[0].price = 0;
  opt[0].state = (uint8_t)e->state;
  memcpy(opt[0].reps, e->rep, sizeof(opt[0].reps));
  e->opt_len = 0;

  for (;;) {
    const enc_opt_t * n = &opt[cur];
    size_t idx = idx0 + cur;
    size_t position = (size_t)e->total + cur;
    unsigned pos_state = (unsigned)position & (unsigned)pos_mask;
    unsigned avail = end - idx > LZMA_MATCH_MAX ? LZMA_MATCH_MAX : (unsigned)(end - idx);
    size_t hist = idx - e->hist_start;
    unsigned state = n->state;
    uint32_t base = n->price;
    uint32_t p_match = base + price1(e, e->probs.is_match[state][pos_state]);
    uint32_t p_rep = p_match + price1(e, e->probs.is_rep[state]);
    uint32_t p_nomatch = p_match + price0(e, e->probs.is_rep[state]);
    unsigned rep_len[4];
    unsigned np = 0, r, k, longest = 0, rep_best = 0;
    unsigned prev_len;

    mf_insert_to(e, idx);
    if (avail >= 2) {
      np = mf_find_all(e, idx, avail, pairs);
    }
    for (r = 0; r < 4; r++) {
      size_t d = (size_t)n->reps[r] + 1u;
      rep_len[r] = 0;
      if (avail >= 2 && d <= hist) {
        rep_len[r] = match_len(e->win + idx, e->win + idx - d, avail);
        if (rep_len[r] < 2) {
          rep_len[r] = 0;
        }
      }
      if (rep_len[r] > rep_best) {
        rep_best = rep_len[r];
      }
    }
    if (np > 0) {
      longest = pairs[np - 1].len;
    }

    /* A match as long as the encoder is willing to look for ends the search:
     * take it and stop. */
    if (rep_best >= e->nice_len || longest >= e->nice_len) {
      unsigned len;
      if (rep_best >= e->nice_len) {
        for (r = 0; r < 4 && rep_len[r] != rep_best; r++) {
        }
        len = rep_best;
        opt_relax(e, cur + len,
            p_rep + (r == 0 ? price0(e, e->probs.is_rep_g0[state]) +
                          price1(e, e->probs.is_rep0_long[state][pos_state])
                : r == 1  ? price1(e, e->probs.is_rep_g0[state]) +
                          price0(e, e->probs.is_rep_g1[state])
                          : price1(e, e->probs.is_rep_g0[state]) +
                          price1(e, e->probs.is_rep_g1[state]) +
                          price_bit(e, e->probs.is_rep_g2[state], r - 2u)) +
                len_price_of(e, 1, pos_state, len),
            cur, ES_REP, len, 0, r);
      }
      else {
        len = longest;
        opt_relax(e, cur + len,
            p_nomatch + len_price_of(e, 0, pos_state, len) +
                dist_price_of(e, pairs[np - 1].dist, lzma_dist_state(len)),
            cur, ES_MATCH, len, pairs[np - 1].dist, 0);
      }
      stop = cur + len;
      break;
    }

    /* A literal. */
    opt_relax(e, cur + 1u,
        base + price0(e, e->probs.is_match[state][pos_state]) +
            literal_price(e, idx, position, state, n->reps[0]),
        cur, ES_LIT, 1, 0, 0);

    /* The one-byte repeat of the last distance. */
    if (hist > n->reps[0] && e->win[idx] == e->win[idx - (size_t)n->reps[0] - 1u]) {
      opt_relax(e, cur + 1u,
          p_rep + price0(e, e->probs.is_rep_g0[state]) +
              price0(e, e->probs.is_rep0_long[state][pos_state]),
          cur, ES_REP, 1, 0, 0);
    }

    /* Repeats of the four recent distances. */
    for (r = 0; r < 4; r++) {
      uint32_t sel;
      unsigned len;
      if (rep_len[r] < 2) {
        continue;
      }
      sel = p_rep +
          (r == 0 ? price0(e, e->probs.is_rep_g0[state]) +
                  price1(e, e->probs.is_rep0_long[state][pos_state])
              : r == 1 ? price1(e, e->probs.is_rep_g0[state]) +
                  price0(e, e->probs.is_rep_g1[state])
                       : price1(e, e->probs.is_rep_g0[state]) +
                  price1(e, e->probs.is_rep_g1[state]) +
                  price_bit(e, e->probs.is_rep_g2[state], r - 2u));
      for (len = 2; len <= rep_len[r]; len++) {
        opt_relax(e, cur + len, sel + len_price_of(e, 1, pos_state, len), cur,
            ES_REP, len, 0, r);
      }
    }

    /* Matches, each length at the nearest distance that reaches it. */
    prev_len = 1;
    for (k = 0; k < np; k++) {
      unsigned len;
      for (len = prev_len + 1u; len <= pairs[k].len; len++) {
        opt_relax(e, cur + len,
            p_nomatch + len_price_of(e, 0, pos_state, len) +
                dist_price_of(e, pairs[k].dist, lzma_dist_state(len)),
            cur, ES_MATCH, len, pairs[k].dist, 0);
      }
      prev_len = pairs[k].len;
    }

    cur++;
    if (cur >= e->opt_len || cur >= ENC_OPT_MAX) {
      stop = cur;
      break;
    }
  }

  /* Back from the node the look-ahead ended at, to the start. */
  {
    uint32_t b = stop;
    size_t first;
    size_t last;
    while (b != 0) {
      queue_sym(e, &opt[b]);
      b = opt[b].prev;
    }
    first = 0;
    last = e->queue_len;
    while (first + 1 < last) {
      enc_sym_t t = e->queue[first];
      e->queue[first] = e->queue[last - 1];
      e->queue[last - 1] = t;
      first++;
      last--;
    }
  }
}

/* Code symbols over [pos, end), stopping early if the coder's output would
 * pass `out_limit` bytes. Returns the bytes covered. */
static size_t enc_symbols(lzma_enc_t * e, size_t end, size_t out_limit) {
  size_t start = e->pos;
  while (e->pos < end && renc_pending(&e->rc) + ENC_SYMBOL_MARGIN < out_limit) {
    enc_sym_t s;
    unsigned n;
    if (e->optimal) {
      if (e->queue_pos == e->queue_len) {
        parse_optimal(e, end);
      }
      s = e->queue[e->queue_pos++];
      e->price_age++;
    }
    else {
      parse_fast(e, end, &s);
    }
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
  /* Symbols planned and not yet coded when the limit stopped the loop stay in
   * the queue for the next call, which carries on from the same position. */
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
  e->price_dirty = 1;
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
  e->queue_len = e->queue_pos = 0;
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
  e->optimal = preset >= 3;
  e->bt_depth = 16u + e->nice_len / 2u;
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
  e->prev = gcomp_calloc(alloc, e->optimal ? 2u * prev_n : prev_n, sizeof(uint32_t));
  e->lit = gcomp_malloc(alloc, e->lit_count * sizeof(uint16_t));
  e->stage = gcomp_malloc(alloc, ENC_STAGE_CAP);
  if (lzma2) {
    e->snap_lit = gcomp_malloc(alloc, e->lit_count * sizeof(uint16_t));
  }
  if (e->optimal) {
    e->opt = gcomp_malloc(
        alloc, (ENC_OPT_MAX + LZMA_MATCH_MAX + 2u) * sizeof(*e->opt));
    e->queue = gcomp_malloc(
        alloc, (ENC_OPT_MAX + LZMA_MATCH_MAX + 2u) * sizeof(*e->queue));
    price_init(e);
  }
  if (!e->win || !e->head || !e->head3 || !e->head2 || !e->prev || !e->lit ||
      !e->stage || (lzma2 && !e->snap_lit) ||
      (e->optimal && (!e->opt || !e->queue))) {
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
  gcomp_free(e->alloc, e->opt);
  gcomp_free(e->alloc, e->queue);
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
    e->queue_len = e->queue_pos = 0; /* planned for a model that is gone */
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
