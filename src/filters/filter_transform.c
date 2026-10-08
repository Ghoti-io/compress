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
 * @file filter_transform.c
 *
 * Delta and the branch converters. See filter_transform.h.
 *
 * Every converter walks the buffer at its instruction's alignment, rewrites
 * the instructions that are branches, and returns how far it got. They share
 * one shape: `dest = encoding ? pos + src : src - pos`, where `pos` is the
 * address the branch sits at (or the one after it, as the architecture counts
 * it). Applying the decoding to the encoded value undoes the encoding because
 * the two are exact inverses modulo the field's width; the rest of each
 * function is finding the field and putting the result back.
 */

#include <ghoti.io/compress/macros.h>

#include "filter_transform.h"

#include <string.h>

/* ---- delta -------------------------------------------------------------- */

void filter_delta_init(filter_delta_state_t * st) {
  memset(st, 0, sizeof(*st));
}

void filter_delta_code(filter_delta_state_t * st, unsigned dist, int encoding,
    uint8_t * buf, size_t size) {
  size_t i;
  uint8_t pos = st->pos;
  for (i = 0; i < size; i++) {
    const uint8_t back = st->hist[(uint8_t)(pos + dist)];
    const uint8_t cur = buf[i];
    if (encoding) {
      buf[i] = (uint8_t)(cur - back);
      st->hist[pos] = cur;
    }
    else {
      const uint8_t plain = (uint8_t)(cur + back);
      buf[i] = plain;
      st->hist[pos] = plain;
    }
    pos--;
  }
  st->pos = pos;
}

/* ---- branch converters -------------------------------------------------- */

void filter_bcj_init(filter_bcj_state_t * st) {
  st->prev_mask = 0;
  st->prev_pos = (uint32_t)-5;
}

int filter_bcj_known(unsigned arch) {
  switch (arch) {
  case FILTER_ARCH_X86:
  case FILTER_ARCH_POWERPC:
  case FILTER_ARCH_IA64:
  case FILTER_ARCH_ARM:
  case FILTER_ARCH_ARMTHUMB:
  case FILTER_ARCH_SPARC:
  case FILTER_ARCH_ARM64:
    return 1;
  default:
    return 0;
  }
}

static inline uint32_t get_le32(const uint8_t * p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
      (uint32_t)p[3] << 24;
}

static inline void put_le32(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t get_be32(const uint8_t * p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
      (uint32_t)p[3];
}

static inline void put_be32(uint8_t * p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* x86: a call (E8) or jump (E9) carries a 32-bit displacement whose top byte
 * is nearly always 00 or FF. Only those are converted, which keeps ordinary
 * data that happens to contain E8 alone, and `prev_mask` remembers the recent
 * E8/E9 bytes that were passed over so that a displacement is not taken for
 * an opcode and the other way round. This follows xz's converter exactly,
 * since the state has to agree for the stream to be readable. */
static size_t bcj_x86(filter_bcj_state_t * st, uint32_t now_pos, int encoding,
    uint8_t * buf, size_t size) {
  static const uint8_t allowed[8] = {1, 1, 1, 0, 1, 0, 0, 0};
  static const uint8_t bit_number[8] = {0, 1, 2, 2, 3, 3, 3, 3};
  uint32_t prev_mask = st->prev_mask;
  uint32_t prev_pos = st->prev_pos;
  size_t pos = 0, limit;
  if (size < 5) {
    return 0;
  }
  if (now_pos - prev_pos > 5) {
    prev_pos = now_pos - 5;
  }
  limit = size - 5;
  while (pos <= limit) {
    uint8_t b = buf[pos];
    uint32_t offset;
    if (b != 0xE8 && b != 0xE9) {
      pos++;
      continue;
    }
    offset = now_pos + (uint32_t)pos - prev_pos;
    prev_pos = now_pos + (uint32_t)pos;
    if (offset > 5) {
      prev_mask = 0;
    }
    else {
      uint32_t i;
      for (i = 0; i < offset; i++) {
        prev_mask &= 0x77;
        prev_mask <<= 1;
      }
    }
    b = buf[pos + 4];
    if ((b == 0x00 || b == 0xFF) && allowed[(prev_mask >> 1) & 7] &&
        (prev_mask >> 1) < 0x10) {
      uint32_t src = (uint32_t)b << 24 | (uint32_t)buf[pos + 3] << 16 |
          (uint32_t)buf[pos + 2] << 8 | (uint32_t)buf[pos + 1];
      uint32_t dest;
      for (;;) {
        if (encoding) {
          dest = src + (now_pos + (uint32_t)pos + 5);
        }
        else {
          dest = src - (now_pos + (uint32_t)pos + 5);
        }
        if (prev_mask == 0) {
          break;
        }
        {
          const uint32_t i = bit_number[prev_mask >> 1];
          b = (uint8_t)(dest >> (24 - i * 8));
          if (!(b == 0x00 || b == 0xFF)) {
            break;
          }
          src = dest ^ (((uint32_t)1 << (32 - i * 8)) - 1);
        }
      }
      buf[pos + 4] = (uint8_t)(~(((dest >> 24) & 1) - 1));
      buf[pos + 3] = (uint8_t)(dest >> 16);
      buf[pos + 2] = (uint8_t)(dest >> 8);
      buf[pos + 1] = (uint8_t)dest;
      pos += 5;
      prev_mask = 0;
    }
    else {
      pos++;
      prev_mask |= 1;
      if (b == 0x00 || b == 0xFF) {
        prev_mask |= 0x10;
      }
    }
  }
  st->prev_mask = prev_mask;
  st->prev_pos = prev_pos;
  return pos;
}

/* PowerPC (big-endian): `bl`, opcode 18 with the link bit set and the
 * absolute-address bit clear, and a 24-bit word displacement. */
static size_t bcj_powerpc(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  size_t i;
  for (i = 0; i + 4 <= size; i += 4) {
    if ((buf[i] >> 2) == 0x12 && (buf[i + 3] & 3) == 1) {
      const uint32_t src = (uint32_t)(buf[i] & 3) << 24 |
          (uint32_t)buf[i + 1] << 16 | (uint32_t)buf[i + 2] << 8 |
          (uint32_t)(buf[i + 3] & ~3u);
      const uint32_t at = now_pos + (uint32_t)i;
      const uint32_t dest = encoding ? at + src : src - at;
      buf[i] = (uint8_t)(0x48 | ((dest >> 24) & 0x03));
      buf[i + 1] = (uint8_t)(dest >> 16);
      buf[i + 2] = (uint8_t)(dest >> 8);
      buf[i + 3] = (uint8_t)((buf[i + 3] & 0x03) | (dest & ~3u));
    }
  }
  return i;
}

/* ARM: `bl`, condition "always" (EB in the top byte), a 24-bit word
 * displacement taken from the address two instructions on. */
static size_t bcj_arm(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  size_t i;
  for (i = 0; i + 4 <= size; i += 4) {
    if (buf[i + 3] == 0xEB) {
      uint32_t src = (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 1] << 8 |
          (uint32_t)buf[i];
      uint32_t dest;
      src <<= 2;
      dest = encoding ? now_pos + (uint32_t)i + 8 + src
                      : src - (now_pos + (uint32_t)i + 8);
      dest >>= 2;
      buf[i + 2] = (uint8_t)(dest >> 16);
      buf[i + 1] = (uint8_t)(dest >> 8);
      buf[i] = (uint8_t)dest;
    }
  }
  return i;
}

/* ARM Thumb: the two-halfword `bl`, F000 F800 with 11 bits in each half. */
static size_t bcj_armthumb(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  size_t i;
  for (i = 0; i + 4 <= size; i += 2) {
    if ((buf[i + 1] & 0xF8) == 0xF0 && (buf[i + 3] & 0xF8) == 0xF8) {
      uint32_t src = (uint32_t)(buf[i + 1] & 7) << 19 | (uint32_t)buf[i] << 11 |
          (uint32_t)(buf[i + 3] & 7) << 8 | (uint32_t)buf[i + 2];
      uint32_t dest;
      src <<= 1;
      dest = encoding ? now_pos + (uint32_t)i + 4 + src
                      : src - (now_pos + (uint32_t)i + 4);
      dest >>= 1;
      buf[i + 1] = (uint8_t)(0xF0 | ((dest >> 19) & 0x07));
      buf[i] = (uint8_t)(dest >> 11);
      buf[i + 3] = (uint8_t)(0xF8 | ((dest >> 8) & 0x07));
      buf[i + 2] = (uint8_t)dest;
      i += 2;
    }
  }
  return i;
}

/* SPARC (big-endian): `call`, whose 30-bit word displacement is sign
 * extended, so only the two ranges where bits 29..22 are all zero or all one
 * are taken for calls. */
static size_t bcj_sparc(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  size_t i;
  for (i = 0; i + 4 <= size; i += 4) {
    if ((buf[i] == 0x40 && (buf[i + 1] & 0xC0) == 0x00) ||
        (buf[i] == 0x7F && (buf[i + 1] & 0xC0) == 0xC0)) {
      uint32_t src = get_be32(buf + i);
      uint32_t dest;
      src <<= 2;
      dest = encoding ? now_pos + (uint32_t)i + src
                      : src - (now_pos + (uint32_t)i);
      dest >>= 2;
      dest = (((0 - ((dest >> 22) & 1)) << 22) & 0x3FFFFFFFu) |
          (dest & 0x3FFFFFu) | 0x40000000u;
      put_be32(buf + i, dest);
    }
  }
  return i;
}

/* IA-64: 128-bit bundles of a 5-bit template and three 41-bit slots. A
 * template says which slots may hold a branch; a branch with opcode 5 and
 * btype 0 has a 21-bit bundle displacement split across the slot. */
static size_t bcj_ia64(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  static const uint8_t branch_table[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
  size_t i;
  for (i = 0; i + 16 <= size; i += 16) {
    const uint32_t mask = branch_table[buf[i] & 0x1F];
    uint32_t bit_pos = 5;
    uint32_t slot;
    for (slot = 0; slot < 3; slot++, bit_pos += 41) {
      uint32_t byte_pos, bit_res, j;
      uint64_t instruction = 0, inst_norm;
      if (((mask >> slot) & 1) == 0) {
        continue;
      }
      byte_pos = bit_pos >> 3;
      bit_res = bit_pos & 7;
      for (j = 0; j < 6; j++) {
        instruction += (uint64_t)buf[i + j + byte_pos] << (8 * j);
      }
      inst_norm = instruction >> bit_res;
      if (((inst_norm >> 37) & 0xF) == 0x5 && ((inst_norm >> 9) & 0x7) == 0) {
        uint32_t src = (uint32_t)((inst_norm >> 13) & 0xFFFFF);
        uint32_t dest;
        src |= (uint32_t)((inst_norm >> 36) & 1) << 20;
        src <<= 4;
        dest = encoding ? now_pos + (uint32_t)i + src
                        : src - (now_pos + (uint32_t)i);
        dest >>= 4;
        inst_norm &= ~((uint64_t)0x8FFFFF << 13);
        inst_norm |= (uint64_t)(dest & 0xFFFFF) << 13;
        inst_norm |= (uint64_t)(dest & 0x100000) << (36 - 20);
        instruction &= ((uint64_t)1 << bit_res) - 1;
        instruction |= inst_norm << bit_res;
        for (j = 0; j < 6; j++) {
          buf[i + j + byte_pos] = (uint8_t)(instruction >> (8 * j));
        }
      }
    }
  }
  return i;
}

/* ARM64: `bl` (26-bit word displacement) and `adrp` (a 21-bit page offset,
 * converted only when it is within +-512 MiB so that ordinary data bits that
 * look like one are not disturbed). */
static size_t bcj_arm64(
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  size_t i;
  for (i = 0; i + 4 <= size; i += 4) {
    uint32_t pc = now_pos + (uint32_t)i;
    uint32_t instr = get_le32(buf + i);
    if ((instr >> 26) == 0x25) {
      const uint32_t src = instr;
      instr = 0x94000000u;
      pc >>= 2;
      if (!encoding) {
        pc = 0u - pc;
      }
      instr |= (src + pc) & 0x03FFFFFFu;
      put_le32(buf + i, instr);
    }
    else if ((instr & 0x9F000000u) == 0x90000000u) {
      uint32_t src = ((instr >> 29) & 3u) | ((instr >> 3) & 0x001FFFFCu);
      uint32_t dest;
      if (((src + 0x00020000u) & 0x001C0000u) != 0) {
        continue;
      }
      pc >>= 12;
      if (!encoding) {
        pc = 0u - pc;
      }
      dest = src + pc;
      instr &= 0x9000001Fu;
      instr |= (dest & 3u) << 29;
      instr |= (dest & 0x0003FFFCu) << 3;
      instr |= (0u - (dest & 0x00020000u)) & 0x00E00000u;
      put_le32(buf + i, instr);
    }
  }
  return i;
}

size_t filter_bcj_code(filter_bcj_state_t * st, unsigned arch,
    uint32_t now_pos, int encoding, uint8_t * buf, size_t size) {
  switch (arch) {
  case FILTER_ARCH_X86:
    return bcj_x86(st, now_pos, encoding, buf, size);
  case FILTER_ARCH_POWERPC:
    return bcj_powerpc(now_pos, encoding, buf, size);
  case FILTER_ARCH_IA64:
    return bcj_ia64(now_pos, encoding, buf, size);
  case FILTER_ARCH_ARM:
    return bcj_arm(now_pos, encoding, buf, size);
  case FILTER_ARCH_ARMTHUMB:
    return bcj_armthumb(now_pos, encoding, buf, size);
  case FILTER_ARCH_SPARC:
    return bcj_sparc(now_pos, encoding, buf, size);
  case FILTER_ARCH_ARM64:
    return bcj_arm64(now_pos, encoding, buf, size);
  default:
    return size;
  }
}
