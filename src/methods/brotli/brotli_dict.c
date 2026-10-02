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
 * @file brotli_dict.c
 *
 * Static dictionary words, RFC 7932 section 8 and Appendix B.
 *
 * A distance past the bytes produced so far names a dictionary word. The copy
 * length selects the word length (4..24) and the distance selects the word
 * and one of 121 transforms. The transformed string is what counts toward the
 * meta-block length, and it may be shorter or longer than the copy length.
 */

#include <ghoti.io/compress/macros.h>

#include "brotli_internal.h"

#include <string.h>

static int ferment_at(uint8_t * word, int word_len, int pos) {
  if (word[pos] < 192) {
    if (word[pos] >= 97 && word[pos] <= 122) {
      word[pos] = (uint8_t)(word[pos] ^ 32);
    }
    return 1;
  }
  if (word[pos] < 224) {
    if (pos + 1 < word_len) {
      word[pos + 1] = (uint8_t)(word[pos + 1] ^ 32);
    }
    return 2;
  }
  if (pos + 2 < word_len) {
    word[pos + 2] = (uint8_t)(word[pos + 2] ^ 5);
  }
  return 3;
}

int brotli_dict_word(int length, uint64_t word_id, uint8_t * dst, int dst_cap,
    int * out_len) {
  const uint8_t * ndbits;
  const uint32_t * doff;
  const uint8_t * dict;
  const brotli_xform_t * xf;
  const uint8_t * blob;
  uint32_t nwords;
  uint32_t index;
  uint64_t transform;
  uint32_t offset;
  uint8_t word[24];
  int word_len;
  int cut;
  const uint8_t * piece;
  int piece_len;
  int total;

  if (length < 4 || length > 24) {
    return -1;
  }
  ndbits = brotli_dict_ndbits();
  doff = brotli_dict_offset();
  dict = brotli_dict_data();
  nwords = 1u << ndbits[length];
  index = (uint32_t)(word_id % nwords);
  transform = word_id >> ndbits[length];
  if (transform > 120) {
    return -1;
  }
  offset = doff[length] + index * (uint32_t)length;
  if ((uint64_t)offset + (uint64_t)length > BROTLI_DICT_SIZE) {
    return -1;
  }
  memcpy(word, dict + offset, (size_t)length);
  word_len = length;
  xf = brotli_xform(transform);
  if (!xf) {
    return -1;
  }
  piece = word;
  piece_len = word_len;
  if (xf->kind == 1) {
    if (word_len > 0) {
      ferment_at(word, word_len, 0);
    }
  }
  else if (xf->kind == 2) {
    int i = 0;
    while (i < word_len) {
      i += ferment_at(word, word_len, i);
    }
  }
  else if (xf->kind >= 3 && xf->kind <= 11) {
    cut = xf->kind - 2;
    if (word_len <= cut) {
      piece_len = 0;
    }
    else {
      piece = word + cut;
      piece_len = word_len - cut;
    }
  }
  else if (xf->kind >= 12 && xf->kind <= 20) {
    cut = xf->kind - 11;
    if (word_len <= cut) {
      piece_len = 0;
    }
    else {
      piece_len = word_len - cut;
    }
  }
  blob = brotli_xform_blob();
  total = (int)xf->prefix_len + piece_len + (int)xf->suffix_len;
  if (total > dst_cap) {
    return -1;
  }
  if (xf->prefix_len) {
    memcpy(dst, blob + xf->prefix_off, xf->prefix_len);
  }
  if (piece_len) {
    memcpy(dst + xf->prefix_len, piece, (size_t)piece_len);
  }
  if (xf->suffix_len) {
    memcpy(dst + xf->prefix_len + piece_len, blob + xf->suffix_off,
        xf->suffix_len);
  }
  *out_len = total;
  return 0;
}
