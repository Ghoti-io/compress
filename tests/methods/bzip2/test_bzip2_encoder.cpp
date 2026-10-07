/**
 * @file test_bzip2_encoder.cpp
 *
 * The bzip2 decoder against libbz2.
 *
 * Everything libbz2 writes, this decoder has to read, and read the same way
 * however the bytes arrive: the bit half of the decoder is a state machine
 * that stops wherever its input does, and the only way to reach every one of
 * those stopping places is to feed a stream one byte at a time. The output
 * half parks a run of repeated bytes when the caller's buffer fills, which one
 * byte out at a time reaches.
 *
 * The refusals are pinned by the detail they give. A malformed stream is also
 * truncated or has a CRC that does not match, so a status alone cannot say
 * which check fired; most of the cases are built bit by bit so that exactly
 * one thing is wrong, and each is decoded by libbz2 too, because a stream the
 * test invented proves nothing until something else agrees it is one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "bzip2_oracle.h"
#include "test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/bzip2.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

TEST(Bzip2Encoder, Libbz2IsActuallyAvailable) {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }
  }
  ASSERT_TRUE(bzref::lib().ok())
      << "libbz2.so.1.0 could not be loaded. The pinned copy is in the oracle "
         "image; `make check-oracle` runs this binary there. Set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to skip this on a machine without it.";
}

uint32_t g_seed = 1;
/* The high half of a linear congruential state: the low bits have short
 * periods, and "random" data that repeats every 64 KiB is not random. */
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

class Bzip2EncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
    ASSERT_TRUE(bzref::lib().ok());
  }

  gcomp_options_t * Level(int64_t level) {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(o, "bzip2.level", level), GCOMP_OK);
    return o;
  }

  gcomp_status_t Encode(const Bytes & in, Bytes & out, size_t in_chunk,
      size_t out_chunk, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    gcomp_status_t s = gcomp_encoder_create(registry_, "bzip2", opts, &enc);
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
    gcomp_encoder_destroy(enc);
    return s;
  }

  gcomp_status_t Decode(const Bytes & in, Bytes & out, size_t in_chunk,
      size_t out_chunk) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t s = gcomp_decoder_create(registry_, "bzip2", nullptr, &dec);
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

  gcomp_registry_t * registry_ = nullptr;
};

struct Corpus {
  const char * name;
  Bytes data;
};

/// Runs of one byte at the lengths where the run-length pass changes shape,
/// and at the 255 where this encoder cuts a run and starts another.
Bytes runs() {
  Bytes out;
  for (size_t len : {1u, 2u, 3u, 4u, 5u, 6u, 254u, 255u, 256u, 258u, 259u, 260u,
           261u, 510u, 511u, 512u, 1000u}) {
    out.insert(out.end(), len, (uint8_t)('a' + len % 7));
    out.push_back('|');
  }
  out.insert(out.end(), 10, 'x');
  out.insert(out.end(), 3, 'x');
  out.insert(out.end(), 4, 'y');
  out.insert(out.end(), 4, 'y');
  return out;
}

const std::vector<Corpus> & corpus() {
  static const std::vector<Corpus> c = {
      {"empty", {}},
      {"one", {'x'}},
      {"hello", {'h', 'e', 'l', 'l', 'o'}},
      {"runs", runs()},
      {"words3k", words(3000)},
      {"words300k", words(300000)},
      {"noise100k", noise(100000, 3)},
      {"zeros1m", Bytes(1 << 20, 0)},
      {"mixed3m", mixed(3u << 20)},
  };
  return c;
}

TEST_F(Bzip2EncoderTest, OurDecoderReadsWhatTheEncoderWritesAtEveryLevel) {
  for (const auto & c : corpus()) {
    for (int level : {1, 2, 5, 9}) {
      gcomp_options_t * o = Level(level);
      Bytes z, out;
      ASSERT_EQ(Encode(c.data, z, c.data.size() + 1, 1 << 16, o), GCOMP_OK)
          << c.name << " level " << level;
      gcomp_options_destroy(o);
      ASSERT_EQ(Decode(z, out, z.size() + 1, c.data.size() + 64), GCOMP_OK)
          << c.name << " level " << level;
      EXPECT_EQ(out, c.data) << c.name << " level " << level;
    }
  }
}

TEST_F(Bzip2EncoderTest, Libbz2ReadsWhatTheEncoderWrites) {
  for (const auto & c : corpus()) {
    for (int level : {1, 2, 5, 9}) {
      gcomp_options_t * o = Level(level);
      Bytes z, out;
      ASSERT_EQ(Encode(c.data, z, c.data.size() + 1, 1 << 16, o), GCOMP_OK);
      gcomp_options_destroy(o);
      ASSERT_TRUE(bzref::decompress(z, out, c.data.size() + 64))
          << c.name << " level " << level;
      EXPECT_EQ(out, c.data) << c.name << " level " << level;
    }
  }
}

/// A block holds level * 100000 - 19 bytes after the first pass. Run-heavy
/// data of lengths straddling that, so a run is cut by the block's end at
/// every possible place, is the shape where block and run bookkeeping meet.
TEST_F(Bzip2EncoderTest, BlockBoundariesFallWhereverARunIs) {
  const size_t block = 100000 - 19;
  for (size_t extra : {0u, 1u, 3u, 4u, 5u, 6u, 18u, 19u, 20u, 21u, 300u}) {
    for (int shape = 0; shape < 3; shape++) {
      Bytes data;
      const size_t want = block + extra - 12;
      while (data.size() < want) {
        switch (shape) {
        case 0: data.insert(data.end(), 255, 'a'); data.push_back('b'); break;
        case 1: data.insert(data.end(), 4, 'a'); data.push_back('b'); break;
        default: data.insert(data.end(), 7, (uint8_t)('a' + data.size() % 3)); break;
        }
      }
      gcomp_options_t * o = Level(1);
      Bytes z, out, ref;
      ASSERT_EQ(Encode(data, z, data.size(), 1 << 16, o), GCOMP_OK);
      gcomp_options_destroy(o);
      ASSERT_EQ(Decode(z, out, z.size(), data.size() + 64), GCOMP_OK)
          << "extra " << extra << " shape " << shape;
      EXPECT_EQ(out, data) << "extra " << extra << " shape " << shape;
      ASSERT_TRUE(bzref::decompress(z, ref, data.size() + 64))
          << "extra " << extra << " shape " << shape;
      EXPECT_EQ(ref, data);
    }
  }
}

/// An empty input is the stream libbz2 writes for one, byte for byte.
TEST_F(Bzip2EncoderTest, NothingInIsTheStreamLibbz2WritesForNothing) {
  for (int level : {1, 9}) {
    gcomp_options_t * o = Level(level);
    Bytes z;
    ASSERT_EQ(Encode({}, z, 1, 100, o), GCOMP_OK);
    gcomp_options_destroy(o);
    EXPECT_EQ(z, bzref::compress({}, level)) << "level " << level;
    EXPECT_EQ(z.size(), 14u);
  }
}

/// The stream depends on the bytes, not on how they are handed over.
TEST_F(Bzip2EncoderTest, TheStreamDoesNotDependOnHowTheInputArrives) {
  const Bytes data = mixed(500000);
  gcomp_options_t * o = Level(1);
  Bytes whole;
  ASSERT_EQ(Encode(data, whole, data.size(), 1 << 16, o), GCOMP_OK);
  for (size_t ic : {size_t(1) << 20, size_t(131345), size_t(4096), size_t(777)}) {
    for (size_t oc : {size_t(1) << 16, size_t(100), size_t(3)}) {
      Bytes z;
      ASSERT_EQ(Encode(data, z, ic, oc, o), GCOMP_OK);
      EXPECT_EQ(z, whole) << "in " << ic << " out " << oc;
    }
  }
  gcomp_options_destroy(o);
}

TEST_F(Bzip2EncoderTest, TheLevelIsInTheHeaderAndNineIsTheDefault) {
  const Bytes data = words(1000);
  Bytes z;
  ASSERT_EQ(Encode(data, z, data.size(), 1 << 16), GCOMP_OK);
  EXPECT_EQ(Bytes(z.begin(), z.begin() + 4), (Bytes{'B', 'Z', 'h', '9'}));
  for (int level = 1; level <= 9; level++) {
    gcomp_options_t * o = Level(level);
    ASSERT_EQ(Encode(data, z, data.size(), 1 << 16, o), GCOMP_OK);
    gcomp_options_destroy(o);
    EXPECT_EQ(z[3], '0' + level);
  }
}

/**
 * The same coding libbz2 does, so the size should be the same to within the
 * few bytes that choosing the tables a little differently costs. A ceiling
 * that is tight is what notices a regression in the table search, the
 * move-to-front pass or the run-length pass, none of which affect correctness.
 */
TEST_F(Bzip2EncoderTest, TheSizeIsLibbz2sToWithinAFewBytes) {
  for (const auto & c : corpus()) {
    for (int level : {1, 9}) {
      gcomp_options_t * o = Level(level);
      Bytes z;
      ASSERT_EQ(Encode(c.data, z, c.data.size() + 1, 1 << 16, o), GCOMP_OK);
      gcomp_options_destroy(o);
      Bytes ref = bzref::compress(c.data, level);
      ASSERT_FALSE(ref.empty());
      EXPECT_LE((double)z.size(), 1.01 * (double)ref.size() + 4.0)
          << c.name << " level " << level << ": " << z.size() << " against "
          << ref.size();
    }
  }
}

/// Every length from none to a few hundred, at three levels, through both
/// decoders: the sizes where an alphabet has one or two symbols, where a table
/// has all its codes of one length, and where there is one selector.
TEST_F(Bzip2EncoderTest, EveryShortInputRoundTrips) {
  for (size_t len = 0; len <= 300; len++) {
    for (int shape = 0; shape < 3; shape++) {
      Bytes data = shape == 0 ? noise(len, (uint32_t)len + 1)
          : shape == 1        ? Bytes(len, 'q')
                              : words(len, (uint32_t)len + 5);
      for (int level : {1, 9}) {
        gcomp_options_t * o = Level(level);
        Bytes z, out, ref;
        ASSERT_EQ(Encode(data, z, 64, 1 << 12, o), GCOMP_OK);
        gcomp_options_destroy(o);
        ASSERT_EQ(Decode(z, out, z.size() + 1, len + 64), GCOMP_OK)
            << "len " << len << " shape " << shape;
        ASSERT_EQ(out, data) << "len " << len << " shape " << shape;
        ASSERT_TRUE(bzref::decompress(z, ref, len + 64))
            << "len " << len << " shape " << shape;
        ASSERT_EQ(ref, data);
      }
    }
  }
}

/// A flush ends the stream so every byte fed decodes, and the next byte fed
/// begins another: the bytes are concatenated streams.
TEST_F(Bzip2EncoderTest, AFlushLeavesEveryByteDecodableAndTheNextInputStartsAStream) {
  const Bytes data = mixed(120000);
  gcomp_encoder_t * enc = nullptr;
  gcomp_options_t * o = Level(1);
  ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
  Bytes z;
  std::vector<size_t> seams;
  size_t fed = 0;
  for (size_t piece : {1u, 4999u, 20000u, 1u, 30000u, 65000u}) {
    gcomp_buffer_t ib = {data.data() + fed, piece, 0};
    Bytes buf(1 << 18);
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(ib.used, piece);
    ASSERT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC), GCOMP_OK);
    z.insert(z.end(), buf.begin(), buf.begin() + ob.used);
    seams.push_back(z.size());
    fed += piece;

    /* What is emitted so far, with no finish, decodes to what was fed. */
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "bzip2", nullptr, &dec), GCOMP_OK);
    Bytes got(fed + 64);
    gcomp_buffer_t di = {z.data(), z.size(), 0};
    gcomp_buffer_t dob = {got.data(), got.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &di, &dob), GCOMP_OK);
    ASSERT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK) << "after " << fed;
    gcomp_decoder_destroy(dec);
    got.resize(dob.used);
    ASSERT_EQ(got, Bytes(data.begin(), data.begin() + fed)) << "after " << fed;
  }
  /* Finish after a flush with nothing new: no empty stream is added. */
  Bytes buf(100);
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_EQ(ob.used, 0u);
  gcomp_encoder_destroy(enc);

  /* Each flushed segment is a stream of its own, which libbz2 reads. */
  size_t from = 0, data_from = 0;
  size_t i = 0;
  for (size_t piece : {1u, 4999u, 20000u, 1u, 30000u, 65000u}) {
    Bytes seg(z.begin() + from, z.begin() + seams[i]), ref;
    ASSERT_TRUE(bzref::decompress(seg, ref, piece + 64)) << "segment " << i;
    EXPECT_EQ(ref, Bytes(data.begin() + data_from, data.begin() + data_from + piece))
        << "segment " << i;
    from = seams[i++];
    data_from += piece;
  }
}

TEST_F(Bzip2EncoderTest, FlushingWithNothingPendingWritesNothingAndFullIsSync) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", nullptr, &enc), GCOMP_OK);
  Bytes buf(100);
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  EXPECT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_FULL), GCOMP_OK);
  EXPECT_EQ(ob.used, 0u);
  gcomp_encoder_destroy(enc);

  const Bytes data = words(5000);
  Bytes outs[2];
  for (int m = 0; m < 2; m++) {
    ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", nullptr, &enc), GCOMP_OK);
    Bytes b(1 << 16);
    gcomp_buffer_t ib = {data.data(), data.size(), 0};
    gcomp_buffer_t o2 = {b.data(), b.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &ib, &o2), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_flush(
                  enc, &o2, m == 0 ? GCOMP_FLUSH_SYNC : GCOMP_FLUSH_FULL),
        GCOMP_OK);
    outs[m].assign(b.begin(), b.begin() + o2.used);
    gcomp_encoder_destroy(enc);
  }
  EXPECT_EQ(outs[0], outs[1]);
  EXPECT_FALSE(outs[0].empty());
}

/// A flush that the output cannot hold is finished on the next call.
TEST_F(Bzip2EncoderTest, AFlushThroughATinyOutputBufferResumes) {
  const Bytes data = words(30000);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", nullptr, &enc), GCOMP_OK);
  Bytes z, tiny(5);
  gcomp_buffer_t ib = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {tiny.data(), tiny.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
  z.insert(z.end(), tiny.begin(), tiny.begin() + ob.used);
  for (;;) {
    ob = {tiny.data(), tiny.size(), 0};
    gcomp_status_t s = gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC);
    z.insert(z.end(), tiny.begin(), tiny.begin() + ob.used);
    if (s == GCOMP_OK) {
      break;
    }
    ASSERT_EQ(s, GCOMP_ERR_LIMIT);
    ASSERT_GT(ob.used, 0u);
  }
  gcomp_encoder_destroy(enc);
  Bytes out;
  ASSERT_EQ(Decode(z, out, z.size(), 1 << 16), GCOMP_OK);
  EXPECT_EQ(out, data);
}

TEST_F(Bzip2EncoderTest, ResetEqualsAFreshEncoder) {
  const Bytes a = mixed(300000), b = words(250000, 3);
  gcomp_options_t * o = Level(1);
  Bytes fresh;
  ASSERT_EQ(Encode(b, fresh, b.size(), 1 << 16, o), GCOMP_OK);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", o, &enc), GCOMP_OK);
  gcomp_options_destroy(o);
  Bytes buf(1 << 20);
  /* Abandon a stream half way, then reset and encode b. */
  gcomp_buffer_t ib = {a.data(), a.size() / 2, 0};
  gcomp_buffer_t ob = {buf.data(), 100, 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  ib = {b.data(), b.size(), 0};
  ob = {buf.data(), buf.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_EQ(Bytes(buf.begin(), buf.begin() + ob.used), fresh);
  /* And after finish. */
  ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  ib = {b.data(), b.size(), 0};
  ob = {buf.data(), buf.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_EQ(Bytes(buf.begin(), buf.begin() + ob.used), fresh);
  gcomp_encoder_destroy(enc);
}

TEST_F(Bzip2EncoderTest, UpdateAndFlushAfterFinishAreRefused) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "bzip2", nullptr, &enc), GCOMP_OK);
  Bytes buf(100);
  gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
  ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  uint8_t x = 1;
  gcomp_buffer_t ib = {&x, 1, 0};
  ob = {buf.data(), buf.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC), GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

/**
 * The bound is the input, 2% of it and 1024 bytes. Incompressible data is the
 * worst case for a Huffman coder, and runs of exactly four are the worst for
 * the first pass, which adds a count byte to each.
 */
TEST_F(Bzip2EncoderTest, TheBoundCoversTheWorstInputsAtEveryLevel) {
  Bytes four_runs;
  g_seed = 4;
  while (four_runs.size() < 400000) {
    four_runs.insert(four_runs.end(), 4, 'a');
    four_runs.push_back((uint8_t)next_rand());
  }
  const std::vector<Corpus> worst = {
      {"noise", noise(500000, 8)},
      {"four-runs", four_runs},
      {"tiny-noise", noise(7, 1)},
      {"empty", {}},
  };
  for (const auto & c : worst) {
    for (int level : {1, 9}) {
      gcomp_options_t * o = Level(level);
      size_t bound = 0;
      ASSERT_EQ(gcomp_encode_bound(registry_, "bzip2", o, c.data.size(), &bound),
          GCOMP_OK);
      Bytes z;
      ASSERT_EQ(Encode(c.data, z, c.data.size() + 1, 1 << 16, o), GCOMP_OK);
      gcomp_options_destroy(o);
      EXPECT_LE(z.size(), bound) << c.name << " level " << level;
      /* And not wildly loose: within 3% of the input plus the constant. */
      EXPECT_LE(bound, c.data.size() + c.data.size() / 40 + 2048) << c.name;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
