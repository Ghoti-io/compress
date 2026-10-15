/**
 * @file test_deflate64.cpp
 *
 * The deflate64 method: what differs from deflate (length code 285 carrying
 * sixteen extra bits, distance codes 30 and 31, a 64 KiB window), round trips
 * at every level and in every buffering, malformed streams, and the limits.
 *
 * The hand-built streams spell out the three changes bit by bit, from the
 * description in PKWARE's APPNOTE section 4.4.5, so they hold whatever a
 * reference implementation does. The golden stream is 7-Zip's.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate64.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t g_seed = 1;
uint32_t next_rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 16;
}

Bytes noise(size_t n, uint32_t seed) {
  g_seed = seed;
  Bytes b(n);
  for (auto & x : b) {
    x = (uint8_t)next_rand();
  }
  return b;
}

Bytes words(size_t n) {
  static const char * w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "sleeps. "};
  Bytes out;
  g_seed = 7;
  while (out.size() < n) {
    const char * s = w[next_rand() % 11];
    out.insert(out.end(), s, s + strlen(s));
  }
  out.resize(n);
  return out;
}

gcomp_options_t * level_opts(int level) {
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  gcomp_options_set_int64(o, "deflate64.level", level);
  return o;
}

Bytes encode(const Bytes & in, int level) {
  gcomp_options_t * o = level_opts(level);
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "deflate64", o, in.size(), &bound), GCOMP_OK);
  Bytes out(bound);
  size_t n = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "deflate64", o, in.data(), in.size(),
                out.data(), out.size(), &n),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(n);
  return out;
}

gcomp_status_t decode(const Bytes & in, Bytes & out, gcomp_options_t * o = nullptr,
    size_t cap = 1u << 24) {
  size_t n = 0;
  out.assign(cap, 0);
  const gcomp_status_t s = gcomp_decode_buffer(
      nullptr, "deflate64", o, in.data(), in.size(), out.data(), cap, &n);
  out.resize(s == GCOMP_OK ? n : 0);
  return s;
}

/// Decode feeding `in_step` bytes at a time into an output of `out_step`.
gcomp_status_t decode_steps(const Bytes & in, Bytes & out, size_t in_step,
    size_t out_step, gcomp_options_t * o = nullptr) {
  gcomp_decoder_t * d = nullptr;
  gcomp_status_t s = gcomp_decoder_create(gcomp_registry_default(), "deflate64", o, &d);
  if (s != GCOMP_OK) {
    return s;
  }
  Bytes chunk(out_step);
  size_t pos = 0;
  out.clear();
  for (;;) {
    gcomp_buffer_t ib = {in.data() + pos, std::min(in_step, in.size() - pos), 0};
    gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
    s = gcomp_decoder_update(d, &ib, &ob);
    out.insert(out.end(), chunk.data(), chunk.data() + ob.used);
    pos += ib.used;
    if (s != GCOMP_OK) {
      break;
    }
    if (pos == in.size() && ib.used == 0 && ob.used == 0) {
      break;
    }
    if (pos == in.size() && ob.used < chunk.size()) {
      break;
    }
  }
  while (s == GCOMP_OK) {
    gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
    s = gcomp_decoder_finish(d, &ob);
    out.insert(out.end(), chunk.data(), chunk.data() + ob.used);
    if (s != GCOMP_ERR_LIMIT) {
      break;
    }
  }
  gcomp_decoder_destroy(d);
  return s;
}

/// A bit writer for streams built by hand: bits go in low first, Huffman codes
/// are put most significant bit first.
struct Bits {
  Bytes out;
  uint64_t acc = 0;
  unsigned n = 0;
  void put(uint32_t v, unsigned bits) {
    acc |= (uint64_t)v << n;
    n += bits;
    while (n >= 8) {
      out.push_back((uint8_t)acc);
      acc >>= 8;
      n -= 8;
    }
  }
  void code(uint32_t c, unsigned len) {
    uint32_t r = 0;
    for (unsigned i = 0; i < len; i++) {
      r = (r << 1) | ((c >> i) & 1u);
    }
    put(r, len);
  }
  /// The fixed Huffman code (RFC 1951 3.2.6) for a literal/length symbol.
  void fixed_sym(unsigned s) {
    if (s < 144) {
      code(0x30 + s, 8);
    }
    else if (s < 256) {
      code(0x190 + s - 144, 9);
    }
    else if (s < 280) {
      code(s - 256, 7);
    }
    else {
      code(0xC0 + s - 280, 8);
    }
  }
  void align() {
    if (n != 0) {
      put(0, 8 - n);
    }
  }
  void stored(const Bytes & b, bool final) {
    put(final ? 1 : 0, 1);
    put(0, 2);
    align();
    put((uint32_t)b.size(), 16);
    put((uint32_t)b.size() ^ 0xFFFFu, 16);
    for (auto x : b) {
      put(x, 8);
    }
  }
  Bytes finish() {
    align();
    return out;
  }
};

}  // namespace

// ---- what deflate64 changes ---------------------------------------------------

/// Length code 285 is 3 plus sixteen extra bits: one 'a', then a match of 1000
/// at distance 1 is 1001 of them. Deflate would read 258 and no extra bits.
TEST(Deflate64Format, LengthCode285CarriesSixteenExtraBits) {
  Bits b;
  b.put(1, 1);
  b.put(1, 2); // a final block, fixed codes
  b.fixed_sym('a');
  b.fixed_sym(285);
  b.put(1000 - 3, 16);
  b.code(0, 5); // distance code 0, distance 1
  b.fixed_sym(256);
  Bytes out;
  ASSERT_EQ(decode(b.finish(), out), GCOMP_OK);
  EXPECT_EQ(out, Bytes(1001, 'a'));
}

TEST(Deflate64Format, TheLongestMatchIs65538) {
  Bits b;
  b.put(1, 1);
  b.put(1, 2);
  b.fixed_sym('a');
  b.fixed_sym(285);
  b.put(65535, 16);
  b.code(0, 5);
  b.fixed_sym(256);
  Bytes out;
  ASSERT_EQ(decode(b.finish(), out), GCOMP_OK);
  EXPECT_EQ(out.size(), 65539u);
}

/// Distance codes 30 and 31 have fourteen extra bits and reach 65536 back.
TEST(Deflate64Format, DistanceCodes30And31ReachTo65536) {
  const Bytes history = noise(70000, 3);
  Bits b;
  b.stored(Bytes(history.begin(), history.begin() + 40000), false);
  b.stored(Bytes(history.begin() + 40000, history.end()), false);
  b.put(1, 1);
  b.put(1, 2); // a final fixed block
  b.fixed_sym(257); // length 3
  b.code(31, 5);
  b.put(65536 - 49153, 14); // distance 65536
  b.fixed_sym(257);
  b.code(30, 5);
  b.put(33000 - 32769, 14); // distance 33000
  b.fixed_sym(256);
  Bytes out;
  ASSERT_EQ(decode(b.finish(), out), GCOMP_OK);
  Bytes want = history;
  for (int i = 0; i < 3; i++) {
    want.push_back(want[want.size() - 65536]);
  }
  for (int i = 0; i < 3; i++) {
    want.push_back(want[want.size() - 33000]);
  }
  EXPECT_EQ(out, want);
}

/// A match a deflate decoder would refuse (more than 32768 back) is fine here,
/// and one past 65536 is not.
TEST(Deflate64Format, ADistanceBeyondTheWindowIsRefused) {
  const Bytes history = noise(40000, 4);
  Bits b;
  b.stored(history, false);
  b.put(1, 1);
  b.put(1, 2);
  b.fixed_sym(257);
  b.code(31, 5);
  b.put(16383, 14); // 65536, but only 40000 bytes exist
  b.fixed_sym(256);
  Bytes out;
  EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT);
}

/// 7-Zip's stream for a phrase, 40000 zeros and the phrase again: the second
/// phrase is a match 40056 back, which needs distance code 30.
TEST(Deflate64Format, ASevenZipStreamWithADistanceOverTheDeflateWindow) {
  // 7z a -tzip -mm=Deflate64 -mx=9, the member's data. 7-Zip 25.01, on a file of our own text.
  static const char * hex =
      "e5dec911824014455143f901584663020eed8834d2a242f4400aeca87376af5e12777f4bf1ee"
      "eea7671cdbfcabe392fff1e85e4d89fc4d6d7ca6bb3a0c7d9cf3753baf3a4a95525376b1e1f1"
      "783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e"
      "8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3"
      "f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1783c"
      "1e8fc7e3f1783c1e8fc7e3f1783c1e8fc7e3f1786bb0bc3c6e04";
  Bytes in;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    in.push_back((uint8_t)std::stoi(std::string(hex + i, 2), nullptr, 16));
  }
  const std::string phrase = "The quick brown fox jumps over the lazy dog, then sleeps. ";
  Bytes want(phrase.begin(), phrase.end());
  want.insert(want.end(), 40000, 0);
  want.insert(want.end(), phrase.begin(), phrase.end());
  Bytes out;
  ASSERT_EQ(decode(in, out), GCOMP_OK);
  EXPECT_EQ(out, want);
}

/// Everything after the final block is left for the caller (a container's
/// trailer), not read.
TEST(Deflate64Format, BytesAfterTheFinalBlockAreLeftUnconsumed) {
  Bytes s = encode(words(5000), 6);
  const size_t n = s.size();
  s.push_back(0xAA);
  s.push_back(0xBB);
  gcomp_decoder_t * d = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "deflate64", nullptr, &d), GCOMP_OK);
  Bytes out(10000);
  gcomp_buffer_t ib = {s.data(), s.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(d, &ib, &ob), GCOMP_OK);
  EXPECT_EQ(ib.used, n);
  EXPECT_EQ(ob.used, 5000u);
  gcomp_decoder_destroy(d);
}

// ---- round trips ----------------------------------------------------------------

TEST(Deflate64Roundtrip, EveryLevelOnEveryKindOfInput) {
  Bytes far = noise(30000, 9);
  Bytes far2 = noise(40000, 10);
  far.insert(far.end(), far2.begin(), far2.end());
  far.insert(far.end(), far.begin(), far.begin() + 30000); // 70000 back
  Bytes mixed = words(100000);
  const Bytes rnd = noise(70000, 5);
  mixed.insert(mixed.end(), rnd.begin(), rnd.end());
  mixed.insert(mixed.end(), 300000, 0);
  const Bytes more = words(80000);
  mixed.insert(mixed.end(), more.begin(), more.end());
  const std::vector<std::pair<const char *, Bytes>> inputs = {
      {"empty", {}},
      {"one", Bytes(1, 'x')},
      {"short", Bytes{97, 98, 99, 97, 98, 99}},
      {"words", words(300000)},
      {"noise", noise(200000, 6)},
      {"zeros", Bytes(1u << 20, 0)},
      {"far", far},
      {"mixed", mixed},
  };
  for (int level = 0; level <= 9; level++) {
    for (const auto & in : inputs) {
      const Bytes z = encode(in.second, level);
      Bytes out;
      ASSERT_EQ(decode(z, out), GCOMP_OK) << in.first << " level " << level;
      EXPECT_EQ(out, in.second) << in.first << " level " << level;
    }
  }
}

TEST(Deflate64Roundtrip, ARunOfZerosUsesTheLongMatches) {
  const Bytes z = encode(Bytes(1u << 20, 0), 6);
  EXPECT_LT(z.size(), 200u) << "1 MiB of zeros should be a few matches of 65538";
}

TEST(Deflate64Roundtrip, TheLevelsTradeSizeForEffort) {
  const Bytes in = words(400000);
  EXPECT_LT(encode(in, 6).size(), encode(in, 1).size());
  EXPECT_LE(encode(in, 9).size(), encode(in, 6).size());
  EXPECT_GT(encode(in, 0).size(), in.size());
}

TEST(Deflate64Roundtrip, EveryBufferingDecodesTheSame) {
  Bytes in = words(60000);
  const Bytes tail = noise(3000, 12);
  in.insert(in.end(), tail.begin(), tail.end());
  in.insert(in.end(), in.begin(), in.begin() + 50000);
  for (int level : {0, 1, 6}) {
    const Bytes z = encode(in, level);
    for (size_t in_step : {size_t(1), size_t(7), size_t(4096), z.size()}) {
      for (size_t out_step : {size_t(1), size_t(13), size_t(100000)}) {
        if (in_step == 1 && out_step == 1 && z.size() > 20000) {
          continue; // two million one-byte calls add nothing
        }
        Bytes out;
        ASSERT_EQ(decode_steps(z, out, in_step, out_step), GCOMP_OK)
            << "level " << level << " in " << in_step << " out " << out_step;
        EXPECT_EQ(out, in);
      }
    }
  }
}

TEST(Deflate64Roundtrip, TheEncoderTakesInputInAnyPieces) {
  const Bytes in = words(250000);
  gcomp_options_t * o = level_opts(6);
  gcomp_encoder_t * e = nullptr;
  ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "deflate64", o, &e), GCOMP_OK);
  Bytes z, chunk(37);
  size_t pos = 0;
  while (pos < in.size()) {
    gcomp_buffer_t ib = {in.data() + pos, std::min<size_t>(1111, in.size() - pos), 0};
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
      ASSERT_EQ(gcomp_encoder_update(e, &ib, &ob), GCOMP_OK);
      z.insert(z.end(), chunk.data(), chunk.data() + ob.used);
    }
    pos += ib.used;
  }
  for (;;) {
    gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
    const gcomp_status_t s = gcomp_encoder_finish(e, &ob);
    z.insert(z.end(), chunk.data(), chunk.data() + ob.used);
    if (s == GCOMP_OK) {
      break;
    }
    ASSERT_EQ(s, GCOMP_ERR_LIMIT);
  }
  gcomp_encoder_destroy(e);
  gcomp_options_destroy(o);
  Bytes out;
  ASSERT_EQ(decode(z, out), GCOMP_OK);
  EXPECT_EQ(out, in);
}

/// A sync flush leaves everything so far decodable from the bytes written; a
/// full flush also lets what follows be decoded without what came before.
TEST(Deflate64Roundtrip, FlushesMakeTheOutputSoFarDecodable) {
  const Bytes a = words(20000), b = words(30000);
  for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
    gcomp_options_t * o = level_opts(6);
    gcomp_encoder_t * e = nullptr;
    ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "deflate64", o, &e), GCOMP_OK);
    Bytes z(1u << 20);
    gcomp_buffer_t ob = {z.data(), z.size(), 0};
    gcomp_buffer_t ia = {a.data(), a.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(e, &ia, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_flush(e, &ob, mode), GCOMP_OK);
    const size_t prefix = ob.used;
    gcomp_buffer_t ib = {b.data(), b.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(e, &ib, &ob), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_finish(e, &ob), GCOMP_OK);
    z.resize(ob.used);
    gcomp_encoder_destroy(e);
    gcomp_options_destroy(o);

    // The prefix alone is a stream cut at a block boundary: the decoder gives
    // everything in it and then reports the stream as unfinished.
    Bytes part;
    Bytes cut(z.begin(), z.begin() + prefix);
    EXPECT_EQ(decode_steps(cut, part, cut.size(), 1u << 20), GCOMP_ERR_CORRUPT);
    EXPECT_EQ(part, a);

    Bytes out;
    ASSERT_EQ(decode(z, out), GCOMP_OK);
    Bytes want = a;
    want.insert(want.end(), b.begin(), b.end());
    EXPECT_EQ(out, want);
  }
}

// ---- malformed streams and limits ---------------------------------------------------

TEST(Deflate64Malformed, ABlockOfTypeThree) {
  Bits b;
  b.put(1, 1);
  b.put(3, 2);
  Bytes out;
  EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT);
}

TEST(Deflate64Malformed, AStoredLengthThatIsNotItsComplement) {
  Bits b;
  b.put(1, 1);
  b.put(0, 2);
  b.align();
  b.put(5, 16);
  b.put(5, 16);
  for (int i = 0; i < 5; i++) {
    b.put('x', 8); // all the bytes it promises, so only the check can refuse it
  }
  Bytes out;
  EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT);
}

TEST(Deflate64Malformed, LengthCodes286And287AreNotDefined) {
  for (unsigned sym : {286u, 287u}) {
    Bits b;
    b.put(1, 1);
    b.put(1, 2);
    b.fixed_sym('a');
    b.fixed_sym(sym);
    Bytes out;
    EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT) << sym;
  }
}

TEST(Deflate64Malformed, AMatchBeforeTheStartOfTheOutput) {
  Bits b;
  b.put(1, 1);
  b.put(1, 2);
  b.fixed_sym('a');
  b.fixed_sym(257);
  b.code(1, 5); // distance 2, one byte exists
  b.fixed_sym(256);
  Bytes out;
  EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT);
}

TEST(Deflate64Malformed, ADynamicBlockWithTooManyLengthCodes) {
  Bits b;
  b.put(1, 1);
  b.put(2, 2);
  b.put(31, 5); // 288 literal/length codes
  b.put(0, 5);
  b.put(0, 4);
  Bytes out;
  EXPECT_EQ(decode(b.finish(), out), GCOMP_ERR_CORRUPT);
}

TEST(Deflate64Malformed, EveryTruncationIsRefusedOrIncomplete) {
  const Bytes in = words(30000);
  for (int level : {0, 1, 6}) {
    const Bytes z = encode(in, level);
    for (size_t cut = 0; cut + 1 < z.size(); cut += 1 + z.size() / 60) {
      Bytes out;
      const Bytes part(z.begin(), z.begin() + cut);
      const gcomp_status_t s = decode(part, out);
      EXPECT_NE(s, GCOMP_OK) << "level " << level << " cut at " << cut;
    }
  }
}

TEST(Deflate64Malformed, DamageNeverCrashesAndANonOutputIsAnError) {
  const Bytes in = words(20000);
  const Bytes z = encode(in, 6);
  g_seed = 99;
  for (int i = 0; i < 400; i++) {
    Bytes bad = z;
    bad[next_rand() % bad.size()] ^= (uint8_t)(1u << (next_rand() % 8));
    Bytes out;
    (void)decode(bad, out);
  }
}

TEST(Deflate64Limits, TheOutputLimitIsEnforced) {
  const Bytes z = encode(words(100000), 6);
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  gcomp_options_set_uint64(o, "limits.max_output_bytes", 50000);
  Bytes out;
  EXPECT_EQ(decode(z, out, o), GCOMP_ERR_LIMIT);
  gcomp_options_set_uint64(o, "limits.max_output_bytes", 100000);
  EXPECT_EQ(decode(z, out, o), GCOMP_OK);
  gcomp_options_destroy(o);
}

TEST(Deflate64Limits, TheExpansionRatioIsEnforced) {
  const Bytes z = encode(Bytes(1u << 22, 0), 6);
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 1000);
  Bytes out;
  EXPECT_EQ(decode(z, out, o, 1u << 23), GCOMP_ERR_LIMIT);
  gcomp_options_destroy(o);
  EXPECT_EQ(decode(z, out, nullptr, 1u << 23), GCOMP_OK);
}

TEST(Deflate64Limits, ABoundThatHoldsForTheWorstInput) {
  for (size_t n : {size_t(0), size_t(1), size_t(65535), size_t(65536), size_t(200000)}) {
    for (int level : {0, 1, 6, 9}) {
      const Bytes in = noise(n, 11);
      gcomp_options_t * o = level_opts(level);
      size_t bound = 0;
      ASSERT_EQ(gcomp_encode_bound(nullptr, "deflate64", o, n, &bound), GCOMP_OK);
      Bytes z(bound);
      size_t used = 0;
      EXPECT_EQ(gcomp_encode_buffer(nullptr, "deflate64", o, in.data(), n,
                    z.data(), z.size(), &used),
          GCOMP_OK)
          << n << " level " << level;
      gcomp_options_destroy(o);
    }
  }
}

TEST(Deflate64Options, ALevelOutOfRangeIsRefused) {
  gcomp_options_t * o = level_opts(10);
  gcomp_encoder_t * e = nullptr;
  EXPECT_NE(gcomp_encoder_create(gcomp_registry_default(), "deflate64", o, &e),
      GCOMP_OK);
  gcomp_options_destroy(o);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
