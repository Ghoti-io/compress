/**
 * @file test_bzip2_decoder.cpp
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

TEST(Bzip2Decoder, Libbz2IsActuallyAvailable) {
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

/// Runs of one byte, of the lengths where the first run-length pass changes
/// shape: below 4 nothing, 4 a count of 0, 258 to 260 the longest a count
/// covers and one over, and a second run right after.
Bytes runs() {
  Bytes out;
  for (size_t len : {1u, 2u, 3u, 4u, 5u, 6u, 258u, 259u, 260u, 261u, 1000u}) {
    out.insert(out.end(), len, (uint8_t)('a' + len % 7));
    out.push_back('|');
  }
  /* The same byte after a run, so a count is followed by that byte again. */
  out.insert(out.end(), 10, 'x');
  out.insert(out.end(), 3, 'x');
  out.insert(out.end(), 4, 'y');
  out.insert(out.end(), 4, 'y');
  return out;
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
      {"runs", runs()},
      {"words3k", words(3000)},
      {"words300k", words(300000)},
      {"noise100k", noise(100000, 3)},
      {"zeros1m", Bytes(1 << 20, 0)},
      {"mixed3m", mixed(3u << 20)},
  };
  return c;
}

/// bzip2's own CRC, bitwise, so the tests' idea of it is not the library's.
uint32_t crc_bz(const Bytes & d) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : d) {
    crc ^= (uint32_t)b << 24;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
  }
  return ~crc;
}

struct BitWriter {
  Bytes out;
  uint32_t acc = 0;
  unsigned n = 0;
  void put(uint64_t v, unsigned bits) {
    for (unsigned i = bits; i-- > 0;) {
      acc = (acc << 1) | (unsigned)((v >> i) & 1u);
      if (++n == 8) {
        out.push_back((uint8_t)acc);
        acc = 0;
        n = 0;
      }
    }
  }
  void pad() {
    while (n) {
      put(0, 1);
    }
  }
};

/**
 * A block built field by field. The tables are the simplest complete prefix
 * code, lengths 1, 2, ... and the last two alike, so symbol i is i ones and a
 * zero, and the last is all ones. Every field a refusal is about can be set to
 * something wrong while the rest stays right.
 */
struct Block {
  int level = 9;
  Bytes used = {'a'};
  uint32_t crc = 0;
  unsigned rand = 0;
  uint32_t orig = 0;
  unsigned n_groups = 2;
  unsigned n_sel = 1;
  std::vector<unsigned> sel = {0};
  unsigned start_len = 0; /* 0: the first table's first length, as built */
  std::vector<unsigned> symbols; /* symbol numbers; the end is alpha - 1 */
  bool end_symbol = true;
};

Bytes build(const Block & b, bool with_end = true, uint32_t stream_crc = 0,
    bool use_given_stream_crc = false) {
  BitWriter w;
  w.put('B', 8);
  w.put('Z', 8);
  w.put('h', 8);
  w.put((uint64_t)('0' + b.level), 8);
  w.put(0x314159265359ull, 48);
  w.put(b.crc, 32);
  w.put(b.rand, 1);
  w.put(b.orig, 24);
  /* the symbol map */
  uint16_t groups = 0;
  for (uint8_t u : b.used) {
    groups |= (uint16_t)(0x8000u >> (u / 16));
  }
  w.put(groups, 16);
  for (unsigned g = 0; g < 16; g++) {
    if (groups & (0x8000u >> g)) {
      uint16_t bits = 0;
      for (uint8_t u : b.used) {
        if (u / 16 == g) {
          bits |= (uint16_t)(0x8000u >> (u % 16));
        }
      }
      w.put(bits, 16);
    }
  }
  w.put(b.n_groups, 3);
  w.put(b.n_sel, 15);
  for (unsigned s : b.sel) {
    for (unsigned i = 0; i < s; i++) {
      w.put(1, 1);
    }
    w.put(0, 1);
  }
  const unsigned alpha = (unsigned)b.used.size() + 2;
  for (unsigned t = 0; t < (b.n_groups < 2 || b.n_groups > 6 ? 2u : b.n_groups); t++) {
    std::vector<unsigned> len(alpha);
    for (unsigned i = 0; i < alpha; i++) {
      len[i] = i + 1 < alpha ? i + 1 : alpha - 1;
    }
    unsigned curr = (t == 0 && b.start_len != 99) ? len[0] : len[0];
    if (t == 0 && b.start_len != 0) {
      curr = b.start_len;
    }
    w.put(curr, 5);
    for (unsigned i = 0; i < alpha; i++) {
      int c = (int)curr;
      while ((int)len[i] != c && c >= 1 && c <= 20 && curr == (unsigned)c) {
        if ((int)len[i] > c) {
          w.put(2, 2); /* 1 then 0: +1 */
          c++;
        }
        else {
          w.put(3, 2); /* 1 then 1: -1 */
          c--;
        }
        curr = (unsigned)c;
      }
      w.put(0, 1);
    }
  }
  for (unsigned s : b.symbols) {
    if (s + 1 < alpha) {
      for (unsigned i = 0; i < s; i++) {
        w.put(1, 1);
      }
      w.put(0, 1);
    }
    else {
      for (unsigned i = 0; i + 1 < alpha; i++) {
        w.put(1, 1);
      }
    }
  }
  if (b.end_symbol) {
    for (unsigned i = 0; i + 1 < alpha; i++) {
      w.put(1, 1);
    }
  }
  if (with_end) {
    w.put(0x177245385090ull, 48);
    w.put(use_given_stream_crc ? stream_crc : b.crc, 32);
  }
  w.pad();
  return w.out;
}

class Bzip2DecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
    ASSERT_TRUE(bzref::lib().ok());
  }

  gcomp_options_t * Opts() {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    return o;
  }

  gcomp_status_t Decode(const Bytes & in, Bytes & out, size_t in_chunk,
      size_t out_chunk, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t s = gcomp_decoder_create(registry_, "bzip2", opts, &dec);
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

  std::string detail_;
  gcomp_registry_t * registry_ = nullptr;
};

/// The corpus is what its names say: ask libbz2 what each entry costs.
TEST_F(Bzip2DecoderTest, TheCorpusHasTheShapesItsNamesClaim) {
  for (const auto & c : corpus()) {
    Bytes z = bzref::compress(c.data, 9);
    ASSERT_FALSE(z.empty()) << c.name;
    const std::string n = c.name;
    if (n == "noise100k") {
      EXPECT_GT(z.size(), c.data.size()) << "noise should not compress";
    }
    else if (n == "zeros1m") {
      EXPECT_LT(z.size(), 100u);
    }
    else if (n == "words300k") {
      EXPECT_LT(z.size(), c.data.size() / 4);
    }
  }
  /* mixed3m has to be many blocks at level 1, which is where the walk from
   * one block to the next, and the combined CRC over them, are exercised. */
  Bytes z = bzref::compress(mixed(3u << 20), 1);
  EXPECT_GT(z.size(), 100000u);
}

TEST_F(Bzip2DecoderTest, ReadsWhatLibbz2WritesAtEveryLevel) {
  for (const auto & c : corpus()) {
    for (int level : {1, 2, 5, 9}) {
      Bytes z = bzref::compress(c.data, level);
      ASSERT_FALSE(z.empty()) << c.name << " level " << level;
      Bytes out;
      ASSERT_EQ(Decode(z, out, z.size() + 1, c.data.size() + 64), GCOMP_OK)
          << c.name << " level " << level << ": " << detail_;
      EXPECT_EQ(out, c.data) << c.name << " level " << level;
    }
  }
}

/**
 * The state machine. One input byte at a time stops it inside every field of
 * the header, every selector, every code length and every symbol; one output
 * byte at a time stops it inside a run of repeated bytes. Both together stop
 * it inside one with its input still arriving.
 */
TEST_F(Bzip2DecoderTest, GivesTheSameAnswerForEveryWindowOfInputAndOutput) {
  const Bytes data = mixed(60000);
  Bytes z = bzref::compress(data, 1);
  ASSERT_FALSE(z.empty());
  Bytes with_runs = runs();
  Bytes zr = bzref::compress(with_runs, 9);
  for (size_t ic : {1u, 2u, 3u, 7u, 100u, 4096u, 1u << 20}) {
    for (size_t oc : {1u, 5u, 259u, 4096u}) {
      Bytes out;
      ASSERT_EQ(Decode(z, out, ic, oc), GCOMP_OK)
          << "in " << ic << " out " << oc << ": " << detail_;
      EXPECT_EQ(out, data) << "in " << ic << " out " << oc;
      ASSERT_EQ(Decode(zr, out, ic, oc), GCOMP_OK)
          << "runs, in " << ic << " out " << oc << ": " << detail_;
      EXPECT_EQ(out, with_runs) << "runs, in " << ic << " out " << oc;
    }
  }
}

/**
 * One call takes all the input when the output has room for what it makes. A
 * decoder that stopped between fields with bytes still waiting would be
 * correct in every test that loops until the input is gone and wrong for a
 * caller that makes one call per buffer: the bit buffer ran dry inside a loop
 * over the selectors and the code lengths, which refill only at the top.
 */
TEST_F(Bzip2DecoderTest, TakesAllItsInputInOneCallWhenTheOutputHasRoom) {
  const Bytes data = mixed(3u << 20);
  for (int level : {1, 9}) {
    Bytes z = bzref::compress(data, level);
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "bzip2", nullptr, &dec), GCOMP_OK);
    Bytes out(data.size() + 100);
    gcomp_buffer_t ib = {z.data(), z.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
    EXPECT_EQ(ib.used, ib.size) << "level " << level;
    ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
    EXPECT_EQ(Bytes(out.begin(), out.begin() + ob.used), data) << level;
    gcomp_decoder_destroy(dec);
  }
}

/// Concatenated streams read as one, as `bzip2 -d` reads them.
TEST_F(Bzip2DecoderTest, ReadsConcatenatedStreamsAtDifferentLevels) {
  const Bytes a = words(70000), b = mixed(250000), c = {'z'};
  Bytes joined = bzref::compress(a, 9);
  Bytes zb = bzref::compress(b, 1), zc = bzref::compress(c, 5);
  Bytes empty = bzref::compress({}, 9);
  joined.insert(joined.end(), zb.begin(), zb.end());
  joined.insert(joined.end(), empty.begin(), empty.end());
  joined.insert(joined.end(), zc.begin(), zc.end());
  Bytes all = a;
  all.insert(all.end(), b.begin(), b.end());
  all.insert(all.end(), c.begin(), c.end());
  for (size_t ic : {size_t(1), size_t(33), joined.size()}) {
    Bytes out;
    ASSERT_EQ(Decode(joined, out, ic, 1000), GCOMP_OK) << ic << ": " << detail_;
    EXPECT_EQ(out, all) << ic;
  }
}

TEST_F(Bzip2DecoderTest, AHandBuiltStreamIsAStreamLibbz2Reads) {
  /* "aaa" is a run of three, which the zero-run code spells RUNA RUNA. */
  Block b;
  b.crc = crc_bz({'a', 'a', 'a'});
  b.symbols = {0, 0};
  Bytes z = build(b);
  Bytes ref, out;
  ASSERT_TRUE(bzref::decompress(z, ref, 100)) << "libbz2 refuses the builder's stream";
  EXPECT_EQ(ref, (Bytes{'a', 'a', 'a'}));
  ASSERT_EQ(Decode(z, out, 1, 1), GCOMP_OK) << detail_;
  EXPECT_EQ(out, ref);
}

TEST_F(Bzip2DecoderTest, EveryTruncationIsAnErrorAndNeverACrash) {
  const Bytes data = words(1500);
  Bytes z = bzref::compress(data, 1);
  ASSERT_FALSE(z.empty());
  for (size_t cut = 0; cut < z.size(); cut++) {
    Bytes head(z.begin(), z.begin() + cut), out;
    gcomp_status_t s = Decode(head, out, 7, 100);
    EXPECT_NE(s, GCOMP_OK) << "cut at " << cut;
    EXPECT_NE(s, GCOMP_ERR_INTERNAL) << "cut at " << cut;
  }
}

TEST_F(Bzip2DecoderTest, ACorruptedByteIsNeverAMemoryError) {
  const Bytes data = words(4000);
  Bytes z = bzref::compress(data, 1);
  ASSERT_FALSE(z.empty());
  for (size_t i = 0; i < z.size(); i++) {
    for (uint8_t mask : {0x01, 0x80, 0xFF}) {
      Bytes bad = z, out;
      bad[i] ^= mask;
      gcomp_status_t s = Decode(bad, out, 11, 333);
      EXPECT_NE(s, GCOMP_ERR_INTERNAL) << "byte " << i;
      EXPECT_LE(out.size(), 64u << 20);
      /* A flipped bit in the data changes the CRC, so a stream that reads
       * clean after one was not damaged where it matters. */
      if (s == GCOMP_OK) {
        EXPECT_EQ(out, data) << "byte " << i << " mask " << (int)mask;
      }
    }
  }
}

struct Refusal {
  const char * name;
  Block block;
  bool with_end;
  gcomp_status_t want;
  const char * why;
};

TEST_F(Bzip2DecoderTest, RefusesEveryMalformedBlockForTheReasonItIs) {
  auto base = [] {
    Block b;
    b.crc = crc_bz({'a', 'a', 'a'});
    b.symbols = {0, 0};
    return b;
  };
  std::vector<Refusal> cases;
  auto add = [&](const char * name, Block b, gcomp_status_t want, const char * why,
                 bool with_end = true) {
    cases.push_back({name, b, with_end, want, why});
  };
  {
    Block b = base();
    b.rand = 1;
    add("a randomised block", b, GCOMP_ERR_UNSUPPORTED, "randomised blocks");
  }
  {
    Block b = base();
    b.used.clear();
    add("a block that uses no byte values", b, GCOMP_ERR_CORRUPT, "no byte values");
  }
  {
    Block b = base();
    b.n_groups = 1;
    add("one table", b, GCOMP_ERR_CORRUPT, "impossible number of tables");
  }
  {
    Block b = base();
    b.n_groups = 7;
    add("seven tables", b, GCOMP_ERR_CORRUPT, "impossible number of tables");
  }
  {
    Block b = base();
    b.n_sel = 0;
    b.sel = {};
    add("no selectors", b, GCOMP_ERR_CORRUPT, "no selectors");
  }
  {
    Block b = base();
    b.sel = {2}; /* a table the block does not have: n_groups is 2 */
    add("a selector past the tables", b, GCOMP_ERR_CORRUPT, "selector names a table");
  }
  {
    Block b = base();
    b.start_len = 21;
    add("a first code length of 21", b, GCOMP_ERR_CORRUPT, "code length is out of range");
  }
  {
    Block b = base();
    b.start_len = 0;
    b.start_len = 31;
    add("a first code length of 31", b, GCOMP_ERR_CORRUPT, "code length is out of range");
  }
  {
    Block b = base();
    b.symbols.assign(25, 1); /* RUNB, 25 times: the run doubles each time */
    add("a run of zeros longer than any block", b, GCOMP_ERR_CORRUPT,
        "run of zeros is longer");
  }
  {
    Block b = base();
    b.level = 1;
    b.symbols.assign(17, 1); /* 2 * (2^17 - 1) bytes: over 100000 */
    add("a block larger than its level", b, GCOMP_ERR_CORRUPT, "larger than its level");
  }
  {
    Block b = base();
    b.used = {'a', 'b'};
    b.symbols.assign(51, 2); /* a literal 51 times: one selector covers 50 */
    add("more symbols than selectors", b, GCOMP_ERR_CORRUPT, "more symbols than selectors");
  }
  {
    Block b = base();
    b.crc = crc_bz({'a', 'a', 'a', 'a'});
    b.symbols = {1, 0}; /* RUNB RUNA: four bytes, a run with no count after it */
    add("a block that ends inside a run", b, GCOMP_ERR_CORRUPT,
        "block ends inside a run");
  }
  {
    Block b = base();
    b.level = 1;
    b.used = {'a', 'b'};
    b.n_sel = 2001; /* 2001 * 50 symbols cover the 100001 below */
    b.sel.assign(2001, 0);
    b.symbols.assign(100001, 2); /* a literal each: one over the level's 100000 */
    add("a block larger than its level, a literal at a time", b,
        GCOMP_ERR_CORRUPT, "larger than its level");
  }
  {
    Block b = base();
    b.symbols = {};
    add("an empty block", b, GCOMP_ERR_CORRUPT, "origin pointer is outside");
  }
  {
    Block b = base();
    b.orig = 3; /* the block holds three bytes: 0, 1 and 2 */
    add("an origin pointer one past the end", b, GCOMP_ERR_CORRUPT,
        "origin pointer is outside");
  }
  {
    Block b = base();
    b.crc ^= 1;
    add("a block CRC that is wrong", b, GCOMP_ERR_CORRUPT, "block CRC mismatch");
  }
  for (const auto & c : cases) {
    Bytes z = build(c.block, c.with_end), out;
    for (size_t ic : {size_t(1), z.size()}) {
      EXPECT_EQ(Decode(z, out, ic, 64), c.want) << c.name << ", in " << ic;
      EXPECT_NE(detail_.find(c.why), std::string::npos)
          << c.name << ": said \"" << detail_ << "\"";
    }
  }
}

TEST_F(Bzip2DecoderTest, RefusesAStreamWhoseEndIsWrong) {
  Block b;
  b.crc = crc_bz({'a', 'a', 'a'});
  b.symbols = {0, 0};
  Bytes out;
  Bytes z = build(b, true, b.crc ^ 0x10, true);
  EXPECT_EQ(Decode(z, out, 1, 64), GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail_.find("stream CRC mismatch"), std::string::npos) << detail_;

  /* Not a stream at all, a level that is not a digit, and nothing after it. */
  EXPECT_EQ(Decode({'B', 'Z', 'h', '0'}, out, 4, 64), GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail_.find("not a bzip2 stream"), std::string::npos) << detail_;
  EXPECT_EQ(Decode({'B', 'Z', 'h', ':'}, out, 4, 64), GCOMP_ERR_CORRUPT);
  EXPECT_EQ(Decode({'B', 'Z', 'x', '9'}, out, 4, 64), GCOMP_ERR_CORRUPT);
  EXPECT_EQ(Decode({}, out, 1, 64), GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail_.find("truncated"), std::string::npos) << detail_;

  /* A stream and then something that is not another. */
  Block good;
  good.crc = crc_bz({'a', 'a', 'a'});
  good.symbols = {0, 0};
  Bytes zg = build(good);
  Bytes tail = zg;
  tail.push_back('x');
  tail.push_back('y');
  tail.push_back('z');
  tail.push_back('!');
  EXPECT_EQ(Decode(tail, out, tail.size(), 64), GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail_.find("data after the last stream"), std::string::npos) << detail_;
  /* And a block magic that is neither a block nor the end. */
  Bytes bad_magic = zg;
  bad_magic[4] ^= 0xFF;
  EXPECT_EQ(Decode(bad_magic, out, 1, 64), GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail_.find("neither a block nor the end"), std::string::npos) << detail_;
}

TEST_F(Bzip2DecoderTest, TheOutputLimitIsAnErrorAndTheOutputStopsAtIt) {
  const Bytes data = words(100000);
  Bytes z = bzref::compress(data, 9), out;
  gcomp_options_t * o = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_output_bytes", 5000), GCOMP_OK);
  EXPECT_EQ(Decode(z, out, 1000, 777, o), GCOMP_ERR_LIMIT);
  EXPECT_LE(out.size(), 5000u);
  gcomp_options_destroy(o);
  /* Exactly the size is not over it. */
  o = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_output_bytes", data.size()),
      GCOMP_OK);
  EXPECT_EQ(Decode(z, out, 1000, 777, o), GCOMP_OK);
  EXPECT_EQ(out, data);
  gcomp_options_destroy(o);
}

TEST_F(Bzip2DecoderTest, TheExpansionLimitStopsAZeroBombAndTheDefaultAllowsIt) {
  const Bytes zeros(64u << 20, 0);
  Bytes z = bzref::compress(zeros, 9), out;
  ASSERT_FALSE(z.empty());
  gcomp_options_t * o = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 50), GCOMP_OK);
  EXPECT_EQ(Decode(z, out, z.size(), 1u << 16, o), GCOMP_ERR_LIMIT);
  gcomp_options_destroy(o);
  EXPECT_EQ(Decode(z, out, z.size(), 1u << 20), GCOMP_OK) << detail_;
  EXPECT_EQ(out.size(), zeros.size());
}

TEST_F(Bzip2DecoderTest, TheMemoryLimitRefusesALevelItCannotHold) {
  Bytes z = bzref::compress(words(1000), 9), out;
  gcomp_options_t * o = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_memory_bytes", 1u << 20), GCOMP_OK);
  EXPECT_EQ(Decode(z, out, z.size(), 4096, o), GCOMP_ERR_LIMIT);
  gcomp_options_destroy(o);
  /* A level-1 stream needs 400 KB and fits. */
  z = bzref::compress(words(1000), 1);
  o = Opts();
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_memory_bytes", 1u << 20), GCOMP_OK);
  EXPECT_EQ(Decode(z, out, z.size(), 4096, o), GCOMP_OK);
  gcomp_options_destroy(o);
}

TEST_F(Bzip2DecoderTest, ResetStartsAnotherStream) {
  const Bytes a = words(9000), b = mixed(9000);
  Bytes sa = bzref::compress(a, 9), sb = bzref::compress(b, 1);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "bzip2", nullptr, &dec), GCOMP_OK);
  Bytes out(20000);
  gcomp_buffer_t ib = {sa.data(), sa.size() / 2, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  ib = {sb.data(), sb.size(), 0};
  ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK)
      << gcomp_decoder_get_error_detail(dec);
  EXPECT_EQ(Bytes(out.begin(), out.begin() + ob.used), b);
  gcomp_decoder_destroy(dec);
}

/// The four bytes are a magic number, so detect names the method, and the
/// method it names decodes the stream.
TEST_F(Bzip2DecoderTest, DetectNamesItAndWhatItNamesDecodes) {
  const Bytes data = words(5000);
  for (int level : {1, 9}) {
    Bytes z = bzref::compress(data, level);
    const char * name = nullptr;
    ASSERT_EQ(gcomp_detect(z.data(), z.size(), &name, nullptr), GCOMP_OK);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "bzip2");
    Bytes out;
    ASSERT_EQ(Decode(z, out, z.size(), 8192), GCOMP_OK);
    EXPECT_EQ(out, data);
  }
  /* Four bytes decide it; a level digit of 0 or ':' is not bzip2. */
  for (uint8_t digit : {(uint8_t)'0', (uint8_t)':', (uint8_t)0}) {
    const uint8_t bad[4] = {'B', 'Z', 'h', digit};
    const char * name = nullptr;
    gcomp_status_t s = gcomp_detect(bad, 4, &name, nullptr);
    EXPECT_TRUE(s != GCOMP_OK || std::strcmp(name, "bzip2") != 0);
  }
}

TEST_F(Bzip2DecoderTest, PeekReadsTheLevel) {
  Bytes z = bzref::compress(words(1000), 7);
  gcomp_stream_info_t info;
  std::memset(&info, 0, sizeof(info));
  size_t needed = 0;
  ASSERT_EQ(gcomp_peek(registry_, "bzip2", nullptr, z.data(), z.size(), &info, &needed),
      GCOMP_OK);
  EXPECT_EQ(info.header_size, 4u);
  EXPECT_EQ(info.window_size, 700000u);
  EXPECT_NE(info.has_checksum, 0);
  EXPECT_EQ(gcomp_peek(registry_, "bzip2", nullptr, z.data(), 3, &info, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 4u);
  z[3] = '0';
  EXPECT_EQ(gcomp_peek(registry_, "bzip2", nullptr, z.data(), z.size(), &info, &needed),
      GCOMP_ERR_CORRUPT);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
