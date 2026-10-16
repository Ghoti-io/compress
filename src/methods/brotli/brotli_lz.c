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
 * @file brotli_lz.c
 *
 * Level 1 of the brotli encoder: one LZ77 meta-block per chunk.
 *
 * One block type per category, no distance postfix, no direct distances, and
 * a single prefix code each for literals, insert-and-copy, and distances.
 * Matches stay inside the chunk and inside the declared window. The last
 * command may be insert-only: RFC 7932 ignores that command's copy length
 * and emits no distance once the meta-block length is satisfied. An empty
 * metadata block follows, which aligns the stream so the next block and the
 * final 0x03 terminator can stay byte-oriented. A block that is not smaller
 * than the stored form is left for the caller to store.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "brotli_bw.h"
#include "brotli_internal.h"

#include <math.h>
#include <string.h>

#define BROTLI_HASH_BITS BROTLI_MF_BITS
#define BROTLI_HASH_SIZE (1u << BROTLI_HASH_BITS)
#define BROTLI_MIN_MATCH 4

/* Why no distance here can outrun the alphabet.
 *
 * With NPOSTFIX and NDIRECT both zero the distance alphabet is 16 short codes
 * and 48 long ones, two per extra-bit width, so the widths run 1..24 and the
 * largest distance it can express is ((3 << 24) - 3) + (1 << 24) - 1, which is
 * 67108860. long_dist_sym used to stop at width 15, which put the ceiling at
 * 131068 instead - and because a command the emitter cannot spell fails the
 * whole meta-block rather than that one command, a single far match turned
 * 256KiB of compressible input into a stored block: at lgwin 20 a 250000-byte
 * file went out at 250015 bytes where the same file at lgwin 16, whose window
 * is too small to find the match at all, went out at 17061.
 *
 * Two things keep it unreachable now, and either alone would do it. Matches
 * stay inside the chunk, so a distance is at most BROTLI_BLOCK, 262144. And
 * the matcher refuses a distance past the window, which at lgwin 24 - the
 * largest RFC 7932 defines - is 16777200. Both are far under 67108860, which
 * is why there is no clamp here: one was written, and a test that removed it
 * could not tell the difference, because nothing a caller can ask for comes
 * within three orders of magnitude of the bound. If matches are ever let
 * cross chunks, the window leg still holds. */

/* What a level asks of the match finder. */
typedef struct brotli_params_s {
  int chain;    /* candidates examined per position */
  size_t nice;  /* a match this long is taken without looking further */
  int lazy;     /* defer a match when the next position starts a better one */
  uint32_t rb[4]; /* the last four distances, as the decoder will have them */
} brotli_params_t;

typedef struct brotli_canon_s {
  uint16_t code;
  uint8_t len;
} brotli_canon_t;

typedef struct brotli_cmd_s {
  uint32_t insert;
  uint32_t copy;
  uint32_t dist;
  uint32_t at;
  int tail;
} brotli_cmd_t;

typedef struct brotli_tok_s {
  uint8_t sym;
  uint8_t nextra;
  uint16_t extra;
} brotli_tok_t;

typedef struct brotli_node_s {
  uint32_t freq;
  int sym;
  int left;
  int right;
} brotli_node_t;

static const uint8_t k_ins_extra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,
    4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint32_t k_ins_base[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26,
    34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t k_copy_extra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3,
    3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
static const uint32_t k_copy_base[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18,
    22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const int k_cl_order[18] = {
    1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static int length_code(uint32_t len, const uint32_t * base,
    const uint8_t * extra, int * code, uint32_t * ebits, int * nbits) {
  int i;
  for (i = 23; i >= 0; i--) {
    uint32_t span;
    if (len < base[i]) {
      continue;
    }
    if (extra[i] > 24) {
      return -1;
    }
    span = 1u << extra[i];
    if (len - base[i] < span) {
      *code = i;
      *ebits = len - base[i];
      *nbits = (int)extra[i];
      return 0;
    }
  }
  return -1;
}

static void split_ic(int sym, int * ins, int * copy, int * implicit) {
  static const int bins[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
  static const int bco[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
  int cell = sym >> 6;
  int within = sym & 63;
  *ins = bins[cell] + (within >> 3);
  *copy = bco[cell] + (within & 7);
  *implicit = sym < 128;
}

static void fill_ic(int imp[24][24], int exp[24][24]) {
  int s;
  int i;
  int c;
  for (i = 0; i < 24; i++) {
    for (c = 0; c < 24; c++) {
      imp[i][c] = -1;
      exp[i][c] = -1;
    }
  }
  for (s = 0; s < 704; s++) {
    int ins;
    int copy;
    int implicit;
    split_ic(s, &ins, &copy, &implicit);
    if (ins < 0 || ins >= 24 || copy < 0 || copy >= 24) {
      continue;
    }
    if (implicit) {
      if (imp[ins][copy] < 0) {
        imp[ins][copy] = s;
      }
    }
    else if (exp[ins][copy] < 0) {
      exp[ins][copy] = s;
    }
  }
}

static void push_dist(uint32_t rb[4], uint32_t distance) {
  rb[3] = rb[2];
  rb[2] = rb[1];
  rb[1] = rb[0];
  rb[0] = distance;
}

static int short_dist_sym(uint32_t dist, const uint32_t rb[4], int * sym) {
  int64_t last = rb[0];
  int64_t prev = rb[1];
  int64_t want = (int64_t)dist;
  int s;
  for (s = 0; s < 16; s++) {
    int64_t v;
    switch (s) {
    case 0:
      v = last;
      break;
    case 1:
      v = prev;
      break;
    case 2:
      v = rb[2];
      break;
    case 3:
      v = rb[3];
      break;
    case 4:
      v = last - 1;
      break;
    case 5:
      v = last + 1;
      break;
    case 6:
      v = last - 2;
      break;
    case 7:
      v = last + 2;
      break;
    case 8:
      v = last - 3;
      break;
    case 9:
      v = last + 3;
      break;
    case 10:
      v = prev - 1;
      break;
    case 11:
      v = prev + 1;
      break;
    case 12:
      v = prev - 2;
      break;
    case 13:
      v = prev + 2;
      break;
    case 14:
      v = prev - 3;
      break;
    default:
      v = prev + 3;
      break;
    }
    if (v > 0 && v == want) {
      *sym = s;
      return 0;
    }
  }
  return -1;
}

static int long_dist_sym(uint32_t dist, int * sym, uint32_t * extra, int * nextra) {
  int nb;
  if (dist == 0) {
    return -1;
  }
  for (nb = 1; nb <= 24; nb++) {
    uint32_t count = 1u << nb;
    uint32_t start0 = (2u << nb) - 3u;
    uint32_t start1 = (3u << nb) - 3u;
    if (dist < start0) {
      break;
    }
    if (dist < start0 + count) {
      *nextra = nb;
      *extra = dist - start0;
      *sym = 16 + 2 * (nb - 1);
      return 0;
    }
    if (dist < start1 + count) {
      *nextra = nb;
      *extra = dist - start1;
      *sym = 16 + 2 * (nb - 1) + 1;
      return 0;
    }
  }
  return -1;
}

static int ceil_log2_int(int n) {
  int l = 0;
  int v = 1;
  while (v < n) {
    v <<= 1;
    l++;
  }
  return l;
}

static int kraft_complete(const uint8_t * lens, int n) {
  int space = 32768;
  int nnz = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (lens[i] == 0) {
      continue;
    }
    if (lens[i] > 15) {
      return 0;
    }
    nnz++;
    space -= 32768 >> lens[i];
  }
  return nnz >= 2 && space == 0;
}

static void set_depth(const brotli_node_t * nodes, int idx, int depth,
    uint8_t * lens, int * maxd) {
  if (nodes[idx].left < 0) {
    lens[nodes[idx].sym] = (uint8_t)depth;
    if (depth > *maxd) {
      *maxd = depth;
    }
    return;
  }
  set_depth(nodes, nodes[idx].left, depth + 1, lens, maxd);
  set_depth(nodes, nodes[idx].right, depth + 1, lens, maxd);
}

static void assign_balanced(const uint32_t * freq, int alphabet, uint8_t * lens) {
  int syms[704];
  int nnz = 0;
  int i;
  int l;
  int deficit;
  for (i = 0; i < alphabet; i++) {
    if (freq[i] != 0) {
      syms[nnz++] = i;
    }
  }
  if (nnz < 2) {
    return;
  }
  for (i = 1; i < nnz; i++) {
    int s = syms[i];
    int j = i;
    while (j > 0) {
      int p = syms[j - 1];
      int less = freq[s] > freq[p] || (freq[s] == freq[p] && s < p);
      if (!less) {
        break;
      }
      syms[j] = p;
      j--;
    }
    syms[j] = s;
  }
  l = ceil_log2_int(nnz);
  deficit = (1 << l) - nnz;
  for (i = 0; i < nnz; i++) {
    lens[syms[i]] = (uint8_t)(i < deficit ? l - 1 : l);
  }
}

static int assign_huffman(const gcomp_allocator_t * alloc, const uint32_t * freq,
    int alphabet, uint8_t * lens) {
  int syms[704];
  int nnz = 0;
  int i;
  int active_n;
  int next;
  int root;
  int maxd = 0;
  int * active;
  brotli_node_t * nodes;
  for (i = 0; i < alphabet; i++) {
    lens[i] = 0;
    if (freq[i] != 0) {
      syms[nnz++] = i;
    }
  }
  if (nnz < 2) {
    if (nnz == 1) {
      lens[syms[0]] = 1;
    }
    return 0;
  }
  nodes = gcomp_malloc(alloc, (size_t)(nnz * 2) * sizeof(*nodes));
  active = gcomp_malloc(alloc, (size_t)nnz * sizeof(*active));
  if (!nodes || !active) {
    gcomp_free(alloc, nodes);
    gcomp_free(alloc, active);
    return -1;
  }
  for (i = 0; i < nnz; i++) {
    nodes[i].freq = freq[syms[i]];
    nodes[i].sym = syms[i];
    nodes[i].left = -1;
    nodes[i].right = -1;
    active[i] = i;
  }
  active_n = nnz;
  next = nnz;
  while (active_n > 1) {
    int a = 0;
    int b = 1;
    int ia;
    int ib;
    int slot;
    if (nodes[active[b]].freq < nodes[active[a]].freq ||
        (nodes[active[b]].freq == nodes[active[a]].freq && active[b] < active[a])) {
      int tmp = a;
      a = b;
      b = tmp;
    }
    for (i = 2; i < active_n; i++) {
      int c = active[i];
      if (nodes[c].freq < nodes[active[a]].freq ||
          (nodes[c].freq == nodes[active[a]].freq && c < active[a])) {
        b = a;
        a = i;
      }
      else if (nodes[c].freq < nodes[active[b]].freq ||
          (nodes[c].freq == nodes[active[b]].freq && c < active[b])) {
        b = i;
      }
    }
    ia = active[a];
    ib = active[b];
    nodes[next].freq = nodes[ia].freq + nodes[ib].freq;
    nodes[next].sym = -1;
    nodes[next].left = ia;
    nodes[next].right = ib;
    slot = 0;
    for (i = 0; i < active_n; i++) {
      if (i != a && i != b) {
        active[slot++] = active[i];
      }
    }
    active[slot++] = next;
    active_n = slot;
    next++;
  }
  root = active[0];
  set_depth(nodes, root, 0, lens, &maxd);
  gcomp_free(alloc, nodes);
  gcomp_free(alloc, active);
  if (maxd > 15 || !kraft_complete(lens, alphabet)) {
    memset(lens, 0, (size_t)alphabet);
    assign_balanced(freq, alphabet, lens);
  }
  return 0;
}

static int alpha_bits(int n) {
  int bits = 0;
  int v = n - 1;
  while (v > 0) {
    v >>= 1;
    bits++;
  }
  return bits;
}

/* The canonical code is numbered MSB first. brotli_bw_put shifts the low bit out
 * first, so the stored code is reversed and one put writes the symbol. */
static uint16_t reverse_code(uint32_t code, int len) {
  uint16_t rev = 0;
  int i;
  for (i = 0; i < len; i++) {
    rev = (uint16_t)((rev << 1) | ((code >> i) & 1u));
  }
  return rev;
}

static void build_canon(const uint8_t * lens, int alphabet, brotli_canon_t * out) {
  int count[16];
  uint32_t next[16];
  uint32_t code = 0;
  int maxb = 0;
  int i;
  memset(count, 0, sizeof(count));
  for (i = 0; i < alphabet; i++) {
    out[i].code = 0;
    out[i].len = lens[i];
    if (lens[i] != 0) {
      count[lens[i]]++;
      if (lens[i] > maxb) {
        maxb = lens[i];
      }
    }
  }
  for (i = 1; i <= maxb; i++) {
    code = (code + (uint32_t)count[i - 1]) << 1;
    next[i] = code;
  }
  for (i = 0; i < alphabet; i++) {
    int len = lens[i];
    if (len != 0) {
      out[i].code = reverse_code(next[len]++, len);
    }
  }
}

static int put_canon(brotli_bw_t * b, const brotli_canon_t * c) {
  if (c->len == 0) {
    return 0;
  }
  return brotli_bw_put(b, c->code, (int)c->len);
}

/* The six code lengths of the code-length code, in the fixed code of RFC 7932
 * section 3.5, each already reversed so one brotli_bw_put writes it.
 *
 * `case 5` is never taken, for a reason worth writing down rather than
 * leaving as a cold line. These values come from assign_balanced() over the
 * 18 code-length symbols, which assigns ceil_log2(nnz) bits, so a 5 needs 17
 * or 18 of them live. plan_lengths() never emits symbol 16, so the ceiling is
 * 17: all fifteen lengths 1..15, plus symbol 0 for a single zero and symbol
 * 17 for a run of them. That needs a prefix code 15 deep that also uses
 * length 1 - one symbol holding more than half the weight, with fifteen
 * halvings below it, which is the Fibonacci chain exactly. Measured over
 * every input in the brotli suites and a search besides, the most that is
 * reached is 16 live symbols, and the one always missing is length 1: a
 * literal frequent enough to earn a one-bit code recurs often enough that
 * the matcher turns those occurrences into copies, which takes it back below
 * half. The arm is correct and stays; what would make it live is a histogram
 * the matcher does not flatten. */
static int write_cl_static(brotli_bw_t * b, int v) {
  switch (v) {
  case 0:
    return brotli_bw_put(b, 0, 2);
  case 3:
    return brotli_bw_put(b, 2, 2);
  case 4:
    return brotli_bw_put(b, 1, 2);
  case 2:
    return brotli_bw_put(b, 3, 3);
  case 1:
    return brotli_bw_put(b, 7, 4);
  case 5:
    return brotli_bw_put(b, 15, 4);
  default:
    return -1;
  }
}

static int emit_zero_run(int * repeat, int z, brotli_tok_t * toks, int * ntok,
    int cap) {
  while (z > 0) {
    int best = 0;
    int best_val = 0;
    int val;
    int found = 0;
    if (z >= 3) {
      for (val = 0; val <= 7; val++) {
        int old = *repeat;
        int rep = old;
        int emit;
        if (rep > 0) {
          rep = (rep - 2) << 3;
        }
        rep += val + 3;
        emit = rep - old;
        if (emit > best && emit <= z) {
          best = emit;
          best_val = val;
          found = 1;
        }
      }
    }
    if (*ntok >= cap) {
      return -1;
    }
    if (found && best >= 3) {
      int old = *repeat;
      int rep = old;
      if (rep > 0) {
        rep = (rep - 2) << 3;
      }
      rep += best_val + 3;
      *repeat = rep;
      toks[*ntok].sym = 17;
      toks[*ntok].nextra = 3;
      toks[*ntok].extra = (uint16_t)best_val;
      (*ntok)++;
      z -= best;
    }
    else {
      toks[*ntok].sym = 0;
      toks[*ntok].nextra = 0;
      toks[*ntok].extra = 0;
      (*ntok)++;
      *repeat = 0;
      z--;
    }
  }
  return 0;
}

static int plan_lengths(const uint8_t * lens, int alphabet, brotli_tok_t * toks,
    int cap, int * ntok_out) {
  int space = 32768;
  int i = 0;
  int repeat = 0;
  int ntok = 0;
  while (i < alphabet && space > 0) {
    if (lens[i] == 0) {
      int z = 0;
      while (i + z < alphabet && lens[i + z] == 0) {
        z++;
      }
      if (emit_zero_run(&repeat, z, toks, &ntok, cap) != 0) {
        return -1;
      }
      i += z;
    }
    else {
      if (ntok >= cap) {
        return -1;
      }
      toks[ntok].sym = lens[i];
      toks[ntok].nextra = 0;
      toks[ntok].extra = 0;
      ntok++;
      repeat = 0;
      space -= 32768 >> lens[i];
      if (space < 0) {
        return -1;
      }
      i++;
    }
  }
  if (space != 0) {
    return -1;
  }
  while (i < alphabet) {
    if (lens[i] != 0) {
      return -1;
    }
    i++;
  }
  *ntok_out = ntok;
  return 0;
}

static int collect_syms(const uint32_t * freq, int alphabet, int * got) {
  int n = 0;
  int i;
  for (i = 0; i < alphabet; i++) {
    if (freq[i] != 0) {
      got[n++] = i;
    }
  }
  for (i = 1; i < n; i++) {
    int s = got[i];
    int j = i;
    while (j > 0) {
      int p = got[j - 1];
      int less = freq[s] > freq[p] || (freq[s] == freq[p] && s < p);
      if (!less) {
        break;
      }
      got[j] = p;
      j--;
    }
    got[j] = s;
  }
  return n;
}

/* The simple prefix codes of RFC 7932 section 3.4. One symbol is not among
 * them: write_prefix answers that case with the single-symbol form before it
 * gets here, so nsym is 2, 3 or 4. */
static void simple_lengths(uint8_t * lens, int alphabet, const int * got, int nsym) {
  memset(lens, 0, (size_t)alphabet);
  if (nsym == 2) {
    lens[got[0]] = 1;
    lens[got[1]] = 1;
    return;
  }
  if (nsym == 3) {
    lens[got[0]] = 1;
    lens[got[1]] = 2;
    lens[got[2]] = 2;
    return;
  }
  lens[got[0]] = 1;
  lens[got[1]] = 2;
  lens[got[2]] = 3;
  lens[got[3]] = 3;
}

static int write_prefix(const gcomp_allocator_t * alloc, brotli_bw_t * b,
    const uint32_t * freq, int alphabet, brotli_canon_t * codes) {
  uint8_t lens_buf[704];
  uint8_t * lens = lens_buf;
  int got[4];
  int nnz;
  int i;
  if (alphabet > 704) {
    return -1;
  }
  memset(codes, 0, (size_t)alphabet * sizeof(*codes));
  if (assign_huffman(alloc, freq, alphabet, lens) != 0) {
    return -1;
  }
  nnz = 0;
  for (i = 0; i < alphabet; i++) {
    if (freq[i] != 0) {
      nnz++;
    }
  }
  if (nnz <= 1) {
    int sym = 0;
    int ab;
    for (i = 0; i < alphabet; i++) {
      if (freq[i] != 0) {
        sym = i;
        break;
      }
    }
    ab = alpha_bits(alphabet);
    if (brotli_bw_put(b, 1, 2) != 0 || brotli_bw_put(b, 0, 2) != 0 ||
        brotli_bw_put(b, (uint32_t)sym, ab) != 0) {
      return -1;
    }
    codes[sym].len = 0;
    return 0;
  }
  if (nnz <= 4) {
    int nsym = collect_syms(freq, alphabet, got);
    int ab = alpha_bits(alphabet);
    simple_lengths(lens, alphabet, got, nsym);
    build_canon(lens, alphabet, codes);
    if (brotli_bw_put(b, 1, 2) != 0 || brotli_bw_put(b, (uint32_t)(nsym - 1), 2) != 0) {
      return -1;
    }
    for (i = 0; i < nsym; i++) {
      if (brotli_bw_put(b, (uint32_t)got[i], ab) != 0) {
        return -1;
      }
    }
    if (nsym == 4 && brotli_bw_put(b, 1, 1) != 0) {
      return -1;
    }
    return 0;
  }
  if (!kraft_complete(lens, alphabet)) {
    return -1;
  }
  {
    brotli_tok_t * toks;
    uint32_t cl_freq[18];
    uint8_t cl_lens[18];
    brotli_canon_t cl_code[18];
    int ntok = 0;
    int cl_nnz;
    int only = 0;
    int cap = alphabet + 8;
    toks = gcomp_malloc(alloc, (size_t)cap * sizeof(*toks));
    if (!toks) {
      return -1;
    }
    if (plan_lengths(lens, alphabet, toks, cap, &ntok) != 0) {
      gcomp_free(alloc, toks);
      return -1;
    }
    memset(cl_freq, 0, sizeof(cl_freq));
    for (i = 0; i < ntok; i++) {
      cl_freq[toks[i].sym]++;
    }
    memset(cl_lens, 0, sizeof(cl_lens));
    cl_nnz = 0;
    for (i = 0; i < 18; i++) {
      if (cl_freq[i] != 0) {
        cl_nnz++;
        only = i;
      }
    }
    if (cl_nnz >= 2) {
      assign_balanced(cl_freq, 18, cl_lens);
    }
    else if (cl_nnz == 1) {
      cl_lens[only] = 1;
    }
    else {
      gcomp_free(alloc, toks);
      return -1;
    }
    if (brotli_bw_put(b, 0, 2) != 0) {
      gcomp_free(alloc, toks);
      return -1;
    }
    if (cl_nnz == 1) {
      for (i = 0; i < 18; i++) {
        if (write_cl_static(b, cl_lens[k_cl_order[i]]) != 0) {
          gcomp_free(alloc, toks);
          return -1;
        }
      }
    }
    else {
      int space = 32;
      int idx = 0;
      while (idx < 18 && space > 0) {
        int l = cl_lens[k_cl_order[idx]];
        if (write_cl_static(b, l) != 0) {
          gcomp_free(alloc, toks);
          return -1;
        }
        if (l != 0) {
          if (l > 5) {
            gcomp_free(alloc, toks);
            return -1;
          }
          space -= 32 >> l;
          if (space < 0) {
            gcomp_free(alloc, toks);
            return -1;
          }
        }
        idx++;
      }
      if (space != 0) {
        gcomp_free(alloc, toks);
        return -1;
      }
      build_canon(cl_lens, 18, cl_code);
    }
    build_canon(lens, alphabet, codes);
    for (i = 0; i < ntok; i++) {
      if (cl_nnz == 1) {
        if (toks[i].sym != only) {
          gcomp_free(alloc, toks);
          return -1;
        }
      }
      else if (put_canon(b, &cl_code[toks[i].sym]) != 0) {
        gcomp_free(alloc, toks);
        return -1;
      }
      if (toks[i].nextra != 0 &&
          brotli_bw_put(b, toks[i].extra, (int)toks[i].nextra) != 0) {
        gcomp_free(alloc, toks);
        return -1;
      }
    }
    gcomp_free(alloc, toks);
  }
  return 0;
}

static uint32_t hash4(const uint8_t * p) {
  uint32_t v = (uint32_t)p[0] * 2654435761u;
  v ^= (uint32_t)p[1] * 2246822519u;
  v ^= (uint32_t)p[2] * 3266489917u;
  v ^= (uint32_t)p[3] * 668265263u;
  return v >> (32 - BROTLI_HASH_BITS);
}

uint32_t brotli_mf_hash(const uint8_t * p) {
  return hash4(p);
}

void brotli_mf_slide(uint32_t * head, uint32_t * prev, size_t keep, size_t shift) {
  size_t i;
  for (i = 0; i < keep; i++) {
    uint32_t v = prev[i + shift];
    prev[i] = v > shift ? v - (uint32_t)shift : 0;
  }
  for (i = 0; i < BROTLI_HASH_SIZE; i++) {
    uint32_t v = head[i];
    head[i] = v > shift ? v - (uint32_t)shift : 0;
  }
}

static size_t match_len(const uint8_t * data, size_t n, size_t from, size_t pos) {
  size_t len = 0;
  size_t max;
  if (from >= pos) {
    return 0;
  }
  max = n - pos;
  while (len + 8u <= max) {
    uint64_t a;
    uint64_t b;
    uint64_t diff;
    memcpy(&a, data + from + len, sizeof(a));
    memcpy(&b, data + pos + len, sizeof(b));
    diff = a ^ b;
    if (diff != 0) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      len += (size_t)(__builtin_clzll(diff) / 8u);
#else
      len += (size_t)(__builtin_ctzll(diff) / 8u);
#endif
      return len;
    }
    len += 8u;
  }
  while (len < max && data[from + len] == data[pos + len]) {
    len++;
  }
  return len;
}

/* Positions are indexes into the buffer that holds the history and then the
 * chunk, stored as index + 1 so that 0 means empty. */
static void note_pos(uint32_t * head, uint32_t * prev, const uint8_t * data,
    size_t n, size_t pos) {
  uint32_t h;
  if (pos + 4 > n) {
    return;
  }
  h = hash4(data + pos);
  prev[pos] = head[h];
  head[h] = (uint32_t)pos + 1u;
}

/* What a match is worth, in half bits: eleven for each byte it covers (about
 * what a literal costs in text) less twice the cost of naming its distance. A
 * distance equal to one of the last four is a few bits whatever it is; any
 * other costs a symbol and as many extra bits as its magnitude has, which is
 * what made a 1 MiB reach for a 4-byte match a loss the greedy parse took. */
static int dist_cost(size_t dist, const uint32_t rep[4]) {
  int bits;
  if (dist == rep[0]) {
    return 1;
  }
  if (dist == rep[1] || dist == rep[2] || dist == rep[3]) {
    return 5;
  }
  bits = 6;
  while (dist > 3u) {
    dist >>= 1;
    bits++;
  }
  return bits - 1;
}

#ifndef BROTLI_LIT_HALF_BITS
#define BROTLI_LIT_HALF_BITS 11
#endif
#ifndef BROTLI_CMD_HALF_BITS
#define BROTLI_CMD_HALF_BITS 14
#endif

static int match_score(size_t len, size_t dist, const uint32_t rep[4]) {
  return (int)(len > 100000u ? 100000u : len) * BROTLI_LIT_HALF_BITS -
         2 * dist_cost(dist, rep) - BROTLI_CMD_HALF_BITS;
}

/* The best match at pos by that measure, with its score (zero when none is
 * worth coding). The chain runs nearest first, so a candidate that is no
 * longer than the best so far is no better either, and the byte one past the
 * best length is enough to tell. The last four distances are tried first,
 * since they are the cheap ones. */
static void find_match(const uint32_t * head, const uint32_t * prev,
    const uint8_t * data, size_t n, size_t pos, uint32_t window, int chain,
    size_t nice, const uint32_t rep[4], size_t * mlen, size_t * mdist,
    int * mscore) {
  uint32_t cur;
  int steps = 0;
  int r;
  *mlen = 0;
  *mdist = 0;
  *mscore = 0;
  if (pos + BROTLI_MIN_MATCH > n) {
    return;
  }
  for (r = 0; r < 4; r++) {
    size_t d = rep[r];
    size_t len;
    int sc;
    if (d == 0 || d > pos || d > window) {
      continue;
    }
    len = match_len(data, n, pos - d, pos);
    if (len < BROTLI_MIN_MATCH) {
      continue;
    }
    sc = match_score(len, d, rep);
    if (sc > *mscore) {
      *mscore = sc;
      *mlen = len;
      *mdist = d;
    }
  }
  cur = head[hash4(data + pos)];
  while (cur != 0 && steps < chain) {
    size_t from = (size_t)cur - 1u;
    size_t dist;
    size_t len;
    int sc;
    steps++;
    if (from >= pos) {
      break;
    }
    dist = pos - from;
    cur = prev[from];
    if (dist > window) {
      break;
    }
    if (*mlen != 0 && pos + *mlen < n &&
        data[from + *mlen] != data[pos + *mlen]) {
      continue;
    }
    len = match_len(data, n, from, pos);
    if (len < BROTLI_MIN_MATCH) {
      continue;
    }
    sc = match_score(len, dist, rep);
    if (sc > *mscore) {
      *mscore = sc;
      *mlen = len;
      *mdist = dist;
      if (len >= nice) {
        break;
      }
    }
  }
  if (*mscore <= 0) {
    *mlen = 0;
    *mdist = 0;
    *mscore = 0;
  }
}

/* Every command is checked against the bytes before it is believed. */
static int verify_cmds(const uint8_t * base, size_t hist, size_t len,
    uint32_t window, const brotli_cmd_t * cmds, int ncmd) {
  size_t p = 0;
  const uint8_t * data = base + hist;
  int c;
  for (c = 0; c < ncmd; c++) {
    size_t i;
    if (cmds[c].at != p) {
      return 0;
    }
    p += cmds[c].insert;
    if (p > len) {
      return 0;
    }
    if (cmds[c].tail) {
      continue;
    }
    if (cmds[c].dist == 0 || cmds[c].dist > p + hist ||
        cmds[c].dist > window) {
      return 0;
    }
    if (p + cmds[c].copy > len) {
      return 0;
    }
    i = 0;
    while (i + 8u <= cmds[c].copy) {
      uint64_t a;
      uint64_t b;
      memcpy(&a, data + p + i, sizeof(a));
      memcpy(&b, (data + p) - cmds[c].dist + i, sizeof(b));
      if (a != b) {
        return 0;
      }
      i += 8u;
    }
    while (i < cmds[c].copy) {
      if (data[p + i] != *((data + p) - cmds[c].dist + i)) {
        return 0;
      }
      i++;
    }
    p += cmds[c].copy;
  }
  return p == len && ncmd != 0;
}

/* The commands for data[hist .. hist + len), which may copy from anywhere in
 * data[0 .. hist + len) that the window allows. head and prev are the match
 * finder's, and are left holding the positions of this chunk, so that the next
 * chunk can match into it. The cmds' `at` fields are relative to the chunk. */
static int parse_cmds(const gcomp_allocator_t * alloc, const uint8_t * base,
    size_t hist, size_t len, uint32_t window, const brotli_params_t * par,
    uint32_t * head, uint32_t * prev, brotli_cmd_t ** cmds_out,
    int * ncmd_out) {
  brotli_cmd_t * cmds;
  const size_t total = hist + len;
  size_t cap;
  size_t pos = hist;
  size_t lit = 0;
  size_t lit_at = hist;
  int ncmd = 0;
  int misses = 0;
  int ok = 0;
  uint32_t rep[4];
  memcpy(rep, par->rb, sizeof(rep));
  cap = len / BROTLI_MIN_MATCH + 2u;
  cmds = gcomp_malloc(alloc, cap * sizeof(*cmds));
  if (!cmds) {
    return -1;
  }
  /* The last three positions of the previous chunk could not be hashed then,
   * for want of the bytes after them. */
  {
    size_t k = hist >= 3u ? hist - 3u : 0;
    for (; k < hist; k++) {
      note_pos(head, prev, base, total, k);
    }
  }
  while (pos < total) {
    size_t mlen = 0;
    size_t mdist = 0;
    int mscore = 0;
    size_t step;
    find_match(head, prev, base, total, pos, window, par->chain, par->nice, rep,
        &mlen, &mdist, &mscore);
    note_pos(head, prev, base, total, pos);
    /* One step of lazy matching: if the next position starts a match worth
     * more than this one by a literal's cost, this one is passed over for it
     * and its first byte becomes a literal. */
    while (par->lazy && mlen >= BROTLI_MIN_MATCH && mlen < par->nice &&
           pos + 1u < total) {
      size_t nlen = 0;
      size_t ndist = 0;
      int nscore = 0;
      find_match(head, prev, base, total, pos + 1u, window, par->chain,
          par->nice, rep, &nlen, &ndist, &nscore);
      if (nlen == 0 || nscore <= mscore + BROTLI_LIT_HALF_BITS) {
        break;
      }
      if (lit == 0) {
        lit_at = pos;
      }
      lit++;
      pos++;
      note_pos(head, prev, base, total, pos);
      mlen = nlen;
      mdist = ndist;
      mscore = nscore;
    }
    if (mlen >= BROTLI_MIN_MATCH) {
      size_t end = pos + mlen;
      size_t k;
      if ((size_t)ncmd >= cap) {
        goto done;
      }
      misses = 0;
      /* A long match is entered every fourth byte, plus its last four, which
       * is what the next search can still reach. */
      if (mlen > 64u) {
        size_t tail = end - 4u;
        for (k = pos + 4u; k < tail; k += 4u) {
          note_pos(head, prev, base, total, k);
        }
        for (k = tail; k < end; k++) {
          note_pos(head, prev, base, total, k);
        }
      }
      else {
        for (k = pos + 1u; k < end; k++) {
          note_pos(head, prev, base, total, k);
        }
      }
      if (mdist != rep[0]) {
        if (mdist == rep[1]) {
          rep[1] = rep[0];
        }
        else if (mdist == rep[2]) {
          rep[2] = rep[1];
          rep[1] = rep[0];
        }
        else {
          rep[3] = rep[2];
          rep[2] = rep[1];
          rep[1] = rep[0];
        }
        rep[0] = (uint32_t)mdist;
      }
      cmds[ncmd].at = (uint32_t)(lit_at - hist);
      cmds[ncmd].insert = (uint32_t)lit;
      cmds[ncmd].copy = (uint32_t)mlen;
      cmds[ncmd].dist = (uint32_t)mdist;
      cmds[ncmd].tail = 0;
      ncmd++;
      pos = end;
      lit = 0;
      lit_at = pos;
    }
    else {
      if (lit == 0) {
        lit_at = pos;
      }
      misses++;
      step = 1;
      if (misses > 64) {
        step = (size_t)(misses >> 6);
        if (step > 16u) {
          step = 16u;
        }
      }
      if (step > total - pos) {
        step = total - pos;
      }
      lit += step;
      pos += step;
    }
  }
  if (lit != 0) {
    if ((size_t)ncmd >= cap) {
      goto done;
    }
    cmds[ncmd].at = (uint32_t)(lit_at - hist);
    cmds[ncmd].insert = (uint32_t)lit;
    cmds[ncmd].copy = 0;
    cmds[ncmd].dist = 0;
    cmds[ncmd].tail = 1;
    ncmd++;
  }
  if (!verify_cmds(base, hist, len, window, cmds, ncmd)) {
    goto done;
  }
  ok = 1;
done:
  if (!ok) {
    gcomp_free(alloc, cmds);
    return 1;
  }
  *cmds_out = cmds;
  *ncmd_out = ncmd;
  return 0;
}

static int resolve_cmd(const brotli_cmd_t * cmd, const uint32_t rb[4], int fresh,
    int imp[24][24], int exp[24][24], int * ic, int * implicit,
    int * dist_sym, uint32_t * ins_extra, int * ins_n, uint32_t * copy_extra,
    int * copy_n, uint32_t * dist_extra, int * dist_n);

/* The optimal parse (level 5): the cheapest way through the chunk by a price
 * for every symbol a command can use, found by a shortest-path pass over the
 * positions. A node is a position; an edge is one literal or one match (with
 * the literals before it priced as the command's insert length, which is why a
 * node keeps the run of literals that led to it). Each node also keeps the
 * decoder's four last distances along its best path, so that a match at one of
 * them is priced as the cheap thing it is.
 *
 * The prices come from the symbols a previous parse used. The first pass has
 * none, so it starts from a guess, and it is the pass that searches: what the
 * match finder offered at each position is kept, and the later passes price
 * that same offer again with the statistics of the pass before. */
#ifndef BROTLI_OPT_PASSES
#define BROTLI_OPT_PASSES 3
#endif
#define BROTLI_OPT_VARIANTS 2
#define BROTLI_OPT_CANDS 12
#define BROTLI_OPT_TAB 2048
#define BROTLI_OPT_INF 1e30f

typedef struct brotli_opt_s {
  float cost;
  uint32_t rep[4];
  uint32_t run;   /* literals since the last match on the best path here */
  uint32_t mlen;  /* the match that arrives here, 0 for a literal */
  uint32_t mdist;
  uint32_t mins;  /* the literals that came before that match */
} brotli_opt_t;

typedef struct brotli_price_s {
  float lit[256];
  float ic[704];
  float dist[64];
} brotli_price_t;

typedef struct brotli_optc_s {
  const gcomp_allocator_t * alloc;
  const uint8_t * base;
  size_t hist;
  size_t len;
  size_t total;
  uint32_t window;
  const brotli_params_t * par;
  uint32_t * head;
  uint32_t * prev;
  brotli_opt_t * nodes;
  uint32_t * off;  /* off[i] .. off[i + 1]: what was offered at hist + i */
  uint32_t * pool_len;
  uint32_t * pool_dist;
  size_t pool_n;
  size_t pool_cap;
  brotli_price_t pr;
  float litscale;
  int imp[24][24];
  int exp[24][24];
  uint8_t ins_tab[BROTLI_OPT_TAB];
  uint8_t copy_tab[BROTLI_OPT_TAB];
} brotli_optc_t;

static int opt_ins_code(const brotli_optc_t * o, uint32_t n, int * nb) {
  int code = 23;
  uint32_t e = 0;
  if (n < BROTLI_OPT_TAB) {
    code = o->ins_tab[n];
  }
  else {
    (void)length_code(n, k_ins_base, k_ins_extra, &code, &e, nb);
  }
  *nb = (int)k_ins_extra[code];
  return code;
}

static int opt_copy_code(const brotli_optc_t * o, uint32_t n, int * nb) {
  int code = 23;
  uint32_t e = 0;
  if (n < BROTLI_OPT_TAB) {
    code = o->copy_tab[n];
  }
  else {
    (void)length_code(n, k_copy_base, k_copy_extra, &code, &e, nb);
  }
  *nb = (int)k_copy_extra[code];
  return code;
}

/* Prices, in bits, from how often each symbol was used. */
static float opt_bits(uint32_t f, uint32_t total, int n) {
  return log2f(((float)total + 0.5f * (float)n) / ((float)f + 0.5f));
}

static void opt_prices(brotli_optc_t * o, const brotli_cmd_t * cmds, int ncmd,
    const uint32_t rb0[4]) {
  uint32_t lf[256];
  uint32_t cf[704];
  uint32_t df[64];
  uint32_t lt = 0;
  uint32_t ct = 0;
  uint32_t dt = 0;
  uint32_t rb[4];
  const uint8_t * data = o->base + o->hist;
  int c;
  int i;
  memset(lf, 0, sizeof(lf));
  memset(cf, 0, sizeof(cf));
  memset(df, 0, sizeof(df));
  memcpy(rb, rb0, sizeof(rb));
  for (c = 0; c < ncmd; c++) {
    int ic = 0;
    int implicit = 0;
    int ds = 0;
    uint32_t e0;
    uint32_t e1;
    uint32_t e2;
    int n0;
    int n1;
    int n2;
    uint32_t k;
    for (k = 0; k < cmds[c].insert; k++) {
      lf[data[cmds[c].at + k]]++;
    }
    lt += cmds[c].insert;
    if (resolve_cmd(&cmds[c], rb, 0, o->imp, o->exp, &ic, &implicit, &ds, &e0,
            &n0, &e1, &n1, &e2, &n2) != 0 || ic < 0 || ic >= 704) {
      continue;
    }
    cf[ic]++;
    ct++;
    if (!cmds[c].tail && !implicit && ds >= 0 && ds < 64) {
      df[ds]++;
      dt++;
      if (ds != 0) {
        push_dist(rb, cmds[c].dist);
      }
    }
  }
  for (i = 0; i < 256; i++) {
    o->pr.lit[i] = opt_bits(lf[i], lt, 256);
  }
  for (i = 0; i < 704; i++) {
    o->pr.ic[i] = opt_bits(cf[i], ct, 704);
  }
  for (i = 0; i < 64; i++) {
    o->pr.dist[i] = opt_bits(df[i], dt, 64);
  }
}

/* What the first pass assumes: literals as often as they occur in the chunk,
 * a command a few bits, a distance by its magnitude. */
static void opt_prior(brotli_optc_t * o) {
  uint32_t lf[256];
  const uint8_t * data = o->base + o->hist;
  size_t i;
  memset(lf, 0, sizeof(lf));
  for (i = 0; i < o->len; i++) {
    lf[data[i]]++;
  }
  for (i = 0; i < 256; i++) {
    o->pr.lit[i] = opt_bits(lf[i], (uint32_t)o->len, 256);
  }
  for (i = 0; i < 704; i++) {
    o->pr.ic[i] = 7.0f;
  }
  for (i = 0; i < 64; i++) {
    o->pr.dist[i] = i == 0 ? 1.5f : (i < 16 ? 5.0f : 4.0f);
  }
}

static int opt_pool_add(brotli_optc_t * o, uint32_t len, uint32_t dist) {
  if (o->pool_n == o->pool_cap) {
    size_t cap = o->pool_cap * 2u;
    uint32_t * nl = gcomp_malloc(o->alloc, cap * sizeof(*nl));
    uint32_t * nd = gcomp_malloc(o->alloc, cap * sizeof(*nd));
    if (!nl || !nd) {
      gcomp_free(o->alloc, nl);
      gcomp_free(o->alloc, nd);
      return -1;
    }
    memcpy(nl, o->pool_len, o->pool_n * sizeof(*nl));
    memcpy(nd, o->pool_dist, o->pool_n * sizeof(*nd));
    gcomp_free(o->alloc, o->pool_len);
    gcomp_free(o->alloc, o->pool_dist);
    o->pool_len = nl;
    o->pool_dist = nd;
    o->pool_cap = cap;
  }
  o->pool_len[o->pool_n] = len;
  o->pool_dist[o->pool_n] = dist;
  o->pool_n++;
  return 0;
}

/* The price of naming distance d after the four in rep: its symbol, its extra
 * bits, and which symbol it was (for the implicit form). */
static float opt_dist_price(const brotli_optc_t * o, uint32_t d,
    const uint32_t rep[4]) {
  int sym = 0;
  uint32_t extra = 0;
  int nb = 0;
  if (short_dist_sym(d, rep, &sym) == 0) {
    return o->pr.dist[sym];
  }
  if (long_dist_sym(d, &sym, &extra, &nb) != 0) {
    return BROTLI_OPT_INF;
  }
  return o->pr.dist[sym] + (float)nb;
}

/* Reach rel + l from rel by a match of length l at distance d, if that is
 * cheaper than what reaches it now. dprice is the distance's price. */
static void opt_edge(brotli_optc_t * o, size_t rel, uint32_t l, uint32_t d,
    float dprice) {
  const brotli_opt_t * f = &o->nodes[rel];
  brotli_opt_t * t = &o->nodes[rel + l];
  int ni;
  int nc;
  int ic = opt_ins_code(o, f->run, &ni);
  int cc = opt_copy_code(o, l, &nc);
  float c = f->cost + (float)(ni + nc);
  if (d == f->rep[0] && ic <= 7 && cc <= 15 && o->imp[ic][cc] >= 0) {
    c += o->pr.ic[o->imp[ic][cc]];
  }
  else {
    if (o->exp[ic][cc] < 0) {
      return;
    }
    c += o->pr.ic[o->exp[ic][cc]] + dprice;
  }
  if (c >= t->cost) {
    return;
  }
  t->cost = c;
  memcpy(t->rep, f->rep, sizeof(t->rep));
  if (d != f->rep[0]) {
    push_dist(t->rep, d);
  }
  t->run = 0;
  t->mlen = l;
  t->mdist = d;
  t->mins = f->run;
}

static void opt_literal(brotli_optc_t * o, size_t rel) {
  const brotli_opt_t * f = &o->nodes[rel];
  brotli_opt_t * t = &o->nodes[rel + 1u];
  float c = f->cost + o->litscale * o->pr.lit[o->base[o->hist + rel]];
  if (c >= t->cost) {
    return;
  }
  t->cost = c;
  memcpy(t->rep, f->rep, sizeof(t->rep));
  t->run = f->run + 1u;
  t->mlen = 0;
  t->mdist = 0;
  t->mins = 0;
}

/* One pass over the chunk. When search is set the match finder is asked at
 * each position and its answers are kept; otherwise the kept answers are
 * priced again. */
static int opt_pass(brotli_optc_t * o, int search, const uint32_t rb0[4]) {
  const size_t total = o->total;
  size_t rel;
  size_t i;
  int misses = 0;
  for (i = 0; i <= o->len; i++) {
    o->nodes[i].cost = BROTLI_OPT_INF;
  }
  o->nodes[0].cost = 0.0f;
  memcpy(o->nodes[0].rep, rb0, sizeof(o->nodes[0].rep));
  o->nodes[0].run = 0;
  o->nodes[0].mlen = 0;
  o->nodes[0].mdist = 0;
  o->nodes[0].mins = 0;
  if (search) {
    o->pool_n = 0;
  }
  rel = 0;
  while (rel < o->len) {
    const size_t p = o->hist + rel;
    const brotli_opt_t * f = &o->nodes[rel];
    uint32_t cl[4 + BROTLI_OPT_CANDS];
    uint32_t cd[4 + BROTLI_OPT_CANDS];
    float cp[4 + BROTLI_OPT_CANDS];
    int nrep = 0;
    int nc = 0;
    int r;
    int k;
    uint32_t longest = 0;
    int longest_k = 0;
    if (search) {
      o->off[rel] = (uint32_t)o->pool_n;
    }
    for (r = 0; r < 4; r++) {
      uint32_t d = f->rep[r];
      uint32_t l;
      int dup = 0;
      int q;
      if (d == 0 || d > p || d > o->window) {
        continue;
      }
      for (q = 0; q < r; q++) {
        dup |= f->rep[q] == d;
      }
      if (dup) {
        continue;
      }
      l = (uint32_t)match_len(o->base, total, p - d, p);
      if (l < BROTLI_MIN_MATCH) {
        continue;
      }
      cl[nc] = l;
      cd[nc] = d;
      cp[nc] = opt_dist_price(o, d, f->rep);
      nc++;
    }
    nrep = nc;
    if (search) {
      uint32_t cur = o->head[hash4(o->base + p)];
      int steps = 0;
      uint32_t best = 0;
      int got = 0;
      if (p + BROTLI_MIN_MATCH <= total) {
        while (cur != 0 && steps < o->par->chain) {
          size_t from = (size_t)cur - 1u;
          size_t dist;
          uint32_t l;
          steps++;
          if (from >= p) {
            break;
          }
          dist = p - from;
          cur = o->prev[from];
          if (dist > o->window) {
            break;
          }
          if (best != 0 && p + best < total &&
              o->base[from + best] != o->base[p + best]) {
            continue;
          }
          l = (uint32_t)match_len(o->base, total, from, p);
          if (l < BROTLI_MIN_MATCH || l <= best) {
            continue;
          }
          best = l;
          if (opt_pool_add(o, l, (uint32_t)dist) != 0) {
            return -1;
          }
          got++;
          if (l >= o->par->nice || got >= BROTLI_OPT_CANDS) {
            break;
          }
        }
      }
      note_pos(o->head, o->prev, o->base, total, p);
    }
    {
      size_t kend = search ? o->pool_n : (size_t)o->off[rel + 1u];
      size_t kk;
      for (kk = o->off[rel]; kk < kend && nc < 4 + BROTLI_OPT_CANDS; kk++) {
        cl[nc] = o->pool_len[kk];
        cd[nc] = o->pool_dist[kk];
        cp[nc] = opt_dist_price(o, cd[nc], f->rep);
        nc++;
      }
    }
    if (nc == 0 && search) {
      misses++;
    }
    else {
      misses = 0;
    }
    for (k = 0; k < nc; k++) {
      if (cl[k] > longest) {
        longest = cl[k];
        longest_k = k;
      }
    }
    opt_literal(o, rel);
    if (longest >= o->par->nice) {
      size_t end;
      uint32_t l = longest;
      if ((size_t)l > o->len - rel) {
        l = (uint32_t)(o->len - rel);
      }
      opt_edge(o, rel, l, cd[longest_k], cp[longest_k]);
      end = rel + l;
      if (search) {
        size_t j;
        for (j = rel + 1u; j < end; j++) {
          o->off[j] = (uint32_t)o->pool_n;
        }
        if (l > 64u) {
          size_t tail = p + l - 4u;
          for (j = p + 4u; j < tail; j += 4u) {
            note_pos(o->head, o->prev, o->base, total, j);
          }
          for (j = tail; j < p + l; j++) {
            note_pos(o->head, o->prev, o->base, total, j);
          }
        }
        else {
          for (j = p + 1u; j < p + l; j++) {
            note_pos(o->head, o->prev, o->base, total, j);
          }
        }
      }
      rel = end;
      continue;
    }
    for (k = 0; k < nc; k++) {
      /* chain answers rise in length: each covers the lengths past the one
       * before it, which a nearer distance already reaches */
      uint32_t lo = k > nrep ? cl[k - 1] + 1u : BROTLI_MIN_MATCH;
      uint32_t l;
      for (l = lo; l <= cl[k] && rel + l <= o->len; l++) {
        opt_edge(o, rel, l, cd[k], cp[k]);
      }
    }
    if (search && misses > 64) {
      size_t step = (size_t)(misses >> 6);
      size_t j;
      if (step > 16u) {
        step = 16u;
      }
      if (step > o->len - rel) {
        step = o->len - rel;
      }
      for (j = 1; j < step; j++) {
        o->off[rel + j] = (uint32_t)o->pool_n;
        opt_literal(o, rel + j);
      }
      rel += step;
      continue;
    }
    rel++;
  }
  if (search) {
    o->off[o->len] = (uint32_t)o->pool_n;
  }
  return 0;
}

/* The path to the end of the chunk as commands; 0 when it is whole. */
static int opt_trace(const brotli_optc_t * o, brotli_cmd_t * cmds, int cap,
    int * ncmd_out) {
  size_t q = o->len;
  uint32_t tail = o->nodes[q].run;
  size_t at = 0;
  int n = 0;
  int a;
  int b;
  if (o->nodes[q].cost >= BROTLI_OPT_INF) {
    return 1;
  }
  q -= tail;
  while (q > 0) {
    const brotli_opt_t * nd = &o->nodes[q];
    if (nd->mlen == 0 || n >= cap || nd->mlen + nd->mins > q) {
      return 1;
    }
    cmds[n].insert = nd->mins;
    cmds[n].copy = nd->mlen;
    cmds[n].dist = nd->mdist;
    cmds[n].tail = 0;
    n++;
    q -= nd->mlen + nd->mins;
  }
  /* they were found last first */
  for (a = 0, b = n - 1; a < b; a++, b--) {
    brotli_cmd_t t = cmds[a];
    cmds[a] = cmds[b];
    cmds[b] = t;
  }
  if (tail != 0) {
    if (n >= cap) {
      return 1;
    }
    cmds[n].insert = tail;
    cmds[n].copy = 0;
    cmds[n].dist = 0;
    cmds[n].tail = 1;
    n++;
  }
  for (a = 0; a < n; a++) {
    cmds[a].at = (uint32_t)at;
    at += cmds[a].insert + cmds[a].copy;
  }
  *ncmd_out = n;
  return 0;
}

/* How much a literal is made to cost, for each parse that is tried. A
 * literal's price comes from the previous parse's own use of literals, and on
 * data where it is low (records with a few random bytes) the parse keeps
 * choosing literals because it chose them before; dearer ones break out of it.
 * The coder's own count of bytes is the judge, in brotli_compress_chunk_h. */
static const float k_opt_litscale[BROTLI_OPT_VARIANTS] = {1.0f, 1.3f};

static int parse_opt(const gcomp_allocator_t * alloc, const uint8_t * base,
    size_t hist, size_t len, uint32_t window, const brotli_params_t * par,
    uint32_t * head, uint32_t * prev, brotli_cmd_t * cmds_out[],
    int ncmd_out[]) {
  brotli_optc_t * o = gcomp_calloc(alloc, 1, sizeof(*o));
  brotli_cmd_t * cmds = NULL;
  int cap = (int)(len / BROTLI_MIN_MATCH) + 3;
  int ncmd = 0;
  int result = 1;
  int variant;
  int pass;
  size_t k;
  if (!o) {
    return -1;
  }
  o->alloc = alloc;
  o->base = base;
  o->hist = hist;
  o->len = len;
  o->total = hist + len;
  o->window = window;
  o->par = par;
  o->head = head;
  o->prev = prev;
  o->pool_cap = len < 4096u ? 4096u : len;
  o->nodes = gcomp_malloc(alloc, (len + 1u) * sizeof(*o->nodes));
  o->off = gcomp_malloc(alloc, (len + 2u) * sizeof(*o->off));
  o->pool_len = gcomp_malloc(alloc, o->pool_cap * sizeof(*o->pool_len));
  o->pool_dist = gcomp_malloc(alloc, o->pool_cap * sizeof(*o->pool_dist));
  cmds = gcomp_malloc(alloc, (size_t)cap * sizeof(*cmds));
  if (!o->nodes || !o->off || !o->pool_len || !o->pool_dist || !cmds) {
    result = -1;
    goto out;
  }
  fill_ic(o->imp, o->exp);
  for (k = 0; k < BROTLI_OPT_TAB; k++) {
    int c = 0;
    int nb = 0;
    uint32_t e = 0;
    (void)length_code((uint32_t)k, k_ins_base, k_ins_extra, &c, &e, &nb);
    o->ins_tab[k] = (uint8_t)c;
    c = 0;
    if (k < 2) {
      o->copy_tab[k] = 0;
      continue;
    }
    (void)length_code((uint32_t)k, k_copy_base, k_copy_extra, &c, &e, &nb);
    o->copy_tab[k] = (uint8_t)c;
  }
  /* The last three positions of the previous chunk could not be hashed then,
   * for want of the bytes after them. */
  for (k = hist >= 3u ? hist - 3u : 0; k < hist; k++) {
    note_pos(head, prev, base, o->total, k);
  }
  for (variant = 0; variant < BROTLI_OPT_VARIANTS; variant++) {
    o->litscale = k_opt_litscale[variant];
    opt_prior(o);
    for (pass = 0; pass < BROTLI_OPT_PASSES; pass++) {
      if (pass > 0) {
        opt_prices(o, cmds, ncmd, par->rb);
      }
      if (opt_pass(o, variant == 0 && pass == 0, par->rb) != 0) {
        result = -1;
        goto out;
      }
      if (opt_trace(o, cmds, cap, &ncmd) != 0) {
        goto out;
      }
    }
    if (!verify_cmds(base, hist, len, window, cmds, ncmd)) {
      goto out;
    }
    cmds_out[variant] = cmds;
    ncmd_out[variant] = ncmd;
    cmds = gcomp_malloc(alloc, (size_t)cap * sizeof(*cmds));
    if (!cmds) {
      result = -1;
      goto out;
    }
  }
  result = 0;
out:
  gcomp_free(alloc, cmds);
  if (result != 0) {
    for (variant = 0; variant < BROTLI_OPT_VARIANTS; variant++) {
      gcomp_free(alloc, cmds_out[variant]);
      cmds_out[variant] = NULL;
    }
  }
  if (o) {
    gcomp_free(alloc, o->nodes);
    gcomp_free(alloc, o->off);
    gcomp_free(alloc, o->pool_len);
    gcomp_free(alloc, o->pool_dist);
  }
  gcomp_free(alloc, o);
  return result;
}

/* fresh is how many distances must still be spelled absolutely. A full flush
 * sets it, because the ring buffer is the one piece of decoder state that
 * survives a meta-block boundary: a short or implicit distance code after the
 * flush names a distance established before it, which is exactly what
 * GCOMP_FLUSH_FULL promises will not happen. Four absolute distances refill
 * every slot the short codes can read, and after that they are safe again. */
static int resolve_cmd(const brotli_cmd_t * cmd, const uint32_t rb[4], int fresh,
    int imp[24][24], int exp[24][24], int * ic, int * implicit,
    int * dist_sym, uint32_t * ins_extra, int * ins_n, uint32_t * copy_extra,
    int * copy_n, uint32_t * dist_extra, int * dist_n) {
  int ins_code = 0;
  int copy_code = 0;
  if (length_code(cmd->insert, k_ins_base, k_ins_extra, &ins_code, ins_extra,
          ins_n) != 0) {
    return -1;
  }
  if (cmd->tail) {
    *copy_extra = 0;
    *copy_n = 0;
    *implicit = 0;
    *dist_sym = -1;
    *dist_extra = 0;
    *dist_n = 0;
    *ic = exp[ins_code][0];
    if (*ic < 0) {
      *ic = imp[ins_code][0];
    }
    return *ic < 0 ? -1 : 0;
  }
  if (length_code(cmd->copy, k_copy_base, k_copy_extra, &copy_code, copy_extra,
          copy_n) != 0) {
    return -1;
  }
  if (!fresh && ins_code <= 7 && copy_code <= 15 && cmd->dist == rb[0] &&
      imp[ins_code][copy_code] >= 0) {
    *implicit = 1;
    *ic = imp[ins_code][copy_code];
    *dist_sym = -1;
    *dist_extra = 0;
    *dist_n = 0;
    return 0;
  }
  *implicit = 0;
  *ic = exp[ins_code][copy_code];
  if (*ic < 0) {
    return -1;
  }
  if (!fresh && short_dist_sym(cmd->dist, rb, dist_sym) == 0) {
    *dist_extra = 0;
    *dist_n = 0;
    return 0;
  }
  return long_dist_sym(cmd->dist, dist_sym, dist_extra, dist_n);
}

static int write_mlen(brotli_bw_t * b, size_t len) {
  uint32_t r;
  int nibbles;
  int i;
  if (len == 0 || len > 16777216u) {
    return -1;
  }
  r = (uint32_t)len - 1u;
  if (r < (1u << 16)) {
    nibbles = 4;
  }
  else if (r < (1u << 20)) {
    nibbles = 5;
  }
  else {
    nibbles = 6;
  }
  if (brotli_bw_put(b, 0, 1) != 0 || brotli_bw_put(b, (uint32_t)(nibbles - 4), 2) != 0) {
    return -1;
  }
  for (i = 0; i < nibbles; i++) {
    if (brotli_bw_put(b, (r >> (i * 4)) & 15u, 4) != 0) {
      return -1;
    }
  }
  return brotli_bw_put(b, 0, 1);
}

static int write_meta_align(brotli_bw_t * b) {
  if (brotli_bw_put(b, 0, 1) != 0 || brotli_bw_put(b, 3, 2) != 0 || brotli_bw_put(b, 0, 1) != 0 ||
      brotli_bw_put(b, 0, 2) != 0) {
    return -1;
  }
  return brotli_bw_align(b);
}

static unsigned floor_log2_u32(uint32_t v) {
  if (v <= 1u) {
    return 0;
  }
  return (unsigned)(31 - __builtin_clz(v));
}

/* ceil(log2(v)) for v >= 1. One more than floor(log2(v - 1)), which is an
 * overestimate of log2(v) and so a safe input to a lower bound. */
static unsigned ceil_log2_u32(uint32_t v) {
  if (v <= 1u) {
    return 0;
  }
  return floor_log2_u32(v - 1u) + 1u;
}

/* 1 when a Huffman encoding of these literals cannot be smaller than the
 * stored form of the whole chunk. The literal cost is a Shannon lower
 * bound (floor log of the count, ceil log of each frequency) plus two bits
 * per used symbol for the code-length alphabet, which is the shortest
 * static code-length symbol. Matches and the meta-block header are treated
 * as free, so a block this rejects would have been rejected after encoding. */
static int literals_cannot_win(const uint32_t lit_freq[256], size_t len) {
  size_t stored = len + 3u * ((len + 65535u) / 65536u);
  size_t nlit = 0;
  int nnz = 0;
  int i;
  unsigned lg_n;
  uint64_t bits = 0;
  for (i = 0; i < 256; i++) {
    if (lit_freq[i] != 0) {
      nlit += lit_freq[i];
      nnz++;
    }
  }
  if (nnz > 1 && nlit > 0) {
    lg_n = floor_log2_u32((uint32_t)nlit);
    for (i = 0; i < 256; i++) {
      uint32_t f = lit_freq[i];
      unsigned lg_f;
      if (f == 0) {
        continue;
      }
      lg_f = ceil_log2_u32(f);
      if (lg_n > lg_f) {
        bits += (uint64_t)f * (unsigned)(lg_n - lg_f);
      }
    }
  }
  if (nnz > 4) {
    bits += (uint64_t)nnz * 2u;
  }
  return bits / 8u >= stored;
}

/* ---- literal context modelling ------------------------------------------ */

#ifndef BROTLI_CLUSTER_MODES
#define BROTLI_CLUSTER_MODES 2
#endif
#ifndef BROTLI_CTX_LEVEL
#define BROTLI_CTX_LEVEL 2
#endif
#ifndef BROTLI_SPLIT_LEVEL
#define BROTLI_SPLIT_LEVEL 4
#endif
#define BROTLI_NCTX 64
#define BROTLI_MAX_TREES 16
#define BROTLI_MAX_TYPES 8
#define BROTLI_MAP_MAX (BROTLI_NCTX * BROTLI_MAX_TYPES)

/* n * log2(n), from a table (the caller's, filled by nlogn_init) below its
 * reach and computed above it. A table per call rather than a global, so that
 * two encoders on two threads share nothing to race on. */
#define BROTLI_NLOGN_N 4096u

static float nlogn(const float * tab, uint32_t n) {
  if (n < BROTLI_NLOGN_N) {
    return tab[n];
  }
  return (float)n * log2f((float)n);
}

static void nlogn_init(float * tab) {
  uint32_t i;
  tab[0] = 0.0f;
  for (i = 1; i < BROTLI_NLOGN_N; i++) {
    tab[i] = (float)i * log2f((float)i);
  }
}

/* An estimate of what a Huffman code over this histogram costs: its entropy,
 * plus about four and a half bits for each symbol the header must name. */
static float hist_bits(const float * tab, const uint32_t h[256], uint32_t total,
    int * nnz_out) {
  float sum = 0.0f;
  int nnz = 0;
  int i;
  for (i = 0; i < 256; i++) {
    if (h[i] != 0) {
      sum += nlogn(tab, h[i]);
      nnz++;
    }
  }
  if (nnz_out) {
    *nnz_out = nnz;
  }
  return nlogn(tab, total) - sum + (nnz > 1 ? 4.5f * (float)nnz + 12.0f : 4.0f);
}

typedef struct brotli_clu_s {
  uint32_t h[256];
  uint32_t total;
  float bits;
  int live;
} brotli_clu_t;

static unsigned ctx_id(int mode, unsigned p1, unsigned p2, const uint8_t * l0,
    const uint8_t * l1, const uint8_t * l2) {
  switch (mode) {
  case 0:
    return p1 & 0x3fu;
  case 1:
    return p1 >> 2;
  case 2:
    return (unsigned)l0[p1] | (unsigned)l1[p2];
  default:
    return ((unsigned)l2[p1] << 3) | (unsigned)l2[p2];
  }
}

/* Merge the contexts' histograms into at most BROTLI_MAX_TREES clusters,
 * greedily, taking at each step the pair whose union costs least more than
 * the two apart and stopping when no union is a saving (a tree header saved
 * is already in hist_bits). map[ctx] is the cluster of each context, numbered
 * in order of first use; returns the number of clusters and, in *bits, the
 * estimated cost of all the literals. */
static int cluster_ctx(const float * tab, brotli_clu_t * c,
    const uint32_t (*ctx)[256], uint8_t map[BROTLI_NCTX], float * bits) {
  int owner[BROTLI_NCTX];
  int n = 0;
  int i, j;
  int live;
  for (i = 0; i < BROTLI_NCTX; i++) {
    uint32_t total = 0;
    int k;
    for (k = 0; k < 256; k++) {
      total += ctx[i][k];
    }
    owner[i] = -1;
    if (total == 0) {
      continue;
    }
    memcpy(c[n].h, ctx[i], sizeof(c[n].h));
    c[n].total = total;
    c[n].bits = hist_bits(tab, c[n].h, total, NULL);
    c[n].live = 1;
    owner[i] = n;
    n++;
  }
  live = n;
  if (n == 0) {
    memset(map, 0, BROTLI_NCTX);
    *bits = 0.0f;
    return 1;
  }
  {
    /* The cost of every union, kept: after a merge only the merged cluster's
     * row changes, so a round costs one row of unions and a scan, not all of
     * them again. */
    static const float huge = 1e30f;
    float delta[BROTLI_NCTX][BROTLI_NCTX];
    for (i = 0; i < n; i++) {
      for (j = i + 1; j < n; j++) {
        uint32_t m[256];
        int k;
        for (k = 0; k < 256; k++) {
          m[k] = c[i].h[k] + c[j].h[k];
        }
        delta[i][j] = hist_bits(tab, m, c[i].total + c[j].total, NULL) -
                      c[i].bits - c[j].bits;
      }
    }
    while (live > 1) {
      float best = huge;
      int bi = -1, bj = -1;
      int k;
      for (i = 0; i < n; i++) {
        if (!c[i].live) {
          continue;
        }
        for (j = i + 1; j < n; j++) {
          if (c[j].live && delta[i][j] < best) {
            best = delta[i][j];
            bi = i;
            bj = j;
          }
        }
      }
      /* Past the limit a union is taken at any price; below it only a saving. */
      if (bi < 0 || (best >= 0.0f && live <= BROTLI_MAX_TREES)) {
        break;
      }
      for (k = 0; k < 256; k++) {
        c[bi].h[k] += c[bj].h[k];
      }
      c[bi].total += c[bj].total;
      c[bi].bits = hist_bits(tab, c[bi].h, c[bi].total, NULL);
      c[bj].live = 0;
      for (k = 0; k < BROTLI_NCTX; k++) {
        if (owner[k] == bj) {
          owner[k] = bi;
        }
      }
      live--;
      for (j = 0; j < n; j++) {
        uint32_t m[256];
        int q;
        int lo, hi;
        if (!c[j].live || j == bi) {
          continue;
        }
        lo = j < bi ? j : bi;
        hi = j < bi ? bi : j;
        for (q = 0; q < 256; q++) {
          m[q] = c[bi].h[q] + c[j].h[q];
        }
        delta[lo][hi] = hist_bits(tab, m, c[bi].total + c[j].total, NULL) -
                        c[bi].bits - c[j].bits;
      }
    }
  }
  {
    int renum[BROTLI_NCTX];
    int next = 0;
    float total_bits = 0.0f;
    for (i = 0; i < n; i++) {
      renum[i] = -1;
    }
    for (i = 0; i < BROTLI_NCTX; i++) {
      if (owner[i] >= 0 && renum[owner[i]] < 0) {
        renum[owner[i]] = next++;
      }
    }
    for (i = 0; i < BROTLI_NCTX; i++) {
      map[i] = owner[i] >= 0 ? (uint8_t)renum[owner[i]] : 0;
    }
    for (i = 0; i < n; i++) {
      if (c[i].live) {
        total_bits += c[i].bits;
      }
    }
    *bits = total_bits;
    return next;
  }
}

/* The variable-length count of RFC 7932 section 9.2 (NBLTYPES, NTREES). */
static int put_count(brotli_bw_t * b, int v) {
  int n;
  if (v == 1) {
    return brotli_bw_put(b, 0, 1);
  }
  if (brotli_bw_put(b, 1, 1) != 0) {
    return -1;
  }
  if (v == 2) {
    return brotli_bw_put(b, 0, 3);
  }
  n = (int)floor_log2_u32((uint32_t)(v - 1));
  if (brotli_bw_put(b, (uint32_t)n, 3) != 0) {
    return -1;
  }
  return brotli_bw_put(b, (uint32_t)(v - 1 - (1 << n)), n);
}

/* A context map (section 7.3): run-length coded zeros under a prefix code, and
 * optionally move-to-front coded. The choice of the two is made by trying
 * them and keeping the one with the fewest estimated bits. */
typedef struct brotli_mtok_s {
  uint16_t sym;
  uint8_t nextra;
  uint16_t extra;
} brotli_mtok_t;

static int cmap_tokens(const uint8_t * v, int n, int rlemax,
    brotli_mtok_t * toks) {
  int nt = 0;
  int i = 0;
  while (i < n) {
    if (v[i] != 0) {
      toks[nt].sym = (uint16_t)(rlemax + v[i]);
      toks[nt].nextra = 0;
      toks[nt].extra = 0;
      nt++;
      i++;
      continue;
    }
    {
      int run = 0;
      while (i + run < n && v[i + run] == 0) {
        run++;
      }
      i += run;
      while (run > 0) {
        int s;
        if (rlemax == 0 || run == 1) {
          toks[nt].sym = 0;
          toks[nt].nextra = 0;
          toks[nt].extra = 0;
          nt++;
          run--;
          continue;
        }
        s = (int)floor_log2_u32((uint32_t)run);
        if (s > rlemax) {
          s = rlemax;
        }
        if (run >= (2 << s)) {
          toks[nt].sym = (uint16_t)s;
          toks[nt].nextra = (uint8_t)s;
          toks[nt].extra = (uint16_t)((1 << s) - 1);
          run -= (2 << s) - 1;
        }
        else {
          toks[nt].sym = (uint16_t)s;
          toks[nt].nextra = (uint8_t)s;
          toks[nt].extra = (uint16_t)(run - (1 << s));
          run = 0;
        }
        nt++;
      }
    }
  }
  return nt;
}

static void mtf_forward(const uint8_t * in, int n, uint8_t * out) {
  uint8_t mtf[256];
  int i, k;
  for (i = 0; i < 256; i++) {
    mtf[i] = (uint8_t)i;
  }
  for (i = 0; i < n; i++) {
    uint8_t v = in[i];
    for (k = 0; mtf[k] != v; k++) {
    }
    out[i] = (uint8_t)k;
    for (; k > 0; k--) {
      mtf[k] = mtf[k - 1];
    }
    mtf[0] = v;
  }
}

static int write_cmap(const float * tab, const gcomp_allocator_t * alloc,
    brotli_bw_t * b, const uint8_t * map, int n, int ntrees) {
  brotli_mtok_t toks[BROTLI_MAP_MAX];
  brotli_mtok_t best_toks[BROTLI_MAP_MAX];
  uint8_t mtf_map[BROTLI_MAP_MAX];
  const uint8_t * vals[2];
  int best_nt = 0, best_rle = 0, best_mtf = 0;
  float best_bits = 0.0f;
  int have = 0;
  int mtfv, rle, i;
  mtf_forward(map, n, mtf_map);
  vals[0] = map;
  vals[1] = mtf_map;
  for (mtfv = 0; mtfv < 2; mtfv++) {
    for (rle = 0; rle <= 6; rle++) {
      uint32_t freq[256 + 16];
      int nt = cmap_tokens(vals[mtfv], n, rle, toks);
      float bits = 8.0f;
      int nz = 0;
      memset(freq, 0, sizeof(freq));
      for (i = 0; i < nt; i++) {
        freq[toks[i].sym]++;
        bits += (float)toks[i].nextra;
      }
      for (i = 0; i < ntrees + rle; i++) {
        if (freq[i] != 0) {
          bits += nlogn(tab, (uint32_t)nt) - nlogn(tab, freq[i]) + 4.0f;
          nz++;
        }
      }
      (void)nz;
      if (!have || bits < best_bits) {
        have = 1;
        best_bits = bits;
        best_nt = nt;
        best_rle = rle;
        best_mtf = mtfv;
        memcpy(best_toks, toks, sizeof(toks));
      }
    }
  }
  {
    uint32_t freq[256 + 16];
    brotli_canon_t code[256 + 16];
    int alphabet = ntrees + best_rle;
    memset(freq, 0, sizeof(freq));
    for (i = 0; i < best_nt; i++) {
      freq[best_toks[i].sym]++;
    }
    if (best_rle == 0) {
      if (brotli_bw_put(b, 0, 1) != 0) {
        return -1;
      }
    }
    else if (brotli_bw_put(b, 1, 1) != 0 ||
             brotli_bw_put(b, (uint32_t)(best_rle - 1), 4) != 0) {
      return -1;
    }
    if (write_prefix(alloc, b, freq, alphabet, code) != 0) {
      return -1;
    }
    for (i = 0; i < best_nt; i++) {
      if (put_canon(b, &code[best_toks[i].sym]) != 0) {
        return -1;
      }
      if (best_toks[i].nextra != 0 &&
          brotli_bw_put(b, best_toks[i].extra, best_toks[i].nextra) != 0) {
        return -1;
      }
    }
    return brotli_bw_put(b, (uint32_t)best_mtf, 1);
  }
}


/* The state the literal context model needs for one block, in one allocation. */
/* ---- literal block splitting -------------------------------------------- */

typedef struct brotli_blk_s {
  uint8_t type;
  uint32_t len;
} brotli_blk_t;

static const uint8_t k_blk_extra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};
static const uint32_t k_blk_base[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81,
    97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433,
    16625};

static int blk_len_code(uint32_t len, int * code, uint32_t * extra, int * nbits) {
  int i;
  for (i = 25; i >= 0; i--) {
    if (len >= k_blk_base[i]) {
      if (len - k_blk_base[i] >= (1u << k_blk_extra[i])) {
        return -1;
      }
      *code = i;
      *extra = len - k_blk_base[i];
      *nbits = k_blk_extra[i];
      return 0;
    }
  }
  return -1;
}

/* What a switch of literal block type costs, in bits, for the choosing: a
 * type symbol, a count symbol and the count's extra bits. */
#define BROTLI_SWITCH_BITS 24.0f

typedef struct brotli_seg_s {
  uint32_t h[256];
  uint32_t n;
} brotli_seg_t;

/* Split the literals into runs that each have their own character: segments
 * of a few hundred literals are given to the best of up to BROTLI_MAX_TYPES
 * histograms, which are then re-made from the segments they were given, by a
 * few rounds of Viterbi over the segments with a price for every switch. A
 * type is added only while it pays for itself. Returns the number of types
 * (1 when splitting does not pay), the blocks, and an estimate of the bits
 * the literals then cost, switches included. */
static int split_literals(const float * tab, const gcomp_allocator_t * alloc,
    const uint8_t * base, size_t hist, const brotli_cmd_t * cmds, int ncmd,
    size_t nlit, brotli_blk_t * blocks, int * nblk, float * est_bits,
    uint8_t * seg_type_out) {
  size_t seg_len = nlit / 512u + 1u;
  size_t nseg;
  brotli_seg_t * seg = NULL;
  uint8_t (*back)[BROTLI_MAX_TYPES] = NULL;
  uint8_t * assign = NULL;
  uint8_t * best_assign = NULL;
  uint32_t cen[BROTLI_MAX_TYPES][256];
  uint32_t cen_n[BROTLI_MAX_TYPES];
  float lg[BROTLI_MAX_TYPES][256];
  float best_est = 0.0f;
  int best_k = 1;
  int k, it, c;
  size_t i;
  int result = 1;
  if (seg_len < 128u) {
    seg_len = 128u;
  }
  nseg = (nlit + seg_len - 1u) / seg_len;
  *nblk = 0;
  if (nseg < 4) {
    return 1;
  }
  seg = gcomp_calloc(alloc, nseg, sizeof(*seg));
  back = gcomp_malloc(alloc, nseg * sizeof(*back));
  assign = gcomp_calloc(alloc, nseg, 1);
  best_assign = gcomp_calloc(alloc, nseg, 1);
  if (!seg || !back || !assign || !best_assign) {
    result = 1;
    goto out;
  }
  {
    size_t idx = 0;
    for (c = 0; c < ncmd; c++) {
      uint32_t j;
      for (j = 0; j < cmds[c].insert; j++, idx++) {
        size_t sg = idx / seg_len;
        seg[sg].h[base[hist + cmds[c].at + j]]++;
        seg[sg].n++;
      }
    }
  }
  /* One type. */
  {
    uint32_t all[256];
    memset(all, 0, sizeof(all));
    for (i = 0; i < nseg; i++) {
      int q;
      for (q = 0; q < 256; q++) {
        all[q] += seg[i].h[q];
      }
    }
    best_est = hist_bits(tab, all, (uint32_t)nlit, NULL);
    memcpy(cen[0], all, sizeof(all));
    cen_n[0] = (uint32_t)nlit;
  }
  for (k = 2; k <= BROTLI_MAX_TYPES; k++) {
    /* Seed type k - 1 with the segment the current types fit worst. */
    int kk = k - 1;
    size_t worst = 0;
    float worst_cost = -1.0f;
    float est = 0.0f;
    int nsw = 0;
    for (it = 0; it < kk; it++) {
      int q;
      for (q = 0; q < 256; q++) {
        lg[it][q] = log2f(((float)cen[it][q] + 1.0f) /
                          ((float)cen_n[it] + 256.0f));
      }
    }
    for (i = 0; i < nseg; i++) {
      float cost = 1e30f;
      int t;
      for (t = 0; t < kk; t++) {
        float cc = 0.0f;
        int q;
        for (q = 0; q < 256; q++) {
          if (seg[i].h[q]) {
            cc -= (float)seg[i].h[q] * lg[t][q];
          }
        }
        if (cc < cost) {
          cost = cc;
        }
      }
      cost /= (float)seg[i].n;
      if (cost > worst_cost) {
        worst_cost = cost;
        worst = i;
      }
    }
    memcpy(cen[kk], seg[worst].h, sizeof(cen[kk]));
    cen_n[kk] = seg[worst].n;
    for (it = 0; it < 4; it++) {
      float dp[BROTLI_MAX_TYPES];
      float nd[BROTLI_MAX_TYPES];
      int t;
      for (t = 0; t < k; t++) {
        int q;
        for (q = 0; q < 256; q++) {
          lg[t][q] = log2f(((float)cen[t][q] + 1.0f) /
                           ((float)cen_n[t] + 256.0f));
        }
      }
      for (t = 0; t < k; t++) {
        dp[t] = 0.0f;
      }
      for (i = 0; i < nseg; i++) {
        float prev_best = dp[0];
        int prev_arg = 0;
        for (t = 1; t < k; t++) {
          if (dp[t] < prev_best) {
            prev_best = dp[t];
            prev_arg = t;
          }
        }
        for (t = 0; t < k; t++) {
          float cc = 0.0f;
          float stay = dp[t];
          float move = prev_best + BROTLI_SWITCH_BITS;
          int q;
          for (q = 0; q < 256; q++) {
            if (seg[i].h[q]) {
              cc -= (float)seg[i].h[q] * lg[t][q];
            }
          }
          if (i == 0) {
            nd[t] = cc + (t == 0 ? 0.0f : BROTLI_SWITCH_BITS);
            back[i][t] = 0;
          }
          else if (stay <= move) {
            nd[t] = stay + cc;
            back[i][t] = (uint8_t)t;
          }
          else {
            nd[t] = move + cc;
            back[i][t] = (uint8_t)prev_arg;
          }
        }
        for (t = 0; t < k; t++) {
          dp[t] = nd[t];
        }
      }
      {
        int arg = 0;
        for (t = 1; t < k; t++) {
          if (dp[t] < dp[arg]) {
            arg = t;
          }
        }
        for (i = nseg; i-- > 0;) {
          assign[i] = (uint8_t)arg;
          arg = back[i][arg];
        }
      }
      for (t = 0; t < k; t++) {
        memset(cen[t], 0, sizeof(cen[t]));
        cen_n[t] = 0;
      }
      for (i = 0; i < nseg; i++) {
        int q;
        for (q = 0; q < 256; q++) {
          cen[assign[i]][q] += seg[i].h[q];
        }
        cen_n[assign[i]] += seg[i].n;
      }
      /* A type nobody got keeps a segment's worth so the next round is
       * defined; it will be dropped below if it still has none. */
      for (t = 0; t < k; t++) {
        if (cen_n[t] == 0) {
          memcpy(cen[t], seg[worst].h, sizeof(cen[t]));
          cen_n[t] = seg[worst].n;
        }
      }
    }
    {
      int t;
      int used[BROTLI_MAX_TYPES];
      memset(used, 0, sizeof(used));
      for (i = 0; i < nseg; i++) {
        used[assign[i]] = 1;
        if (i > 0 && assign[i] != assign[i - 1]) {
          nsw++;
        }
      }
      for (t = 0; t < k; t++) {
        if (used[t]) {
          uint32_t h2[256];
          size_t q2;
          memset(h2, 0, sizeof(h2));
          for (q2 = 0; q2 < nseg; q2++) {
            if (assign[q2] == t) {
              int q;
              for (q = 0; q < 256; q++) {
                h2[q] += seg[q2].h[q];
              }
            }
          }
          {
            uint32_t tot = 0;
            int q;
            for (q = 0; q < 256; q++) {
              tot += h2[q];
            }
            est += hist_bits(tab, h2, tot, NULL);
          }
        }
      }
      est += (float)nsw * BROTLI_SWITCH_BITS + 40.0f;
    }
    if (est + 64.0f < best_est) {
      best_est = est;
      best_k = k;
      memcpy(best_assign, assign, nseg);
    }
    else {
      break;
    }
  }
  if (best_k < 2) {
    result = 1;
    goto out;
  }
  /* Renumber the types in order of first use, then make the blocks. */
  {
    int renum[BROTLI_MAX_TYPES];
    int next = 0;
    int nb = 0;
    for (c = 0; c < BROTLI_MAX_TYPES; c++) {
      renum[c] = -1;
    }
    for (i = 0; i < nseg; i++) {
      uint32_t len = (uint32_t)(i + 1 == nseg ? nlit - i * seg_len : seg_len);
      if (renum[best_assign[i]] < 0) {
        renum[best_assign[i]] = next++;
      }
      if (nb > 0 && blocks[nb - 1].type == renum[best_assign[i]]) {
        blocks[nb - 1].len += len;
      }
      else {
        blocks[nb].type = (uint8_t)renum[best_assign[i]];
        blocks[nb].len = len;
        nb++;
      }
    }
    *nblk = nb;
    *est_bits = best_est;
    if (seg_type_out) {
      (void)seg_type_out;
    }
    result = next;
    if (next < 2) {
      result = 1;
      *nblk = 0;
    }
  }
out:
  gcomp_free(alloc, seg);
  gcomp_free(alloc, back);
  gcomp_free(alloc, assign);
  gcomp_free(alloc, best_assign);
  return result;
}

/* The state the literal model needs for one block, in one allocation. */
typedef struct brotli_ctxwork_s {
  float tab[BROTLI_NLOGN_N];
  brotli_clu_t clu[BROTLI_NCTX];
} brotli_ctxwork_t;

/* How one block type's literals are coded: a context mode, and for each of
 * its 64 contexts the tree (numbered within the type) that codes it. */
typedef struct brotli_tplan_s {
  int mode;
  int k;
  uint8_t map[BROTLI_NCTX];
  float bits;
} brotli_tplan_t;

typedef uint32_t brotli_ctxh_t[4][BROTLI_NCTX][256];

/* The best of one tree for all the literals and each context mode clustered. */
static void plan_type(brotli_ctxwork_t * w, brotli_ctxh_t * h,
    brotli_tplan_t * out) {
  uint32_t flat[256];
  uint32_t total = 0;
  int m, i;
  memset(flat, 0, sizeof(flat));
  for (i = 0; i < BROTLI_NCTX; i++) {
    int q;
    for (q = 0; q < 256; q++) {
      flat[q] += (*h)[0][i][q];
    }
  }
  for (i = 0; i < 256; i++) {
    total += flat[i];
  }
  out->mode = 0;
  out->k = 1;
  memset(out->map, 0, sizeof(out->map));
  out->bits = total ? hist_bits(w->tab, flat, total, NULL) : 0.0f;
  if (total < 256u) {
    return;
  }
  /* Rank the four modes by what the contexts cost each with a code of its
   * own, which is cheap, and cluster only the best few, which is not. */
  {
    float rank[4];
    int order[4] = {0, 1, 2, 3};
    int a2, b2;
    for (m = 0; m < 4; m++) {
      float sum = 0.0f;
      for (i = 0; i < BROTLI_NCTX; i++) {
        uint32_t t2 = 0;
        int q;
        for (q = 0; q < 256; q++) {
          t2 += (*h)[m][i][q];
        }
        if (t2 != 0) {
          sum += hist_bits(w->tab, (*h)[m][i], t2, NULL);
        }
      }
      rank[m] = sum;
    }
    for (a2 = 0; a2 < 3; a2++) {
      for (b2 = a2 + 1; b2 < 4; b2++) {
        if (rank[order[b2]] < rank[order[a2]]) {
          int tmp = order[a2];
          order[a2] = order[b2];
          order[b2] = tmp;
        }
      }
    }
    for (a2 = 0; a2 < BROTLI_CLUSTER_MODES; a2++) {
      uint8_t mmap[BROTLI_NCTX];
      float bits = 0.0f;
      int k;
      m = order[a2];
      k = cluster_ctx(w->tab, w->clu, (const uint32_t (*)[256])(*h)[m], mmap,
          &bits);
      bits += 24.0f + 6.0f * (float)k;
      if (k >= 2 && bits < out->bits) {
        out->bits = bits;
        out->mode = m;
        out->k = k;
        memcpy(out->map, mmap, BROTLI_NCTX);
      }
    }
  }
}

/* A block type or count symbol to the stream. */
static int put_block_switch(brotli_bw_t * b, const brotli_canon_t * type_code,
    const brotli_canon_t * len_code, int tsym, uint32_t len) {
  int lc = 0;
  uint32_t le = 0;
  int ln = 0;
  if (blk_len_code(len, &lc, &le, &ln) != 0) {
    return -1;
  }
  if (type_code && put_canon(b, &type_code[tsym]) != 0) {
    return -1;
  }
  if (put_canon(b, &len_code[lc]) != 0) {
    return -1;
  }
  return brotli_bw_put(b, le, ln);
}

static int block_type_sym(int prev, int cur, int ntypes, int t) {
  if (t == cur + 1 || (cur + 1 == ntypes && t == 0)) {
    return 1;
  }
  if (t == prev) {
    return 0;
  }
  return t + 2;
}

static int emit_block(const gcomp_allocator_t * alloc, brotli_bw_t * b,
    const uint8_t * base, size_t hist, const brotli_cmd_t * cmds, int ncmd,
    size_t len, int level, uint32_t rb[4], int * fresh) {
  uint32_t lit_freq[256];
  uint32_t ic_freq[704];
  uint32_t dist_freq[64];
  brotli_canon_t * lit_code = NULL;
  brotli_canon_t * ic_code = NULL;
  brotli_canon_t * dist_code = NULL;
  brotli_ctxwork_t * w = NULL;
  brotli_ctxh_t * ch = NULL;     /* [type][mode][context][byte] counts */
  uint32_t (*cfreq)[256] = NULL;
  brotli_blk_t * blocks = NULL;
  brotli_tplan_t plan[BROTLI_MAX_TYPES];
  int toff[BROTLI_MAX_TYPES];
  uint8_t map[BROTLI_MAP_MAX];
  int imp[24][24];
  int exp[24][24];
  uint32_t origin[4];
  int origin_fresh = *fresh;
  const uint8_t * l0 = brotli_lut0();
  const uint8_t * l1 = brotli_lut1();
  const uint8_t * l2 = brotli_lut2();
  int ntrees = 1;
  int ntypes = 1;
  int nblocks = 0;
  int chone = 0;
  int ctx_on;
  size_t nlit = 0;
  int result = -1;
  int c;
  int i;
  memset(lit_freq, 0, sizeof(lit_freq));
  memset(ic_freq, 0, sizeof(ic_freq));
  memset(dist_freq, 0, sizeof(dist_freq));
  memset(map, 0, sizeof(map));
  memset(plan, 0, sizeof(plan));
  memset(toff, 0, sizeof(toff));
  memcpy(origin, rb, sizeof(origin));
  fill_ic(imp, exp);
  /* A literal's context is the two bytes before it, which after a full flush
   * a decoder that has been following along knows and one that has not does
   * not. Until the four absolute distances have been written the stream must
   * read the same either way, so there is no context model then. */
  ctx_on = level >= BROTLI_CTX_LEVEL && origin_fresh == 0;
  if (ctx_on) {
    w = gcomp_calloc(alloc, 1, sizeof(*w));
    if (w) {
      nlogn_init(w->tab);
    }
    else {
      ctx_on = 0;
    }
  }
  for (c = 0; c < ncmd; c++) {
    int ic = 0;
    int implicit = 0;
    int dist_sym = 0;
    uint32_t ins_extra = 0;
    uint32_t copy_extra = 0;
    uint32_t dist_extra = 0;
    int ins_n = 0;
    int copy_n = 0;
    int dist_n = 0;
    if (resolve_cmd(&cmds[c], rb, *fresh, imp, exp, &ic, &implicit, &dist_sym,
            &ins_extra, &ins_n, &copy_extra, &copy_n, &dist_extra,
            &dist_n) != 0) {
      goto out;
    }
    if (ic < 0 || ic >= 704) {
      goto out;
    }
    ic_freq[ic]++;
    for (i = 0; i < (int)cmds[c].insert; i++) {
      lit_freq[base[hist + cmds[c].at + (uint32_t)i]]++;
    }
    nlit += cmds[c].insert;
    if (!cmds[c].tail && !implicit) {
      if (dist_sym < 0 || dist_sym >= 64) {
        goto out;
      }
      dist_freq[dist_sym]++;
      if (dist_sym != 0) {
        push_dist(rb, cmds[c].dist);
      }
      if (*fresh > 0) {
        (*fresh)--;
      }
    }
  }
  if (literals_cannot_win(lit_freq, len)) {
    result = 1;
    goto out;
  }
  /* Choose how the literals are modelled: one code for all of them; a code for
   * each class of context (the two bytes before it); a code for each run of
   * literals that look alike (block types); or both. Each is priced and the
   * cheapest taken, if it beats one code by more than saying so costs. */
  if (ctx_on && nlit >= 1024u) {
    float base_bits = hist_bits(w->tab, lit_freq, (uint32_t)nlit, NULL);
    float est1;
    float estn = 1e30f;
    brotli_tplan_t plan1;
    int st = 1;
    int sn = 0;
    float sbits = 0.0f;
    brotli_blk_t * sb = NULL;
    brotli_ctxh_t * c1;
    int t, m, q, k;
    if (level >= BROTLI_SPLIT_LEVEL) {
      sb = gcomp_malloc(alloc, (nlit / 128u + 8u) * sizeof(*sb));
      if (sb) {
        st = split_literals(w->tab, alloc, base, hist, cmds, ncmd, nlit, sb,
            &sn, &sbits, NULL);
        if (st < 2) {
          gcomp_free(alloc, sb);
          sb = NULL;
          st = 1;
        }
      }
    }
    ch = gcomp_calloc(alloc, (size_t)st + 1u, sizeof(*ch));
    if (!ch) {
      gcomp_free(alloc, sb);
      result = -2;
      goto out;
    }
    chone = st;
    c1 = &ch[st];
    {
      int bi = 0;
      uint32_t left = st >= 2 ? sb[0].len : 0;
      for (c = 0; c < ncmd; c++) {
        uint32_t j;
        for (j = 0; j < cmds[c].insert; j++) {
          size_t pos = hist + cmds[c].at + j;
          unsigned p1 = pos >= 1 ? base[pos - 1] : 0;
          unsigned p2 = pos >= 2 ? base[pos - 2] : 0;
          uint8_t byte = base[pos];
          int ty = 0;
          if (st >= 2) {
            if (left == 0) {
              bi++;
              left = sb[bi].len;
            }
            left--;
            ty = sb[bi].type;
          }
          for (m = 0; m < 4; m++) {
            unsigned cx = ctx_id(m, p1, p2, l0, l1, l2);
            if (st >= 2) {
              ch[ty][m][cx][byte]++;
            }
            (*c1)[m][cx][byte]++;
          }
        }
      }
    }
    plan_type(w, c1, &plan1);
    est1 = plan1.bits;
    if (st >= 2) {
      float sum = 0.0f;
      int total_trees = 0;
      for (t = 0; t < st; t++) {
        plan_type(w, &ch[t], &plan[t]);
        sum += plan[t].bits;
        total_trees += plan[t].k;
      }
      estn = sum + (float)sn * BROTLI_SWITCH_BITS + 60.0f +
             8.0f * (float)st;
      if (total_trees > 256) {
        estn = 1e30f;
      }
    }
    if (estn < est1 && estn + 32.0f < base_bits) {
      ntypes = st;
      nblocks = sn;
      blocks = sb;
      sb = NULL;
      for (t = 0, k = 0; t < ntypes; t++) {
        toff[t] = k;
        for (q = 0; q < BROTLI_NCTX; q++) {
          map[t * BROTLI_NCTX + q] = (uint8_t)(toff[t] + plan[t].map[q]);
        }
        k += plan[t].k;
      }
      ntrees = k;
    }
    else if (est1 + 32.0f < base_bits) {
      plan[0] = plan1;
      memcpy(map, plan1.map, BROTLI_NCTX);
      ntrees = plan1.k;
    }
    gcomp_free(alloc, sb);
  }
  lit_code = gcomp_calloc(alloc, (size_t)ntrees * 256, sizeof(*lit_code));
  ic_code = gcomp_calloc(alloc, 704, sizeof(*ic_code));
  dist_code = gcomp_calloc(alloc, 64, sizeof(*dist_code));
  cfreq = gcomp_calloc(alloc, (size_t)ntrees, sizeof(*cfreq));
  if (!lit_code || !ic_code || !dist_code || !cfreq) {
    result = -2;
    goto out;
  }
  if (ntrees == 1) {
    memcpy(cfreq[0], lit_freq, sizeof(lit_freq));
  }
  else if (ntypes >= 2) {
    int t, q, z;
    for (t = 0; t < ntypes; t++) {
      for (q = 0; q < BROTLI_NCTX; q++) {
        for (z = 0; z < 256; z++) {
          cfreq[map[t * BROTLI_NCTX + q]][z] += ch[t][plan[t].mode][q][z];
        }
      }
    }
  }
  else {
    int q, z;
    for (q = 0; q < BROTLI_NCTX; q++) {
      for (z = 0; z < 256; z++) {
        cfreq[map[q]][z] += ch[chone][plan[0].mode][q][z];
      }
    }
  }
  /* The header: the literal block types (a count, and when there are several
   * the codes for their switches and the first count), one type of each of
   * the others, no distance postfix or direct codes, the literal context mode
   * of each type, the literal trees' count and map, and one distance tree. */
  {
    brotli_canon_t type_code[BROTLI_MAX_TYPES + 2];
    brotli_canon_t len_code[26];
    if (put_count(b, ntypes) != 0) {
      goto out;
    }
    if (ntypes >= 2) {
      uint32_t tf[BROTLI_MAX_TYPES + 2];
      uint32_t lf[26];
      int prev = 1, cur = 0;
      int lc = 0, ln = 0;
      uint32_t le = 0;
      memset(tf, 0, sizeof(tf));
      memset(lf, 0, sizeof(lf));
      for (i = 0; i < nblocks; i++) {
        if (blk_len_code(blocks[i].len, &lc, &le, &ln) != 0) {
          goto out;
        }
        lf[lc]++;
        if (i > 0) {
          tf[block_type_sym(prev, cur, ntypes, blocks[i].type)]++;
          prev = cur;
          cur = blocks[i].type;
        }
      }
      if (write_prefix(alloc, b, tf, ntypes + 2, type_code) != 0 ||
          write_prefix(alloc, b, lf, 26, len_code) != 0) {
        goto out;
      }
      if (blk_len_code(blocks[0].len, &lc, &le, &ln) != 0 ||
          put_canon(b, &len_code[lc]) != 0 || brotli_bw_put(b, le, ln) != 0) {
        goto out;
      }
    }
    if (brotli_bw_put(b, 0, 1) != 0 || brotli_bw_put(b, 0, 1) != 0 ||
        brotli_bw_put(b, 0, 2) != 0 || brotli_bw_put(b, 0, 4) != 0) {
      goto out;
    }
    for (i = 0; i < ntypes; i++) {
      if (brotli_bw_put(b, (uint32_t)plan[i].mode, 2) != 0) {
        goto out;
      }
    }
    if (put_count(b, ntrees) != 0) {
      goto out;
    }
    if (ntrees >= 2 &&
        write_cmap(w->tab, alloc, b, map, BROTLI_NCTX * ntypes, ntrees) != 0) {
      goto out;
    }
    if (brotli_bw_put(b, 0, 1) != 0) {
      goto out;
    }
    for (i = 0; i < ntrees; i++) {
      if (write_prefix(alloc, b, cfreq[i], 256, lit_code + (size_t)i * 256) != 0) {
        goto out;
      }
    }
    if (write_prefix(alloc, b, ic_freq, 704, ic_code) != 0 ||
        write_prefix(alloc, b, dist_freq, 64, dist_code) != 0) {
      goto out;
    }
    memcpy(rb, origin, sizeof(origin));
    *fresh = origin_fresh;
    {
      int bi = 0;
      uint32_t left = ntypes >= 2 ? blocks[0].len : 0;
      int prev = 1, cur = 0;
      for (c = 0; c < ncmd; c++) {
        int ic = 0;
        int implicit = 0;
        int dist_sym = 0;
        uint32_t ins_extra = 0;
        uint32_t copy_extra = 0;
        uint32_t dist_extra = 0;
        int ins_n = 0;
        int copy_n = 0;
        int dist_n = 0;
        if (resolve_cmd(&cmds[c], rb, *fresh, imp, exp, &ic, &implicit,
                &dist_sym, &ins_extra, &ins_n, &copy_extra, &copy_n,
                &dist_extra, &dist_n) != 0) {
          goto out;
        }
        if (put_canon(b, &ic_code[ic]) != 0 ||
            brotli_bw_put(b, ins_extra, ins_n) != 0 ||
            brotli_bw_put(b, copy_extra, copy_n) != 0) {
          goto out;
        }
        for (i = 0; i < (int)cmds[c].insert; i++) {
          size_t pos = hist + cmds[c].at + (uint32_t)i;
          uint8_t byte = base[pos];
          const brotli_canon_t * code = lit_code;
          int ty = 0;
          if (ntypes >= 2) {
            if (left == 0) {
              bi++;
              if (put_block_switch(b, type_code, len_code,
                      block_type_sym(prev, cur, ntypes, blocks[bi].type),
                      blocks[bi].len) != 0) {
                goto out;
              }
              prev = cur;
              cur = blocks[bi].type;
              left = blocks[bi].len;
            }
            left--;
            ty = blocks[bi].type;
          }
          if (ntrees > 1) {
            unsigned p1 = pos >= 1 ? base[pos - 1] : 0;
            unsigned p2 = pos >= 2 ? base[pos - 2] : 0;
            code = lit_code + (size_t)map[ty * BROTLI_NCTX +
                ctx_id(plan[ty].mode, p1, p2, l0, l1, l2)] * 256;
          }
          if (put_canon(b, &code[byte]) != 0) {
            goto out;
          }
        }
        if (!cmds[c].tail && !implicit) {
          if (put_canon(b, &dist_code[dist_sym]) != 0 ||
              brotli_bw_put(b, dist_extra, dist_n) != 0) {
            goto out;
          }
          if (dist_sym != 0) {
            push_dist(rb, cmds[c].dist);
          }
          if (*fresh > 0) {
            (*fresh)--;
          }
        }
      }
    }
  }
  result = 0;
out:
  gcomp_free(alloc, lit_code);
  gcomp_free(alloc, ic_code);
  gcomp_free(alloc, dist_code);
  gcomp_free(alloc, cfreq);
  gcomp_free(alloc, blocks);
  gcomp_free(alloc, ch);
  gcomp_free(alloc, w);
  return result;
}

/* One way of coding a chunk's commands: 0 with the bytes it takes and where it
 * leaves the decoder's distances, 1 when it is no smaller than storing, -1 on
 * allocation failure. */
static int try_emit(const gcomp_allocator_t * alloc, uint8_t * buf, size_t cap,
    const brotli_cmd_t * cmds, int ncmd, const uint8_t * base, size_t hist,
    size_t len, int level, const uint32_t dist_rb[4], int rb_fresh,
    size_t stored, size_t * n, uint32_t rb_out[4], int * fresh_out) {
  brotli_bw_t bw;
  int er;
  memset(&bw, 0, sizeof(bw));
  bw.buf = buf;
  bw.cap = cap < stored ? cap : stored;
  memcpy(rb_out, dist_rb, 4u * sizeof(uint32_t));
  *fresh_out = rb_fresh;
  if (write_mlen(&bw, len) != 0) {
    return 1;
  }
  er = emit_block(alloc, &bw, base, hist, cmds, ncmd, len, level, rb_out,
      fresh_out);
  if (er != 0) {
    return er == -2 ? -1 : 1;
  }
  if (write_meta_align(&bw) != 0 || bw.nbits != 0 || bw.len >= stored) {
    return 1;
  }
  *n = bw.len;
  return 0;
}

int brotli_compress_chunk_h(const gcomp_allocator_t * alloc, uint8_t * dst,
    size_t dst_cap, size_t * out_n, const uint8_t * base, size_t hist,
    size_t len, uint32_t window, int level, uint32_t dist_rb[4],
    int * rb_fresh, uint32_t * head, uint32_t * prev) {
  brotli_params_t par;
  brotli_cmd_t * cmds[BROTLI_OPT_VARIANTS + 1] = {NULL};
  int ncmd[BROTLI_OPT_VARIANTS + 1] = {0};
  size_t stored;
  size_t best_n = 0;
  uint32_t best_rb[4];
  int best_fresh = 0;
  int have = 0;
  int result = 1;
  int pr;
  int i;
  uint8_t * scratch = NULL;
  const uint8_t * data = base ? base + hist : NULL;
  /* Level 1 takes the first match worth having; 2 defers one that the next
   * position beats; 3 and 4 look through more of the chain; 5 finds the
   * cheapest way through the chunk. */
  switch (level) {
  case 1:
    par.chain = 16;
    par.nice = 128;
    par.lazy = 0;
    break;
  case 2:
    par.chain = 16;
    par.nice = 128;
    par.lazy = 1;
    break;
  case 3:
    par.chain = 48;
    par.nice = 256;
    par.lazy = 1;
    break;
  default:
    par.chain = 128;
    par.nice = 256;
    par.lazy = 1;
    break;
  }
  if (!dst || !out_n || !data || !dist_rb || !rb_fresh || len == 0
      || len > 16777216u || !head || !prev) {
    return 1;
  }
  memcpy(par.rb, dist_rb, sizeof(par.rb));
  stored = len + 3u * ((len + 65535u) / 65536u);
  if (dst_cap < 8) {
    return 1;
  }
  if (level >= 5) {
    /* The lazy parse of level 4 is a second candidate, and the smaller of the
     * two codings is kept: the prices the cheapest parse works from are
     * estimates, and on some data (records with random fields) they lead it
     * somewhere the lazy parse does not go. Both fill the match finder, so it
     * is put back between them. */
    uint32_t * saved = gcomp_malloc(alloc, BROTLI_HASH_SIZE * sizeof(*saved));
    if (!saved) {
      return -1;
    }
    memcpy(saved, head, BROTLI_HASH_SIZE * sizeof(*saved));
    pr = parse_cmds(
        alloc, base, hist, len, window, &par, head, prev,
        &cmds[BROTLI_OPT_VARIANTS], &ncmd[BROTLI_OPT_VARIANTS]);
    memcpy(head, saved, BROTLI_HASH_SIZE * sizeof(*saved));
    gcomp_free(alloc, saved);
    if (pr < 0) {
      return pr;
    }
    pr = parse_opt(alloc, base, hist, len, window, &par, head, prev, cmds, ncmd);
    if (pr < 0) {
      gcomp_free(alloc, cmds[BROTLI_OPT_VARIANTS]);
      return pr;
    }
    scratch = gcomp_malloc(alloc, dst_cap);
    if (!scratch) {
      result = -1;
      goto out;
    }
  }
  else {
    pr = parse_cmds(
        alloc, base, hist, len, window, &par, head, prev, &cmds[0], &ncmd[0]);
    if (pr != 0) {
      return pr;
    }
  }
  for (i = 0; i <= BROTLI_OPT_VARIANTS; i++) {
    size_t n = 0;
    uint32_t rb[4];
    int fresh = 0;
    int r;
    if (!cmds[i]) {
      continue;
    }
    r = try_emit(alloc, i == 0 ? dst : scratch, dst_cap, cmds[i], ncmd[i], base,
        hist, len, level, dist_rb, *rb_fresh, stored, &n, rb, &fresh);
    if (r < 0) {
      result = -1;
      goto out;
    }
    if (r == 0 && (!have || n < best_n)) {
      if (i != 0) {
        memcpy(dst, scratch, n);
      }
      have = 1;
      best_n = n;
      memcpy(best_rb, rb, sizeof(best_rb));
      best_fresh = fresh;
    }
  }
  if (have) {
    memcpy(dist_rb, best_rb, sizeof(best_rb));
    *rb_fresh = best_fresh;
    *out_n = best_n;
    result = 0;
  }
out:
  gcomp_free(alloc, scratch);
  for (i = 0; i <= BROTLI_OPT_VARIANTS; i++) {
    gcomp_free(alloc, cmds[i]);
  }
  return result;
}

int brotli_compress_chunk(const gcomp_allocator_t * alloc, uint8_t * dst,
    size_t dst_cap, size_t * out_n, const uint8_t * data, size_t len,
    uint32_t window, uint32_t dist_rb[4], int * rb_fresh) {
  uint32_t * head;
  uint32_t * prev;
  int r;
  if (!data || len == 0 || len > 16777216u) {
    return 1;
  }
  head = gcomp_calloc(alloc, BROTLI_HASH_SIZE, sizeof(*head));
  prev = gcomp_calloc(alloc, len, sizeof(*prev));
  if (!head || !prev) {
    gcomp_free(alloc, head);
    gcomp_free(alloc, prev);
    return -1;
  }
  r = brotli_compress_chunk_h(alloc, dst, dst_cap, out_n, data, 0, len, window,
      1, dist_rb, rb_fresh, head, prev);
  gcomp_free(alloc, head);
  gcomp_free(alloc, prev);
  return r;
}
