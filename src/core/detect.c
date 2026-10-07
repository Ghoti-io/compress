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
 * @file detect.c
 *
 * gcomp_detect(): which method wrote these bytes.
 *
 * ## What can and cannot be detected
 *
 * Three of the seven formats begin with a magic number, and those three can be
 * told apart from each other and from anything else with near certainty:
 *
 * | format | bytes | from |
 * | --- | --- | --- |
 * | gzip | `1f 8b 08` | RFC 1952 section 2.3.1 (ID1, ID2, CM) |
 * | zstd | `28 b5 2f fd` | RFC 8878 section 3.1.1, Magic_Number 0xFD2FB528 |
 * | lz4 | `04 22 4d 18` | LZ4 Frame Format, Magic Number 0x184D2204 |
 * | bzip2 | `42 5a 68 31`..`39` | "BZh" and a level digit, which libbz2's format opens with |
 *
 * zlib has no magic number.  RFC 1950 section 2.2 gives it two header bytes
 * with enough structure to test: the compression method must be 8 (one byte
 * value in sixteen), the window must not exceed 32 KB (one in two), and the
 * pair must be a multiple of 31 (one in 31).  Together that is about one
 * random byte pair in 992 - better than the divisibility test alone would
 * give, and still a real rate rather than none.  zlib is therefore reported
 * last and only when nothing else matches, and the documentation says what
 * that answer is worth.
 *
 * deflate, LZW and RLE begin with data.  There is nothing to recognise, and a
 * guess would be worse than an answer of "not known": a caller who believes it
 * will hand the bytes to the wrong decoder.
 *
 * ## Skippable frames
 *
 * Both LZ4 and Zstandard define skippable frames on the same magic range,
 * `0x184D2A50` to `0x184D2A5F` (LZ4 Frame Format, "Skippable Frames"; RFC 8878
 * section 3.1.2), with the same layout.  A stream that starts with one cannot
 * be identified from its first frame by either specification, because the frame
 * belongs to both.
 *
 * So a skippable frame is not an answer.  It is stepped over - its size is in
 * its header - and the frame after it is examined instead.  A caller with only
 * the first few kilobytes gets ::GCOMP_ERR_LIMIT and the offset to read to,
 * rather than a coin toss.
 */

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

/// Bytes needed before anything at all can be said.
#define GCOMP_DETECT_MIN_BYTES 4u

/**
 * @brief Read a little-endian 32-bit value.
 *
 * Local rather than shared: this file must not depend on a method's internal
 * headers, since it is about telling the methods apart.
 */
static uint32_t detect_read_le32(const uint8_t * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

/**
 * @brief Whether these two bytes could open a zlib stream.
 *
 * RFC 1950 section 2.2: CM (the low nibble of CMF) is 8 for deflate, CINFO
 * (the high nibble) is at most 7 because a window above 32 KB is not allowed,
 * and CMF*256 + FLG must be a multiple of 31.
 */
static int detect_looks_like_zlib(const uint8_t * p) {
  uint8_t cmf = p[0];
  uint8_t flg = p[1];
  if ((cmf & 0x0F) != 8) {
    return 0;
  }
  if ((cmf >> 4) > 7) {
    return 0;
  }
  return (((unsigned)cmf << 8) | flg) % 31u == 0;
}

gcomp_status_t gcomp_detect(const void * input, size_t input_size,
    const char ** method_name_out, size_t * needed_out) {
  if (!method_name_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *method_name_out = NULL;
  if (needed_out) {
    *needed_out = GCOMP_DETECT_MIN_BYTES;
  }
  if (!input && input_size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const uint8_t * p = (const uint8_t *)input;
  size_t offset = 0;

  // Step over any skippable frames in front of the data.  Bounded by the
  // input, so a stream of nothing but skippable frames ends in GCOMP_ERR_LIMIT
  // or GCOMP_ERR_UNSUPPORTED rather than a loop.
  for (;;) {
    size_t avail = input_size - offset;
    if (avail < GCOMP_DETECT_MIN_BYTES) {
      if (needed_out) {
        *needed_out = offset + GCOMP_DETECT_MIN_BYTES;
      }
      return GCOMP_ERR_LIMIT;
    }

    uint32_t magic = detect_read_le32(p + offset);

    if (magic >= 0x184D2A50u && magic <= 0x184D2A5Fu) {
      // Skippable, in both formats.  Header is the magic and a four-byte
      // little-endian size; neither format says whose frame it is.
      if (avail < 8u) {
        if (needed_out) {
          *needed_out = offset + 8u;
        }
        return GCOMP_ERR_LIMIT;
      }
      uint32_t payload = detect_read_le32(p + offset + 4);
      uint64_t next = (uint64_t)offset + 8u + (uint64_t)payload;
      if (next > (uint64_t)input_size) {
        if (needed_out) {
          // The frame after this one is where the answer is; say how far that
          // is, saturating rather than wrapping on a declared size that
          // overflows the address space.
          uint64_t want = next + GCOMP_DETECT_MIN_BYTES;
          *needed_out = (want > (uint64_t)(size_t)-1) ? (size_t)-1
                                                      : (size_t)want;
        }
        return GCOMP_ERR_LIMIT;
      }
      offset = (size_t)next;
      continue;
    }

    if (magic == 0xFD2FB528u) {
      *method_name_out = "zstd";
      return GCOMP_OK;
    }
    if (magic == 0x184D2204u) {
      *method_name_out = "lz4";
      return GCOMP_OK;
    }
    // RFC 1952 section 2.3.1: ID1, ID2, then CM, which is 8 for deflate and
    // the only value the specification defines.
    if (p[offset] == 0x1Fu && p[offset + 1] == 0x8Bu && p[offset + 2] == 0x08u) {
      *method_name_out = "gzip";
      return GCOMP_OK;
    }
    // "BZh" and a level digit, 1 to 9: bzip2's stream header. Four bytes of
    // it, and the digit has nine values of 256, so one random word in 2^27.
    if (p[offset] == 'B' && p[offset + 1] == 'Z' && p[offset + 2] == 'h' &&
        p[offset + 3] >= '1' && p[offset + 3] <= '9') {
      *method_name_out = "bzip2";
      return GCOMP_OK;
    }
    // Last, and a guess: see the note at the top of this file.
    if (detect_looks_like_zlib(p + offset)) {
      *method_name_out = "zlib";
      return GCOMP_OK;
    }

    return GCOMP_ERR_UNSUPPORTED;
  }
}
