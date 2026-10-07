/**
 * @file test_lzma_decoder.cpp
 *
 * The LZMA and LZMA2 decoders against liblzma.
 *
 * Everything liblzma writes, this decoder has to read, and has to read the
 * same way however the bytes arrive. That second half is the one a decoder
 * that works on whole buffers never meets: this one parks a half-arrived
 * symbol in a 20-byte carry and finishes it on the next call, and the only way
 * to reach that arm for every field of every symbol is to feed the stream one
 * byte at a time and take the output one byte at a time as well.
 *
 * A stream a test built is worth nothing until something else agrees it is a
 * stream, so the hand-assembled LZMA2 cases are checked against liblzma too.
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

/// The sentinel the oracle gate looks for: see ORACLE_SENTINEL in the Makefile.
TEST(LzmaDecoder, LiblzmaIsActuallyAvailable) {
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
/* The high half of a linear congruential state: its low bits have short
 * periods (bits 8 to 15 repeat every 65536 steps), and a "random" block that
 * repeats every 64 KiB is one match to a compressor, not noise. */
uint32_t next_rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 16;
}

Bytes words(size_t n) {
  static const char * w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "sleeps. "};
  Bytes out;
  g_seed = 7;
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

/// A block of noise followed by the same block: a match 300000 bytes back.
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

class LzmaDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
    ASSERT_TRUE(lzmaref::lib().ok());
  }

  gcomp_options_t * Opts() {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    return o;
  }

  /// Decode with the given input and output windows. The status is the first
  /// one that is not OK, or the finish status; `out` is everything produced.
  gcomp_status_t Decode(const char * method, const Bytes & in, Bytes & out,
      size_t in_chunk, size_t out_chunk, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t s = gcomp_decoder_create(registry_, method, opts, &dec);
    if (s != GCOMP_OK) {
      return s;
    }
    out.clear();
    detail_.clear();
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
          Remember(dec);
          gcomp_decoder_destroy(dec);
          return s;
        }
        if (ib.used == before && ob.used == 0) {
          gcomp_decoder_destroy(dec);
          return GCOMP_ERR_INTERNAL; // no progress: the harness's own check
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
    Remember(dec);
    gcomp_decoder_destroy(dec);
    return s;
  }

  void Remember(gcomp_decoder_t * dec) {
    const char * d = gcomp_decoder_get_error_detail(dec);
    detail_ = d ? d : "";
  }

  /// What the last Decode() said about why it failed.
  std::string detail_;
  gcomp_registry_t * registry_ = nullptr;
};

/**
 * The corpus is what it says it is. An earlier generator's "noise" repeated
 * every 65536 bytes and compressed to a fraction, so the stored-chunk arm of
 * LZMA2 and the 300000-byte distance were both being tested by data that did
 * neither. Ask the reference what each one costs.
 */
TEST_F(LzmaDecoderTest, TheCorpusHasTheShapesItsNamesClaim) {
  for (const auto & c : corpus()) {
    Bytes z = lzmaref::alone_encode(c.data, lzmaref::options(9));
    ASSERT_FALSE(z.empty()) << c.name;
    const std::string n = c.name;
    if (n == "noise100k") {
      EXPECT_GT(z.size(), c.data.size()) << "noise should not compress";
    }
    else if (n == "far") {
      /* Half of it is a copy of the other half, and the first half is noise. */
      EXPECT_GT(z.size(), c.data.size() / 2 - 1000);
      EXPECT_LT(z.size(), c.data.size() / 2 + 6000);
    }
    else if (n == "zeros1m") {
      EXPECT_LT(z.size(), 400u);
    }
  }
}

TEST_F(LzmaDecoderTest, ReadsWhatLiblzmaWritesAtEveryPresetAndWindow) {
  for (const auto & c : corpus()) {
    for (uint32_t preset : {0u, 1u, 3u, 6u, 9u}) {
      Bytes in = lzmaref::alone_encode(c.data, lzmaref::options(preset));
      ASSERT_FALSE(in.empty()) << c.name << " preset " << preset;
      Bytes out;
      ASSERT_EQ(Decode("lzma", in, out, in.size() + 1, c.data.size() + 64),
          GCOMP_OK)
          << c.name << " preset " << preset;
      EXPECT_EQ(out, c.data) << c.name << " preset " << preset;
    }
  }
}

/* liblzma writes lc + lp up to 4; the format allows lc to 8 and lp to 4, which
 * only this library's own encoder can produce (test_lzma_encoder.cpp). */
TEST_F(LzmaDecoderTest, ReadsEveryLcLpPbCombinationLiblzmaWrites) {
  const Bytes data = words(20000);
  for (int lc = 0; lc <= 4; lc++) {
    for (int lp = 0; lp <= 4 - lc; lp++) {
      for (int pb : {0, 2, 4}) {
        Bytes in = lzmaref::alone_encode(
            data, lzmaref::options(3, lc, lp, pb));
        ASSERT_FALSE(in.empty());
        Bytes out;
        ASSERT_EQ(Decode("lzma", in, out, in.size(), data.size()), GCOMP_OK)
            << lc << " " << lp << " " << pb;
        EXPECT_EQ(out, data) << lc << " " << lp << " " << pb;
      }
    }
  }
}

/**
 * The carry. One input byte at a time reaches the half-arrived-symbol arm for
 * every byte of every symbol; one output byte at a time reaches the parked
 * match. Both together reach a parked match behind a parked symbol.
 */
TEST_F(LzmaDecoderTest, GivesTheSameAnswerForEveryWindowOfInputAndOutput) {
  const Bytes data = mixed(60000);
  Bytes in = lzmaref::alone_encode(data, lzmaref::options(6));
  ASSERT_FALSE(in.empty());
  for (size_t ic : {1u, 2u, 3u, 7u, 19u, 20u, 21u, 64u, 4096u}) {
    for (size_t oc : {1u, 5u, 273u, 4096u}) {
      Bytes out;
      ASSERT_EQ(Decode("lzma", in, out, ic, oc), GCOMP_OK)
          << "in " << ic << " out " << oc;
      EXPECT_EQ(out, data) << "in " << ic << " out " << oc;
    }
  }
}

TEST_F(LzmaDecoderTest, ReadsRawLzma1WithTheCallersProperties) {
  const Bytes data = mixed(100000);
  for (int lc : {0, 3, 4}) {
    lzmaref::Options o = lzmaref::options(5, lc, 0, 2);
    Bytes in = lzmaref::raw_encode(lzmaref::kFilterLzma1, data, o);
    ASSERT_FALSE(in.empty());
    gcomp_options_t * opts = Opts();
    ASSERT_EQ(gcomp_options_set_bool(opts, "lzma.raw", 1), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "lzma.lc", lc), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "lzma.lp", 0), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "lzma.pb", 2), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "lzma.dict_size", o.dict_size),
        GCOMP_OK);
    Bytes out;
    ASSERT_EQ(Decode("lzma", in, out, 13, 1000, opts), GCOMP_OK) << lc;
    EXPECT_EQ(out, data) << lc;
    gcomp_options_destroy(opts);
  }
}

/// The header's size field: a known size stops the decoder there, and a stream
/// that also carries a marker is read up to it and through it.
TEST_F(LzmaDecoderTest, HonoursADeclaredSizeAndAMarkerAfterIt) {
  const Bytes data = words(50000);
  Bytes in = lzmaref::alone_encode(data, lzmaref::options(6));
  ASSERT_FALSE(in.empty());
  for (int i = 0; i < 8; i++) {
    in[5 + i] = (uint8_t)((uint64_t)data.size() >> (8 * i));
  }
  Bytes out;
  ASSERT_EQ(Decode("lzma", in, out, 1, 100), GCOMP_OK);
  EXPECT_EQ(out, data);

  /* A size larger than the data that is there: the marker arrives early. */
  for (int i = 0; i < 8; i++) {
    in[5 + i] = (uint8_t)((uint64_t)(data.size() + 1) >> (8 * i));
  }
  EXPECT_EQ(Decode("lzma", in, out, in.size(), data.size() + 10),
      GCOMP_ERR_CORRUPT);

  /* A size smaller than the data: the stream goes on past it. */
  for (int i = 0; i < 8; i++) {
    in[5 + i] = (uint8_t)((uint64_t)(data.size() - 1) >> (8 * i));
  }
  EXPECT_EQ(Decode("lzma", in, out, in.size(), data.size() + 10),
      GCOMP_ERR_CORRUPT);
}

/**
 * The header's dictionary size is a promise the stream keeps: a match that
 * reaches further back than it, even into bytes the decoder still holds, is
 * corrupt. liblzma never writes one, so the test takes a stream with a match
 * 300000 bytes back and shrinks the header's window under it.
 */
TEST_F(LzmaDecoderTest, RefusesADistanceBeyondTheHeadersWindow) {
  const Bytes data = far_repeat();
  Bytes in = lzmaref::alone_encode(data, lzmaref::options(9));
  ASSERT_FALSE(in.empty());
  Bytes out;
  ASSERT_EQ(Decode("lzma", in, out, in.size(), data.size()), GCOMP_OK);
  ASSERT_EQ(out, data);
  /* 128 KiB, which the 300000-byte match does not fit in. */
  in[1] = 0;
  in[2] = 0;
  in[3] = 2;
  in[4] = 0;
  EXPECT_EQ(Decode("lzma", in, out, in.size(), data.size()),
      GCOMP_ERR_CORRUPT);
  /* And the largest window that still holds it reads it. */
  in[2] = 0;
  in[3] = 0x08; /* 0x080000 = 524288 */
  EXPECT_EQ(Decode("lzma", in, out, in.size(), data.size()), GCOMP_OK);
  EXPECT_EQ(out, data);
}

/**
 * The same, with one match and nothing after it. In the stream above a second
 * rep match follows the first and is refused on its own account, so a check on
 * the first match could be missing and the test would still pass.
 */
TEST_F(LzmaDecoderTest, RefusesAOneMatchStreamWhoseDistanceIsBeyondTheWindow) {
  Bytes data = noise(300000, 11);
  data.insert(data.end(), data.begin(), data.begin() + 100);
  Bytes in = lzmaref::alone_encode(data, lzmaref::options(9));
  ASSERT_FALSE(in.empty());
  Bytes out;
  ASSERT_EQ(Decode("lzma", in, out, in.size(), data.size()), GCOMP_OK);
  ASSERT_EQ(out, data);
  in[1] = 0;
  in[2] = 0;
  in[3] = 2;
  in[4] = 0;
  EXPECT_EQ(Decode("lzma", in, out, in.size(), data.size()),
      GCOMP_ERR_CORRUPT);
}

/// An LZMA2 LZMA chunk: control, 16 low bits of usize-1, csize-1, properties.
Bytes lzma2_chunk(uint8_t control_base, size_t usize, size_t csize,
    const Bytes * props, const Bytes & body) {
  Bytes c;
  c.push_back((uint8_t)(control_base | (((usize - 1) >> 16) & 0x1F)));
  c.push_back((uint8_t)((usize - 1) >> 8));
  c.push_back((uint8_t)(usize - 1));
  c.push_back((uint8_t)((csize - 1) >> 8));
  c.push_back((uint8_t)(csize - 1));
  if (props) {
    c.insert(c.end(), props->begin(), props->end());
  }
  c.insert(c.end(), body.begin(), body.end());
  return c;
}

/// The body of an `.lzma` file: the range-coded stream after its 13 bytes.
Bytes alone_body(const Bytes & data) {
  Bytes z = lzmaref::alone_encode(data, lzmaref::options(6));
  return Bytes(z.begin() + 13, z.end());
}

/**
 * A chunk's header says how many bytes it holds, and the decoder has to find
 * the last symbol at the last byte. One more byte inside the chunk than the
 * symbols use is corrupt; here it is a zero, which is also what an end byte
 * looks like, so a decoder that ignored the count would read the stray byte as
 * the end of the stream and accept the rest as trailing data.
 */
TEST_F(LzmaDecoderTest, RefusesAChunkWithABytePastItsLastSymbol) {
  const Bytes data = words(3000);
  Bytes z = lzmaref::raw_encode(lzmaref::kFilterLzma2, data, lzmaref::options(6));
  ASSERT_FALSE(z.empty());
  ASSERT_GE(z.size(), 8u);
  ASSERT_EQ(z.back(), 0);
  /* z is one chunk: header (6 bytes), body, end byte. */
  const size_t csize = (((size_t)z[3] << 8) | z[4]) + 1;
  ASSERT_EQ(z.size(), 6 + csize + 1);
  Bytes good = z;
  Bytes out;
  ASSERT_EQ(Decode("lzma2", good, out, good.size(), 8192), GCOMP_OK);
  ASSERT_EQ(out, data);
  Bytes bad = z;
  bad.pop_back();
  bad.push_back(0); /* the stray byte, still inside the chunk */
  bad.push_back(0); /* the end byte */
  bad[3] = (uint8_t)(csize >> 8);
  bad[4] = (uint8_t)csize; /* csize - 1 + 1 */
  EXPECT_EQ(Decode("lzma2", bad, out, 3, 100), GCOMP_ERR_CORRUPT);
  EXPECT_EQ(Decode("lzma2", bad, out, bad.size(), 8192), GCOMP_ERR_CORRUPT);
}

/// An LZMA2 chunk has no use for the end marker LZMA-alone ends with.
TEST_F(LzmaDecoderTest, RefusesAnEndMarkerInsideALzma2Chunk) {
  const Bytes data = words(3000);
  Bytes body = alone_body(data);
  /* One byte more than the data, so the decoder goes looking for a symbol
   * where the marker is. */
  /* lc 3, lp 0, pb 2: (2 * 5 + 0) * 9 + 3. */
  Bytes props = {93};
  Bytes stream = lzma2_chunk(0xE0, data.size() + 1, body.size(), &props, body);
  stream.push_back(0);
  Bytes out;
  EXPECT_EQ(Decode("lzma2", stream, out, stream.size(), 8192),
      GCOMP_ERR_CORRUPT);
  EXPECT_EQ(Decode("lzma2", stream, out, 1, 1), GCOMP_ERR_CORRUPT);
  /* And with exactly the data's length it is one symbol short of the marker:
   * the range coder has bytes left over, which is the other refusal. */
  stream = lzma2_chunk(0xE0, data.size(), body.size(), &props, body);
  stream.push_back(0);
  EXPECT_EQ(Decode("lzma2", stream, out, stream.size(), 8192),
      GCOMP_ERR_CORRUPT);
}

/**
 * Chunk headers the format forbids, each as the second or first chunk of an
 * otherwise sound stream. The control byte's rules are what keep a decoder
 * from running a chunk against a dictionary or properties that were never
 * set, so each rule is its own case.
 */
TEST_F(LzmaDecoderTest, RefusesEveryMalformedLzma2ChunkHeader) {
  struct Case {
    const char * name;
    Bytes stream;
    gcomp_status_t want;
    const char * why; /* a phrase the refusal has to contain */
    Bytes out = {};
  };
  const Bytes stored_a = {0x01, 0x00, 0x00, 'a'};
  auto after_stored_a = [&](Bytes tail) {
    Bytes s = stored_a;
    s.insert(s.end(), tail.begin(), tail.end());
    return s;
  };
  const std::vector<Case> cases = {
      {"a stored chunk then another then the end",
          after_stored_a({0x02, 0x00, 0x00, 'b', 0x00}), GCOMP_OK, "",
          {'a', 'b'}},
      {"an empty stream", {0x00}, GCOMP_OK, "", {}},
      {"control 3", after_stored_a({0x03, 0x00, 0x00, 'x', 0x00}),
          GCOMP_ERR_CORRUPT, "invalid chunk control byte"},
      {"control 0x7f", after_stored_a({0x7F, 0x00, 0x00, 'x', 0x00}),
          GCOMP_ERR_CORRUPT, "invalid chunk control byte"},
      {"first chunk stored without a dictionary reset",
          {0x02, 0x00, 0x00, 'a', 0x00}, GCOMP_ERR_CORRUPT,
          "does not reset the dictionary"},
      {"first chunk LZMA without a dictionary reset",
          {0x80, 0x00, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0}, GCOMP_ERR_CORRUPT,
          "does not reset the dictionary"},
      {"an LZMA chunk before any properties",
          after_stored_a({0x80, 0x00, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0}),
          GCOMP_ERR_CORRUPT, "no earlier chunk set"},
      {"properties that spell no lc, lp, pb",
          {0xE0, 0x00, 0x00, 0x00, 0x04, 225, 0, 0, 0, 0, 0, 0},
          GCOMP_ERR_CORRUPT, "invalid chunk properties"},
      {"lc + lp over four (lc 4, lp 1)",
          {0xE0, 0x00, 0x00, 0x00, 0x04, 13, 0, 0, 0, 0, 0, 0},
          GCOMP_ERR_CORRUPT, "invalid chunk properties"},
      {"a range coder that does not start with zero",
          {0xE0, 0x00, 0x00, 0x00, 0x04, 93, 1, 0, 0, 0, 0, 0},
          GCOMP_ERR_CORRUPT, "zero byte"},
      {"a stream that stops after a stored chunk", stored_a, GCOMP_ERR_CORRUPT,
          "truncated"},
      {"a stored chunk cut short", {0x01, 0x00, 0x03, 'a', 'b'},
          GCOMP_ERR_CORRUPT, "truncated"},
  };
  for (const auto & c : cases) {
    for (size_t ic : {size_t(1), c.stream.size()}) {
      Bytes out;
      EXPECT_EQ(Decode("lzma2", c.stream, out, ic, 16), c.want)
          << c.name << ", input in " << ic;
      if (c.want == GCOMP_OK) {
        EXPECT_EQ(out, c.out) << c.name;
      }
      else {
        EXPECT_NE(detail_.find(c.why), std::string::npos)
            << c.name << ": said \"" << detail_ << "\"";
      }
    }
  }
}

TEST_F(LzmaDecoderTest, ReadsLzma2FromLiblzma) {
  for (const auto & c : corpus()) {
    for (uint32_t preset : {0u, 6u}) {
      Bytes in = lzmaref::raw_encode(
          lzmaref::kFilterLzma2, c.data, lzmaref::options(preset));
      ASSERT_FALSE(in.empty()) << c.name;
      for (size_t ic : {size_t(1), in.size() + 1}) {
        Bytes out;
        ASSERT_EQ(Decode("lzma2", in, out, ic, 4096), GCOMP_OK)
            << c.name << " preset " << preset << " in " << ic;
        EXPECT_EQ(out, c.data) << c.name << " preset " << preset;
      }
    }
  }
}

/**
 * LZMA2 streams joined by hand. Dropping the end byte of the first and
 * putting the second after it is a stream whose second half begins with a
 * dictionary reset, which liblzma never writes mid-stream and the format
 * allows. Each is decoded by liblzma as well, so it is a stream and not just
 * something this decoder agrees with itself about.
 */
TEST_F(LzmaDecoderTest, ReadsADictionaryResetInTheMiddle) {
  const Bytes a = words(70000), b = mixed(90000);
  Bytes sa = lzmaref::raw_encode(lzmaref::kFilterLzma2, a, lzmaref::options(6));
  Bytes sb = lzmaref::raw_encode(lzmaref::kFilterLzma2, b, lzmaref::options(6));
  ASSERT_FALSE(sa.empty());
  ASSERT_FALSE(sb.empty());
  ASSERT_EQ(sa.back(), 0);
  sa.pop_back();
  sa.insert(sa.end(), sb.begin(), sb.end());
  Bytes both = a;
  both.insert(both.end(), b.begin(), b.end());

  Bytes ref;
  ASSERT_TRUE(lzmaref::raw_decode(lzmaref::kFilterLzma2, sa,
      lzmaref::options(6), ref, both.size() + 16));
  ASSERT_EQ(ref, both) << "liblzma does not accept the stream built here";

  for (size_t ic : {size_t(1), size_t(100), sa.size()}) {
    Bytes out;
    ASSERT_EQ(Decode("lzma2", sa, out, ic, 1000), GCOMP_OK) << ic;
    EXPECT_EQ(out, both) << ic;
  }
}

TEST_F(LzmaDecoderTest, EveryTruncationIsAnErrorAndNeverACrash) {
  const Bytes data = mixed(3000);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes in = std::string(method) == "lzma"
        ? lzmaref::alone_encode(data, lzmaref::options(6))
        : lzmaref::raw_encode(lzmaref::kFilterLzma2, data, lzmaref::options(6));
    ASSERT_FALSE(in.empty());
    for (size_t cut = 0; cut < in.size(); cut++) {
      Bytes head(in.begin(), in.begin() + cut), out;
      gcomp_status_t s = Decode(method, head, out, 7, 100);
      EXPECT_NE(s, GCOMP_OK) << method << " cut at " << cut;
      EXPECT_NE(s, GCOMP_ERR_INTERNAL) << method << " cut at " << cut;
    }
  }
}

/// Flip every byte of a small stream: whatever it says, it says it without
/// reading outside its input or writing outside its output.
TEST_F(LzmaDecoderTest, ACorruptedByteIsNeverAMemoryError) {
  const Bytes data = words(4000);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes in = std::string(method) == "lzma"
        ? lzmaref::alone_encode(data, lzmaref::options(3))
        : lzmaref::raw_encode(lzmaref::kFilterLzma2, data, lzmaref::options(3));
    ASSERT_FALSE(in.empty());
    std::set<std::string> messages;
    for (size_t i = 0; i < in.size(); i++) {
      for (uint8_t mask : {0x01, 0x80, 0xFF}) {
        Bytes bad = in, out;
        bad[i] ^= mask;
        gcomp_status_t s = Decode(method, bad, out, 11, 333);
        EXPECT_NE(s, GCOMP_ERR_INTERNAL) << method << " byte " << i;
        EXPECT_LE(out.size(), 64u << 20);
      }
    }
  }
}

TEST_F(LzmaDecoderTest, TheOutputLimitIsAnErrorAndTheOutputStopsAtIt) {
  const Bytes data = words(100000);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes in = std::string(method) == "lzma"
        ? lzmaref::alone_encode(data, lzmaref::options(6))
        : lzmaref::raw_encode(lzmaref::kFilterLzma2, data, lzmaref::options(6));
    gcomp_options_t * opts = Opts();
    ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 5000),
        GCOMP_OK);
    Bytes out;
    EXPECT_EQ(Decode(method, in, out, 1000, 777, opts), GCOMP_ERR_LIMIT)
        << method;
    EXPECT_LE(out.size(), 5000u) << method;
    gcomp_options_destroy(opts);

    /* Exactly the size is not over it. */
    opts = Opts();
    ASSERT_EQ(gcomp_options_set_uint64(
                  opts, "limits.max_output_bytes", data.size()),
        GCOMP_OK);
    EXPECT_EQ(Decode(method, in, out, 1000, 777, opts), GCOMP_OK) << method;
    EXPECT_EQ(out, data) << method;
    gcomp_options_destroy(opts);
  }
}

TEST_F(LzmaDecoderTest, TheExpansionLimitStopsAZeroBomb) {
  const Bytes zeros(8u << 20, 0);
  for (const char * method : {"lzma", "lzma2"}) {
    Bytes in = std::string(method) == "lzma"
        ? lzmaref::alone_encode(zeros, lzmaref::options(6))
        : lzmaref::raw_encode(lzmaref::kFilterLzma2, zeros, lzmaref::options(6));
    gcomp_options_t * opts = Opts();
    ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 50),
        GCOMP_OK);
    Bytes out;
    EXPECT_EQ(Decode(method, in, out, in.size(), 1u << 16, opts),
        GCOMP_ERR_LIMIT)
        << method;
    gcomp_options_destroy(opts);
    /* And the default, the format's own ceiling, lets the same stream by. */
    EXPECT_EQ(Decode(method, in, out, in.size(), 1u << 16), GCOMP_OK) << method;
    EXPECT_EQ(out.size(), zeros.size()) << method;
  }
}

/**
 * A header that claims a gigabyte of dictionary costs nothing until the stream
 * produces that much: the window grows with the output. Run under a memory
 * limit far below the claim, a small stream decodes and a large one is refused
 * for the limit rather than for the claim.
 */
TEST_F(LzmaDecoderTest, AHugeWindowClaimCostsNothingUntilTheOutputBacksIt) {
  const Bytes small = words(5000);
  Bytes in = lzmaref::alone_encode(small, lzmaref::options(3));
  ASSERT_FALSE(in.empty());
  in[1] = 0;
  in[2] = 0;
  in[3] = 0;
  in[4] = 0x40; /* 1 GiB */
  gcomp_options_t * opts = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 4u << 20),
      GCOMP_OK);
  Bytes out;
  ASSERT_EQ(Decode("lzma", in, out, in.size(), 8192, opts), GCOMP_OK);
  EXPECT_EQ(out, small);

  const Bytes big = words(12u << 20);
  Bytes in2 = lzmaref::alone_encode(big, lzmaref::options(3));
  ASSERT_FALSE(in2.empty());
  in2[1] = 0;
  in2[2] = 0;
  in2[3] = 0;
  in2[4] = 0x40;
  EXPECT_EQ(Decode("lzma", in2, out, in2.size(), 1u << 16, opts),
      GCOMP_ERR_LIMIT);
  gcomp_options_destroy(opts);

  /* The window limit refuses the claim itself, before any output. */
  opts = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_window_bytes", 1u << 20),
      GCOMP_OK);
  EXPECT_EQ(Decode("lzma", in, out, in.size(), 8192, opts), GCOMP_ERR_LIMIT);
  gcomp_options_destroy(opts);
}

TEST_F(LzmaDecoderTest, ResetStartsAnotherStream) {
  const Bytes a = words(9000), b = mixed(9000);
  Bytes sa = lzmaref::alone_encode(a, lzmaref::options(6));
  Bytes sb = lzmaref::alone_encode(b, lzmaref::options(1));
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "lzma", nullptr, &dec), GCOMP_OK);
  Bytes out(20000);
  /* Abandon a stream half way, then reset and read a different one. */
  gcomp_buffer_t ib = {sa.data(), sa.size() / 2, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  ib = {sb.data(), sb.size(), 0};
  ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  EXPECT_EQ(Bytes(out.begin(), out.begin() + ob.used), b);
  gcomp_decoder_destroy(dec);
}

TEST_F(LzmaDecoderTest, PeekReadsTheHeader) {
  const Bytes data = words(1000);
  Bytes in = lzmaref::alone_encode(data, lzmaref::options(6));
  gcomp_stream_info_t info;
  std::memset(&info, 0, sizeof(info));
  size_t needed = 0;
  ASSERT_EQ(gcomp_peek(registry_, "lzma", nullptr, in.data(), in.size(), &info,
                &needed),
      GCOMP_OK);
  EXPECT_EQ(info.header_size, 13u);
  EXPECT_EQ(info.window_size, 1u << 23);
  EXPECT_EQ(info.has_content_size, 0); /* liblzma writes "unknown" */
  EXPECT_EQ(gcomp_peek(registry_, "lzma", nullptr, in.data(), 12, &info,
                &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 13u);
  in[0] = 225; /* the first byte value no lc/lp/pb can spell */
  EXPECT_EQ(gcomp_peek(registry_, "lzma", nullptr, in.data(), in.size(), &info,
                &needed),
      GCOMP_ERR_CORRUPT);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
