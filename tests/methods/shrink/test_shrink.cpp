/**
 * @file test_shrink.cpp
 *
 * The shrink method (ZIP method 1): streams built bit by bit from PKWARE's
 * APPNOTE section 5.1, round trips over every kind of input, the table filling
 * and being cleared, malformed streams, and the limits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../legacy_zip_helpers.h"
#include <ghoti.io/compress/shrink.h>

namespace {
auto none = [](gcomp_options_t *) {};

Bytes codes9(std::initializer_list<unsigned> codes) {
  Bits b;
  for (unsigned c : codes) {
    b.put(c, 9);
  }
  return b.finish();
}
}  // namespace

// ---- what the APPNOTE says --------------------------------------------------------

/// Plain Lempel-Ziv-Welch: each code after the first makes an entry of the code
/// before it and the first byte of its own string.
TEST(ShrinkFormat, CodesAreStringsAndMakeEntries) {
  // a b, then 257 (= "ab"), then 258 (= "ba"): "ab" "ab" "ba"
  Bytes out;
  ASSERT_EQ(decode_with("shrink", codes9({'a', 'b', 257, 258}), out, none),
      GCOMP_OK);
  EXPECT_EQ(out, (Bytes{'a', 'b', 'a', 'b', 'b', 'a'}));
}

/// A code that names the entry about to be made: the string before it and its
/// own first byte.
TEST(ShrinkFormat, ACodeNamingTheEntryAboutToBeMade) {
  Bytes out;
  ASSERT_EQ(decode_with("shrink", codes9({'a', 257}), out, none), GCOMP_OK);
  EXPECT_EQ(out, (Bytes{'a', 'a', 'a'}));
}

/// 256 then 1 makes the codes after it one bit longer; nothing else does.
TEST(ShrinkFormat, TheCodeSizeGrowsOnlyWhenTheCompressorSaysSo) {
  Bits b;
  b.put('x', 9);
  b.put(256, 9);
  b.put(1, 9);
  b.put('y', 10);
  b.put('z', 10);
  Bytes out;
  ASSERT_EQ(decode_with("shrink", b.finish(), out, none), GCOMP_OK);
  EXPECT_EQ(out, (Bytes{'x', 'y', 'z'}));
}

/// 256 then 2 frees every entry nothing builds on, and the freed codes are made
/// again lowest first: after "abc" the entries are 257 = ab and 258 = bc, both
/// leaves; the clear frees both; the next entry is 257 again, = "cd".
TEST(ShrinkFormat, APartialClearFreesTheLeavesAndTheLowestCodeIsMadeAgain) {
  Bits b;
  for (unsigned c : {10u, 20u, 30u}) {
    b.put(c, 9);
  }
  b.put(256, 9);
  b.put(2, 9);
  b.put(40, 9);
  b.put(257, 9);
  Bytes out;
  ASSERT_EQ(decode_with("shrink", b.finish(), out, none), GCOMP_OK);
  EXPECT_EQ(out, (Bytes{10, 20, 30, 40, 30, 40}));
}

/// An entry that something builds on survives a partial clear.
TEST(ShrinkFormat, AnEntryWithAChildSurvivesAPartialClear) {
  Bits b;
  // 1 2 3 -> 257=(1,2) 258=(2,3); 257 -> 259=(3,1)... then 258 whose entry
  // (257,2) builds on 257, so 257 stands when the leaves are freed.
  for (unsigned c : {1u, 2u, 3u, 257u, 258u}) {
    b.put(c, 9);
  }
  b.put(256, 9);
  b.put(2, 9);
  b.put(257, 9);
  Bytes out;
  ASSERT_EQ(decode_with("shrink", b.finish(), out, none), GCOMP_OK);
  EXPECT_EQ(out, (Bytes{1, 2, 3, 1, 2, 2, 3, 1, 2}));
}

/// A stream that unzip 6.00 (Info-ZIP) and 7-Zip 25.01 both extract to "hello
/// hello hello hello world hello hello" as ZIP method 1. This encoder made it;
/// the two readers are what say it is a stream, which no reader of ours can.
TEST(ShrinkFormat, AStreamTwoOtherReadersAccept) {
  const Bytes z = from_hex("68cab061f30644c081050f12342870e19d3772d8906188902241");
  const std::string text = "hello hello hello hello world hello hello";
  Bytes out;
  ASSERT_EQ(decode_with("shrink", z, out, none), GCOMP_OK);
  EXPECT_EQ(std::string(out.begin(), out.end()), text);
}

TEST(ShrinkFormat, TheTableFillsAndIsClearedAtThirteenBits) {
  // Every code after the first makes an entry, so 7936 literals fill the table
  // (entries 257 to 8191); the compressor then frees the leaves, which are all
  // of them, with the code before it not a leaf only if it is a byte.
  Bits b;
  Bytes want;
  g_seed = 5;
  const unsigned cs = 9;
  for (int i = 0; i < 7936; i++) {
    const unsigned c = next_rand() & 255u;
    b.put(c, cs);
    want.push_back((uint8_t)c);
  }
  // codes this large are never used here, so the size stays at nine; the table
  // is nonetheless full: free it and use the lowest code again
  b.put(256, cs);
  b.put(2, cs);
  b.put('Q', cs);
  b.put(257, cs); // (last literal, 'Q')
  Bytes out;
  ASSERT_EQ(decode_with("shrink", b.finish(), out, none), GCOMP_OK);
  want.push_back('Q');
  want.push_back(want[want.size() - 2]);
  want.push_back('Q');
  EXPECT_EQ(out, want);
}

// ---- round trips -------------------------------------------------------------------

TEST(ShrinkRoundtrip, EveryKindOfInput) {
  Bytes far = noise(50000, 9);
  far.insert(far.end(), far.begin(), far.begin() + 20000);
  Bytes mixed = words(100000);
  const Bytes rnd = noise(60000, 5);
  mixed.insert(mixed.end(), rnd.begin(), rnd.end());
  mixed.insert(mixed.end(), 200000, 0);
  const Bytes more = words(80000);
  mixed.insert(mixed.end(), more.begin(), more.end());
  const std::vector<std::pair<const char *, Bytes>> inputs = {
      {"empty", {}},
      {"one", Bytes(1, 'x')},
      {"two", Bytes(2, 'x')},
      {"short", Bytes{97, 98, 99, 97, 98, 99}},
      {"words", words(400000)},
      {"noise", noise(300000, 6)},
      {"zeros", Bytes(1u << 20, 0)},
      {"far", far},
      {"mixed", mixed},
  };
  for (const auto & in : inputs) {
    const Bytes z = encode_with("shrink", in.second, none);
    Bytes out;
    ASSERT_EQ(decode_with("shrink", z, out, none), GCOMP_OK) << in.first;
    EXPECT_EQ(out, in.second) << in.first;
  }
}

TEST(ShrinkRoundtrip, TheTableIsClearedManyTimes) {
  // text long enough to fill the table over and over
  const Bytes in = words(1500000);
  const Bytes z = encode_with("shrink", in, none);
  Bytes out;
  ASSERT_EQ(decode_with("shrink", z, out, none), GCOMP_OK);
  EXPECT_EQ(out, in);
}

TEST(ShrinkRoundtrip, EveryBufferingDecodesTheSame) {
  Bytes in = words(60000);
  const Bytes tail = noise(30000, 12);
  in.insert(in.end(), tail.begin(), tail.end());
  const Bytes z = encode_with("shrink", in, none);
  for (size_t in_step : {size_t(1), size_t(7), size_t(4096), z.size()}) {
    for (size_t out_step : {size_t(1), size_t(13), size_t(100000)}) {
      if (in_step == 1 && out_step == 1) {
        continue;
      }
      Bytes out;
      ASSERT_EQ(decode_steps("shrink", z, out, in_step, out_step, none), GCOMP_OK)
          << "in " << in_step << " out " << out_step;
      EXPECT_EQ(out, in);
    }
  }
}

TEST(ShrinkRoundtrip, TheEncoderTakesInputInAnyPieces) {
  const Bytes in = words(200000);
  gcomp_encoder_t * e = nullptr;
  ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "shrink", nullptr, &e),
      GCOMP_OK);
  Bytes z, chunk(5);
  size_t pos = 0;
  while (pos < in.size()) {
    gcomp_buffer_t ib = {in.data() + pos, std::min<size_t>(777, in.size() - pos), 0};
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
  Bytes out;
  ASSERT_EQ(decode_with("shrink", z, out, none), GCOMP_OK);
  EXPECT_EQ(out, in);
}

TEST(ShrinkRoundtrip, ARunOfOneByteGrowsTheStringsToTheTableLimit) {
  const Bytes z = encode_with("shrink", Bytes(1u << 20, 7), none);
  EXPECT_LT(z.size(), 2000u);
}

// ---- malformed streams and limits -------------------------------------------------------

TEST(ShrinkMalformed, ACodeNotInTheTable) {
  Bytes out;
  EXPECT_EQ(decode_with("shrink", codes9({'a', 300}), out, none), GCOMP_ERR_CORRUPT);
}

TEST(ShrinkMalformed, TheFirstCodeMustBeAByte) {
  Bytes out;
  EXPECT_EQ(decode_with("shrink", codes9({257}), out, none), GCOMP_ERR_CORRUPT);
}

TEST(ShrinkMalformed, AControlCodeOtherThanOneOrTwo) {
  Bytes out;
  EXPECT_EQ(decode_with("shrink", codes9({'a', 256, 3, 'b'}), out, none),
      GCOMP_ERR_CORRUPT);
}

TEST(ShrinkMalformed, TheCodeSizeRaisedPastThirteen) {
  Bits b;
  b.put('a', 9);
  unsigned cs = 9;
  for (int i = 0; i < 5; i++) {
    b.put(256, cs);
    b.put(1, cs);
    if (cs < 13) {
      cs++;
    }
  }
  Bytes out;
  EXPECT_EQ(decode_with("shrink", b.finish(), out, none), GCOMP_ERR_CORRUPT);
}

TEST(ShrinkMalformed, AControlCodeCutShort) {
  Bytes out;
  EXPECT_EQ(decode_with("shrink", codes9({'a', 'b', 256}), out, none),
      GCOMP_ERR_CORRUPT);
}

TEST(ShrinkMalformed, EveryTruncationIsAnErrorOrAShorterStream) {
  const Bytes in = words(30000);
  const Bytes z = encode_with("shrink", in, none);
  for (size_t cut = 1; cut + 1 < z.size(); cut += 1 + z.size() / 80) {
    Bytes out;
    const Bytes part(z.begin(), z.begin() + cut);
    const gcomp_status_t s = decode_with("shrink", part, out, none);
    // no end marker, so a cut stream is a shorter one unless a code is split
    if (s == GCOMP_OK) {
      ASSERT_LE(out.size(), in.size());
      EXPECT_TRUE(std::equal(out.begin(), out.end(), in.begin())) << cut;
    }
  }
}

TEST(ShrinkMalformed, DamageNeverCrashes) {
  const Bytes z = encode_with("shrink", words(20000), none);
  g_seed = 99;
  for (int i = 0; i < 300; i++) {
    Bytes bad = z;
    bad[next_rand() % bad.size()] ^= (uint8_t)(1u << (next_rand() % 8));
    Bytes out;
    (void)decode_with("shrink", bad, out, none);
  }
}

TEST(ShrinkLimits, TheOutputLimitIsEnforced) {
  const Bytes z = encode_with("shrink", words(100000), none);
  Bytes out;
  EXPECT_EQ(decode_with("shrink", z, out,
                [](gcomp_options_t * o) {
                  gcomp_options_set_uint64(o, "limits.max_output_bytes", 50000);
                }),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(decode_with("shrink", z, out,
                [](gcomp_options_t * o) {
                  gcomp_options_set_uint64(o, "limits.max_output_bytes", 100000);
                }),
      GCOMP_OK);
}

TEST(ShrinkLimits, TheExpansionRatioIsEnforced) {
  const Bytes z = encode_with("shrink", Bytes(1u << 22, 0), none);
  Bytes out;
  EXPECT_EQ(decode_with("shrink", z, out,
                [](gcomp_options_t * o) {
                  gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 500);
                },
                1u << 23),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(decode_with("shrink", z, out, none, 1u << 23), GCOMP_OK);
}

TEST(ShrinkLimits, ABoundThatHoldsForTheWorstInput) {
  for (size_t n : {size_t(0), size_t(1), size_t(100000), size_t(500000)}) {
    const Bytes in = noise(n, 11);
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "shrink", nullptr, n, &bound), GCOMP_OK);
    Bytes z(bound);
    size_t used = 0;
    EXPECT_EQ(gcomp_encode_buffer(nullptr, "shrink", nullptr, in.data(), n,
                  z.data(), z.size(), &used),
        GCOMP_OK)
        << n;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
