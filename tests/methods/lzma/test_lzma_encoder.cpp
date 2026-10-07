/**
 * @file test_lzma_encoder.cpp
 *
 * The LZMA and LZMA2 encoders: that what they write is a stream, and that it
 * is the stream they were asked for.
 *
 * "Is a stream" has two witnesses. This library's decoder reads it, which
 * shows the two halves agree; and liblzma reads it, which shows they agree
 * with something that was not written alongside them. Each shape is run
 * through both, and where liblzma cannot be asked (an lc of 8 is legal in the
 * format and liblzma refuses to write or read it) the test says so rather
 * than quietly dropping the case.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lzma_oracle.h"
#include "test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzma.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

TEST(LzmaEncoder, LiblzmaIsActuallyAvailable) {
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

uint32_t g_seed = 1;
/* The high half of a linear congruential state: the low bits have short
 * periods, and a "random" block that repeats every 64 KiB is a match. */
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

/// Text, then noise, then the same text again: matches 300000 bytes back.
Bytes far_repeat() {
  Bytes a = noise(300000, 5);
  Bytes b = a;
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

struct Corpus {
  const char * name;
  Bytes data;
};

const std::vector<Corpus> & corpus() {
  static const std::vector<Corpus> c = {
      {"empty", {}},
      {"one", {'x'}},
      {"two", {'x', 'x'}},
      {"hello", {'h', 'e', 'l', 'l', 'o'}},
      {"words3k", words(3000)},
      {"words200k", words(200000)},
      {"noise100k", noise(100000, 3)},
      {"zeros1m", Bytes(1 << 20, 0)},
      {"mixed3m", mixed(3u << 20)},
      {"far", far_repeat()},
  };
  return c;
}

class LzmaEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
    ASSERT_TRUE(lzmaref::lib().ok());
  }

  /// Options with the keys of `method` set from name/value pairs.
  gcomp_options_t * Opts(const char * method,
      std::initializer_list<std::pair<const char *, int64_t>> kv) {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    for (const auto & p : kv) {
      std::string key = std::string(method) + "." + p.first;
      gcomp_status_t s = std::string(p.first) == "raw"
          ? gcomp_options_set_bool(o, key.c_str(), (int)p.second)
          : (std::string(p.first) == "dict_size" ||
                    std::string(p.first) == "uncompressed_size")
              ? gcomp_options_set_uint64(o, key.c_str(), (uint64_t)p.second)
              : gcomp_options_set_int64(o, key.c_str(), p.second);
      EXPECT_EQ(s, GCOMP_OK) << key;
    }
    return o;
  }

  /// Encode with the given input and output windows.
  gcomp_status_t Encode(const char * method, const Bytes & in, Bytes & out,
      size_t in_chunk, size_t out_chunk, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    gcomp_status_t s = gcomp_encoder_create(registry_, method, opts, &enc);
    if (s != GCOMP_OK) {
      return s;
    }
    out.clear();
    Bytes chunk(out_chunk);
    size_t pos = 0;
    while (pos < in.size()) {
      size_t n = std::min(in_chunk, in.size() - pos);
      gcomp_buffer_t ib = {in.data() + pos, n, 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
        size_t before = ib.used;
        s = gcomp_encoder_update(enc, &ib, &ob);
        out.insert(out.end(), chunk.begin(), chunk.begin() + ob.used);
        if (s != GCOMP_OK) {
          Remember(enc);
          gcomp_encoder_destroy(enc);
          return s;
        }
        if (ib.used == before && ob.used == 0) {
          gcomp_encoder_destroy(enc);
          return GCOMP_ERR_INTERNAL; // no progress: the harness's own check
        }
      }
      pos += n;
    }
    for (;;) {
      gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
      s = gcomp_encoder_finish(enc, &ob);
      out.insert(out.end(), chunk.begin(), chunk.begin() + ob.used);
      if (s != GCOMP_ERR_LIMIT) {
        break;
      }
      if (ob.used == 0) {
        s = GCOMP_ERR_INTERNAL;
        break;
      }
    }
    Remember(enc);
    gcomp_encoder_destroy(enc);
    return s;
  }

  gcomp_status_t Decode(const char * method, const Bytes & in, Bytes & out,
      size_t in_chunk, size_t out_chunk, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t s = gcomp_decoder_create(registry_, method, opts, &dec);
    if (s != GCOMP_OK) {
      return s;
    }
    out.clear();
    Bytes chunk(out_chunk);
    size_t pos = 0;
    while (pos < in.size()) {
      size_t n = std::min(in_chunk, in.size() - pos);
      gcomp_buffer_t ib = {in.data() + pos, n, 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
        size_t before = ib.used;
        s = gcomp_decoder_update(dec, &ib, &ob);
        out.insert(out.end(), chunk.begin(), chunk.begin() + ob.used);
        if (s != GCOMP_OK) {
          gcomp_decoder_destroy(dec);
          return s;
        }
        if (ib.used == before && ob.used == 0) {
          gcomp_decoder_destroy(dec);
          return GCOMP_ERR_INTERNAL;
        }
      }
      pos += n;
    }
    for (;;) {
      gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
      s = gcomp_decoder_finish(dec, &ob);
      out.insert(out.end(), chunk.begin(), chunk.begin() + ob.used);
      if (s != GCOMP_ERR_LIMIT || ob.used == 0) {
        break;
      }
    }
    gcomp_decoder_destroy(dec);
    return s;
  }

  void Remember(gcomp_encoder_t * enc) {
    const char * d = gcomp_encoder_get_error_detail(enc);
    detail_ = d ? d : "";
  }

  std::string detail_;
  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(LzmaEncoderTest, OurDecoderReadsWhatTheEncoderWritesAtEveryPreset) {
  for (const char * method : {"lzma", "lzma2"}) {
    for (const auto & c : corpus()) {
      for (int preset : {0, 1, 4, 6}) {
        gcomp_options_t * o =
            Opts(method, {{"preset", preset}});
        Bytes z, out;
        ASSERT_EQ(Encode(method, c.data, z, c.data.size() + 1, 1 << 16, o),
            GCOMP_OK)
            << method << " " << c.name << " preset " << preset;
        gcomp_options_destroy(o);
        ASSERT_EQ(Decode(method, z, out, z.size() + 1, c.data.size() + 64),
            GCOMP_OK)
            << method << " " << c.name << " preset " << preset;
        EXPECT_EQ(out, c.data) << method << " " << c.name << " preset " << preset;
      }
    }
  }
}

TEST_F(LzmaEncoderTest, LiblzmaReadsWhatTheEncoderWrites) {
  for (const auto & c : corpus()) {
    for (int preset : {0, 3, 6}) {
      gcomp_options_t * o = Opts("lzma", {{"preset", preset}});
      Bytes z, out;
      ASSERT_EQ(Encode("lzma", c.data, z, c.data.size() + 1, 1 << 16, o),
          GCOMP_OK);
      gcomp_options_destroy(o);
      ASSERT_TRUE(lzmaref::alone_decode(z, out, c.data.size() + 64))
          << "lzma " << c.name << " preset " << preset;
      EXPECT_EQ(out, c.data) << "lzma " << c.name << " preset " << preset;

      o = Opts("lzma2", {{"preset", preset}});
      ASSERT_EQ(Encode("lzma2", c.data, z, c.data.size() + 1, 1 << 16, o),
          GCOMP_OK);
      gcomp_options_destroy(o);
      ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma2, z,
          lzmaref::options(preset), out, c.data.size() + 64))
          << "lzma2 " << c.name << " preset " << preset;
      EXPECT_EQ(out, c.data) << "lzma2 " << c.name << " preset " << preset;
    }
  }
}


struct Chunk {
  uint8_t control;
  size_t usize;
  size_t csize;
};

/// The chunks of an LZMA2 stream, which must end with the zero byte.
std::vector<Chunk> walk(const Bytes & z) {
  std::vector<Chunk> chunks;
  size_t i = 0;
  while (i < z.size() && z[i] != 0) {
    Chunk c;
    c.control = z[i];
    if (c.control < 0x80) {
      c.usize = (((size_t)z[i + 1] << 8) | z[i + 2]) + 1;
      c.csize = c.usize;
      i += 3 + c.usize;
    }
    else {
      c.usize = (((size_t)(c.control & 0x1F) << 16) | ((size_t)z[i + 1] << 8) |
                    z[i + 2]) + 1;
      c.csize = (((size_t)z[i + 3] << 8) | z[i + 4]) + 1;
      i += (c.control >= 0xC0 ? 6 : 5) + c.csize;
    }
    chunks.push_back(c);
  }
  EXPECT_EQ(i + 1, z.size()) << "the stream does not end at its zero byte";
  return chunks;
}

/// Every lc, lp and pb liblzma will write: the shapes the coder's contexts
/// take. lc and lp pick the literal coder, pb the position state.
TEST_F(LzmaEncoderTest, EveryLcLpPbIsReadByBothDecoders) {
  const Bytes data = words(30000);
  for (int lc = 0; lc <= 4; lc++) {
    for (int lp = 0; lp <= 4 - lc; lp++) {
      for (int pb : {0, 1, 2, 4}) {
        for (const char * method : {"lzma", "lzma2"}) {
          gcomp_options_t * o =
              Opts(method, {{"preset", 1}, {"lc", lc}, {"lp", lp}, {"pb", pb}});
          Bytes z, out, ref;
          ASSERT_EQ(Encode(method, data, z, data.size(), 1 << 16, o), GCOMP_OK)
              << method << " " << lc << lp << pb;
          gcomp_options_destroy(o);
          ASSERT_EQ(Decode(method, z, out, 1000, 4096), GCOMP_OK)
              << method << " " << lc << lp << pb;
          EXPECT_EQ(out, data) << method << " " << lc << lp << pb;
          if (std::string(method) == "lzma") {
            ASSERT_TRUE(lzmaref::alone_decode(z, ref, data.size() + 64))
                << lc << lp << pb;
          }
          else {
            ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma2, z,
                lzmaref::options(1, lc, lp, pb), ref, data.size() + 64))
                << lc << lp << pb;
          }
          EXPECT_EQ(ref, data) << method << " " << lc << lp << pb;
        }
      }
    }
  }
}

/// The format allows lc to 8 and lp to 4, sums to 12; liblzma refuses to write
/// or read those, so this library's decoder is the only witness. LZMA2 caps the
/// sum at 4 and says so at create time.
TEST_F(LzmaEncoderTest, TheWholeRangeOfLcAndLpRoundTripsAndLzma2RefusesTheRest) {
  const Bytes data = words(30000);
  for (auto lclp : {std::pair<int, int>{8, 0}, {0, 4}, {8, 4}, {5, 0}, {4, 1}}) {
    gcomp_options_t * o = Opts("lzma", {{"preset", 0}, {"lc", lclp.first},
                                           {"lp", lclp.second}, {"pb", 4}});
    Bytes z, out;
    ASSERT_EQ(Encode("lzma", data, z, 5000, 4096, o), GCOMP_OK);
    gcomp_options_destroy(o);
    ASSERT_EQ(Decode("lzma", z, out, 5000, 4096), GCOMP_OK)
        << lclp.first << " " << lclp.second;
    EXPECT_EQ(out, data) << lclp.first << " " << lclp.second;
  }
  gcomp_options_t * o = Opts("lzma2", {{"lc", 4}, {"lp", 1}});
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "lzma2", o, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(o);
  o = Opts("lzma2", {{"lc", 4}, {"lp", 0}});
  ASSERT_EQ(gcomp_encoder_create(registry_, "lzma2", o, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(o);
}

/**
 * The window slides, and no match reaches past the dictionary. Six megabytes
 * through a 64 KiB dictionary slides it a hundred times; liblzma then decodes
 * the result with that same 64 KiB, which it enforces, so a match one byte too
 * far is an error there. Data that repeats every 70000 bytes is the control
 * for the bound being used and not only respected: just out of reach of 64 KiB
 * and well inside 128 KiB.
 */
TEST_F(LzmaEncoderTest, TheWindowSlidesAndNoMatchReachesPastTheDictionary) {
  const Bytes data = mixed(6u << 20);
  for (const char * method : {"lzma", "lzma2"}) {
    gcomp_options_t * o = Opts(method, {{"preset", 1}, {"dict_size", 65536}});
    Bytes z, out, ref;
    ASSERT_EQ(Encode(method, data, z, 1 << 20, 1 << 16, o), GCOMP_OK) << method;
    gcomp_options_destroy(o);
    ASSERT_EQ(Decode(method, z, out, 1 << 20, 1 << 20), GCOMP_OK) << method;
    EXPECT_EQ(out, data) << method;
    if (std::string(method) == "lzma") {
      ASSERT_TRUE(lzmaref::alone_decode(z, ref, data.size() + 64));
    }
    else {
      ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma2, z,
          lzmaref::options(1, -1, -1, -1, 65536), ref, data.size() + 64));
    }
    EXPECT_EQ(ref, data) << method;
    /* A window that slid without telling the match finder finds nothing after
     * the first slide and still round-trips, so the size is what shows it. */
    Bytes theirs = lzmaref::alone_encode(
        data, lzmaref::options(1, -1, -1, -1, 65536));
    ASSERT_FALSE(theirs.empty());
    EXPECT_LT(z.size(), theirs.size() * 11 / 10) << method;
  }

  /* Where the match finder's memory matters. Periodic data would not show it:
   * a repeat at one distance is carried across a slide by the repeat-distance
   * slots and never asks a table. This copies 200-byte slices from random
   * places in the last 60000 bytes, each at a distance of its own, so every
   * match is a table lookup; a table that forgets the history at each slide
   * loses a fraction of the matches for 60000 bytes after it. */
  {
    Bytes d = noise(60000, 20);
    g_seed = 123;
    while (d.size() < (6u << 20)) {
      size_t src = d.size() - 59000 + (next_rand() % 58000);
      for (size_t i = 0; i < 200; i++) {
        d.push_back(d[src + i]);
      }
      for (int i = 0; i < 4; i++) {
        d.push_back((uint8_t)next_rand());
      }
    }
    Bytes theirs = lzmaref::alone_encode(
        d, lzmaref::options(1, -1, -1, -1, 65536));
    ASSERT_FALSE(theirs.empty());
    for (const char * method : {"lzma", "lzma2"}) {
      gcomp_options_t * o = Opts(method, {{"preset", 1}, {"dict_size", 65536}});
      Bytes z, out;
      ASSERT_EQ(Encode(method, d, z, 1 << 20, 1 << 16, o), GCOMP_OK);
      gcomp_options_destroy(o);
      ASSERT_EQ(Decode(method, z, out, 1 << 20, 1 << 20), GCOMP_OK);
      EXPECT_EQ(out, d) << method;
      EXPECT_LT(z.size(), theirs.size() * 12 / 10) << method << " " << z.size()
          << " against " << theirs.size();
    }
  }

  Bytes period = noise(70000, 21);
  Bytes rep;
  for (int i = 0; i < 10; i++) {
    rep.insert(rep.end(), period.begin(), period.end());
  }
  size_t small = 0, large = 0;
  for (uint64_t dict : {65536u, 131072u}) {
    gcomp_options_t * o = Opts("lzma", {{"preset", 1}, {"dict_size", (int64_t)dict}});
    Bytes z;
    ASSERT_EQ(Encode("lzma", rep, z, rep.size(), 1 << 16, o), GCOMP_OK);
    gcomp_options_destroy(o);
    (dict == 65536u ? small : large) = z.size();
  }
  EXPECT_GT(small, rep.size() * 9 / 10) << "a match 70000 back fits no 64 KiB window";
  EXPECT_LT(large, rep.size() / 5) << "and fits a 128 KiB one";
}

/// One byte at a time in, one byte at a time out, and the sizes between: the
/// answer does not depend on how the bytes arrive.
TEST_F(LzmaEncoderTest, EveryWindowOfInputAndOutputGivesAStreamThatDecodes) {
  const Bytes data = mixed(40000);
  for (const char * method : {"lzma", "lzma2"}) {
    for (size_t ic : {size_t(1), size_t(7), size_t(4096), size_t(1) << 20}) {
      for (size_t oc : {size_t(1), size_t(3), size_t(100), size_t(1) << 16}) {
        if (ic == 1 && oc == 1) {
          continue; /* 40000 calls each way: the 1-in, 3-out case covers it */
        }
        gcomp_options_t * o = Opts(method, {{"preset", 1}});
        Bytes z, out;
        ASSERT_EQ(Encode(method, data, z, ic, oc, o), GCOMP_OK)
            << method << " in " << ic << " out " << oc;
        gcomp_options_destroy(o);
        ASSERT_EQ(Decode(method, z, out, 1 << 20, 1 << 20), GCOMP_OK)
            << method << " in " << ic << " out " << oc;
        EXPECT_EQ(out, data) << method << " in " << ic << " out " << oc;
      }
    }
  }
}

/// Coding that does not shrink a chunk falls back to storing it, and what
/// follows has to say that the decoder's state was not moved by the attempt.
TEST_F(LzmaEncoderTest, IncompressibleDataIsStoredAndTheNextCodedChunkResetsState) {
  const Bytes data = noise(300000, 31);
  Bytes z, out;
  ASSERT_EQ(Encode("lzma2", data, z, data.size(), 1 << 16), GCOMP_OK);
  auto chunks = walk(z);
  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(chunks[0].control, 0x01) << "the first chunk resets the dictionary";
  size_t stored = 0;
  for (size_t i = 0; i < chunks.size(); i++) {
    EXPECT_LT(chunks[i].control, 0x80) << "chunk " << i << " should be stored";
    if (i > 0) {
      EXPECT_EQ(chunks[i].control, 0x02);
    }
    stored += chunks[i].usize;
  }
  EXPECT_EQ(stored, data.size());
  EXPECT_EQ(z.size(), data.size() + 3 * chunks.size() + 1);
  ASSERT_EQ(Decode("lzma2", z, out, z.size(), 1 << 20), GCOMP_OK);
  EXPECT_EQ(out, data);

  /* Noise, then text: the first chunk is stored with a dictionary reset, so
   * the first coded chunk has to carry properties (0xC0), and a coded chunk
   * after any stored one has to reset state (0xA0 or more). */
  Bytes both = noise(70000, 32);
  Bytes text = words(400000);
  both.insert(both.end(), text.begin(), text.end());
  both.insert(both.end(), data.begin(), data.begin() + 100000);
  both.insert(both.end(), text.begin(), text.begin() + 200000);
  ASSERT_EQ(Encode("lzma2", both, z, 1 << 20, 1 << 16), GCOMP_OK);
  chunks = walk(z);
  bool saw_props_after_stored_first = false;
  bool saw_reset_after_stored = false;
  for (size_t i = 1; i < chunks.size(); i++) {
    if (chunks[i].control >= 0x80 && chunks[i - 1].control < 0x80) {
      EXPECT_GE(chunks[i].control, 0xA0) << "chunk " << i;
      saw_reset_after_stored = true;
      if (i == 1 || chunks[i - 1].control == 0x01) {
        EXPECT_GE(chunks[i].control, 0xC0) << "chunk " << i;
        saw_props_after_stored_first = true;
      }
    }
  }
  EXPECT_TRUE(saw_reset_after_stored);
  EXPECT_TRUE(saw_props_after_stored_first);
  ASSERT_EQ(Decode("lzma2", z, out, 100, 4096), GCOMP_OK);
  EXPECT_EQ(out, both);
  Bytes ref;
  ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma2, z, lzmaref::options(6),
      ref, both.size() + 64));
  EXPECT_EQ(ref, both);
}

/// A chunk codes at most 2 MiB and 64 KiB of output; zeros hit the first.
TEST_F(LzmaEncoderTest, ChunksStayInsideTheFormatsLimits) {
  for (const auto & c : corpus()) {
    if (c.data.empty()) {
      continue;
    }
    Bytes z;
    ASSERT_EQ(Encode("lzma2", c.data, z, 1 << 20, 1 << 16), GCOMP_OK) << c.name;
    for (const auto & ch : walk(z)) {
      EXPECT_LE(ch.usize, (size_t)1 << 21) << c.name;
      /* Short of the format's 65536 by more than one symbol can write: the
       * coder stops with room in hand, since the symbol that crosses the line
       * is already coded. */
      EXPECT_LE(ch.csize, ((size_t)1 << 16) - 64) << c.name;
      EXPECT_GE(ch.usize, 1u) << c.name;
    }
  }
}

TEST_F(LzmaEncoderTest, ASyncFlushLeavesEveryByteDecodableAtEachPoint) {
  const Bytes data = mixed(90000);
  gcomp_encoder_t * enc = nullptr;
  gcomp_options_t * o = Opts("lzma2", {{"preset", 1}});
  ASSERT_EQ(gcomp_encoder_create(registry_, "lzma2", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
  Bytes z;
  size_t fed = 0;
  for (size_t piece : {1u, 4999u, 20000u, 1u, 30000u, 35000u}) {
    gcomp_buffer_t ib = {data.data() + fed, piece, 0};
    Bytes buf(1 << 17);
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(ib.used, piece);
    ASSERT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC), GCOMP_OK);
    z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
    fed += piece;

    /* What is emitted so far, with no end byte, decodes to what was fed. */
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "lzma2", nullptr, &dec), GCOMP_OK);
    Bytes got(fed + 64);
    gcomp_buffer_t di = {z.data(), z.size(), 0};
    gcomp_buffer_t dob = {got.data(), got.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &di, &dob), GCOMP_OK);
    gcomp_decoder_destroy(dec);
    got.resize(dob.used);
    ASSERT_EQ(got, Bytes(data.begin(), data.begin() + fed)) << "after " << fed;
  }
  gcomp_encoder_destroy(enc);

  /* A flush with nothing pending writes nothing. */
  ASSERT_EQ(gcomp_encoder_create(registry_, "lzma2", nullptr, &enc), GCOMP_OK);
  Bytes buf(100);
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  EXPECT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC), GCOMP_OK);
  EXPECT_EQ(ob.used, 0u);
  gcomp_encoder_destroy(enc);
}

/// A full flush forgets the history: what follows it decodes by itself.
TEST_F(LzmaEncoderTest, AFullFlushLetsTheSecondHalfBeReadAlone) {
  const Bytes a = words(60000), b = words(60000, 8);
  for (const Bytes & second : {b, noise(5000, 77)}) {
    gcomp_encoder_t * enc = nullptr;
    gcomp_options_t * o = Opts("lzma2", {{"preset", 1}});
    ASSERT_EQ(gcomp_encoder_create(registry_, "lzma2", o, &enc), GCOMP_OK);
    gcomp_options_destroy(o);
    Bytes buf(1 << 18), z1, z2;
    gcomp_buffer_t ib = {a.data(), a.size(), 0};
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_FULL), GCOMP_OK);
    z1.assign(buf.begin(), buf.begin() + ob.used);
    ib = {second.data(), second.size(), 0};
    ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    z2.assign(buf.begin(), buf.begin() + ob.used);
    gcomp_encoder_destroy(enc);

    EXPECT_TRUE(z2[0] == 0xE0 || z2[0] == 0x01)
        << "the second half begins with a dictionary reset, not " << (int)z2[0];
    Bytes out;
    ASSERT_EQ(Decode("lzma2", z2, out, z2.size(), 1 << 20), GCOMP_OK);
    EXPECT_EQ(out, second);
    Bytes whole = z1;
    whole.insert(whole.end(), z2.begin(), z2.end());
    ASSERT_EQ(Decode("lzma2", whole, out, 4096, 4096), GCOMP_OK);
    Bytes both = a;
    both.insert(both.end(), second.begin(), second.end());
    EXPECT_EQ(out, both);
  }
}

TEST_F(LzmaEncoderTest, OnlyLzma2CanFlush) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lzma", nullptr, &enc), GCOMP_OK);
  Bytes buf(100);
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  EXPECT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_UNSUPPORTED);
  gcomp_encoder_destroy(enc);
}

/// A reset encoder is a fresh one: the same input gives the same bytes.
TEST_F(LzmaEncoderTest, ResetEqualsAFreshEncoder) {
  const Bytes a = mixed(300000), b = words(250000, 3);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes fresh;
    ASSERT_EQ(Encode(method, b, fresh, b.size(), 1 << 16), GCOMP_OK);

    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, method, nullptr, &enc), GCOMP_OK);
    Bytes buf(1 << 20), z;
    /* Abandon a stream half way, then reset and encode b. */
    gcomp_buffer_t ib = {a.data(), a.size() / 2, 0};
    gcomp_buffer_t ob = {buf.data(), 100, 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
    ib = {b.data(), b.size(), 0};
    ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    z.assign(buf.begin(), buf.begin() + ob.used);
    EXPECT_EQ(z, fresh) << method;

    /* And after finish. */
    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
    ib = {b.data(), b.size(), 0};
    ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    EXPECT_EQ(Bytes(buf.begin(), buf.begin() + ob.used), fresh) << method;
    gcomp_encoder_destroy(enc);
  }
}

TEST_F(LzmaEncoderTest, UpdateAfterFinishIsRefused) {
  for (const char * method : {"lzma", "lzma2"}) {
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, method, nullptr, &enc), GCOMP_OK);
    Bytes buf(100);
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    uint8_t x = 1;
    gcomp_buffer_t ib = {&x, 1, 0};
    ob = {buf.data(), buf.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_ERR_INVALID_ARG)
        << method;
    gcomp_encoder_destroy(enc);
  }
}

/// A declared size goes in the header, ends the stream without a marker, and
/// is held to: more input or less is the caller's error.
TEST_F(LzmaEncoderTest, ADeclaredSizeIsWrittenKeptAndHeldTo) {
  const Bytes data = words(50000);
  gcomp_options_t * o = Opts("lzma", {{"preset", 1},
      {"uncompressed_size", (int64_t)data.size()}});
  Bytes z, out, ref;
  ASSERT_EQ(Encode("lzma", data, z, data.size(), 1 << 16, o), GCOMP_OK);
  uint64_t size = 0;
  for (int i = 0; i < 8; i++) {
    size |= (uint64_t)z[5 + i] << (8 * i);
  }
  EXPECT_EQ(size, data.size());
  ASSERT_EQ(Decode("lzma", z, out, 1000, 4096), GCOMP_OK);
  EXPECT_EQ(out, data);
  ASSERT_TRUE(lzmaref::alone_decode(z, ref, data.size() + 64));
  EXPECT_EQ(ref, data);
  /* No marker: unstated, the same data is longer by about the marker's six
   * bytes. */
  Bytes unstated;
  gcomp_options_t * p = Opts("lzma", {{"preset", 1}});
  ASSERT_EQ(Encode("lzma", data, unstated, data.size(), 1 << 16, p),
      GCOMP_OK);
  gcomp_options_destroy(p);
  EXPECT_LT(z.size(), unstated.size());

  Bytes shorter(data.begin(), data.end() - 1);
  EXPECT_EQ(Encode("lzma", shorter, z, shorter.size(), 1 << 16, o),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_NE(detail_.find("lzma.uncompressed_size"), std::string::npos) << detail_;
  Bytes longer = data;
  longer.push_back('x');
  EXPECT_EQ(Encode("lzma", longer, z, longer.size(), 1 << 16, o),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_NE(detail_.find("lzma.uncompressed_size"), std::string::npos) << detail_;
  gcomp_options_destroy(o);
}

/// Raw: the stream alone, which is what zip method 14 and 7z hold.
TEST_F(LzmaEncoderTest, ARawStreamHasNoHeaderAndLiblzmaReadsItWithTheProperties) {
  const Bytes data = mixed(200000);
  for (int lc : {0, 3, 4}) {
    gcomp_options_t * o = Opts("lzma",
        {{"raw", 1}, {"preset", 3}, {"lc", lc}, {"lp", 0}, {"pb", 2}});
    Bytes z, out, ref;
    ASSERT_EQ(Encode("lzma", data, z, data.size(), 1 << 16, o), GCOMP_OK);
    gcomp_options_destroy(o);
    EXPECT_EQ(z[0], 0) << "a range coder's first byte is zero, not a header";

    gcomp_options_t * d = Opts("lzma", {{"raw", 1}, {"lc", lc}, {"lp", 0},
        {"pb", 2}, {"dict_size", 1 << 22}});
    ASSERT_EQ(Decode("lzma", z, out, 777, 4096, d), GCOMP_OK) << lc;
    gcomp_options_destroy(d);
    EXPECT_EQ(out, data) << lc;
    ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma1, z,
        lzmaref::options(3, lc, 0, 2, 1 << 22), ref, data.size() + 64))
        << lc;
    EXPECT_EQ(ref, data) << lc;
  }
}

TEST_F(LzmaEncoderTest, TheHeaderCarriesThePropertiesAndTheDictionary) {
  const Bytes data = words(1000);
  gcomp_options_t * o = Opts("lzma",
      {{"preset", 0}, {"lc", 2}, {"lp", 1}, {"pb", 3}, {"dict_size", 100000}});
  Bytes z;
  ASSERT_EQ(Encode("lzma", data, z, data.size(), 1 << 16, o), GCOMP_OK);
  gcomp_options_destroy(o);
  EXPECT_EQ(z[0], (3 * 5 + 1) * 9 + 2);
  EXPECT_EQ((uint32_t)z[1] | (uint32_t)z[2] << 8 | (uint32_t)z[3] << 16 |
                (uint32_t)z[4] << 24,
      100000u);
  gcomp_stream_info_t info;
  std::memset(&info, 0, sizeof(info));
  size_t needed = 0;
  ASSERT_EQ(gcomp_peek(registry_, "lzma", nullptr, z.data(), z.size(), &info,
                &needed),
      GCOMP_OK);
  EXPECT_EQ(info.window_size, 100000u);
  EXPECT_EQ(info.header_size, 13u);

  /* With no dict_size the preset's is written: 256 KiB at 0, 8 MiB at 6. */
  for (auto pd : {std::pair<int, uint32_t>{0, 1u << 18}, {6, 1u << 23}}) {
    gcomp_options_t * q = Opts("lzma", {{"preset", pd.first}});
    ASSERT_EQ(Encode("lzma", data, z, data.size(), 1 << 16, q), GCOMP_OK);
    gcomp_options_destroy(q);
    EXPECT_EQ((uint32_t)z[1] | (uint32_t)z[2] << 8 | (uint32_t)z[3] << 16 |
                  (uint32_t)z[4] << 24,
        pd.second)
        << "preset " << pd.first;
  }
}

TEST_F(LzmaEncoderTest, TheBoundCoversEveryStreamAndOnlyLzma2HasOne) {
  for (const auto & c : corpus()) {
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(registry_, "lzma2", nullptr, c.data.size(),
                  &bound),
        GCOMP_OK);
    Bytes z;
    ASSERT_EQ(Encode("lzma2", c.data, z, c.data.size() + 1, 1 << 16), GCOMP_OK);
    EXPECT_LE(z.size(), bound) << c.name;
  }
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(registry_, "lzma", nullptr, 100, &bound),
      GCOMP_ERR_UNSUPPORTED);
  /* The allocating helper rides on the bound, so it works for lzma2 only. */
  const Bytes data = words(5000);
  void * out = nullptr;
  size_t out_size = 0;
  EXPECT_EQ(gcomp_encode_alloc(registry_, "lzma", nullptr, data.data(),
                data.size(), &out, &out_size),
      GCOMP_ERR_UNSUPPORTED);
  ASSERT_EQ(gcomp_encode_alloc(registry_, "lzma2", nullptr, data.data(),
                data.size(), &out, &out_size),
      GCOMP_OK);
  Bytes z((uint8_t *)out, (uint8_t *)out + out_size), back;
  ASSERT_EQ(Decode("lzma2", z, back, z.size(), 1 << 16), GCOMP_OK);
  EXPECT_EQ(back, data);
  gcomp_buffer_free(registry_, out);
}

/**
 * How close the fast parser gets to liblzma, which at its higher presets
 * parses optimally. The ceilings are measured with a margin, so that they fail
 * on a regression and not on noise, and the one that is wide says so: preset 6
 * on text is the gap an optimal parser exists to close.
 */
TEST_F(LzmaEncoderTest, TheRatioStaysNearLiblzmaAtTheFastPresets) {
  struct Case {
    const char * name;
    int preset;
    double ceiling; /* ours over liblzma's */
  };
  const std::vector<Case> cases = {
      {"words200k", 0, 1.05},
      {"words200k", 1, 1.03},
      {"mixed3m", 0, 1.03},
      {"mixed3m", 1, 1.03},
      {"words200k", 6, 1.45},
      {"mixed3m", 6, 1.08},
      {"noise100k", 1, 1.001},
      {"zeros1m", 1, 1.05},
      {"far", 1, 1.01},
  };
  for (const auto & k : cases) {
    const Bytes * data = nullptr;
    for (const auto & c : corpus()) {
      if (std::string(c.name) == k.name) {
        data = &c.data;
      }
    }
    ASSERT_NE(data, nullptr) << k.name;
    gcomp_options_t * o = Opts("lzma", {{"preset", k.preset}});
    Bytes z;
    ASSERT_EQ(Encode("lzma", *data, z, data->size(), 1 << 16, o), GCOMP_OK);
    gcomp_options_destroy(o);
    Bytes ref = lzmaref::alone_encode(*data, lzmaref::options(k.preset));
    ASSERT_FALSE(ref.empty());
    EXPECT_LE((double)z.size(), k.ceiling * (double)ref.size())
        << k.name << " preset " << k.preset << ": " << z.size() << " against "
        << ref.size();
  }
}

/**
 * The stream depends on the bytes and not on how they were handed over: where
 * a batch is cut is a function of how many bytes have arrived. A caller can
 * therefore compare, cache or deduplicate output without fixing its buffer
 * sizes first.
 */
TEST_F(LzmaEncoderTest, TheStreamDoesNotDependOnHowTheInputArrives) {
  const Bytes data = mixed(900000);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes whole;
    ASSERT_EQ(Encode(method, data, whole, data.size(), 1 << 16), GCOMP_OK);
    for (size_t ic : {size_t(131072), size_t(131345), size_t(4096), size_t(777)}) {
      Bytes z;
      ASSERT_EQ(Encode(method, data, z, ic, 1 << 16), GCOMP_OK);
      EXPECT_EQ(z, whole) << method << " in " << ic;
    }
  }
}

/// lzma has no bound, so the shared detect test cannot encode it; the property
/// it checks is checked here: a .lzma file is not mistaken for a format with a
/// magic number.
TEST_F(LzmaEncoderTest, DetectDoesNotClaimAnLzmaFile) {
  const Bytes data = words(5000);
  Bytes z;
  ASSERT_EQ(Encode("lzma", data, z, data.size(), 1 << 16), GCOMP_OK);
  const char * name = nullptr;
  gcomp_status_t s = gcomp_detect(z.data(), z.size(), &name, nullptr);
  if (s == GCOMP_OK) {
    EXPECT_STREQ(name, "zlib") << "an lzma stream detected as " << name;
  }
  else {
    EXPECT_EQ(s, GCOMP_ERR_UNSUPPORTED);
  }
}

/**
 * The ceiling is the format's, and the encoder gets close to it: 32 MiB of
 * zeros is the cheapest stream there is, and both must sit under the constant
 * the decoders default to and not far below it.
 */
TEST_F(LzmaEncoderTest, TheExpansionCeilingIsAboveWhatTheEncoderReachesAndNearIt) {
  const Bytes zeros(32u << 20, 0);
  for (const char * method : {"lzma", "lzma2"}) {
    gcomp_options_t * o = Opts(method, {{"preset", 1}});
    Bytes z, out;
    ASSERT_EQ(Encode(method, zeros, z, zeros.size(), 1 << 16, o), GCOMP_OK);
    gcomp_options_destroy(o);
    const double ratio = (double)zeros.size() / (double)z.size();
    EXPECT_LT(ratio, (double)GCOMP_LZMA_MAX_EXPANSION_RATIO) << method;
    /* lzma2 pays a chunk header and a coder flush for every 2 MiB. */
    const double floor_fraction = std::string(method) == "lzma" ? 0.8 : 0.4;
    EXPECT_GT(ratio, GCOMP_LZMA_MAX_EXPANSION_RATIO * floor_fraction)
        << method << " " << ratio;
    ASSERT_EQ(Decode(method, z, out, z.size(), 1 << 20), GCOMP_OK) << method;
    EXPECT_EQ(out.size(), zeros.size());
  }
}

TEST_F(LzmaEncoderTest, TheSameInputGivesTheSameBytes) {
  const Bytes data = mixed(500000);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes a, b;
    ASSERT_EQ(Encode(method, data, a, data.size(), 1 << 16), GCOMP_OK);
    ASSERT_EQ(Encode(method, data, b, data.size(), 1 << 16), GCOMP_OK);
    EXPECT_EQ(a, b) << method;
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
