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

#include <string.h>

#define BROTLI_HASH_BITS 15
#define BROTLI_HASH_SIZE (1u << BROTLI_HASH_BITS)
#define BROTLI_CHAIN 128
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
      len += (size_t)(__builtin_ctzll(diff) / 8u);
      return len;
    }
    len += 8u;
  }
  while (len < max && data[from + len] == data[pos + len]) {
    len++;
  }
  return len;
}

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

static void find_match(const uint32_t * head, const uint32_t * prev,
    const uint8_t * data, size_t n, size_t pos, uint32_t window, size_t * mlen,
    size_t * mdist) {
  uint32_t cur;
  int steps = 0;
  *mlen = 0;
  *mdist = 0;
  if (pos + BROTLI_MIN_MATCH > n) {
    return;
  }
  cur = head[hash4(data + pos)];
  while (cur != 0 && steps < BROTLI_CHAIN) {
    size_t from = (size_t)cur - 1u;
    size_t dist;
    size_t len;
    steps++;
    if (from >= pos) {
      break;
    }
    dist = pos - from;
    cur = prev[from];
    if (dist == 0 || dist > window || dist > pos) {
      continue;
    }
    len = match_len(data, n, from, pos);
    if (len >= BROTLI_MIN_MATCH &&
        (len > *mlen || (len == *mlen && dist < *mdist))) {
      *mlen = len;
      *mdist = dist;
    }
  }
}

static int parse_cmds(const gcomp_allocator_t * alloc, const uint8_t * data,
    size_t len, uint32_t window, brotli_cmd_t ** cmds_out, int * ncmd_out) {
  uint32_t * head;
  uint32_t * prev;
  brotli_cmd_t * cmds;
  size_t cap;
  size_t pos = 0;
  size_t lit = 0;
  size_t lit_at = 0;
  int ncmd = 0;
  int misses = 0;
  int ok = 0;
  head = gcomp_calloc(alloc, BROTLI_HASH_SIZE, sizeof(*head));
  prev = gcomp_calloc(alloc, len, sizeof(*prev));
  cap = len / BROTLI_MIN_MATCH + 2u;
  cmds = gcomp_malloc(alloc, cap * sizeof(*cmds));
  if (!head || !prev || !cmds) {
    gcomp_free(alloc, head);
    gcomp_free(alloc, prev);
    gcomp_free(alloc, cmds);
    return -1;
  }
  while (pos < len) {
    size_t mlen = 0;
    size_t mdist = 0;
    size_t step;
    find_match(head, prev, data, len, pos, window, &mlen, &mdist);
    note_pos(head, prev, data, len, pos);
    if (mlen >= BROTLI_MIN_MATCH) {
      size_t end = pos + mlen;
      size_t k;
      if ((size_t)ncmd >= cap) {
        goto done;
      }
      misses = 0;
      /* A match that ends the chunk is never searched again, so the
       * positions inside it do not belong in the chain. A long match
       * that does not end the chunk is entered every fourth byte, plus
       * its last four, which is what the next search can still reach. */
      if (end < len && mlen > 64u) {
        size_t tail = end - 4u;
        for (k = pos + 4u; k < tail; k += 4u) {
          note_pos(head, prev, data, len, k);
        }
        for (k = tail; k < end; k++) {
          note_pos(head, prev, data, len, k);
        }
      }
      else if (end < len) {
        for (k = pos + 1u; k < end; k++) {
          note_pos(head, prev, data, len, k);
        }
      }
      cmds[ncmd].at = (uint32_t)lit_at;
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
      if (step > len - pos) {
        step = len - pos;
      }
      lit += step;
      pos += step;
    }
  }
  if (lit != 0) {
    if ((size_t)ncmd >= cap) {
      goto done;
    }
    cmds[ncmd].at = (uint32_t)lit_at;
    cmds[ncmd].insert = (uint32_t)lit;
    cmds[ncmd].copy = 0;
    cmds[ncmd].dist = 0;
    cmds[ncmd].tail = 1;
    ncmd++;
  }
  {
    size_t p = 0;
    int c;
    for (c = 0; c < ncmd; c++) {
      size_t i;
      if (cmds[c].at != p) {
        goto done;
      }
      p += cmds[c].insert;
      if (p > len) {
        goto done;
      }
      if (cmds[c].tail) {
        continue;
      }
      if (cmds[c].dist == 0 || cmds[c].dist > p || cmds[c].dist > window) {
        goto done;
      }
      if (p + cmds[c].copy > len) {
        goto done;
      }
      i = 0;
      while (i + 8u <= cmds[c].copy) {
        uint64_t a;
        uint64_t b;
        memcpy(&a, data + p + i, sizeof(a));
        memcpy(&b, data + (p - cmds[c].dist) + i, sizeof(b));
        if (a != b) {
          goto done;
        }
        i += 8u;
      }
      while (i < cmds[c].copy) {
        if (data[p + i] != data[(p - cmds[c].dist) + i]) {
          goto done;
        }
        i++;
      }
      p += cmds[c].copy;
    }
    if (p != len || ncmd == 0) {
      goto done;
    }
  }
  ok = 1;
done:
  gcomp_free(alloc, head);
  gcomp_free(alloc, prev);
  if (!ok) {
    gcomp_free(alloc, cmds);
    return 1;
  }
  *cmds_out = cmds;
  *ncmd_out = ncmd;
  return 0;
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

static int emit_block(const gcomp_allocator_t * alloc, brotli_bw_t * b,
    const uint8_t * data, const brotli_cmd_t * cmds, int ncmd, size_t len,
    uint32_t rb[4], int * fresh) {
  uint32_t lit_freq[256];
  uint32_t ic_freq[704];
  uint32_t dist_freq[64];
  brotli_canon_t * lit_code;
  brotli_canon_t * ic_code;
  brotli_canon_t * dist_code;
  int imp[24][24];
  int exp[24][24];
  uint32_t origin[4];
  int origin_fresh = *fresh;
  int c;
  int i;
  memset(lit_freq, 0, sizeof(lit_freq));
  memset(ic_freq, 0, sizeof(ic_freq));
  memset(dist_freq, 0, sizeof(dist_freq));
  memcpy(origin, rb, sizeof(origin));
  fill_ic(imp, exp);
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
      return -1;
    }
    if (ic < 0 || ic >= 704) {
      return -1;
    }
    ic_freq[ic]++;
    for (i = 0; i < (int)cmds[c].insert; i++) {
      lit_freq[data[cmds[c].at + (uint32_t)i]]++;
    }
    if (!cmds[c].tail && !implicit) {
      if (dist_sym < 0 || dist_sym >= 64) {
        return -1;
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
    return 1;
  }
  lit_code = gcomp_calloc(alloc, 256, sizeof(*lit_code));
  ic_code = gcomp_calloc(alloc, 704, sizeof(*ic_code));
  dist_code = gcomp_calloc(alloc, 64, sizeof(*dist_code));
  if (!lit_code || !ic_code || !dist_code) {
    gcomp_free(alloc, lit_code);
    gcomp_free(alloc, ic_code);
    gcomp_free(alloc, dist_code);
    return -1;
  }
  if (write_prefix(alloc, b, lit_freq, 256, lit_code) != 0 ||
      write_prefix(alloc, b, ic_freq, 704, ic_code) != 0 ||
      write_prefix(alloc, b, dist_freq, 64, dist_code) != 0) {
    gcomp_free(alloc, lit_code);
    gcomp_free(alloc, ic_code);
    gcomp_free(alloc, dist_code);
    return -1;
  }
  memcpy(rb, origin, sizeof(origin));
  *fresh = origin_fresh;
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
      gcomp_free(alloc, lit_code);
      gcomp_free(alloc, ic_code);
      gcomp_free(alloc, dist_code);
      return -1;
    }
    if (put_canon(b, &ic_code[ic]) != 0 || brotli_bw_put(b, ins_extra, ins_n) != 0 ||
        brotli_bw_put(b, copy_extra, copy_n) != 0) {
      gcomp_free(alloc, lit_code);
      gcomp_free(alloc, ic_code);
      gcomp_free(alloc, dist_code);
      return -1;
    }
    for (i = 0; i < (int)cmds[c].insert; i++) {
      uint8_t byte = data[cmds[c].at + (uint32_t)i];
      if (put_canon(b, &lit_code[byte]) != 0) {
        gcomp_free(alloc, lit_code);
        gcomp_free(alloc, ic_code);
        gcomp_free(alloc, dist_code);
        return -1;
      }
    }
    if (!cmds[c].tail && !implicit) {
      if (put_canon(b, &dist_code[dist_sym]) != 0 ||
          brotli_bw_put(b, dist_extra, dist_n) != 0) {
        gcomp_free(alloc, lit_code);
        gcomp_free(alloc, ic_code);
        gcomp_free(alloc, dist_code);
        return -1;
      }
      if (dist_sym != 0) {
        push_dist(rb, cmds[c].dist);
      }
      if (*fresh > 0) {
        (*fresh)--;
      }
    }
  }
  gcomp_free(alloc, lit_code);
  gcomp_free(alloc, ic_code);
  gcomp_free(alloc, dist_code);
  return 0;
}

int brotli_compress_chunk(const gcomp_allocator_t * alloc, uint8_t * dst,
    size_t dst_cap, size_t * out_n, const uint8_t * data, size_t len,
    uint32_t window, uint32_t dist_rb[4], int * rb_fresh) {
  brotli_cmd_t * cmds = NULL;
  brotli_bw_t bw;
  uint32_t rb[4];
  int fresh;
  size_t stored;
  int ncmd = 0;
  int pr;
  if (!dst || !out_n || !data || !dist_rb || !rb_fresh || len == 0
      || len > 16777216u) {
    return 1;
  }
  stored = len + 3u * ((len + 65535u) / 65536u);
  if (dst_cap < 8) {
    return 1;
  }
  pr = parse_cmds(alloc, data, len, window, &cmds, &ncmd);
  if (pr != 0) {
    return pr;
  }
  memset(&bw, 0, sizeof(bw));
  bw.buf = dst;
  bw.cap = dst_cap < stored ? dst_cap : stored;
  memcpy(rb, dist_rb, sizeof(rb));
  fresh = *rb_fresh;
  if (write_mlen(&bw, len) != 0 || brotli_bw_put(&bw, 0, 1) != 0 ||
      brotli_bw_put(&bw, 0, 1) != 0 || brotli_bw_put(&bw, 0, 1) != 0 ||
      brotli_bw_put(&bw, 0, 2) != 0 || brotli_bw_put(&bw, 0, 4) != 0 ||
      brotli_bw_put(&bw, 0, 2) != 0 || brotli_bw_put(&bw, 0, 1) != 0 ||
      brotli_bw_put(&bw, 0, 1) != 0) {
    gcomp_free(alloc, cmds);
    return 1;
  }
  if (emit_block(alloc, &bw, data, cmds, ncmd, len, rb, &fresh) != 0) {
    gcomp_free(alloc, cmds);
    return 1;
  }
  gcomp_free(alloc, cmds);
  if (write_meta_align(&bw) != 0 || bw.nbits != 0 || bw.len >= stored) {
    return 1;
  }
  memcpy(dist_rb, rb, sizeof(rb));
  *rb_fresh = fresh;
  *out_n = bw.len;
  return 0;
}
