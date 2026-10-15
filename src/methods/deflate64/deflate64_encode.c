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
 * @file deflate64_encode.c
 *
 * The deflate64 encoder: hash-chain LZ77 over a 64 KiB window, with matches up
 * to 65538 bytes, coded as stored, fixed or dynamic Huffman blocks, whichever
 * is smallest for the block. A block is the input that arrived since the last
 * one, up to 64 KiB, so the encoder holds that and the window behind it.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "deflate64_internal.h"
#include <ghoti.io/compress/deflate64.h>
#include <string.h>

#define E_BLOCK D64_WINDOW /* input per block */
#define E_BUF (D64_WINDOW + E_BLOCK)
#define E_HASH_BITS 15u
#define E_OUT_CAP (2u * E_BLOCK + 4096u)

typedef struct {
  unsigned chain, nice;
  int lazy;
} e_level_t;

static const e_level_t g_levels[10] = {{0, 0, 0}, {4, 8, 0}, {8, 16, 0},
    {16, 32, 0}, {32, 32, 1}, {64, 64, 1}, {128, 128, 1}, {256, 258, 1},
    {1024, 1024, 1}, {4096, D64_MAX_MATCH, 1}};

typedef struct {
  gcomp_encoder_t * pub;
  const gcomp_allocator_t * alloc;
  int level;
  e_level_t lv;

  uint8_t * buf; /* history, then the block being collected */
  size_t start, fill, ins;
  int32_t * head; /* by hash: a position + 1, or 0 */
  int32_t * prev; /* by position: the earlier one with the same hash, + 1 */
  uint32_t * tlen; /* per token: the length, 0 for a literal */
  uint32_t * tval; /* the literal byte, or the distance */
  size_t ntok;

  uint8_t * ob; /* output staged for the caller */
  size_t ob_len, ob_pos;
  uint64_t acc;
  unsigned nacc;

  int finished, dirty;
} d64_enc_t;

/* ---- bits out -------------------------------------------------------------- */

static void put(d64_enc_t * e, uint32_t v, unsigned n) {
  e->acc |= (uint64_t)v << e->nacc;
  e->nacc += n;
  while (e->nacc >= 8) {
    e->ob[e->ob_len++] = (uint8_t)e->acc;
    e->acc >>= 8;
    e->nacc -= 8;
  }
}

static void align(d64_enc_t * e) {
  if (e->nacc != 0) {
    put(e, 0, 8u - e->nacc);
  }
}

/* ---- Huffman codes ----------------------------------------------------------- */

/* Code lengths of at most `limit` bits for the frequencies, by Huffman's
 * method, halving the frequencies and trying again while the code is too deep.
 * At least two symbols get a code, so that the code is complete. */
static void build_lens(const uint32_t * freq, unsigned n, unsigned limit,
    uint8_t * lens) {
  uint32_t f[288];
  unsigned i, m;
  for (i = 0; i < n; i++) {
    f[i] = freq[i];
  }
  for (m = 0, i = 0; i < n; i++) {
    m += f[i] != 0;
  }
  for (i = 0; m < 2 && i < n; i++) {
    if (f[i] == 0) {
      f[i] = 1;
      m++;
    }
  }
  for (;;) {
    unsigned order[288], cnt = 0, a, b, k, li = 0, ij = 0, depth_ok = 1;
    uint32_t lw[288], iw[288];
    unsigned lp[288], ip[288], ls[288], id[288];
    for (i = 0; i < n; i++) {
      if (f[i] != 0) {
        order[cnt++] = i;
      }
    }
    /* insertion sort by frequency: n is at most 288 */
    for (a = 1; a < cnt; a++) {
      const unsigned s = order[a];
      b = a;
      while (b > 0 && f[order[b - 1]] > f[s]) {
        order[b] = order[b - 1];
        b--;
      }
      order[b] = s;
    }
    for (i = 0; i < cnt; i++) {
      lw[i] = f[order[i]];
      ls[i] = order[i];
    }
    for (k = 0; k + 1 < cnt; k++) {
      unsigned p;
      for (p = 0; p < 2; p++) {
        if (li < cnt && (ij >= k || lw[li] <= iw[ij])) {
          lp[li] = k;
          iw[k] = (p == 0 ? 0u : iw[k]) + lw[li];
          li++;
        }
        else {
          ip[ij] = k;
          iw[k] = (p == 0 ? 0u : iw[k]) + iw[ij];
          ij++;
        }
      }
    }
    id[cnt - 2] = 0;
    for (k = cnt - 2; k-- > 0;) {
      id[k] = id[ip[k]] + 1;
    }
    memset(lens, 0, n);
    for (i = 0; i < cnt; i++) {
      const unsigned d = id[lp[i]] + 1;
      lens[ls[i]] = (uint8_t)d;
      if (d > limit) {
        depth_ok = 0;
      }
    }
    if (depth_ok) {
      return;
    }
    for (i = 0; i < n; i++) {
      if (f[i] != 0) {
        f[i] = (f[i] + 1) >> 1;
      }
    }
  }
}

/* The codes for the lengths, bit-reversed as the stream sends them. */
static void build_codes(const uint8_t * lens, unsigned n, uint16_t * codes) {
  unsigned count[16] = {0}, next[16], i, code = 0, len;
  for (i = 0; i < n; i++) {
    count[lens[i]]++;
  }
  count[0] = 0;
  for (len = 1; len < 16; len++) {
    code = (code + count[len - 1]) << 1;
    next[len] = code;
  }
  for (i = 0; i < n; i++) {
    len = lens[i];
    if (len != 0) {
      unsigned c = next[len]++, r = 0, k;
      for (k = 0; k < len; k++) {
        r = (r << 1) | ((c >> k) & 1u);
      }
      codes[i] = (uint16_t)r;
    }
  }
}

static unsigned len_code(unsigned len) {
  unsigned i;
  if (len >= 258) {
    return 28;
  }
  for (i = 27; g_d64_len_base[i] > len; i--) {
  }
  return i;
}

static unsigned dist_code(uint32_t dist) {
  const uint32_t d = dist - 1u;
  unsigned n;
  if (d < 4) {
    return d;
  }
  n = 31u - (unsigned)__builtin_clz(d);
  return 2u * n + ((d >> (n - 1)) & 1u);
}

/* ---- the match finder ---------------------------------------------------------- */

static unsigned hash3(const uint8_t * p) {
  const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  return (v * 2654435761u) >> (32u - E_HASH_BITS);
}

/* Enter every position below `upto` that has three bytes after it. */
static void insert_to(d64_enc_t * e, size_t upto) {
  while (e->ins < upto && e->ins + 3u <= e->fill) {
    const unsigned h = hash3(e->buf + e->ins);
    e->prev[e->ins] = e->head[h];
    e->head[h] = (int32_t)e->ins + 1;
    e->ins++;
  }
}

static unsigned match_len(
    const uint8_t * a, const uint8_t * b, unsigned limit) {
  unsigned n = 0;
  while (n < limit && a[n] == b[n]) {
    n++;
  }
  return n;
}

/* The longest match at pos, from the chain of positions with its hash. */
static unsigned find(d64_enc_t * e, size_t pos, size_t end, uint32_t * dist) {
  const unsigned maxlen =
      (end - pos) > D64_MAX_MATCH ? D64_MAX_MATCH : (unsigned)(end - pos);
  unsigned best = 2, chain = e->lv.chain;
  int32_t cand;
  *dist = 0;
  if (maxlen < D64_MIN_MATCH) {
    return 0;
  }
  cand = e->head[hash3(e->buf + pos)];
  while (cand != 0 && chain-- != 0) {
    const size_t c = (size_t)cand - 1u;
    const size_t d = pos - c;
    if (d > D64_WINDOW) {
      break;
    }
    if (e->buf[c + best] == e->buf[pos + best] || best >= maxlen) {
      const unsigned l = match_len(e->buf + c, e->buf + pos, maxlen);
      if (l > best) {
        best = l;
        *dist = (uint32_t)d;
        if (l >= e->lv.nice || l == maxlen) {
          break;
        }
      }
    }
    cand = e->prev[c];
  }
  return best >= D64_MIN_MATCH ? best : 0;
}

static void tokenize(d64_enc_t * e, size_t end) {
  size_t pos = e->start;
  unsigned len = 0, nlen;
  uint32_t dist = 0, ndist;
  int have = 0;
  e->ntok = 0;
  while (pos < end) {
    if (!have) {
      insert_to(e, pos);
      len = find(e, pos, end, &dist);
      insert_to(e, pos + 1);
    }
    have = 0;
    if (len == D64_MIN_MATCH && dist > 4096u) {
      len = 0;
    }
    if (len != 0) {
      if (e->lv.lazy && len < e->lv.nice && pos + 1 < end) {
        insert_to(e, pos + 1);
        nlen = find(e, pos + 1, end, &ndist);
        insert_to(e, pos + 2);
        if (nlen > len) {
          e->tlen[e->ntok] = 0;
          e->tval[e->ntok++] = e->buf[pos];
          pos++;
          len = nlen;
          dist = ndist;
          have = 1;
          continue;
        }
      }
      e->tlen[e->ntok] = len;
      e->tval[e->ntok++] = dist;
      pos += len;
      insert_to(e, pos);
    }
    else {
      e->tlen[e->ntok] = 0;
      e->tval[e->ntok++] = e->buf[pos];
      pos++;
    }
  }
}

/* ---- blocks -------------------------------------------------------------------- */

static void stored_blocks(d64_enc_t * e, const uint8_t * p, size_t n, int final) {
  do {
    const size_t chunk = n > 65535u ? 65535u : n;
    const int last = final && chunk == n;
    put(e, last ? 1u : 0u, 1);
    put(e, 0, 2);
    align(e);
    put(e, (uint32_t)chunk, 16);
    put(e, (uint32_t)chunk ^ 0xFFFFu, 16);
    memcpy(e->ob + e->ob_len, p, chunk);
    e->ob_len += chunk;
    p += chunk;
    n -= chunk;
  } while (n != 0);
}

static void send_tokens(d64_enc_t * e, const uint16_t * lc, const uint8_t * ll,
    const uint16_t * dc, const uint8_t * dl) {
  size_t i;
  for (i = 0; i < e->ntok; i++) {
    if (e->tlen[i] == 0) {
      put(e, lc[e->tval[i]], ll[e->tval[i]]);
    }
    else {
      const unsigned l = e->tlen[i];
      const unsigned lcx = len_code(l);
      const unsigned dcx = dist_code(e->tval[i]);
      put(e, lc[257 + lcx], ll[257 + lcx]);
      if (g_d64_len_extra[lcx] != 0) {
        put(e, l - g_d64_len_base[lcx], g_d64_len_extra[lcx]);
      }
      put(e, dc[dcx], dl[dcx]);
      if (g_d64_dist_extra[dcx] != 0) {
        put(e, e->tval[i] - g_d64_dist_base[dcx], g_d64_dist_extra[dcx]);
      }
    }
  }
  put(e, lc[256], ll[256]);
}

/* Code [start, end) as one block (or, when it is more than a stored block can
 * hold, as stored blocks), whichever of stored, fixed and dynamic is smallest. */
static void emit_block(d64_enc_t * e, size_t end, int final) {
  const size_t n = end - e->start;
  uint32_t lfreq[286] = {0}, dfreq[32] = {0};
  uint8_t llens[286], dlens[32], cl_lens[19], fl[288], fd[32];
  uint16_t lcodes[288], dcodes[32], cl_codes[19];
  uint8_t rl_sym[320], rl_extra[320];
  uint32_t cl_freq[19] = {0};
  unsigned hlit, hdist, hclen, nrl = 0, i;
  uint64_t dyn_bits, fixed_bits, stored_bits;
  uint64_t extra_bits = 0;
  size_t t;

  if (e->level == 0) {
    stored_blocks(e, e->buf + e->start, n, final);
    return;
  }
  tokenize(e, end);
  for (t = 0; t < e->ntok; t++) {
    if (e->tlen[t] == 0) {
      lfreq[e->tval[t]]++;
    }
    else {
      const unsigned lcx = len_code(e->tlen[t]);
      const unsigned dcx = dist_code(e->tval[t]);
      lfreq[257 + lcx]++;
      dfreq[dcx]++;
      extra_bits += g_d64_len_extra[lcx] + g_d64_dist_extra[dcx];
    }
  }
  lfreq[256] = 1;
  build_lens(lfreq, 286, 15, llens);
  build_lens(dfreq, 32, 15, dlens);
  for (hlit = 286; hlit > 257 && llens[hlit - 1] == 0; hlit--) {
  }
  for (hdist = 32; hdist > 1 && dlens[hdist - 1] == 0; hdist--) {
  }
  /* run-length code the lengths */
  {
    uint8_t all[286 + 32];
    unsigned total = hlit + hdist;
    memcpy(all, llens, hlit);
    memcpy(all + hlit, dlens, hdist);
    for (i = 0; i < total;) {
      unsigned run = 1, v = all[i];
      while (i + run < total && all[i + run] == v) {
        run++;
      }
      i += run;
      if (v == 0) {
        while (run >= 11) {
          const unsigned r = run > 138 ? 138 : run;
          rl_sym[nrl] = 18;
          rl_extra[nrl++] = (uint8_t)(r - 11);
          run -= r;
        }
        if (run >= 3) {
          rl_sym[nrl] = 17;
          rl_extra[nrl++] = (uint8_t)(run - 3);
          run = 0;
        }
        while (run--) {
          rl_sym[nrl] = 0;
          rl_extra[nrl++] = 0;
        }
      }
      else {
        rl_sym[nrl] = (uint8_t)v;
        rl_extra[nrl++] = 0;
        run--;
        while (run >= 3) {
          const unsigned r = run > 6 ? 6 : run;
          rl_sym[nrl] = 16;
          rl_extra[nrl++] = (uint8_t)(r - 3);
          run -= r;
        }
        while (run--) {
          rl_sym[nrl] = (uint8_t)v;
          rl_extra[nrl++] = 0;
        }
      }
    }
  }
  for (i = 0; i < nrl; i++) {
    cl_freq[rl_sym[i]]++;
  }
  build_lens(cl_freq, 19, 7, cl_lens);
  for (hclen = 19; hclen > 4 && cl_lens[g_d64_cl_order[hclen - 1]] == 0; hclen--) {
  }

  /* what each way of coding it costs, in bits */
  dyn_bits = 3 + 14 + 3u * hclen;
  for (i = 0; i < nrl; i++) {
    dyn_bits += cl_lens[rl_sym[i]] + (rl_sym[i] == 16 ? 2u : rl_sym[i] == 17 ? 3u : rl_sym[i] == 18 ? 7u : 0u);
  }
  fixed_bits = 3;
  for (i = 0; i < 286; i++) {
    dyn_bits += (uint64_t)lfreq[i] * llens[i];
    fixed_bits += (uint64_t)lfreq[i] * (i < 144 ? 8u : i < 256 ? 9u : i < 280 ? 7u : 8u);
  }
  for (i = 0; i < 32; i++) {
    dyn_bits += (uint64_t)dfreq[i] * dlens[i];
    fixed_bits += (uint64_t)dfreq[i] * 5u;
  }
  dyn_bits += extra_bits;
  fixed_bits += extra_bits;
  stored_bits =
      8u * ((uint64_t)n + 5u * (n == 0 ? 1u : (n + 65534u) / 65535u)) + 7u;

  if (stored_bits <= dyn_bits && stored_bits <= fixed_bits) {
    stored_blocks(e, e->buf + e->start, n, final);
    return;
  }
  put(e, final ? 1u : 0u, 1);
  if (fixed_bits <= dyn_bits) {
    for (i = 0; i < 288; i++) {
      fl[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    }
    for (i = 0; i < 32; i++) {
      fd[i] = 5;
    }
    build_codes(fl, 288, lcodes);
    build_codes(fd, 32, dcodes);
    put(e, 1, 2);
    send_tokens(e, lcodes, fl, dcodes, fd);
    return;
  }
  build_codes(llens, 286, lcodes);
  build_codes(dlens, 32, dcodes);
  build_codes(cl_lens, 19, cl_codes);
  put(e, 2, 2);
  put(e, hlit - 257u, 5);
  put(e, hdist - 1u, 5);
  put(e, hclen - 4u, 4);
  for (i = 0; i < hclen; i++) {
    put(e, cl_lens[g_d64_cl_order[i]], 3);
  }
  for (i = 0; i < nrl; i++) {
    const unsigned s = rl_sym[i];
    put(e, cl_codes[s], cl_lens[s]);
    if (s == 16) {
      put(e, rl_extra[i], 2);
    }
    else if (s == 17) {
      put(e, rl_extra[i], 3);
    }
    else if (s == 18) {
      put(e, rl_extra[i], 7);
    }
  }
  send_tokens(e, lcodes, llens, dcodes, dlens);
}

/* ---- the stream ------------------------------------------------------------------- */

static void clear_history(d64_enc_t * e) {
  memset(e->head, 0, ((size_t)1 << E_HASH_BITS) * sizeof(*e->head));
  e->start = e->fill = e->ins = 0;
}

static void state_reset(d64_enc_t * e) {
  clear_history(e);
  e->ntok = 0;
  e->ob_len = e->ob_pos = 0;
  e->acc = 0;
  e->nacc = 0;
  e->finished = 0;
  e->dirty = 0;
}

static void drain(d64_enc_t * e, gcomp_buffer_t * out) {
  size_t room = out->size - out->used;
  size_t n = e->ob_len - e->ob_pos;
  if (n > room) {
    n = room;
  }
  if (n != 0) {
    memcpy((uint8_t *)out->data + out->used, e->ob + e->ob_pos, n);
    out->used += n;
    e->ob_pos += n;
  }
  if (e->ob_pos == e->ob_len) {
    e->ob_pos = e->ob_len = 0;
  }
}

/* Code what has been collected as one block, then make room behind it. */
static void close_block(d64_enc_t * e, int final) {
  emit_block(e, e->fill, final);
  e->dirty = 1;
  e->start = e->fill;
  if (e->start > D64_WINDOW && !final) {
    const size_t shift = e->start - D64_WINDOW;
    size_t i;
    memmove(e->buf, e->buf + shift, e->fill - shift);
    for (i = 0; i < ((size_t)1 << E_HASH_BITS); i++) {
      e->head[i] = e->head[i] > (int32_t)shift ? e->head[i] - (int32_t)shift : 0;
    }
    for (i = shift; i < e->fill; i++) {
      e->prev[i - shift] = e->prev[i] > (int32_t)shift ? e->prev[i] - (int32_t)shift : 0;
    }
    e->fill -= shift;
    e->start -= shift;
    e->ins = e->ins > shift ? e->ins - shift : 0;
  }
}

gcomp_status_t d64_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  const gcomp_allocator_t * alloc;
  d64_enc_t * e;
  int64_t level = 6;
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }
  if (options) {
    (void)gcomp_options_get_int64(options, "deflate64.level", &level);
  }
  if (level < 0 || level > 9) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "deflate64.level must be 0 to 9");
  }
  alloc = gcomp_registry_get_allocator(registry);
  e = gcomp_calloc(alloc, 1, sizeof(*e));
  if (!e) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "deflate64: out of memory");
  }
  e->pub = encoder;
  e->alloc = alloc;
  e->level = (int)level;
  e->lv = g_levels[level];
  e->buf = gcomp_malloc(alloc, E_BUF + 16u);
  e->head = gcomp_calloc(alloc, (size_t)1 << E_HASH_BITS, sizeof(int32_t));
  e->prev = gcomp_calloc(alloc, E_BUF + 16u, sizeof(int32_t));
  e->tlen = gcomp_malloc(alloc, (E_BLOCK + 16u) * sizeof(uint32_t));
  e->tval = gcomp_malloc(alloc, (E_BLOCK + 16u) * sizeof(uint32_t));
  e->ob = gcomp_malloc(alloc, E_OUT_CAP);
  encoder->method_state = e;
  if (!e->buf || !e->head || !e->prev || !e->tlen || !e->tval || !e->ob) {
    d64_encoder_destroy(encoder);
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "deflate64: out of memory");
  }
  state_reset(e);
  return GCOMP_OK;
}

void d64_encoder_destroy(gcomp_encoder_t * encoder) {
  d64_enc_t * e;
  if (!encoder || !encoder->method_state) {
    return;
  }
  e = encoder->method_state;
  gcomp_free(e->alloc, e->buf);
  gcomp_free(e->alloc, e->head);
  gcomp_free(e->alloc, e->prev);
  gcomp_free(e->alloc, e->tlen);
  gcomp_free(e->alloc, e->tval);
  gcomp_free(e->alloc, e->ob);
  gcomp_free(e->alloc, e);
  encoder->method_state = NULL;
}

gcomp_status_t d64_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  state_reset(encoder->method_state);
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';
  return GCOMP_OK;
}

gcomp_status_t d64_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  d64_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "deflate64: encoder update after finish");
  }
  for (;;) {
    size_t avail, take;
    drain(e, output);
    if (e->ob_len != 0) {
      return GCOMP_OK;
    }
    avail = input->size - input->used;
    if (avail == 0) {
      return GCOMP_OK;
    }
    take = E_BLOCK - (e->fill - e->start);
    if (take > avail) {
      take = avail;
    }
    memcpy(e->buf + e->fill, (const uint8_t *)input->data + input->used, take);
    e->fill += take;
    input->used += take;
    if (e->fill - e->start == E_BLOCK) {
      close_block(e, 0);
    }
  }
}

gcomp_status_t d64_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  d64_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  for (;;) {
    drain(e, output);
    if (e->ob_len != 0) {
      return GCOMP_ERR_LIMIT;
    }
    if (e->finished) {
      return GCOMP_OK;
    }
    close_block(e, 1);
    align(e);
    e->finished = 1;
  }
}

/* A sync flush ends the block in progress and sends an empty stored block, so
 * that everything so far is on a byte boundary and a reader can decode it; a
 * full flush also forgets the window, so that what follows needs nothing
 * before it. */
gcomp_status_t d64_encoder_flush(gcomp_encoder_t * encoder,
    gcomp_buffer_t * output, gcomp_flush_t mode) {
  d64_enc_t * e = encoder->method_state;
  if (!e) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (e->finished) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "deflate64: encoder cannot flush after finish");
  }
  drain(e, output);
  if (e->ob_len != 0) {
    return GCOMP_ERR_LIMIT;
  }
  if (e->fill > e->start) {
    close_block(e, 0);
  }
  if (e->dirty) {
    put(e, 0, 3);
    align(e);
    put(e, 0, 16);
    put(e, 0xFFFFu, 16);
    e->dirty = 0;
  }
  if (mode == GCOMP_FLUSH_FULL) {
    clear_history(e);
  }
  drain(e, output);
  return e->ob_len != 0 ? GCOMP_ERR_LIMIT : GCOMP_OK;
}
