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
 * @file test_filters.cpp
 *
 * The `delta` and `bcj` methods against liblzma, which is the only other
 * implementation of them that matters: an xz stream carries these filters and
 * a stream written here has to be read there.
 *
 * liblzma cannot run a filter alone, so the oracle is indirect (see
 * lzmaref::filter_apply): data goes in through [filter, LZMA2] and comes out
 * through [LZMA2], which is what the filter made of it.
 *
 * The test data is the point. A branch converter does nothing to bytes that
 * do not look like its branches, and random bytes almost never do, so each
 * architecture has a generator that makes them often, with the fields that
 * decide whether a converter fires (the displacement's top byte for x86, the
 * opcode bits for the rest) set both ways. A converter that never fired would
 * agree with liblzma's on all of random data.
 */

#include "../lzma/lzma_oracle.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/filter.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

TEST(Filters, LiblzmaIsActuallyAvailable) {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }
  }
  ASSERT_TRUE(lzmaref::lib().ok())
      << "liblzma.so.5 could not be loaded. The pinned copy is in the oracle "
         "image; `make check-oracle` runs this binary there. Set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to skip this on a machine without it.";
}

/* ---- data --------------------------------------------------------------- */

uint32_t g_seed = 1;
uint32_t next_rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 16;
}

/// Bits [pos, pos + len) of a bundle, little-endian bit order, set to @p v.
void set_bits(uint8_t * b, unsigned pos, unsigned len, uint64_t v) {
  for (unsigned i = 0; i < len; i++) {
    const unsigned at = pos + i;
    const uint8_t m = (uint8_t)(1u << (at & 7));
    if ((v >> i) & 1) {
      b[at >> 3] = (uint8_t)(b[at >> 3] | m);
    }
    else {
      b[at >> 3] = (uint8_t)(b[at >> 3] & ~m);
    }
  }
}

struct Arch {
  const char * name;
  uint64_t id;
  size_t align;
};

const Arch kArches[] = {
    {"x86", 0x04, 1},
    {"powerpc", 0x05, 4},
    {"ia64", 0x06, 16},
    {"arm", 0x07, 4},
    {"armthumb", 0x08, 2},
    {"sparc", 0x09, 4},
    {"arm64", 0x0A, 4},
};

/// Bytes that are a good fraction branches of the architecture, and the rest
/// noise, `n` long exactly (so a stream can end in the middle of one).
Bytes code_like(const Arch & a, size_t n, uint32_t seed) {
  Bytes out;
  g_seed = seed;
  const std::string name = a.name;
  while (out.size() < n) {
    const uint32_t r = next_rand();
    const uint32_t q = next_rand();
    if (name == "x86") {
      if (r % 5 == 0) {
        out.push_back((r & 0x100) ? 0xE8 : 0xE9);
        out.push_back((uint8_t)q);
        out.push_back((uint8_t)(q >> 8));
        out.push_back((uint8_t)next_rand());
        out.push_back((r & 0x200) ? 0xFF : 0x00);
      }
      else {
        out.push_back((uint8_t)(r % 11 == 0 ? 0xE8 : q));
      }
    }
    else if (name == "powerpc") {
      uint8_t w[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)q,
          (uint8_t)(q >> 8)};
      if (r % 5 < 2) {
        w[0] = (uint8_t)(0x48 | (r & 3));
        w[3] = (uint8_t)((w[3] & ~3u) | ((r & 0x400) ? 1u : (q & 3u)));
      }
      out.insert(out.end(), w, w + 4);
    }
    else if (name == "arm") {
      uint8_t w[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)q,
          (uint8_t)(q >> 8)};
      if (r % 5 < 2) {
        w[3] = 0xEB;
      }
      out.insert(out.end(), w, w + 4);
    }
    else if (name == "armthumb") {
      uint8_t w[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)q,
          (uint8_t)(q >> 8)};
      if (r % 5 < 2) {
        w[1] = (uint8_t)(0xF0 | (r & 7));
        w[3] = (uint8_t)(0xF8 | (q & 7));
      }
      out.insert(out.end(), w, w + ((r & 0x800) ? 4 : 2));
    }
    else if (name == "sparc") {
      uint8_t w[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)q,
          (uint8_t)(q >> 8)};
      if (r % 5 < 2) {
        if (r & 0x400) {
          w[0] = 0x40;
          w[1] = (uint8_t)(w[1] & 0x3F);
        }
        else {
          w[0] = 0x7F;
          w[1] = (uint8_t)(w[1] | 0xC0);
        }
      }
      out.insert(out.end(), w, w + 4);
    }
    else if (name == "ia64") {
      uint8_t b[16];
      for (int i = 0; i < 16; i++) {
        b[i] = (uint8_t)next_rand();
      }
      /* Every template, so that the ones with no branch slot are tried too. */
      b[0] = (uint8_t)((b[0] & 0xE0) | (r % 32));
      for (unsigned slot = 0; slot < 3; slot++) {
        if (next_rand() % 4 != 0) {
          const unsigned base = 5 + 41 * slot;
          set_bits(b, base + 37, 4, 5);
          set_bits(b, base + 9, 3, (next_rand() % 8 == 0) ? 3 : 0);
        }
      }
      out.insert(out.end(), b, b + 16);
    }
    else if (name == "arm64") {
      uint32_t w = (uint32_t)next_rand() << 16 | next_rand();
      if (r % 5 < 2) {
        w = 0x94000000u | (w & 0x03FFFFFFu);
      }
      else if (r % 5 == 2) {
        w = 0x90000000u | (w & 0x60FFFFFFu);
        if (r & 0x400) {
          /* Within the +-512 MiB the converter accepts, and not. */
          w = (w & ~0x00E00000u) | ((r & 0x800) ? 0u : 0x00E00000u);
        }
      }
      for (int i = 0; i < 4; i++) {
        out.push_back((uint8_t)(w >> (8 * i)));
      }
    }
  }
  out.resize(n);
  return out;
}

Bytes noise(size_t n, uint32_t seed) {
  Bytes out(n);
  g_seed = seed;
  for (auto & b : out) {
    b = (uint8_t)next_rand();
  }
  return out;
}

/// Slowly changing data with some noise: what delta is for.
Bytes signal(size_t n, uint32_t seed) {
  Bytes out(n);
  g_seed = seed;
  int v = 100;
  for (size_t i = 0; i < n; i++) {
    v += (int)(next_rand() % 7) - 3;
    out[i] = (uint8_t)v;
    if (next_rand() % 97 == 0) {
      out[i] = (uint8_t)next_rand();
    }
  }
  return out;
}

/* ---- running our methods ------------------------------------------------ */

gcomp_options_t * Opts(std::initializer_list<std::pair<const char *, const char *>> s,
    std::initializer_list<std::pair<const char *, int64_t>> n = {}) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  for (const auto & p : s) {
    EXPECT_EQ(gcomp_options_set_string(o, p.first, p.second), GCOMP_OK)
        << p.first;
  }
  for (const auto & p : n) {
    const std::string k = p.first;
    gcomp_status_t st = (k == "bcj.start_offset")
        ? gcomp_options_set_uint64(o, p.first, (uint64_t)p.second)
        : gcomp_options_set_int64(o, p.first, p.second);
    EXPECT_EQ(st, GCOMP_OK) << p.first;
  }
  return o;
}

/// Our method over @p in with the given input and output windows. Empty and
/// @p ok false when a call refuses or stops making progress.
Bytes run(bool encode, const char * method, gcomp_options_t * o,
    const Bytes & in, size_t ic, size_t oc, bool & ok) {
  gcomp_registry_t * reg = gcomp_registry_default();
  Bytes out, buf(oc);
  gcomp_encoder_t * enc = nullptr;
  gcomp_decoder_t * dec = nullptr;
  ok = false;
  if (encode) {
    if (gcomp_encoder_create(reg, method, o, &enc) != GCOMP_OK) {
      return {};
    }
  }
  else if (gcomp_decoder_create(reg, method, o, &dec) != GCOMP_OK) {
    return {};
  }
  size_t pos = 0;
  bool bad = false;
  while (pos < in.size() && !bad) {
    const size_t take = std::min(ic, in.size() - pos);
    gcomp_buffer_t ib = {(void *)(in.data() + pos), take, 0};
    size_t spins = 0;
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {buf.data(), oc, 0};
      const size_t before = ib.used;
      gcomp_status_t s = encode ? gcomp_encoder_update(enc, &ib, &ob)
                                : gcomp_decoder_update(dec, &ib, &ob);
      out.insert(out.end(), buf.begin(), buf.begin() + ob.used);
      if (s != GCOMP_OK || (ob.used == 0 && ib.used == before && ++spins > 4)) {
        bad = true;
        break;
      }
    }
    pos += take;
  }
  for (size_t spins = 0; !bad; spins++) {
    gcomp_buffer_t ob = {buf.data(), oc, 0};
    gcomp_status_t s =
        encode ? gcomp_encoder_finish(enc, &ob) : gcomp_decoder_finish(dec, &ob);
    out.insert(out.end(), buf.begin(), buf.begin() + ob.used);
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || spins > in.size() + 16) {
      bad = true;
    }
  }
  if (enc) {
    gcomp_encoder_destroy(enc);
  }
  if (dec) {
    gcomp_decoder_destroy(dec);
  }
  ok = !bad;
  return out;
}

Bytes bcj(bool encode, const Arch & a, const Bytes & in, uint32_t start = 0,
    size_t ic = SIZE_MAX / 2, size_t oc = 1 << 16) {
  gcomp_options_t * o =
      Opts({{"bcj.arch", a.name}}, {{"bcj.start_offset", (int64_t)start}});
  bool ok = false;
  Bytes r = run(encode, "bcj", o, in, std::min(ic, in.size() + 1), oc, ok);
  gcomp_options_destroy(o);
  EXPECT_TRUE(ok) << a.name;
  return r;
}

Bytes delta(bool encode, unsigned dist, const Bytes & in, size_t ic = SIZE_MAX / 2,
    size_t oc = 1 << 16) {
  gcomp_options_t * o = Opts({}, {{"delta.distance", dist}});
  bool ok = false;
  Bytes r = run(encode, "delta", o, in, std::min(ic, in.size() + 1), oc, ok);
  gcomp_options_destroy(o);
  EXPECT_TRUE(ok) << dist;
  return r;
}

/* ---- against liblzma ---------------------------------------------------- */

TEST(Filters, EveryBranchConverterMatchesLiblzmaBothWays) {
  for (const Arch & a : kArches) {
    for (size_t n : {0u, 1u, 3u, 4u, 5u, 6u, 15u, 16u, 17u, 33u, 255u, 1000u,
             4097u, 30000u}) {
      for (uint32_t seed : {1u, 2u}) {
        const Bytes in = code_like(a, n, seed * 1000 + (uint32_t)n);
        lzmaref::BcjOptions bo = {0};
        bool ok = false;
        const Bytes want = lzmaref::filter_apply(a.id, &bo, in, true, ok);
        ASSERT_TRUE(ok) << a.name << " " << n << ": liblzma refused";
        const Bytes got = bcj(true, a, in);
        ASSERT_EQ(got.size(), in.size()) << a.name << " " << n;
        ASSERT_EQ(got, want) << a.name << " encode, " << n << " bytes, seed "
                             << seed;

        /* The inverse: ours on arbitrary bytes against liblzma's. */
        const Bytes back_want = lzmaref::filter_apply(a.id, &bo, in, false, ok);
        ASSERT_TRUE(ok) << a.name << " " << n;
        ASSERT_EQ(bcj(false, a, in), back_want)
            << a.name << " decode, " << n << " bytes, seed " << seed;
      }
    }
  }
}

/// A converter that never fired would agree with liblzma's on all of this, so
/// say that the data did make them fire.
TEST(Filters, TheGeneratorsReallyMakeTheConvertersFire) {
  for (const Arch & a : kArches) {
    const Bytes in = code_like(a, 20000, 5);
    EXPECT_NE(bcj(true, a, in), in) << a.name << ": nothing was converted";
  }
}

TEST(Filters, AStartOffsetMovesTheAddressesAsItDoesInLiblzma) {
  for (const Arch & a : kArches) {
    for (uint32_t start : {16u, 4096u, 0x7FFFFFF0u, 0xFFFFFFF0u}) {
      const Bytes in = code_like(a, 6000, start | 1);
      lzmaref::BcjOptions bo = {start};
      bool ok = false;
      const Bytes want = lzmaref::filter_apply(a.id, &bo, in, true, ok);
      ASSERT_TRUE(ok) << a.name << " start " << start;
      ASSERT_EQ(bcj(true, a, in, start), want) << a.name << " start " << start;
      ASSERT_EQ(bcj(false, a, want, start), in) << a.name << " start " << start;
    }
  }
}

TEST(Filters, X86OnRealCodeMatchesLiblzma) {
  /* This very program: real compiled code of whatever machine runs it, which
   * has the call and jump density and the misleading E8 bytes that a
   * generator only imitates. */
  FILE * f = std::fopen("/proc/self/exe", "rb");
  if (!f) {
    GTEST_SKIP() << "/proc/self/exe is not readable here";
  }
  Bytes in(1 << 20);
  in.resize(std::fread(in.data(), 1, in.size(), f));
  std::fclose(f);
  ASSERT_GT(in.size(), 100000u);
  lzmaref::BcjOptions bo = {0};
  bool ok = false;
  const Bytes want = lzmaref::filter_apply(0x04, &bo, in, true, ok);
  ASSERT_TRUE(ok);
  const Arch & x86 = kArches[0];
  const Bytes got = bcj(true, x86, in);
  ASSERT_EQ(got, want);
  EXPECT_NE(got, in);
  EXPECT_EQ(bcj(false, x86, got), in);
}

TEST(Filters, DeltaMatchesLiblzmaAtEveryDistance) {
  const Bytes in = signal(3000, 4);
  const Bytes rough = noise(700, 9);
  for (unsigned dist = 1; dist <= 256; dist++) {
    lzmaref::DeltaOptions d = lzmaref::delta_options(dist);
    for (const Bytes * data : {&in, &rough}) {
      bool ok = false;
      const Bytes want =
          lzmaref::filter_apply(lzmaref::kFilterDelta, &d, *data, true, ok);
      ASSERT_TRUE(ok) << dist;
      const Bytes got = delta(true, dist, *data);
      ASSERT_EQ(got, want) << "delta encode, distance " << dist;
      ASSERT_EQ(delta(false, dist, got), *data) << "distance " << dist;
      ASSERT_EQ(delta(false, dist, *data),
          lzmaref::filter_apply(lzmaref::kFilterDelta, &d, *data, false, ok))
          << "delta decode, distance " << dist;
    }
  }
}

/* ---- how the bytes arrive ----------------------------------------------- */

/// The output is a function of the bytes, not of how they were cut up: a
/// converter holds back the end of what it is given, and the place it does so
/// moves with the cut.
TEST(Filters, TheOutputDoesNotDependOnHowTheBytesArrive) {
  for (const Arch & a : kArches) {
    const Bytes in = code_like(a, 9000, 77);
    const Bytes whole = bcj(true, a, in);
    for (size_t ic : {size_t(1), size_t(2), size_t(3), size_t(5), size_t(7),
             size_t(16), size_t(4095), size_t(4096), size_t(4097)}) {
      for (size_t oc : {size_t(1), size_t(3), size_t(16), size_t(4096)}) {
        if (ic == 1 && oc == 1 && &a != &kArches[0] && &a != &kArches[2]) {
          continue; /* the slowest corner, for two architectures only */
        }
        ASSERT_EQ(bcj(true, a, in, 0, ic, oc), whole)
            << a.name << " in " << ic << " out " << oc;
        ASSERT_EQ(bcj(false, a, whole, 0, ic, oc), bcj(false, a, whole))
            << a.name << " decode in " << ic << " out " << oc;
      }
    }
  }
  const Bytes sig = signal(5000, 3);
  const Bytes whole = delta(true, 3, sig);
  for (size_t ic : {size_t(1), size_t(7), size_t(4097)}) {
    for (size_t oc : {size_t(1), size_t(5), size_t(4096)}) {
      ASSERT_EQ(delta(true, 3, sig, ic, oc), whole) << ic << " " << oc;
    }
  }
}

TEST(Filters, OurDecoderUndoesOurEncoderOnEverything) {
  for (const Arch & a : kArches) {
    for (size_t n : {0u, 1u, 4u, 5u, 17u, 5000u}) {
      for (const Bytes & in : {code_like(a, n, 3), noise(n, 4)}) {
        ASSERT_EQ(bcj(false, a, bcj(true, a, in)), in) << a.name << " " << n;
      }
    }
  }
  for (unsigned dist : {1u, 2u, 7u, 255u, 256u}) {
    const Bytes in = signal(2000, dist);
    ASSERT_EQ(delta(false, dist, delta(true, dist, in)), in) << dist;
  }
}

/* ---- the method contract ------------------------------------------------ */

TEST(Filters, ABadOptionIsRefusedWhereItIsSet) {
  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_encoder_t * enc = nullptr;
  for (const char * bad : {"mips", "X86", ""}) {
    gcomp_options_t * o = Opts({{"bcj.arch", bad}});
    EXPECT_NE(gcomp_encoder_create(reg, "bcj", o, &enc), GCOMP_OK) << bad;
    gcomp_options_destroy(o);
  }
  for (int64_t d : {0, -1, 257, 1000}) {
    gcomp_options_t * o = Opts({}, {{"delta.distance", d}});
    EXPECT_NE(gcomp_encoder_create(reg, "delta", o, &enc), GCOMP_OK) << d;
    gcomp_options_destroy(o);
  }
  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "bcj.start_offset", 1ull << 32), GCOMP_OK);
  EXPECT_NE(gcomp_encoder_create(reg, "bcj", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
}

TEST(Filters, TheDefaultsAreX86AndDistanceOne) {
  const Bytes in = code_like(kArches[0], 3000, 8);
  bool ok = false;
  EXPECT_EQ(run(true, "bcj", nullptr, in, 5000, 4096, ok), bcj(true, kArches[0], in));
  EXPECT_TRUE(ok);
  const Bytes sig = signal(1000, 8);
  EXPECT_EQ(run(true, "delta", nullptr, sig, 5000, 4096, ok), delta(true, 1, sig));
  EXPECT_TRUE(ok);
}

TEST(Filters, UpdateAfterFinishIsRefusedAndResetStartsOver) {
  gcomp_registry_t * reg = gcomp_registry_default();
  for (const char * method : {"delta", "bcj"}) {
    const Bytes in = code_like(kArches[0], 400, 6);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(reg, method, nullptr, &enc), GCOMP_OK);
    Bytes first, buf(4096);
    for (int round = 0; round < 2; round++) {
      Bytes out;
      gcomp_buffer_t ib = {(void *)in.data(), in.size(), 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
        ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
        out.insert(out.end(), buf.begin(), buf.begin() + ob.used);
      }
      gcomp_status_t s;
      do {
        gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
        s = gcomp_encoder_finish(enc, &ob);
        out.insert(out.end(), buf.begin(), buf.begin() + ob.used);
      } while (s == GCOMP_ERR_LIMIT);
      ASSERT_EQ(s, GCOMP_OK);
      uint8_t x = 1;
      gcomp_buffer_t more = {&x, 1, 0};
      gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
      EXPECT_EQ(gcomp_encoder_update(enc, &more, &ob), GCOMP_ERR_INVALID_ARG)
          << method;
      if (round == 0) {
        first = out;
        ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
      }
      else {
        EXPECT_EQ(out, first) << method << ": reset is not a fresh encoder";
      }
    }
    gcomp_encoder_destroy(enc);
  }
}

TEST(Filters, OnlyDeltaCanFlushAndItLeavesNothingBehind) {
  gcomp_registry_t * reg = gcomp_registry_default();
  const Bytes in = signal(1000, 2);
  Bytes buf(4096);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "delta", nullptr, &enc), GCOMP_OK);
  gcomp_buffer_t ib = {(void *)in.data(), in.size(), 0};
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(ib.used, in.size());
  gcomp_buffer_t fb = {buf.data() + ob.used, buf.size() - ob.used, 0};
  ASSERT_EQ(gcomp_encoder_flush(enc, &fb, GCOMP_FLUSH_SYNC), GCOMP_OK);
  EXPECT_EQ(ob.used + fb.used, in.size()) << "a flush must emit what it consumed";
  gcomp_encoder_destroy(enc);

  ASSERT_EQ(gcomp_encoder_create(reg, "bcj", nullptr, &enc), GCOMP_OK);
  gcomp_buffer_t nb = {buf.data(), buf.size(), 0};
  EXPECT_EQ(gcomp_encoder_flush(enc, &nb, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_UNSUPPORTED);
  gcomp_encoder_destroy(enc);
}

TEST(Filters, TheBoundIsTheLengthAndNothingExpands) {
  for (const char * method : {"delta", "bcj"}) {
    for (size_t n : {size_t(0), size_t(1), size_t(100000)}) {
      size_t bound = 0;
      ASSERT_EQ(gcomp_encode_bound(gcomp_registry_default(), method, nullptr, n,
                    &bound),
          GCOMP_OK);
      EXPECT_EQ(bound, n) << method;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
