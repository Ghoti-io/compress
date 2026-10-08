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
 * @file test_xz.cpp
 *
 * The xz method against liblzma, which it loads at run time.
 *
 * Both directions are checked: liblzma's streams through this decoder, and
 * this encoder's through liblzma's decoder, which verifies the checks and the
 * index. The decoder's refusals are checked against liblzma's the same way: a
 * valid stream is changed one byte at a time, and the two decoders must agree
 * on every change about whether the result is still a stream. A differential
 * on valid input alone cannot see the error paths, which is most of a
 * container's code.
 */

#include "../lzma/lzma_oracle.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xz.h>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

TEST(Xz, LiblzmaIsActuallyAvailable) {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }
  }
  ASSERT_TRUE(lzmaref::lib().ok() && lzmaref::lib().xz_ok())
      << "liblzma.so.5 could not be loaded, or lacks the .xz functions. The "
         "pinned copy is in the oracle image; `make check-oracle` runs this "
         "binary there. Set GCOMP_SKIP_ORACLE_TESTS=1 to skip this on a "
         "machine without it.";
}

/* ---- data --------------------------------------------------------------- */

uint32_t g_seed = 1;
uint32_t next_rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 16;
}

Bytes words(size_t n, uint32_t seed = 7) {
  static const char * w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "sleeps. "};
  Bytes out;
  g_seed = seed;
  while (out.size() < n) {
    const char * s = w[next_rand() % 11];
    out.insert(out.end(), s, s + std::strlen(s));
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

Bytes mixed(size_t n) {
  Bytes t = words(n / 3), z(n / 3, 'Z'), r = noise(n - 2 * (n / 3), 99);
  t.insert(t.end(), z.begin(), z.end());
  t.insert(t.end(), r.begin(), r.end());
  return t;
}

struct Case {
  const char * name;
  Bytes data;
};

std::vector<Case> corpus() {
  return {{"empty", {}}, {"one", {42}}, {"text", words(100000)},
      {"mixed", mixed(300000)}, {"zeros", Bytes(1 << 20, 0)},
      {"noise", noise(70000, 5)}};
}

const int kChecks[] = {lzmaref::kCheckNone, lzmaref::kCheckCrc32,
    lzmaref::kCheckCrc64, lzmaref::kCheckSha256};
const char * const kCheckNames[] = {"none", "crc32", "crc64", "sha256"};

const char * check_name(int c) {
  for (size_t i = 0; i < 4; i++) {
    if (kChecks[i] == c) {
      return kCheckNames[i];
    }
  }
  return "?";
}

/* ---- running ours ------------------------------------------------------- */

struct XzRun {
  gcomp_status_t status = GCOMP_OK;
  Bytes out;
  std::string detail;
};

gcomp_options_t * Opts(
    std::initializer_list<std::pair<const char *, const char *>> s,
    std::initializer_list<std::pair<const char *, int64_t>> n = {}) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  for (const auto & p : s) {
    EXPECT_EQ(gcomp_options_set_string(o, p.first, p.second), GCOMP_OK)
        << p.first;
  }
  for (const auto & p : n) {
    const std::string k = p.first;
    const bool unsigned_key = k == "xz.dict_size" || k == "xz.block_size" ||
        k.rfind("limits.", 0) == 0;
    gcomp_status_t st = unsigned_key
        ? gcomp_options_set_uint64(o, p.first, (uint64_t)p.second)
        : gcomp_options_set_int64(o, p.first, p.second);
    EXPECT_EQ(st, GCOMP_OK) << p.first;
  }
  return o;
}

XzRun run_xz(bool encode, gcomp_options_t * o, const Bytes & in, size_t ic,
    size_t oc) {
  gcomp_registry_t * reg = gcomp_registry_default();
  XzRun r;
  Bytes buf(oc);
  gcomp_encoder_t * enc = nullptr;
  gcomp_decoder_t * dec = nullptr;
  if (encode) {
    r.status = gcomp_encoder_create(reg, "xz", o, &enc);
  }
  else {
    r.status = gcomp_decoder_create(reg, "xz", o, &dec);
  }
  if (r.status != GCOMP_OK) {
    return r;
  }
  auto detail = [&]() {
    const char * d = encode ? gcomp_encoder_get_error_detail(enc)
                            : gcomp_decoder_get_error_detail(dec);
    return std::string(d ? d : "");
  };
  size_t pos = 0;
  while (pos < in.size() && r.status == GCOMP_OK) {
    const size_t take = std::min(ic, in.size() - pos);
    gcomp_buffer_t ib = {(void *)(in.data() + pos), take, 0};
    size_t spins = 0;
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {buf.data(), oc, 0};
      const size_t before = ib.used;
      gcomp_status_t s = encode ? gcomp_encoder_update(enc, &ib, &ob)
                                : gcomp_decoder_update(dec, &ib, &ob);
      r.out.insert(r.out.end(), buf.begin(), buf.begin() + ob.used);
      if (s != GCOMP_OK) {
        r.status = s;
        r.detail = detail();
        break;
      }
      if (ob.used == 0 && ib.used == before && ++spins > 4) {
        r.status = GCOMP_ERR_INTERNAL;
        r.detail = "no progress";
        break;
      }
    }
    pos += take;
  }
  for (size_t spins = 0; r.status == GCOMP_OK; spins++) {
    gcomp_buffer_t ob = {buf.data(), oc, 0};
    gcomp_status_t s =
        encode ? gcomp_encoder_finish(enc, &ob) : gcomp_decoder_finish(dec, &ob);
    r.out.insert(r.out.end(), buf.begin(), buf.begin() + ob.used);
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || spins > in.size() + r.out.size() + 64) {
      r.status = s;
      r.detail = detail();
    }
  }
  if (enc) {
    gcomp_encoder_destroy(enc);
  }
  if (dec) {
    gcomp_decoder_destroy(dec);
  }
  return r;
}

constexpr size_t kAll = SIZE_MAX / 2;

XzRun encode(const Bytes & in, gcomp_options_t * o = nullptr, size_t ic = kAll,
    size_t oc = 1 << 16) {
  return run_xz(true, o, in, std::min(ic, in.size() + 1), oc);
}

XzRun decode(const Bytes & in, gcomp_options_t * o = nullptr, size_t ic = kAll,
    size_t oc = 1 << 16) {
  return run_xz(false, o, in, std::min(ic, in.size() + 1), oc);
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto & p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

/* ---- reading a stream's index, to say what blocks it has ---------------- */

struct Rec {
  uint64_t unpadded, uncompressed;
};

bool vli(const Bytes & b, size_t & p, uint64_t & v) {
  v = 0;
  for (unsigned i = 0; i < 9 && p < b.size(); i++) {
    const uint8_t c = b[p++];
    v |= (uint64_t)(c & 0x7F) << (7 * i);
    if (!(c & 0x80)) {
      return true;
    }
  }
  return false;
}

/// The block records of a single stream, from its footer and index.
std::vector<Rec> blocks_of(const Bytes & z) {
  std::vector<Rec> recs;
  if (z.size() < 24) {
    return recs;
  }
  const uint32_t back = (uint32_t)z[z.size() - 8] |
      (uint32_t)z[z.size() - 7] << 8 | (uint32_t)z[z.size() - 6] << 16 |
      (uint32_t)z[z.size() - 5] << 24;
  const size_t index_size = ((size_t)back + 1) * 4;
  if (index_size + 12 > z.size()) {
    return recs;
  }
  size_t p = z.size() - 12 - index_size + 1;
  uint64_t n = 0;
  if (!vli(z, p, n)) {
    return recs;
  }
  for (uint64_t i = 0; i < n; i++) {
    Rec r;
    if (!vli(z, p, r.unpadded) || !vli(z, p, r.uncompressed)) {
      return {};
    }
    recs.push_back(r);
  }
  return recs;
}

/* ---- liblzma's streams through ours ------------------------------------- */

TEST(Xz, OurDecoderReadsLiblzmasStreamsAtEveryCheckAndPreset) {
  for (const auto & c : corpus()) {
    for (uint32_t preset : {0u, 6u}) {
      for (int check : kChecks) {
        const Bytes z = lzmaref::xz_encode(c.data, preset, check);
        ASSERT_FALSE(z.empty()) << c.name;
        XzRun r = decode(z);
        ASSERT_EQ(r.status, GCOMP_OK)
            << c.name << " preset " << preset << " " << check_name(check)
            << ": " << r.detail;
        ASSERT_EQ(r.out, c.data)
            << c.name << " preset " << preset << " " << check_name(check);
      }
    }
  }
}

TEST(Xz, OurDecoderReadsItOneByteAtATimeAndIntoTinyOutputs) {
  for (const auto & c : corpus()) {
    if (c.data.size() > 100000) {
      continue;
    }
    const Bytes z = lzmaref::xz_encode(c.data, 1, lzmaref::kCheckSha256);
    for (size_t ic : {size_t(1), size_t(3), size_t(31), size_t(4096)}) {
      for (size_t oc : {size_t(1), size_t(7), size_t(4096)}) {
        if (ic == 1 && oc == 1 && c.data.size() > 5000) {
          continue; /* the slowest corner, on small inputs only */
        }
        XzRun r = decode(z, nullptr, ic, oc);
        ASSERT_EQ(r.status, GCOMP_OK)
            << c.name << " in " << ic << " out " << oc << ": " << r.detail;
        ASSERT_EQ(r.out, c.data) << c.name << " in " << ic << " out " << oc;
      }
    }
  }
}

TEST(Xz, OurDecoderReadsLiblzmasMultiBlockStreams) {
  const Bytes data = mixed(500000);
  for (int check : kChecks) {
    for (uint64_t block : {uint64_t(65536), uint64_t(150000)}) {
      const Bytes z = lzmaref::xz_encode_blocks(data, 1, check, block);
      ASSERT_FALSE(z.empty()) << "liblzma could not make blocks";
      EXPECT_GE(blocks_of(z).size(), 3u) << "the oracle made too few blocks";
      XzRun r = decode(z, nullptr, 777, 1000);
      ASSERT_EQ(r.status, GCOMP_OK)
          << check_name(check) << " block " << block << ": " << r.detail;
      ASSERT_EQ(r.out, data);
    }
  }
}

TEST(Xz, OurDecoderReadsFilterChains) {
  const Bytes sig = [] {
    Bytes b(60000);
    int v = 100;
    g_seed = 3;
    for (auto & x : b) {
      v += (int)(next_rand() % 5) - 2;
      x = (uint8_t)v;
    }
    return b;
  }();
  lzmaref::Options o = lzmaref::options(1);
  for (unsigned dist : {1u, 2u, 17u, 256u}) {
    lzmaref::DeltaOptions d = lzmaref::delta_options(dist);
    lzmaref::Filter chain[3] = {{lzmaref::kFilterDelta, &d},
        {lzmaref::kFilterLzma2, &o}, {lzmaref::kVliUnknown, nullptr}};
    const Bytes z = lzmaref::xz_encode_chain(sig, chain, lzmaref::kCheckCrc64);
    ASSERT_FALSE(z.empty());
    XzRun r = decode(z);
    ASSERT_EQ(r.status, GCOMP_OK) << "delta " << dist << ": " << r.detail;
    ASSERT_EQ(r.out, sig) << "delta " << dist;
  }
  /* Branch converters, with and without a start offset, and two in a row. */
  Bytes code;
  g_seed = 9;
  for (int i = 0; i < 20000; i++) {
    code.push_back((i % 7 == 0) ? 0xE8 : (uint8_t)next_rand());
    if (i % 7 == 0) {
      for (int k = 0; k < 3; k++) {
        code.push_back((uint8_t)next_rand());
      }
      code.push_back((i & 8) ? 0xFF : 0x00);
    }
  }
  for (uint32_t start : {0u, 4096u}) {
    lzmaref::BcjOptions b = {start};
    lzmaref::Filter chain[3] = {{lzmaref::kFilterX86, start ? (void *)&b : nullptr},
        {lzmaref::kFilterLzma2, &o}, {lzmaref::kVliUnknown, nullptr}};
    const Bytes z = lzmaref::xz_encode_chain(code, chain, lzmaref::kCheckCrc32);
    ASSERT_FALSE(z.empty());
    XzRun r = decode(z);
    ASSERT_EQ(r.status, GCOMP_OK) << "x86 start " << start << ": " << r.detail;
    ASSERT_EQ(r.out, code) << "x86 start " << start;
  }
  {
    lzmaref::DeltaOptions d = lzmaref::delta_options(4);
    lzmaref::Filter chain[4] = {{lzmaref::kFilterX86, nullptr},
        {lzmaref::kFilterDelta, &d}, {lzmaref::kFilterLzma2, &o},
        {lzmaref::kVliUnknown, nullptr}};
    const Bytes z = lzmaref::xz_encode_chain(code, chain, lzmaref::kCheckSha256);
    ASSERT_FALSE(z.empty());
    XzRun r = decode(z);
    ASSERT_EQ(r.status, GCOMP_OK) << r.detail;
    ASSERT_EQ(r.out, code);
  }
}

TEST(Xz, StreamsFollowEachOtherAndTheirPaddingIsZeroInFours) {
  const Bytes a = words(5000, 1), b = noise(3000, 2), c = words(7000, 3);
  const Bytes za = lzmaref::xz_encode(a, 1, lzmaref::kCheckCrc32);
  const Bytes zb = lzmaref::xz_encode(b, 1, lzmaref::kCheckSha256);
  const Bytes zc = lzmaref::xz_encode(c, 1, lzmaref::kCheckNone);
  const Bytes want = cat({a, b, c});
  const Bytes pad4(4, 0), pad8(8, 0), pad3(3, 0);

  XzRun r = decode(cat({za, zb, zc}));
  ASSERT_EQ(r.status, GCOMP_OK) << r.detail;
  EXPECT_EQ(r.out, want);

  r = decode(cat({za, pad4, zb, pad8, zc, pad4}));
  ASSERT_EQ(r.status, GCOMP_OK) << r.detail;
  EXPECT_EQ(r.out, want) << "padding between and after streams";
  EXPECT_EQ(lzmaref::xz_decode(cat({za, pad4, zb, pad8, zc, pad4}), 20000).ret, 1);

  for (const Bytes & bad : {cat({za, pad3, zb}), cat({za, pad3}),
           cat({za, Bytes{0, 0, 0, 0, 0}}), cat({za, Bytes{'x'}}),
           cat({za, pad4, Bytes{0xFD, '7', 'z', 'X'}})}) {
    r = decode(bad);
    EXPECT_NE(r.status, GCOMP_OK);
    EXPECT_NE(lzmaref::xz_decode(bad, 20000).ret, 1)
        << "liblzma took what this refuses";
  }
}

/* ---- ours through liblzma ----------------------------------------------- */

TEST(Xz, LiblzmaReadsWhatTheEncoderWritesAtEveryCheckAndPreset) {
  for (const auto & c : corpus()) {
    for (int preset : {0, 3, 6}) {
      if (preset == 6 && c.data.size() > 400000) {
        continue;
      }
      for (size_t k = 0; k < 4; k++) {
        gcomp_options_t * o =
            Opts({{"xz.check", kCheckNames[k]}}, {{"xz.preset", preset}});
        XzRun z = encode(c.data, o);
        gcomp_options_destroy(o);
        ASSERT_EQ(z.status, GCOMP_OK) << c.name << ": " << z.detail;
        lzmaref::XzResult r = lzmaref::xz_decode(z.out, c.data.size());
        ASSERT_EQ(r.ret, 1) << c.name << " preset " << preset << " "
                            << kCheckNames[k] << ": liblzma said " << r.ret;
        ASSERT_EQ(r.out, c.data) << c.name << " preset " << preset;
        ASSERT_EQ(r.consumed, z.out.size()) << "bytes left over";
        XzRun back = decode(z.out);
        ASSERT_EQ(back.status, GCOMP_OK) << back.detail;
        ASSERT_EQ(back.out, c.data);
      }
    }
  }
}

TEST(Xz, TheEmptyStreamIsThirtyTwoBytesLikeLiblzmas) {
  for (size_t k = 0; k < 4; k++) {
    gcomp_options_t * o = Opts({{"xz.check", kCheckNames[k]}});
    XzRun z = encode({}, o);
    gcomp_options_destroy(o);
    ASSERT_EQ(z.status, GCOMP_OK);
    EXPECT_EQ(z.out, lzmaref::xz_encode({}, 6, kChecks[k])) << kCheckNames[k];
    EXPECT_EQ(z.out.size(), 32u);
  }
}

TEST(Xz, TheSizeIsLiblzmasToWithinAFewPercent) {
  const Bytes text = words(400000, 11);
  const Bytes mix = mixed(400000);
  for (const Bytes * d : {&text, &mix}) {
    for (int preset : {1, 6}) {
      gcomp_options_t * o = Opts({}, {{"xz.preset", preset}});
      XzRun z = encode(*d, o);
      gcomp_options_destroy(o);
      ASSERT_EQ(z.status, GCOMP_OK);
      const size_t theirs =
          lzmaref::xz_encode(*d, (uint32_t)preset, lzmaref::kCheckCrc64).size();
      EXPECT_LE(z.out.size(), theirs + theirs / 50 + 64)
          << "preset " << preset << ": " << z.out.size() << " against " << theirs;
    }
  }
}

TEST(Xz, BlocksAreCutWhereBlockSizeSaysAndEachIsAFreshLzma2Stream) {
  const Bytes data = mixed(500000);
  for (uint64_t block : {uint64_t(1), uint64_t(1000), uint64_t(65536),
           uint64_t(250000), uint64_t(500000), uint64_t(600000)}) {
    if (block == 1) {
      continue; /* a block a byte long is legal and too slow to be worth it */
    }
    gcomp_options_t * o = Opts({{"xz.check", "sha256"}}, {{"xz.preset", 0},
                                   {"xz.block_size", (int64_t)block}});
    XzRun z = encode(data, o, 4097, 333);
    gcomp_options_destroy(o);
    ASSERT_EQ(z.status, GCOMP_OK) << z.detail;
    const auto recs = blocks_of(z.out);
    ASSERT_EQ(recs.size(), (data.size() + block - 1) / block) << block;
    uint64_t total = 0;
    for (size_t i = 0; i < recs.size(); i++) {
      EXPECT_EQ(recs[i].uncompressed,
          std::min<uint64_t>(block, data.size() - total))
          << "block " << i;
      total += recs[i].uncompressed;
    }
    EXPECT_EQ(total, data.size());
    lzmaref::XzResult r = lzmaref::xz_decode(z.out, data.size());
    ASSERT_EQ(r.ret, 1) << "block " << block;
    ASSERT_EQ(r.out, data);
  }
}

TEST(Xz, FiltersInFrontOfLzma2AreReadByLiblzma) {
  Bytes code;
  g_seed = 13;
  for (int i = 0; i < 30000; i++) {
    code.push_back((i % 9 == 0) ? 0xE8 : (uint8_t)next_rand());
    if (i % 9 == 0) {
      for (int k = 0; k < 3; k++) {
        code.push_back((uint8_t)next_rand());
      }
      code.push_back((i & 16) ? 0xFF : 0x00);
    }
  }
  const char * specs[] = {"delta", "delta:7", "x86", "x86:4096", "arm64",
      "powerpc", "ia64:16", "arm", "armthumb", "sparc", "x86,delta:4",
      "delta:2,arm64,delta:8"};
  for (const char * spec : specs) {
    gcomp_options_t * o = Opts({{"xz.filters", spec}, {"xz.check", "crc32"}},
        {{"xz.preset", 1}});
    XzRun z = encode(code, o);
    gcomp_options_destroy(o);
    ASSERT_EQ(z.status, GCOMP_OK) << spec << ": " << z.detail;
    lzmaref::XzResult r = lzmaref::xz_decode(z.out, code.size());
    ASSERT_EQ(r.ret, 1) << spec << ": liblzma said " << r.ret;
    ASSERT_EQ(r.out, code) << spec;
    XzRun back = decode(z.out);
    ASSERT_EQ(back.status, GCOMP_OK) << spec << ": " << back.detail;
    ASSERT_EQ(back.out, code) << spec;
  }
}

TEST(Xz, TheStreamDoesNotDependOnHowTheBytesArrive) {
  const Bytes data = mixed(300000);
  gcomp_options_t * o = Opts({{"xz.filters", "x86"}},
      {{"xz.preset", 1}, {"xz.block_size", 100000}});
  const XzRun whole = encode(data, o);
  ASSERT_EQ(whole.status, GCOMP_OK);
  for (size_t ic : {size_t(1000), size_t(4097), size_t(99999), size_t(100001)}) {
    for (size_t oc : {size_t(1), size_t(13), size_t(4096)}) {
      if (ic < 4000 && oc == 1) {
        continue;
      }
      XzRun z = encode(data, o, ic, oc);
      ASSERT_EQ(z.status, GCOMP_OK) << z.detail;
      ASSERT_EQ(z.out, whole.out) << "in " << ic << " out " << oc;
    }
  }
  gcomp_options_destroy(o);
}

TEST(Xz, AFlushEndsTheBlockAndWhatFollowsIsAnotherOne) {
  gcomp_registry_t * reg = gcomp_registry_default();
  const Bytes a = words(40000, 4), b = noise(20000, 6);
  gcomp_options_t * o = Opts({}, {{"xz.preset", 1}});
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK);
  Bytes z, buf(4096);
  auto feed = [&](const Bytes & d) {
    gcomp_buffer_t ib = {(void *)d.data(), d.size(), 0};
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
      ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
      z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
    }
  };
  feed(a);
  gcomp_status_t s;
  size_t before_flush = z.size();
  do {
    gcomp_buffer_t ob = {buf.data(), 1000, 0};
    s = gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC);
    z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
  } while (s == GCOMP_ERR_LIMIT);
  ASSERT_EQ(s, GCOMP_OK);
  EXPECT_GT(z.size(), before_flush) << "a flush must write the open block out";
  feed(b);
  do {
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    s = gcomp_encoder_finish(enc, &ob);
    z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
  } while (s == GCOMP_ERR_LIMIT);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(o);
  const auto recs = blocks_of(z);
  ASSERT_EQ(recs.size(), 2u);
  EXPECT_EQ(recs[0].uncompressed, a.size());
  EXPECT_EQ(recs[1].uncompressed, b.size());
  lzmaref::XzResult r = lzmaref::xz_decode(z, 100000);
  ASSERT_EQ(r.ret, 1);
  EXPECT_EQ(r.out, cat({a, b}));
}

TEST(Xz, ResetEqualsAFreshEncoderAndUpdateAfterFinishIsRefused) {
  gcomp_registry_t * reg = gcomp_registry_default();
  const Bytes data = words(30000, 8);
  gcomp_options_t * o = Opts({{"xz.check", "sha256"}}, {{"xz.preset", 1}});
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK);
  Bytes first, buf(8192);
  for (int round = 0; round < 3; round++) {
    Bytes z;
    gcomp_buffer_t ib = {(void *)data.data(), data.size(), 0};
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
      ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
      z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
    }
    gcomp_status_t s;
    do {
      gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
      s = gcomp_encoder_finish(enc, &ob);
      z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
    } while (s == GCOMP_ERR_LIMIT);
    ASSERT_EQ(s, GCOMP_OK);
    uint8_t x = 1;
    gcomp_buffer_t more = {&x, 1, 0};
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(enc, &more, &ob), GCOMP_ERR_INVALID_ARG);
    if (round == 0) {
      first = z;
    }
    else {
      EXPECT_EQ(z, first) << "round " << round;
    }
    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  }
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(o);
}

/* ---- refusals, against liblzma's ---------------------------------------- */

/// Whether liblzma takes @p z as a complete stream, and what it gives.
bool theirs_accept(const Bytes & z, Bytes & out, size_t cap) {
  lzmaref::XzResult r = lzmaref::xz_decode(z, cap);
  out = r.out;
  return r.ret == 1;
}

bool ours_accept(const Bytes & z, Bytes & out) {
  XzRun r = decode(z);
  out = r.out;
  return r.status == GCOMP_OK;
}

/// A small stream with every part in it: three blocks, a filter in front of
/// LZMA2, the longest check.
Bytes rich_stream(Bytes & original) {
  original = words(3000, 21);
  gcomp_options_t * o = Opts({{"xz.check", "sha256"}, {"xz.filters", "delta:2"}},
      {{"xz.preset", 0}, {"xz.block_size", 1100}});
  XzRun z = encode(original, o);
  gcomp_options_destroy(o);
  EXPECT_EQ(z.status, GCOMP_OK);
  return z.out;
}

TEST(Xz, EveryByteChangedIsRefusedOrAcceptedAsLiblzmaDoes) {
  Bytes want;
  const Bytes z = rich_stream(want);
  ASSERT_EQ(blocks_of(z).size(), 3u);
  size_t disagreements = 0, refused = 0;
  for (size_t i = 0; i < z.size(); i++) {
    for (uint8_t x : {uint8_t(0x01), uint8_t(0x80), uint8_t(0xFF)}) {
      Bytes m = z;
      m[i] ^= x;
      Bytes a, b;
      const bool ours = ours_accept(m, a);
      const bool theirs = theirs_accept(m, b, want.size() + 64);
      refused += !ours;
      if (ours != theirs) {
        if (disagreements++ < 12) {
          ADD_FAILURE() << "byte " << i << " ^ 0x" << std::hex << (int)x
                        << std::dec << " (of " << z.size() << "): ours "
                        << (ours ? "accepts" : "refuses") << ", liblzma "
                        << (theirs ? "accepts" : "refuses");
        }
      }
      if (ours) {
        EXPECT_EQ(a, want) << "accepted a change and decoded different bytes";
      }
    }
  }
  EXPECT_EQ(disagreements, 0u);
  EXPECT_GT(refused, z.size() * 2) << "the changes were mostly harmless?";
}

TEST(Xz, EveryProperPrefixIsRefused) {
  Bytes want;
  const Bytes z = rich_stream(want);
  for (size_t n = 0; n < z.size(); n++) {
    const Bytes cut(z.begin(), z.begin() + n);
    Bytes a, b;
    EXPECT_FALSE(ours_accept(cut, a)) << "a prefix of " << n << " bytes";
    EXPECT_FALSE(theirs_accept(cut, b, want.size() + 64)) << n;
  }
}

TEST(Xz, TrailingBytesThatAreNotAStreamAreRefused) {
  Bytes want;
  const Bytes z = rich_stream(want);
  for (const Bytes & tail : {Bytes{1}, Bytes{0xFD}, Bytes{'Y', 'Z'},
           Bytes(5, 0), Bytes(16, 7)}) {
    Bytes m = cat({z, tail});
    Bytes a, b;
    EXPECT_EQ(ours_accept(m, a), theirs_accept(m, b, want.size() + 64))
        << "a tail of " << tail.size();
  }
}

TEST(Xz, AnUnsupportedCheckIsRefusedNotSkipped) {
  Bytes want;
  Bytes z = rich_stream(want);
  /* Check type 2 is reserved. Rewrite the header's flags and CRC, and the
   * footer's to match: only the check is unusual. */
  auto crc32 = [](const uint8_t * p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
      c ^= p[i];
      for (int k = 0; k < 8; k++) {
        c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
      }
    }
    return ~c;
  };
  z[7] = 2;
  uint32_t h = crc32(&z[6], 2);
  for (int i = 0; i < 4; i++) {
    z[8 + i] = (uint8_t)(h >> (8 * i));
  }
  z[z.size() - 3] = 2;
  h = crc32(&z[z.size() - 8], 6);
  for (int i = 0; i < 4; i++) {
    z[z.size() - 12 + i] = (uint8_t)(h >> (8 * i));
  }
  XzRun r = decode(z);
  EXPECT_EQ(r.status, GCOMP_ERR_UNSUPPORTED) << r.detail;
}

/* ---- forged streams: one field wrong, every CRC right -------------------- */

/* A flipped byte is almost always caught by the CRC over the field it fell in,
 * so the checks behind the CRCs are never reached by flipping. These streams
 * are built from parts, with the CRCs computed over whatever the parts say, so
 * that each field can be wrong by itself and the decoder has only the
 * field's own rule to refuse it by. liblzma reads the same bytes, and the two
 * must agree. */

uint32_t crc32b(const uint8_t * p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
    }
  }
  return ~c;
}

void put32(Bytes & b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    b[at + i] = (uint8_t)(v >> (8 * i));
  }
}

void put_vli(Bytes & b, uint64_t v) {
  while (v >= 0x80) {
    b.push_back((uint8_t)(v | 0x80));
    v >>= 7;
  }
  b.push_back((uint8_t)v);
}

size_t check_bytes(unsigned id) {
  return id == 0 ? 0 : (size_t)4 << ((id - 1) / 3);
}

struct Hdr {
  bool has_comp = false, has_unc = false;
  uint64_t comp = 0, unc = 0;
  unsigned reserved = 0; /* bits 2..5 of the flags byte */
  std::vector<std::pair<uint64_t, Bytes>> filters;
  int noisy_padding = 0; /* make the first padding byte nonzero */
  int extra_padding = 0; /* four more bytes of header, all zero */
};

Bytes make_header(const Hdr & h) {
  Bytes b = {0, 0};
  b[1] = (uint8_t)((h.filters.size() - 1) | h.reserved | (h.has_comp ? 0x40 : 0) |
      (h.has_unc ? 0x80 : 0));
  if (h.has_comp) {
    put_vli(b, h.comp);
  }
  if (h.has_unc) {
    put_vli(b, h.unc);
  }
  for (const auto & f : h.filters) {
    put_vli(b, f.first);
    put_vli(b, f.second.size());
    b.insert(b.end(), f.second.begin(), f.second.end());
  }
  const size_t body = b.size();
  size_t size = (body + 4 + 3) & ~(size_t)3;
  size += (size_t)h.extra_padding * 4;
  b.resize(size - 4, 0);
  if (h.noisy_padding && b.size() > body) {
    b[body] = 0x55;
  }
  b[0] = (uint8_t)(size / 4 - 1);
  b.resize(size);
  put32(b, size - 4, crc32b(b.data(), size - 4));
  return b;
}

bool read_vli(const Bytes & b, size_t & p, size_t limit, uint64_t & v) {
  v = 0;
  for (unsigned i = 0; i < 9 && p < limit; i++) {
    const uint8_t c = b[p++];
    v |= (uint64_t)(c & 0x7F) << (7 * i);
    if (!(c & 0x80)) {
      return true;
    }
  }
  return false;
}

Hdr parse_header(const Bytes & b) {
  Hdr h;
  size_t p = 2;
  const size_t limit = b.size() - 4;
  const unsigned flags = b[1];
  h.has_comp = flags & 0x40;
  h.has_unc = flags & 0x80;
  if (h.has_comp) {
    read_vli(b, p, limit, h.comp);
  }
  if (h.has_unc) {
    read_vli(b, p, limit, h.unc);
  }
  for (unsigned i = 0; i <= (flags & 3); i++) {
    uint64_t id, n;
    read_vli(b, p, limit, id);
    read_vli(b, p, limit, n);
    h.filters.push_back({id, Bytes(b.begin() + p, b.begin() + p + n)});
    p += n;
  }
  return h;
}

struct Blk {
  Hdr h;
  Bytes data, check;
  uint64_t unc = 0;
};

struct Strm {
  uint8_t f1 = 4; /* the check id */
  uint8_t f0 = 0;
  std::vector<Blk> blocks;
  std::vector<std::pair<uint64_t, uint64_t>> recs;
  bool recs_set = false;
};

struct Layout {
  size_t idx_start = 0, idx_end = 0, footer = 0;
  std::vector<size_t> block_start, header_size;
};

Bytes make_index(const std::vector<std::pair<uint64_t, uint64_t>> & recs) {
  Bytes b = {0};
  put_vli(b, recs.size());
  for (const auto & r : recs) {
    put_vli(b, r.first);
    put_vli(b, r.second);
  }
  while (b.size() % 4) {
    b.push_back(0);
  }
  b.resize(b.size() + 4);
  put32(b, b.size() - 4, crc32b(b.data(), b.size() - 4));
  return b;
}

Bytes build(Strm & s, Layout & L) {
  Bytes z = {0xFD, '7', 'z', 'X', 'Z', 0, s.f0, s.f1, 0, 0, 0, 0};
  put32(z, 8, crc32b(&z[6], 2));
  std::vector<std::pair<uint64_t, uint64_t>> recs;
  for (const auto & blk : s.blocks) {
    const Bytes hdr = make_header(blk.h);
    L.block_start.push_back(z.size());
    L.header_size.push_back(hdr.size());
    z.insert(z.end(), hdr.begin(), hdr.end());
    z.insert(z.end(), blk.data.begin(), blk.data.end());
    while (z.size() % 4) {
      z.push_back(0);
    }
    z.insert(z.end(), blk.check.begin(), blk.check.end());
    recs.push_back({hdr.size() + blk.data.size() + blk.check.size(), blk.unc});
  }
  if (!s.recs_set) {
    s.recs = recs;
  }
  const Bytes idx = make_index(s.recs);
  L.idx_start = z.size();
  z.insert(z.end(), idx.begin(), idx.end());
  L.idx_end = z.size();
  L.footer = z.size();
  Bytes f(12, 0);
  put32(f, 4, (uint32_t)(idx.size() / 4 - 1));
  f[8] = s.f0;
  f[9] = s.f1;
  f[10] = 'Y';
  f[11] = 'Z';
  put32(f, 0, crc32b(&f[4], 6));
  z.insert(z.end(), f.begin(), f.end());
  return z;
}

/// Take a single-stream .xz apart into the parts build() puts back together.
Strm split(const Bytes & z) {
  Strm s;
  s.f0 = z[6];
  s.f1 = z[7];
  const size_t csize = check_bytes(s.f1 & 0x0F);
  const auto recs = blocks_of(z);
  s.recs.clear();
  size_t pos = 12;
  for (const Rec & r : recs) {
    const size_t hs = ((size_t)z[pos] + 1) * 4;
    Blk b;
    b.h = parse_header(Bytes(z.begin() + pos, z.begin() + pos + hs));
    const size_t clen = (size_t)r.unpadded - hs - csize;
    b.data.assign(z.begin() + pos + hs, z.begin() + pos + hs + clen);
    const size_t padded = ((size_t)r.unpadded + 3) & ~(size_t)3;
    b.check.assign(z.begin() + pos + padded - csize, z.begin() + pos + padded);
    b.unc = r.uncompressed;
    s.blocks.push_back(b);
    s.recs.push_back({r.unpadded, r.uncompressed});
    pos += padded;
  }
  return s;
}

void fix_index(Bytes & z, const Layout & L) {
  put32(z, L.idx_end - 4, crc32b(&z[L.idx_start], L.idx_end - 4 - L.idx_start));
}

void fix_footer(Bytes & z, const Layout & L) {
  put32(z, L.footer, crc32b(&z[L.footer + 4], 6));
}

void fix_stream_header(Bytes & z) {
  put32(z, 8, crc32b(&z[6], 2));
}

struct Variant {
  const char * name;
  bool valid; /* both decoders must take it, and give the original bytes */
  std::function<void(Strm &)> model;
  std::function<void(Bytes &, const Layout &)> patch;
  /// Puts a filter in front of data that was not made with it. Only a stream
  /// of zeros survives that unchanged (every filter maps zeros to zeros), so
  /// only there is the filter's own rule what decides, and not the check.
  bool zeros_only = false;
};

Bytes lzma2_prop(uint8_t p) {
  return Bytes{p};
}

std::vector<Variant> variants() {
  using V = Variant;
  auto none = [](Strm &) {};
  auto nopatch = [](Bytes &, const Layout &) {};
  std::vector<V> v;
  v.push_back({"unchanged", true, none, nopatch});
  v.push_back({"declared sizes right", true, [](Strm & s) {
    for (auto & b : s.blocks) {
      b.h.has_comp = b.h.has_unc = true;
      b.h.comp = b.data.size();
      b.h.unc = b.unc;
    }
  }, nopatch});
  v.push_back({"declared compressed size alone", true, [](Strm & s) {
    s.blocks[0].h.has_comp = true;
    s.blocks[0].h.comp = s.blocks[0].data.size();
  }, nopatch});
  v.push_back({"delta properties right", true, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x03, Bytes{3}});
  }, nopatch});
  v.push_back({"extra header padding", true, [](Strm & s) {
    s.blocks[0].h.extra_padding = 1;
  }, nopatch});
  for (int d : {-1, 1}) {
    v.push_back({d < 0 ? "declared compressed one short" : "declared compressed one long", false,
        [d](Strm & s) {
          s.blocks[0].h.has_comp = true;
          s.blocks[0].h.comp = s.blocks[0].data.size() + d;
        }, nopatch});
    v.push_back({d < 0 ? "declared uncompressed one short" : "declared uncompressed one long", false,
        [d](Strm & s) {
          s.blocks[0].h.has_unc = true;
          s.blocks[0].h.unc = s.blocks[0].unc + d;
        }, nopatch});
  }
  v.push_back({"declared compressed zero", false, [](Strm & s) {
    s.blocks[0].h.has_comp = true;
    s.blocks[0].h.comp = 0;
  }, nopatch});
  v.push_back({"declared uncompressed zero", false, [](Strm & s) {
    s.blocks[0].h.has_unc = true;
    s.blocks[0].h.unc = 0;
  }, nopatch});
  for (unsigned bit : {0x04u, 0x08u, 0x10u, 0x20u}) {
    v.push_back({"block flag reserved bit", false, [bit](Strm & s) {
      s.blocks[0].h.reserved = bit;
    }, nopatch});
  }
  v.push_back({"header padding not zero", false, [](Strm & s) {
    s.blocks[0].h.noisy_padding = 1;
  }, nopatch});
  v.push_back({"lzma2 dictionary property 41", false, [](Strm & s) {
    s.blocks[0].h.filters.back().second = lzma2_prop(41);
  }, nopatch});
  v.push_back({"lzma2 properties empty", false, [](Strm & s) {
    s.blocks[0].h.filters.back().second = Bytes{};
  }, nopatch});
  v.push_back({"lzma2 properties two bytes", false, [](Strm & s) {
    s.blocks[0].h.filters.back().second = Bytes{20, 0};
  }, nopatch});
  v.push_back({"last filter not lzma2", false, [](Strm & s) {
    s.blocks[0].h.filters.back().first = 0x04;
    s.blocks[0].h.filters.back().second = Bytes{};
  }, nopatch});
  v.push_back({"last filter is delta with a property", false, [](Strm & s) {
    s.blocks[0].h.filters.back().first = 0x03;
    s.blocks[0].h.filters.back().second = Bytes{0};
  }, nopatch});
  v.push_back({"last filter is a branch converter with four properties", false,
      [](Strm & s) {
        s.blocks[0].h.filters.back().first = 0x04;
        s.blocks[0].h.filters.back().second = Bytes{0, 0, 0, 0};
      }, nopatch});
  v.push_back({"lzma2 first, then a filter", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    std::swap(f.front(), f.back());
  }, nopatch});
  v.push_back({"unknown filter before lzma2", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x40, Bytes{}});
  }, nopatch});
  v.push_back({"filter id above 2^62", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x4000000000000000ull, Bytes{}});
  }, nopatch});
  v.push_back({"delta properties empty", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x03, Bytes{}});
  }, nopatch});
  v.push_back({"delta properties two bytes", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x03, Bytes{1, 1}});
  }, nopatch});
  v.push_back({"x86 with no properties", true, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x04, Bytes{}});
  }, nopatch});
  for (size_t n : {1, 2, 3, 5, 8}) {
    v.push_back({"x86 properties of a wrong size", false, [n](Strm & s) {
      auto & f = s.blocks[0].h.filters;
      f.insert(f.begin(), {0x04, Bytes(n, 0)});
    }, nopatch});
  }
  v.push_back({"x86 start offset", true, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x04, Bytes{3, 0, 0, 0}});
  }, nopatch});
  v.push_back({"arm start offset misaligned", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x07, Bytes{2, 0, 0, 0}});
  }, nopatch});
  v.push_back({"arm start offset aligned", true, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x07, Bytes{8, 0, 0, 0}});
  }, nopatch});
  v.push_back({"armthumb start offset odd", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x08, Bytes{1, 0, 0, 0}});
  }, nopatch});
  v.push_back({"ia64 start offset not 16", false, [](Strm & s) {
    auto & f = s.blocks[0].h.filters;
    f.insert(f.begin(), {0x06, Bytes{8, 0, 0, 0}});
  }, nopatch});
  v.push_back({"block padding not zero", false, none,
      [](Bytes & z, const Layout & L) {
        /* The first byte after the first block's data, when there is
         * padding. Only streams whose first block needs some use this. */
        const size_t end = L.block_start.size() > 1 ? L.block_start[1] : L.idx_start;
        const size_t cs = check_bytes(z[7] & 0x0F);
        if (end - cs > 0 && z[end - cs - 1] == 0) {
          z[end - cs - 1] = 0x77;
        }
      }});
  v.push_back({"check wrong", false, none, [](Bytes & z, const Layout & L) {
    const size_t cs = check_bytes(z[7] & 0x0F);
    if (cs) {
      const size_t end = L.block_start.size() > 1 ? L.block_start[1] : L.idx_start;
      z[end - 1] ^= 0x10;
    }
  }});
  /* The true records, which a variant then spoils one way or another. */
  auto true_recs = [](Strm & s) {
    s.recs_set = true;
    s.recs.clear();
    for (auto & b : s.blocks) {
      const Bytes h = make_header(b.h);
      s.recs.push_back({h.size() + b.data.size() + b.check.size(), b.unc});
    }
  };
  v.push_back({"index lists one block fewer", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs.pop_back();
  }, nopatch});
  v.push_back({"index lists one block more", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs.push_back(s.recs.back());
  }, nopatch});
  v.push_back({"index lists a block twice and omits another", false,
      [true_recs](Strm & s) {
        true_recs(s);
        if (s.recs.size() > 1) {
          s.recs[1] = s.recs[0];
        }
        else {
          s.recs[0].second += 1;
        }
      }, nopatch});
  v.push_back({"index unpadded size one short", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs[0].first -= 1;
  }, nopatch});
  v.push_back({"index unpadded size one long", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs.back().first += 1;
  }, nopatch});
  v.push_back({"index uncompressed size one long", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs.back().second += 1;
  }, nopatch});
  v.push_back({"index uncompressed size one short", false, [true_recs](Strm & s) {
    true_recs(s);
    s.recs[0].second -= 1;
  }, nopatch});
  v.push_back({"index padding not zero", false, none,
      [](Bytes & z, const Layout & L) {
        /* Padding is whatever lies between the records and the CRC. */
        const size_t pad_at = L.idx_end - 5;
        if (z[pad_at] == 0 && (L.idx_end - L.idx_start) > 8) {
          z[pad_at] = 0x33;
          fix_index(z, L);
        }
      }});
  v.push_back({"index CRC wrong", false, none, [](Bytes & z, const Layout & L) {
    z[L.idx_end - 1] ^= 1;
  }});
  v.push_back({"index record count padded long", false, none,
      [](Bytes & z, const Layout & L) {
        /* The count 0x03 spelled 0x83 0x00: a longer way to say the same. */
        if (z[L.idx_start + 1] < 0x80) {
          Bytes w(z.begin(), z.begin() + L.idx_start + 1);
          w.push_back((uint8_t)(z[L.idx_start + 1] | 0x80));
          w.push_back(0);
          w.insert(w.end(), z.begin() + L.idx_start + 2, z.end());
          /* One byte more in the index: the padding after it is one less. */
          const size_t idx_end = L.idx_end + 1;
          const size_t pad_at = idx_end - 5;
          if (w[pad_at] == 0) {
            w.erase(w.begin() + pad_at);
            Layout M = L;
            M.idx_end = idx_end - 1;
            M.footer = L.footer;
            fix_index(w, M);
            /* The backward size is unchanged: same length index. */
            z = w;
          }
        }
      }});
  v.push_back({"footer CRC wrong", false, none, [](Bytes & z, const Layout &) {
    z[z.size() - 12] ^= 1;
  }});
  v.push_back({"footer backward size short", false, none,
      [](Bytes & z, const Layout & L) {
        z[L.footer + 4] -= 1;
        fix_footer(z, L);
      }});
  v.push_back({"footer backward size long", false, none,
      [](Bytes & z, const Layout & L) {
        z[L.footer + 4] += 1;
        fix_footer(z, L);
      }});
  v.push_back({"footer flags differ", false, none,
      [](Bytes & z, const Layout & L) {
        z[L.footer + 9] = (uint8_t)(z[L.footer + 9] == 1 ? 4 : 1);
        fix_footer(z, L);
      }});
  v.push_back({"footer reserved flag", false, none,
      [](Bytes & z, const Layout & L) {
        z[L.footer + 8] = 1;
        fix_footer(z, L);
      }});
  v.push_back({"footer magic wrong", false, none,
      [](Bytes & z, const Layout &) { z[z.size() - 1] = 'Z'; }});
  v.push_back({"stream header reserved flag byte", false, none,
      [](Bytes & z, const Layout & L) {
        z[6] = 1;
        fix_stream_header(z);
        z[L.footer + 8] = 1;
        fix_footer(z, L);
      }});
  v.push_back({"stream header reserved high nibble", false, none,
      [](Bytes & z, const Layout & L) {
        z[7] |= 0x10;
        fix_stream_header(z);
        z[L.footer + 9] |= 0x10;
        fix_footer(z, L);
      }});
  v.push_back({"stream header CRC wrong", false, none,
      [](Bytes & z, const Layout &) { z[8] ^= 1; }});
  v.push_back({"magic wrong", false, none,
      [](Bytes & z, const Layout &) { z[5] = 1; }});
  for (Variant & x : v) {
    const std::string n = x.name;
    for (const char * tok : {"delta properties", "x86", "arm", "ia64",
             "start offset", "last filter is", "unknown filter",
             "filter id above"}) {
      if (n.find(tok) != std::string::npos) {
        x.zeros_only = true;
      }
    }
  }
  return v;
}

TEST(Xz, ForgedFieldsAreRefusedAsLiblzmaRefusesThemWithEveryCrcRight) {
  Bytes want_rich;
  const Bytes rich = rich_stream(want_rich);
  /* A liblzma stream carries the sizes in its block header; ours does not. */
  const Bytes want_lib = words(3000, 22);
  const Bytes theirs_z = lzmaref::xz_encode(want_lib, 0, lzmaref::kCheckCrc64);
  /* Zeros, in blocks, from this encoder: any filter leaves them as they are. */
  const Bytes want_zero(3000, 0);
  gcomp_options_t * zo = Opts({{"xz.check", "crc32"}},
      {{"xz.preset", 0}, {"xz.block_size", 1100}});
  const XzRun zz = encode(want_zero, zo);
  gcomp_options_destroy(zo);
  ASSERT_EQ(zz.status, GCOMP_OK);
  struct Base {
    const char * name;
    Bytes z;
    Bytes want;
  } bases[] = {{"ours", rich, want_rich}, {"liblzma", theirs_z, want_lib},
      {"zeros", zz.out, want_zero}};
  size_t compared = 0;
  for (const Base & base : bases) {
    for (const Variant & var : variants()) {
      if (var.zeros_only && std::strcmp(base.name, "zeros") != 0) {
        continue;
      }
      Strm s = split(base.z);
      var.model(s);
      Layout L;
      Bytes z = build(s, L);
      var.patch(z, L);
      Bytes a, b;
      const bool ours = ours_accept(z, a);
      const bool theirs = theirs_accept(z, b, base.want.size() + 64);
      compared++;
      EXPECT_EQ(ours, theirs) << base.name << " / " << var.name
                              << ": ours " << (ours ? "accepts" : "refuses")
                              << ", liblzma " << (theirs ? "accepts" : "refuses");
      if (var.valid) {
        EXPECT_TRUE(theirs) << base.name << " / " << var.name
                            << ": the oracle refuses what was meant to be valid";
        EXPECT_TRUE(ours) << base.name << " / " << var.name;
        EXPECT_EQ(a, base.want) << base.name << " / " << var.name;
      }
      if (ours) {
        EXPECT_EQ(a, base.want) << base.name << " / " << var.name
                                << ": decoded other bytes";
      }
    }
  }
  EXPECT_GT(compared, 120u);
}

/// The one field liblzma accepts and this decoder holds to a limit: a block
/// that names a 4 GiB dictionary. The window is what a header can make the
/// decoder reserve, so the default ceiling is xz's 1.5 GiB and a caller who
/// wants more says so.
TEST(Xz, ADictionaryPastTheDefaultWindowIsALimitNotAnError) {
  Bytes want;
  const Bytes rich = rich_stream(want);
  Strm s = split(rich);
  for (auto & b : s.blocks) {
    b.h.filters.back().second = Bytes{40};
  }
  Layout L;
  const Bytes z = build(s, L);
  Bytes b;
  EXPECT_TRUE(theirs_accept(z, b, want.size() + 64));
  XzRun r = decode(z);
  EXPECT_EQ(r.status, GCOMP_ERR_LIMIT) << r.detail;
  gcomp_options_t * o = Opts({}, {{"limits.max_window_bytes", 0xFFFFFFFFll}});
  r = decode(z, o);
  gcomp_options_destroy(o);
  EXPECT_EQ(r.status, GCOMP_OK) << r.detail;
  EXPECT_EQ(r.out, want);
}

/// The dictionary a stream names is the smallest xz can spell that holds the
/// one asked for, and no smaller: a decoder sizes its window by it.
TEST(Xz, TheDictionaryPropertyIsTheSmallestThatHoldsTheDictionary) {
  auto want_prop = [](uint64_t dict) {
    for (unsigned p = 0; p < 40; p++) {
      if (((uint64_t)(2 | (p & 1)) << (p / 2 + 11)) >= dict) {
        return p;
      }
    }
    return 40u;
  };
  const Bytes data = words(5000, 3);
  for (uint64_t dict : {uint64_t(4096), uint64_t(4097), uint64_t(6144),
           uint64_t(8192), uint64_t(65536), uint64_t(65537), uint64_t(98304),
           uint64_t(1) << 26, (uint64_t(1) << 26) + 1}) {
    gcomp_options_t * o = Opts({}, {{"xz.preset", 0},
                                   {"xz.dict_size", (int64_t)dict}});
    XzRun z = encode(data, o);
    gcomp_options_destroy(o);
    ASSERT_EQ(z.status, GCOMP_OK) << dict << ": " << z.detail;
    const Strm s = split(z.out);
    ASSERT_EQ(s.blocks.size(), 1u);
    EXPECT_EQ(s.blocks[0].h.filters.back().second[0], want_prop(dict)) << dict;
    EXPECT_EQ(lzmaref::xz_decode(z.out, data.size()).ret, 1) << dict;
  }
}

TEST(Xz, TheOutputLimitHoldsWhereTheHeaderDeclaresNoSize) {
  /* liblzma's blocks declare their sizes, which the decoder can refuse at the
   * header; ours do not, and the limit has to be found as the bytes appear. */
  const Bytes data = words(200000, 5);
  gcomp_options_t * e = Opts({}, {{"xz.preset", 1}});
  XzRun z = encode(data, e);
  gcomp_options_destroy(e);
  ASSERT_EQ(z.status, GCOMP_OK);
  for (uint64_t lim : {uint64_t(1), uint64_t(1000), uint64_t(199999)}) {
    gcomp_options_t * o = Opts({}, {{"limits.max_output_bytes", (int64_t)lim}});
    XzRun r = decode(z.out, o, 4096, 777);
    gcomp_options_destroy(o);
    EXPECT_EQ(r.status, GCOMP_ERR_LIMIT) << lim << ": " << r.detail;
    EXPECT_LE(r.out.size(), lim);
  }
  gcomp_options_t * o = Opts({}, {{"limits.max_output_bytes", 200000}});
  XzRun r = decode(z.out, o);
  gcomp_options_destroy(o);
  EXPECT_EQ(r.status, GCOMP_OK) << "exactly the limit is allowed: " << r.detail;
}

/* ---- limits ------------------------------------------------------------- */

TEST(Xz, TheLimitsHold) {
  const Bytes data = words(200000, 5);
  const Bytes z = lzmaref::xz_encode(data, 6, lzmaref::kCheckCrc64);
  {
    gcomp_options_t * o = Opts({}, {{"limits.max_output_bytes", 1000}});
    XzRun r = decode(z, o);
    gcomp_options_destroy(o);
    EXPECT_EQ(r.status, GCOMP_ERR_LIMIT) << r.detail;
    EXPECT_TRUE(r.out.empty())
        << "liblzma's blocks declare their size, so the limit is met at the "
           "header and nothing is decoded";
  }
  {
    /* Preset 6 names an 8 MiB dictionary. */
    gcomp_options_t * o = Opts({}, {{"limits.max_window_bytes", 1 << 20}});
    XzRun r = decode(z, o);
    gcomp_options_destroy(o);
    EXPECT_EQ(r.status, GCOMP_ERR_LIMIT) << r.detail;
  }
  {
    gcomp_options_t * o = Opts({}, {{"limits.max_window_bytes", 8 << 20}});
    XzRun r = decode(z, o);
    gcomp_options_destroy(o);
    EXPECT_EQ(r.status, GCOMP_OK) << r.detail;
  }
  {
    const Bytes zeros(4 << 20, 0);
    const Bytes zz = lzmaref::xz_encode(zeros, 6, lzmaref::kCheckCrc64);
    gcomp_options_t * o = Opts({}, {{"limits.max_expansion_ratio", 50}});
    XzRun r = decode(zz, o);
    gcomp_options_destroy(o);
    EXPECT_EQ(r.status, GCOMP_ERR_LIMIT) << r.detail;
  }
}

/* ---- the method contract ------------------------------------------------ */

TEST(Xz, BadOptionsAreRefusedWhereTheyAreSet) {
  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_encoder_t * enc = nullptr;
  for (const char * f : {"mips", "delta:0", "delta:257", "x86,x86,x86,x86",
           "x86:", "x86:99999999999", "delta,", ",x86", "x86 arm"}) {
    gcomp_options_t * o = Opts({{"xz.filters", f}});
    EXPECT_NE(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK) << f;
    gcomp_options_destroy(o);
  }
  gcomp_options_t * o = Opts({{"xz.check", "md5"}});
  EXPECT_NE(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
  o = Opts({}, {{"xz.preset", 10}});
  EXPECT_NE(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
  o = Opts({}, {{"xz.lc", 4}, {"xz.lp", 4}});
  EXPECT_NE(gcomp_encoder_create(reg, "xz", o, &enc), GCOMP_OK)
      << "lc + lp over 4 is not LZMA2";
  gcomp_options_destroy(o);
  EXPECT_EQ(gcomp_encoder_create(reg, "xz", nullptr, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST(Xz, TheBoundCoversEveryStreamTheEncoderWrites) {
  gcomp_registry_t * reg = gcomp_registry_default();
  for (const auto & c : corpus()) {
    for (uint64_t block : {uint64_t(0), uint64_t(4096)}) {
      for (size_t k = 0; k < 4; k++) {
        gcomp_options_t * o = Opts({{"xz.check", kCheckNames[k]},
                                       {"xz.filters", "x86,delta"}},
            {{"xz.preset", 0}, {"xz.block_size", (int64_t)block}});
        size_t bound = 0;
        ASSERT_EQ(gcomp_encode_bound(reg, "xz", o, c.data.size(), &bound),
            GCOMP_OK);
        XzRun z = encode(c.data, o);
        gcomp_options_destroy(o);
        ASSERT_EQ(z.status, GCOMP_OK) << z.detail;
        EXPECT_LE(z.out.size(), bound)
            << c.name << " block " << block << " " << kCheckNames[k];
      }
    }
  }
}

TEST(Xz, DetectAndPeekRecogniseTheHeader) {
  const Bytes z = lzmaref::xz_encode(words(100), 0, lzmaref::kCheckSha256);
  const char * name = nullptr;
  size_t needed = 0;
  ASSERT_EQ(gcomp_detect(z.data(), z.size(), &name, &needed), GCOMP_OK);
  EXPECT_STREQ(name, "xz");
  EXPECT_EQ(gcomp_detect(z.data(), 5, &name, &needed), GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 6u);
  const uint8_t near[] = {0xFD, '7', 'z', 'X', 'Z', 0x01, 0, 0};
  EXPECT_NE(gcomp_detect(near, sizeof(near), &name, &needed), GCOMP_OK)
      << "a sixth byte that is not zero is not an xz stream";

  gcomp_stream_info_t info;
  std::memset(&info, 0, sizeof(info));
  ASSERT_EQ(gcomp_peek(gcomp_registry_default(), "xz", nullptr, z.data(),
                z.size(), &info, &needed),
      GCOMP_OK);
  EXPECT_EQ(info.header_size, 12u);
  EXPECT_TRUE(info.has_checksum);
  const Bytes none = lzmaref::xz_encode(words(100), 0, lzmaref::kCheckNone);
  ASSERT_EQ(gcomp_peek(gcomp_registry_default(), "xz", nullptr, none.data(),
                none.size(), &info, &needed),
      GCOMP_OK);
  EXPECT_FALSE(info.has_checksum);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
