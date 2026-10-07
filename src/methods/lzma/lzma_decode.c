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
 * @file lzma_decode.c
 *
 * The LZMA and LZMA2 decoders.
 *
 * ## One parse, written once
 *
 * A streaming decoder has to know, before it commits to a symbol, that the
 * whole symbol's input is there: the range coder reads a byte at a time in the
 * middle of a decision, and a decoder that runs out half way has already moved
 * its probabilities. The usual answers are a resumable state machine, which is
 * a second copy of the model written as a coroutine, or a trial decode that
 * leaves the probabilities alone, which is a second copy written read-only.
 *
 * This file does the second with one function. `parse()` decodes one symbol
 * and takes a `dry` flag; with it set no probability is written, so the same
 * code that decodes a symbol also answers whether the input holds one. It is
 * run dry only when fewer than LZMA_REQUIRED_INPUT_MAX bytes are in hand, which
 * is the edge of a call, and a symbol that is not all there is parked in
 * `carry[]` until more input arrives. Nothing else resumes mid-symbol.
 *
 * ## The window
 *
 * The dictionary is a ring that doubles up to the stream's window rather than
 * being allocated at the size a header asks for. A header can name 4 GiB; a
 * stream that has produced a kilobyte needs a kilobyte. Growth stops at the
 * window, which is what a distance may reach, and after that the ring wraps.
 *
 * ## LZMA2
 *
 * LZMA2 is a framing: chunks that are either stored or LZMA-coded, each LZMA
 * chunk restarting the range coder and optionally resetting the state, the
 * properties, or the dictionary. This decoder drives the same core through it;
 * the dictionary persists across chunks and the counter that picks a position
 * state counts every byte since the last dictionary reset, stored chunks
 * included.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "lzma_internal.h"

#include <ghoti.io/compress/limits.h>

#include <string.h>

#define LZMA_DEFAULT_MAX_WINDOW (3ull << 29)
#define LZMA_DICT_START 65536u

typedef struct lzma_rc_s {
  uint32_t range;
  uint32_t code;
  const uint8_t * in;
  const uint8_t * in_end;
  int starved;
} lzma_rc_t;

typedef enum { SYM_LIT, SYM_MATCH, SYM_SHORTREP, SYM_REP } lzma_sym_kind_t;

typedef struct lzma_sym_s {
  lzma_sym_kind_t kind;
  unsigned len;
  uint32_t dist;
  unsigned rep;
  uint8_t byte;
} lzma_sym_t;

typedef enum {
  RUN_NEED_INPUT,
  RUN_NEED_OUTPUT,
  RUN_LIMIT,
  RUN_EOS,
  RUN_ERROR
} lzma_run_t;

typedef struct lzma_dec_s {
  const gcomp_allocator_t * alloc;
  unsigned lc, lp, pb;
  uint32_t window; /* the largest distance + 1 a stream may use */
  lzma_probs_t probs;
  uint16_t * lit;
  size_t lit_count;
  unsigned state;
  uint32_t rep[4];
  uint8_t * buf;
  size_t cap;
  size_t cap_max;
  size_t pos;
  uint64_t total; /* bytes since the last dictionary reset */
  uint32_t range, code;
  int rc_ready;
  uint8_t rc_init[5];
  unsigned rc_have;
  uint32_t remain_len;
  uint8_t carry[LZMA_REQUIRED_INPUT_MAX];
  unsigned carry_len;
  uint64_t max_mem;
  const char * err;
  gcomp_status_t err_status;
} lzma_dec_t;

/* ---- range decoder ------------------------------------------------------- */

static inline void rc_norm(lzma_rc_t * rc) {
  if (rc->range < LZMA_TOP) {
    if (rc->in == rc->in_end) {
      rc->starved = 1;
      return;
    }
    rc->range <<= 8;
    rc->code = (rc->code << 8) | *rc->in++;
  }
}

static inline unsigned rc_bit(lzma_rc_t * rc, uint16_t * prob, int dry) {
  uint32_t p = *prob;
  uint32_t bound = (rc->range >> LZMA_PROB_BITS) * p;
  unsigned bit;
  if (rc->code < bound) {
    rc->range = bound;
    if (!dry) {
      *prob = (uint16_t)(p + ((LZMA_PROB_ONE - p) >> LZMA_MOVE_BITS));
    }
    bit = 0;
  }
  else {
    rc->range -= bound;
    rc->code -= bound;
    if (!dry) {
      *prob = (uint16_t)(p - (p >> LZMA_MOVE_BITS));
    }
    bit = 1;
  }
  rc_norm(rc);
  return bit;
}

static inline uint32_t rc_direct(lzma_rc_t * rc, unsigned count) {
  uint32_t res = 0;
  while (count--) {
    uint32_t t;
    rc->range >>= 1;
    rc->code -= rc->range;
    t = 0u - (rc->code >> 31);
    rc->code += rc->range & t;
    rc_norm(rc);
    res = (res << 1) + t + 1u;
  }
  return res;
}

static inline unsigned bit_tree(
    lzma_rc_t * rc, uint16_t * probs, unsigned bits, int dry) {
  unsigned m = 1;
  unsigned i;
  for (i = 0; i < bits; i++) {
    m = (m << 1) | rc_bit(rc, probs + m, dry);
  }
  return m - (1u << bits);
}

static inline unsigned bit_tree_reverse(
    lzma_rc_t * rc, uint16_t * probs, unsigned bits, int dry) {
  unsigned m = 1;
  unsigned sym = 0;
  unsigned i;
  for (i = 0; i < bits; i++) {
    unsigned bit = rc_bit(rc, probs + m, dry);
    m = (m << 1) | bit;
    sym |= bit << i;
  }
  return sym;
}

/* ---- window ------------------------------------------------------------- */

/* The byte `dist` back, 1 being the one just written. Zero before there is
 * one, which is what the literal coder's context expects of an empty window. */
static inline uint8_t dict_byte(const lzma_dec_t * d, uint64_t dist) {
  size_t p;
  if (dist > d->total || d->cap == 0) {
    return 0;
  }
  p = d->pos >= dist ? d->pos - (size_t)dist : d->pos + d->cap - (size_t)dist;
  return d->buf[p];
}

static int dict_grow(lzma_dec_t * d) {
  size_t want = d->cap == 0 ? LZMA_DICT_START : d->cap * 2u;
  uint8_t * nb;
  if (want > d->cap_max || want < d->cap) {
    want = d->cap_max;
  }
  if (gcomp_limits_check_memory(want + d->lit_count * sizeof(uint16_t),
          d->max_mem) != GCOMP_OK) {
    d->err = "lzma: window exceeds limits.max_memory_bytes";
    d->err_status = GCOMP_ERR_LIMIT;
    return -1;
  }
  nb = gcomp_realloc(d->alloc, d->buf, want);
  if (!nb) {
    d->err = "lzma: out of memory growing the window";
    d->err_status = GCOMP_ERR_MEMORY;
    return -1;
  }
  d->buf = nb;
  d->cap = want;
  return 0;
}

static inline int dict_put(lzma_dec_t * d, uint8_t b) {
  if (d->pos == d->cap) {
    if (d->cap < d->cap_max) {
      if (dict_grow(d) != 0) {
        return -1;
      }
    }
    else {
      d->pos = 0;
    }
  }
  d->buf[d->pos++] = b;
  d->total++;
  return 0;
}

/* ---- one symbol --------------------------------------------------------- */

static inline unsigned parse_len(lzma_rc_t * rc, lzma_len_probs_t * l,
    unsigned pos_state, int dry) {
  if (!rc_bit(rc, &l->choice, dry)) {
    return bit_tree(rc, l->low[pos_state], LZMA_LEN_LOW_BITS, dry);
  }
  if (!rc_bit(rc, &l->choice2, dry)) {
    return LZMA_LEN_LOW_SYMBOLS +
        bit_tree(rc, l->mid[pos_state], LZMA_LEN_MID_BITS, dry);
  }
  return LZMA_LEN_LOW_SYMBOLS + LZMA_LEN_MID_SYMBOLS +
      bit_tree(rc, l->high, LZMA_LEN_HIGH_BITS, dry);
}

static inline uint32_t parse_dist(
    lzma_rc_t * rc, lzma_probs_t * P, unsigned len, int dry) {
  unsigned dist_state = len < LZMA_DIST_STATES - 1 ? len : LZMA_DIST_STATES - 1;
  unsigned slot = bit_tree(rc, P->pos_slot[dist_state], LZMA_DIST_SLOT_BITS, dry);
  unsigned direct_bits;
  uint32_t dist;
  if (slot < 4) {
    return slot;
  }
  direct_bits = (slot >> 1) - 1;
  dist = (2u | (slot & 1u)) << direct_bits;
  if (slot < LZMA_END_POS_MODEL) {
    dist += bit_tree_reverse(rc, P->pos_spec + dist - slot, direct_bits, dry);
  }
  else {
    dist += rc_direct(rc, direct_bits - LZMA_ALIGN_BITS) << LZMA_ALIGN_BITS;
    dist += bit_tree_reverse(rc, P->align, LZMA_ALIGN_BITS, dry);
  }
  return dist;
}

/* Decode one symbol from rc. With dry set nothing but rc is written, so the
 * same code answers "is the whole symbol here" and "what is it". */
static inline void parse(
    lzma_dec_t * d, lzma_rc_t * rc, lzma_sym_t * s, int dry) {
  lzma_probs_t * P = &d->probs;
  unsigned state = d->state;
  unsigned pos_state = (unsigned)d->total & ((1u << d->pb) - 1u);
  unsigned len;

  if (!rc_bit(rc, &P->is_match[state][pos_state], dry)) {
    unsigned prev = dict_byte(d, 1);
    size_t ctx = ((size_t)d->total & (((size_t)1 << d->lp) - 1u)) << d->lc;
    uint16_t * probs;
    unsigned symbol = 1;
    ctx += prev >> (8u - d->lc);
    probs = d->lit + (size_t)LZMA_LIT_CODER * ctx;
    if (state >= LZMA_LIT_STATES) {
      /* After a match the literal is coded against the byte the last
       * distance points at, until the two first differ. */
      unsigned match_byte = dict_byte(d, (uint64_t)d->rep[0] + 1u);
      do {
        unsigned match_bit = (match_byte >> 7) & 1u;
        unsigned bit;
        match_byte <<= 1;
        bit = rc_bit(rc, &probs[((1u + match_bit) << 8) + symbol], dry);
        symbol = (symbol << 1) | bit;
        if (match_bit != bit) {
          break;
        }
      } while (symbol < 0x100u);
    }
    while (symbol < 0x100u) {
      symbol = (symbol << 1) | rc_bit(rc, &probs[symbol], dry);
    }
    s->kind = SYM_LIT;
    s->byte = (uint8_t)symbol;
    return;
  }

  if (!rc_bit(rc, &P->is_rep[state], dry)) {
    len = parse_len(rc, &P->len, pos_state, dry);
    s->kind = SYM_MATCH;
    s->dist = parse_dist(rc, P, len, dry);
    s->len = len + LZMA_MATCH_MIN;
    return;
  }

  if (!rc_bit(rc, &P->is_rep_g0[state], dry)) {
    if (!rc_bit(rc, &P->is_rep0_long[state][pos_state], dry)) {
      s->kind = SYM_SHORTREP;
      s->len = 1;
      s->rep = 0;
      return;
    }
    s->rep = 0;
  }
  else if (!rc_bit(rc, &P->is_rep_g1[state], dry)) {
    s->rep = 1;
  }
  else {
    s->rep = 2u + rc_bit(rc, &P->is_rep_g2[state], dry);
  }
  len = parse_len(rc, &P->rep_len, pos_state, dry);
  s->kind = SYM_REP;
  s->len = len + LZMA_MATCH_MIN;
}

/* ---- the core: parse, apply, and the streaming around it ---------------- */

static int core_set_props(lzma_dec_t * d, unsigned lc, unsigned lp, unsigned pb) {
  size_t n = lzma_lit_count(lc, lp);
  if (n != d->lit_count) {
    uint16_t * nl;
    if (gcomp_limits_check_memory(n * sizeof(uint16_t), d->max_mem) !=
        GCOMP_OK) {
      d->err = "lzma: literal coders exceed limits.max_memory_bytes";
      d->err_status = GCOMP_ERR_LIMIT;
      return -1;
    }
    nl = gcomp_realloc(d->alloc, d->lit, n * sizeof(uint16_t));
    if (!nl) {
      d->err = "lzma: out of memory for the literal coders";
      d->err_status = GCOMP_ERR_MEMORY;
      return -1;
    }
    d->lit = nl;
    d->lit_count = n;
  }
  d->lc = lc;
  d->lp = lp;
  d->pb = pb;
  return 0;
}

static void core_state_reset(lzma_dec_t * d) {
  lzma_probs_init(&d->probs, d->lit, d->lit_count);
  d->state = 0;
  d->rep[0] = d->rep[1] = d->rep[2] = d->rep[3] = 0;
  d->remain_len = 0;
}

static void core_dict_reset(lzma_dec_t * d) {
  d->pos = 0;
  d->total = 0;
}

static void core_rc_start(lzma_dec_t * d) {
  d->rc_ready = 0;
  d->rc_have = 0;
  d->carry_len = 0;
}

static lzma_run_t core_fail(lzma_dec_t * d, const char * msg) {
  d->err = msg;
  d->err_status = GCOMP_ERR_CORRUPT;
  return RUN_ERROR;
}

/**
 * Decode until the output is full, the input is out, `produce_max` bytes have
 * been written, or the end marker is read.
 *
 * Bytes go to the caller's buffer and to the window together, so nothing is
 * staged: a match that does not fit is parked in remain_len and finished first
 * on the next call.
 *
 * With `eos_only` set and `produce_max` spent, the next symbol must be the end
 * marker. That is the case a stream with a declared size and a marker both:
 * the spec says the marker may follow, so a range coder that is not at zero
 * when the size is reached is not yet an error.
 */
static lzma_run_t core_run(lzma_dec_t * d, const uint8_t ** inp,
    const uint8_t * in_end, uint8_t * out, size_t * out_used, size_t out_size,
    uint64_t produce_max, int allow_eos, int eos_only) {
  const uint8_t * in = *inp;
  uint64_t left = produce_max;
  size_t room = out_size - *out_used;
  lzma_run_t result;

  if (!d->rc_ready) {
    while (d->rc_have < 5u && in < in_end) {
      d->rc_init[d->rc_have++] = *in++;
    }
    if (d->rc_have < 5u) {
      result = RUN_NEED_INPUT;
      goto done;
    }
    if (d->rc_init[0] != 0) {
      result = core_fail(d, "lzma: range coder does not start with a zero byte");
      goto done;
    }
    d->range = 0xFFFFFFFFu;
    d->code = ((uint32_t)d->rc_init[1] << 24) | ((uint32_t)d->rc_init[2] << 16) |
        ((uint32_t)d->rc_init[3] << 8) | (uint32_t)d->rc_init[4];
    d->rc_ready = 1;
  }

  for (;;) {
    lzma_sym_t s;
    lzma_rc_t rc;
    uint8_t tmp[LZMA_REQUIRED_INPUT_MAX];
    size_t avail;

    while (d->remain_len != 0 && left != 0 && room != 0) {
      uint8_t b = dict_byte(d, (uint64_t)d->rep[0] + 1u);
      if (dict_put(d, b) != 0) {
        result = RUN_ERROR;
        goto done;
      }
      out[(*out_used)++] = b;
      d->remain_len--;
      left--;
      room--;
    }
    if (left == 0 && !eos_only) {
      result = RUN_LIMIT;
      goto done;
    }
    if (left != 0 && room == 0) {
      result = RUN_NEED_OUTPUT;
      goto done;
    }

    avail = (size_t)(in_end - in);
    if (d->carry_len == 0 && avail >= LZMA_REQUIRED_INPUT_MAX) {
      rc.range = d->range;
      rc.code = d->code;
      rc.in = in;
      rc.in_end = in_end;
      rc.starved = 0;
      parse(d, &rc, &s, 0);
      in = rc.in;
    }
    else {
      size_t take = LZMA_REQUIRED_INPUT_MAX - d->carry_len;
      size_t have;
      size_t used;
      if (take > avail) {
        take = avail;
      }
      memcpy(tmp, d->carry, d->carry_len);
      if (take != 0) {
        memcpy(tmp + d->carry_len, in, take);
      }
      have = d->carry_len + take;
      rc.range = d->range;
      rc.code = d->code;
      rc.in = tmp;
      rc.in_end = tmp + have;
      rc.starved = 0;
      parse(d, &rc, &s, 1);
      if (rc.starved) {
        if (have >= LZMA_REQUIRED_INPUT_MAX) {
          result = core_fail(d, "lzma: a symbol needs more input than any can");
          goto done;
        }
        if (take != 0) {
          memcpy(d->carry + d->carry_len, in, take);
        }
        d->carry_len = (unsigned)have;
        in += take;
        result = RUN_NEED_INPUT;
        goto done;
      }
      rc.range = d->range;
      rc.code = d->code;
      rc.in = tmp;
      rc.starved = 0;
      parse(d, &rc, &s, 0);
      used = (size_t)(rc.in - tmp);
      if (used <= d->carry_len) {
        memmove(d->carry, d->carry + used, d->carry_len - used);
        d->carry_len -= (unsigned)used;
      }
      else {
        in += used - d->carry_len;
        d->carry_len = 0;
      }
    }
    if (rc.starved) {
      result = core_fail(d, "lzma: input ran out inside a checked symbol");
      goto done;
    }
    d->range = rc.range;
    d->code = rc.code;

    if (left == 0 && !(s.kind == SYM_MATCH && s.dist == LZMA_EOS_DISTANCE)) {
      /* The declared size is spent and the stream goes on with something
       * other than the marker that may follow it. */
      result = core_fail(d, "lzma: data continues past the declared size");
      goto done;
    }

    switch (s.kind) {
    case SYM_LIT:
      if (dict_put(d, s.byte) != 0) {
        result = RUN_ERROR;
        goto done;
      }
      out[(*out_used)++] = s.byte;
      left--;
      room--;
      d->state = lzma_state_literal(d->state);
      break;
    case SYM_SHORTREP:
      /* Only the count is checked, not the window: every distance in rep[]
       * was checked against both when it was read, and a dictionary reset
       * comes with a state reset, which zeroes them. A window check here
       * could never fire. */
      if ((uint64_t)d->rep[0] >= d->total) {
        result = core_fail(d, "lzma: short rep with nothing to repeat");
        goto done;
      }
      s.byte = dict_byte(d, (uint64_t)d->rep[0] + 1u);
      if (dict_put(d, s.byte) != 0) {
        result = RUN_ERROR;
        goto done;
      }
      out[(*out_used)++] = s.byte;
      left--;
      room--;
      d->state = lzma_state_short_rep(d->state);
      break;
    case SYM_MATCH:
      d->rep[3] = d->rep[2];
      d->rep[2] = d->rep[1];
      d->rep[1] = d->rep[0];
      d->rep[0] = s.dist;
      if (s.dist == LZMA_EOS_DISTANCE) {
        if (!allow_eos) {
          result = core_fail(d, "lzma: end marker where none is allowed");
          goto done;
        }
        if (d->code != 0) {
          result = core_fail(d, "lzma: end marker is not at the end of the data");
          goto done;
        }
        result = RUN_EOS;
        goto done;
      }
      if ((uint64_t)s.dist >= d->total || s.dist >= d->window) {
        result = core_fail(d, "lzma: match distance is beyond the window");
        goto done;
      }
      d->state = lzma_state_match(d->state);
      d->remain_len = s.len;
      break;
    case SYM_REP: {
      uint32_t dist = d->rep[s.rep];
      if ((uint64_t)dist >= d->total) {
        result = core_fail(d, "lzma: rep match with nothing to repeat");
        goto done;
      }
      while (s.rep > 0) {
        d->rep[s.rep] = d->rep[s.rep - 1];
        s.rep--;
      }
      d->rep[0] = dist;
      d->state = lzma_state_rep(d->state);
      d->remain_len = s.len;
      break;
    }
    }
  }

done:
  *inp = in;
  return result;
}

static void core_free(lzma_dec_t * d) {
  gcomp_free(d->alloc, d->buf);
  gcomp_free(d->alloc, d->lit);
  d->buf = NULL;
  d->lit = NULL;
  d->cap = 0;
  d->lit_count = 0;
}

/* ---- the decoder -------------------------------------------------------- */

/* Why a step stopped: it wants input, or it wants room to write into. */
#define BLOCKED_INPUT 1
#define BLOCKED_OUTPUT 2

typedef enum {
  PH_HEADER,   /* alone: the 13-byte header */
  PH_BODY,     /* alone and raw: the range-coded stream */
  PH_CONTROL,  /* lzma2: a chunk's control byte */
  PH_CHDR,     /* lzma2: the rest of a chunk header */
  PH_LZMA,     /* lzma2: an LZMA chunk's body */
  PH_STORED,   /* lzma2: a stored chunk's body */
  PH_DONE
} lzma_phase_t;

typedef struct lzma_decoder_s {
  gcomp_decoder_t * pub;
  const gcomp_allocator_t * alloc;
  int lzma2;
  int raw;
  lzma_dec_t core;
  uint64_t max_out, max_mem, max_ratio, max_window;
  unsigned opt_lc, opt_lp, opt_pb;
  uint64_t opt_dict, opt_size;
  lzma_phase_t phase;
  uint8_t hdr[13];
  unsigned hdr_len;
  uint64_t known_size; /* UINT64_MAX when the stream does not say */
  uint64_t produced;
  uint64_t in_total;
  /* lzma2 */
  uint8_t control;
  uint8_t chdr[5];
  unsigned chdr_have, chdr_need;
  uint32_t usize_left, csize_left;
  int need_dict_reset, need_props;
  int expect_eos; /* the declared size is spent and a marker must follow */
  int failed;
  gcomp_status_t err;
} lzma_decoder_t;

static gcomp_status_t dec_fail(
    lzma_decoder_t * st, gcomp_status_t status, const char * msg) {
  st->failed = 1;
  st->err = gcomp_decoder_set_error(st->pub, status, "%s", msg);
  return st->err;
}

static gcomp_status_t dec_core_error(lzma_decoder_t * st) {
  return dec_fail(st,
      st->core.err_status != GCOMP_OK ? st->core.err_status : GCOMP_ERR_CORRUPT,
      st->core.err ? st->core.err : "lzma: corrupt stream");
}

/* The window a stream may use, clamped to what this decoder allows. */
static gcomp_status_t dec_set_window(lzma_decoder_t * st, uint64_t dict) {
  if (dict < LZMA_DICT_MIN) {
    dict = LZMA_DICT_MIN;
  }
  if (dict > st->max_window) {
    return dec_fail(st, GCOMP_ERR_LIMIT,
        "lzma: window exceeds limits.max_window_bytes");
  }
  st->core.window = (uint32_t)dict;
  st->core.cap_max = (size_t)dict;
  return GCOMP_OK;
}

static void dec_reset_fields(lzma_decoder_t * st) {
  lzma_dec_t * c = &st->core;
  st->failed = 0;
  st->err = GCOMP_OK;
  st->produced = 0;
  st->in_total = 0;
  st->hdr_len = 0;
  st->chdr_have = 0;
  st->chdr_need = 0;
  st->usize_left = st->csize_left = 0;
  st->need_dict_reset = st->need_props = 1;
  st->known_size = UINT64_MAX;
  st->expect_eos = 0;
  c->err = NULL;
  c->err_status = GCOMP_OK;
  core_dict_reset(c);
  core_rc_start(c);
  c->remain_len = 0;
  if (st->lzma2) {
    st->phase = PH_CONTROL;
  }
  else if (st->raw) {
    st->phase = PH_BODY;
    st->known_size = st->opt_size;
    if (c->lit) {
      core_state_reset(c);
    }
  }
  else {
    st->phase = PH_HEADER;
  }
}

gcomp_status_t lzma_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder, int lzma2) {
  const gcomp_allocator_t * alloc;
  lzma_decoder_t * st;
  int64_t lc = 3, lp = 0, pb = 2;
  uint64_t dict = 1u << 23, size = UINT64_MAX;
  int raw = 0;
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  alloc = gcomp_registry_get_allocator(registry);
  st = gcomp_calloc(alloc, 1, sizeof(*st));
  if (!st) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "lzma: out of memory");
  }
  st->pub = decoder;
  st->alloc = alloc;
  st->core.alloc = alloc;
  st->lzma2 = lzma2;
  if (options && !lzma2) {
    (void)gcomp_options_get_bool(options, "lzma.raw", &raw);
    (void)gcomp_options_get_int64(options, "lzma.lc", &lc);
    (void)gcomp_options_get_int64(options, "lzma.lp", &lp);
    (void)gcomp_options_get_int64(options, "lzma.pb", &pb);
    (void)gcomp_options_get_uint64(options, "lzma.dict_size", &dict);
    (void)gcomp_options_get_uint64(options, "lzma.uncompressed_size", &size);
  }
  st->raw = raw;
  st->opt_lc = (unsigned)lc;
  st->opt_lp = (unsigned)lp;
  st->opt_pb = (unsigned)pb;
  st->opt_dict = dict;
  st->opt_size = size;
  st->max_out = gcomp_limits_read_output_max(options, 0);
  st->max_mem = gcomp_limits_read_memory_max(options, 0);
  st->max_window =
      gcomp_limits_read_window_max(options, LZMA_DEFAULT_MAX_WINDOW);
  st->max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_LZMA_MAX_EXPANSION_RATIO);
  st->core.max_mem = st->max_mem;
  decoder->method_state = st;
  if (lzma2) {
    /* The window is what the stream may reach; LZMA2's own dictionary
     * property is a promise the caller has already used to size memory. */
    uint64_t w = st->max_window < 0xFFFFFFFFull ? st->max_window : 0xFFFFFFFFull;
    st->core.window = (uint32_t)w;
    st->core.cap_max = (size_t)w;
  }
  else if (raw) {
    if (dec_set_window(st, dict) != GCOMP_OK) {
      gcomp_free(alloc, st);
      decoder->method_state = NULL;
      return GCOMP_ERR_LIMIT;
    }
    if (core_set_props(&st->core, st->opt_lc, st->opt_lp, st->opt_pb) != 0) {
      gcomp_status_t s = st->core.err_status;
      core_free(&st->core);
      gcomp_free(alloc, st);
      decoder->method_state = NULL;
      return s;
    }
    core_state_reset(&st->core);
  }
  dec_reset_fields(st);
  return GCOMP_OK;
}

void lzma_decoder_destroy(gcomp_decoder_t * decoder) {
  lzma_decoder_t * st;
  if (!decoder || !decoder->method_state) {
    return;
  }
  st = decoder->method_state;
  core_free(&st->core);
  gcomp_free(st->alloc, st);
  decoder->method_state = NULL;
}

gcomp_status_t lzma_decoder_reset(gcomp_decoder_t * decoder) {
  lzma_decoder_t * st;
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  dec_reset_fields(st);
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

/* Run the core over what this phase's input is, then settle the accounting. */
static gcomp_status_t dec_run_core(lzma_decoder_t * st, const uint8_t ** in,
    const uint8_t * in_end, gcomp_buffer_t * output, uint64_t produce_max,
    int allow_eos, int eos_only, lzma_run_t * run_out) {
  size_t before = output->used;
  const uint8_t * start = *in;
  uint64_t allow = UINT64_MAX;
  lzma_run_t r;
  uint8_t * dst = (uint8_t *)output->data;
  if (st->max_out != 0) {
    allow = st->max_out > st->produced ? st->max_out - st->produced : 0;
    if (allow != UINT64_MAX) {
      allow += 1; /* one more, so that exceeding is seen and equalling is not */
    }
    if (allow < produce_max) {
      produce_max = allow;
    }
  }
  r = core_run(&st->core, in, in_end, dst, &output->used, output->size,
      produce_max, allow_eos, eos_only);
  st->in_total += (uint64_t)(*in - start);
  st->produced += (uint64_t)(output->used - before);
  if (r == RUN_ERROR) {
    return dec_core_error(st);
  }
  if (st->max_out != 0 && st->produced > st->max_out) {
    output->used -= (size_t)(st->produced - st->max_out);
    st->produced = st->max_out;
    return dec_fail(
        st, GCOMP_ERR_LIMIT, "lzma: decompressed output exceeds the limit");
  }
  if (gcomp_limits_check_expansion_ratio(
          st->in_total, st->produced, st->max_ratio) != GCOMP_OK) {
    return dec_fail(
        st, GCOMP_ERR_LIMIT, "lzma: expansion ratio exceeds the limit");
  }
  *run_out = r;
  return GCOMP_OK;
}

/* The header of an .lzma file: properties, dictionary size, unpacked size. */
static gcomp_status_t dec_alone_header(lzma_decoder_t * st) {
  unsigned lc, lp, pb;
  uint64_t dict, size = 0;
  unsigned i;
  gcomp_status_t s;
  if (!lzma_props_decode(st->hdr[0], &lc, &lp, &pb)) {
    return dec_fail(st, GCOMP_ERR_CORRUPT, "lzma: invalid properties byte");
  }
  dict = (uint64_t)st->hdr[1] | ((uint64_t)st->hdr[2] << 8) |
      ((uint64_t)st->hdr[3] << 16) | ((uint64_t)st->hdr[4] << 24);
  for (i = 0; i < 8; i++) {
    size |= (uint64_t)st->hdr[5 + i] << (8u * i);
  }
  s = dec_set_window(st, dict);
  if (s != GCOMP_OK) {
    return s;
  }
  if (core_set_props(&st->core, lc, lp, pb) != 0) {
    return dec_core_error(st);
  }
  core_state_reset(&st->core);
  st->known_size = size;
  st->phase = PH_BODY;
  return GCOMP_OK;
}

/* Alone and raw: everything after the header is one range-coded stream. */
static gcomp_status_t dec_body(lzma_decoder_t * st, const uint8_t ** in,
    const uint8_t * in_end, gcomp_buffer_t * output, int * blocked) {
  uint64_t remaining = UINT64_MAX;
  lzma_run_t r;
  gcomp_status_t s;
  lzma_dec_t * c = &st->core;
  if (st->known_size != UINT64_MAX) {
    remaining = st->known_size - st->produced;
  }
  s = dec_run_core(st, in, in_end, output, remaining, 1, st->expect_eos, &r);
  if (s != GCOMP_OK) {
    return s;
  }
  switch (r) {
  case RUN_EOS:
    if (st->known_size != UINT64_MAX && st->produced != st->known_size) {
      return dec_fail(st, GCOMP_ERR_CORRUPT,
          "lzma: end marker before the declared size");
    }
    st->phase = PH_DONE;
    return GCOMP_OK;
  case RUN_LIMIT:
    if (st->known_size != UINT64_MAX && st->produced == st->known_size) {
      if (c->remain_len != 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT,
            "lzma: a match runs past the declared size");
      }
      if (c->code != 0) {
        /* Not finished yet: only an end marker may follow. */
        st->expect_eos = 1;
        return GCOMP_OK;
      }
      st->phase = PH_DONE;
      return GCOMP_OK;
    }
    /* The only other bound is limits.max_output_bytes, which dec_run_core has
     * already turned into an error. */
    return dec_fail(st, GCOMP_ERR_INTERNAL, "lzma: stopped short of its bound");
  case RUN_NEED_OUTPUT:
    *blocked = BLOCKED_OUTPUT;
    return GCOMP_OK;
  default:
    *blocked = BLOCKED_INPUT;
    return GCOMP_OK;
  }
}

static gcomp_status_t dec_lzma2_chunk_header(lzma_decoder_t * st) {
  uint8_t ctl = st->control;
  lzma_dec_t * c = &st->core;
  if (ctl == 0) {
    st->phase = PH_DONE;
    return GCOMP_OK;
  }
  if (ctl >= 0xE0 || ctl == 1) {
    st->need_props = 1;
    st->need_dict_reset = 0;
    core_dict_reset(c);
  }
  else if (st->need_dict_reset) {
    return dec_fail(st, GCOMP_ERR_CORRUPT,
        "lzma2: the first chunk does not reset the dictionary");
  }
  if (ctl >= 0x80) {
    st->usize_left = ((uint32_t)(ctl & 0x1F) << 16) +
        (((uint32_t)st->chdr[0] << 8) | st->chdr[1]) + 1u;
    st->csize_left = (((uint32_t)st->chdr[2] << 8) | st->chdr[3]) + 1u;
    if (ctl >= 0xC0) {
      unsigned lc, lp, pb;
      if (!lzma_props_decode(st->chdr[4], &lc, &lp, &pb) || lc + lp > 4) {
        return dec_fail(
            st, GCOMP_ERR_CORRUPT, "lzma2: invalid chunk properties");
      }
      st->need_props = 0;
      if (core_set_props(c, lc, lp, pb) != 0) {
        return dec_core_error(st);
      }
      core_state_reset(c);
    }
    else if (st->need_props) {
      return dec_fail(st, GCOMP_ERR_CORRUPT,
          "lzma2: a chunk needs properties that no earlier chunk set");
    }
    else if (ctl >= 0xA0) {
      core_state_reset(c);
    }
    core_rc_start(c);
    st->phase = PH_LZMA;
  }
  else {
    if (ctl > 2) {
      return dec_fail(
          st, GCOMP_ERR_CORRUPT, "lzma2: invalid chunk control byte");
    }
    st->usize_left = (((uint32_t)st->chdr[0] << 8) | st->chdr[1]) + 1u;
    st->phase = PH_STORED;
  }
  return GCOMP_OK;
}

static gcomp_status_t dec_lzma2_step(lzma_decoder_t * st, const uint8_t ** in,
    const uint8_t * in_end, gcomp_buffer_t * output, int * blocked) {
  lzma_dec_t * c = &st->core;
  switch (st->phase) {
  case PH_CONTROL:
    if (*in == in_end) {
      *blocked = BLOCKED_INPUT;
      return GCOMP_OK;
    }
    st->control = *(*in)++;
    st->in_total++;
    st->chdr_have = 0;
    st->chdr_need = st->control == 0 ? 0 : (st->control < 0x80 ? 2u : (st->control >= 0xC0 ? 5u : 4u));
    st->phase = PH_CHDR;
    return GCOMP_OK;
  case PH_CHDR:
    while (st->chdr_have < st->chdr_need && *in < in_end) {
      st->chdr[st->chdr_have++] = *(*in)++;
      st->in_total++;
    }
    if (st->chdr_have < st->chdr_need) {
      *blocked = BLOCKED_INPUT;
      return GCOMP_OK;
    }
    return dec_lzma2_chunk_header(st);
  case PH_STORED: {
    size_t room = output->size - output->used;
    size_t avail = (size_t)(in_end - *in);
    size_t n = st->usize_left;
    size_t i;
    uint8_t * dst = (uint8_t *)output->data;
    if (n > room) {
      n = room;
    }
    if (n > avail) {
      n = avail;
    }
    if (st->max_out != 0 && st->produced + n > st->max_out) {
      return dec_fail(
          st, GCOMP_ERR_LIMIT, "lzma2: decompressed output exceeds the limit");
    }
    for (i = 0; i < n; i++) {
      if (dict_put(c, (*in)[i]) != 0) {
        return dec_core_error(st);
      }
      dst[output->used + i] = (*in)[i];
    }
    *in += n;
    output->used += n;
    st->in_total += n;
    st->produced += n;
    st->usize_left -= (uint32_t)n;
    if (gcomp_limits_check_expansion_ratio(
            st->in_total, st->produced, st->max_ratio) != GCOMP_OK) {
      return dec_fail(
          st, GCOMP_ERR_LIMIT, "lzma2: expansion ratio exceeds the limit");
    }
    if (st->usize_left == 0) {
      st->phase = PH_CONTROL;
    }
    else {
      *blocked = output->used >= output->size ? BLOCKED_OUTPUT : BLOCKED_INPUT;
    }
    return GCOMP_OK;
  }
  case PH_LZMA: {
    size_t avail = (size_t)(in_end - *in);
    const uint8_t * lim = *in + (avail < st->csize_left ? avail : st->csize_left);
    const uint8_t * before = *in;
    uint64_t produced_before = st->produced;
    lzma_run_t r;
    gcomp_status_t s;
    s = dec_run_core(st, in, lim, output, st->usize_left, 0, 0, &r);
    if (s != GCOMP_OK) {
      return s;
    }
    st->csize_left -= (uint32_t)(*in - before);
    st->usize_left -= (uint32_t)(st->produced - produced_before);
    switch (r) {
    case RUN_LIMIT:
      /* usize_left reached zero (or the output limit, which dec_run_core has
       * already turned into an error). */
      return GCOMP_OK;
    case RUN_NEED_INPUT:
      if (st->csize_left == 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT,
            "lzma2: a chunk's data ends inside a symbol");
      }
      *blocked = BLOCKED_INPUT;
      return GCOMP_OK;
    case RUN_NEED_OUTPUT:
      *blocked = BLOCKED_OUTPUT;
      return GCOMP_OK;
    default:
      return dec_fail(
          st, GCOMP_ERR_INTERNAL, "lzma2: a chunk stopped for no reason");
    }
  }
  default:
    return GCOMP_OK;
  }
}

static gcomp_status_t dec_drive(lzma_decoder_t * st, const uint8_t ** in,
    const uint8_t * in_end, gcomp_buffer_t * output, int * why) {
  int blocked = 0;
  while (!blocked && st->phase != PH_DONE) {
    gcomp_status_t s = GCOMP_OK;
    if (st->lzma2) {
      s = dec_lzma2_step(st, in, in_end, output, &blocked);
    }
    else if (st->phase == PH_HEADER) {
      while (st->hdr_len < 13u && *in < in_end) {
        st->hdr[st->hdr_len++] = *(*in)++;
        st->in_total++;
      }
      if (st->hdr_len < 13u) {
        blocked = BLOCKED_INPUT;
      }
      else {
        s = dec_alone_header(st);
      }
    }
    else {
      s = dec_body(st, in, in_end, output, &blocked);
    }
    if (s != GCOMP_OK) {
      return s;
    }
    if (st->lzma2 && st->phase == PH_LZMA && st->usize_left == 0) {
      lzma_dec_t * c = &st->core;
      if (st->csize_left != 0 || c->carry_len != 0 || c->remain_len != 0 ||
          c->code != 0) {
        return dec_fail(st, GCOMP_ERR_CORRUPT,
            "lzma2: a chunk's data does not end where its header says");
      }
      st->phase = PH_CONTROL;
    }
  }
  *why = blocked;
  return GCOMP_OK;
}

/* A pointer to nothing that is still a pointer: arithmetic on a null one, even
 * by zero, is undefined, and update() and finish() are both called with no
 * input. */
static const uint8_t g_no_input[1] = {0};

gcomp_status_t lzma_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  lzma_decoder_t * st;
  const uint8_t * in;
  const uint8_t * in_end;
  gcomp_status_t s;
  int why = 0;
  if (!decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  if (st->failed) {
    return st->err;
  }
  if (st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  if (input->data) {
    in = (const uint8_t *)input->data + input->used;
    in_end = in + (input->size - input->used);
  }
  else {
    in = in_end = g_no_input;
  }
  s = dec_drive(st, &in, in_end, output, &why);
  if (input->data) {
    input->used = (size_t)(in - (const uint8_t *)input->data);
  }
  return s;
}

gcomp_status_t lzma_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  lzma_decoder_t * st;
  const uint8_t * none = g_no_input;
  gcomp_status_t s;
  int why = 0;
  if (!decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  st = decoder->method_state;
  if (st->failed) {
    return st->err;
  }
  if (st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  s = dec_drive(st, &none, none, output, &why);
  if (s != GCOMP_OK) {
    return s;
  }
  if (st->phase == PH_DONE) {
    return GCOMP_OK;
  }
  if (why == BLOCKED_OUTPUT) {
    return GCOMP_ERR_LIMIT;
  }
  return dec_fail(st, GCOMP_ERR_CORRUPT, "lzma: truncated stream");
}
