/**
 * @file test_implode.cpp
 *
 * The implode method (ZIP method 6): streams built by hand from PKWARE's
 * APPNOTE section 5.3 with the code construction written out a second time
 * here, one stream that unzip 6.00 and 7-Zip 25.01 both read, round trips in
 * every configuration, malformed streams and the limits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../legacy_zip_helpers.h"
#include <ghoti.io/compress/implode.h>

namespace {

auto cfg(unsigned dict, bool lit, uint64_t size = 0) {
  return [=](gcomp_options_t * o) {
    gcomp_options_set_uint64(o, "implode.dictionary_size", dict);
    gcomp_options_set_bool(o, "implode.literal_tree", lit);
    gcomp_options_set_uint64(o, "implode.uncompressed_size", size);
  };
}

/// The APPNOTE's construction (5.3.8), written again: the codes for lengths
/// given in the order the file stores them, as the bits in the order they are
/// read (the first bit read is the lowest).
std::vector<uint16_t> spec_codes(const std::vector<unsigned> & lens) {
  const size_t n = lens.size();
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; i++) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
      [&](size_t a, size_t b) { return lens[a] < lens[b]; });
  std::vector<uint16_t> code(n);
  uint32_t c = 0, inc = 0;
  unsigned last = 0;
  for (size_t i = n; i-- > 0;) {
    c += inc;
    if (lens[order[i]] != last) {
      last = lens[order[i]];
      inc = 1u << (16 - last);
    }
    uint16_t r = 0;
    for (int k = 0; k < 16; k++) {
      r = (uint16_t)((r << 1) | ((c >> k) & 1u));
    }
    code[order[i]] = r;
  }
  return code;
}

/// A tree's lengths as the file stores them: a byte of (runs - 1), then a byte
/// per run, (count - 1) << 4 | (length - 1).
void put_tree(Bits & b, const std::vector<unsigned> & lens) {
  std::vector<uint8_t> runs;
  for (size_t i = 0; i < lens.size();) {
    size_t run = 1;
    while (i + run < lens.size() && lens[i + run] == lens[i] && run < 16) {
      run++;
    }
    runs.push_back((uint8_t)(((run - 1) << 4) | (lens[i] - 1)));
    i += run;
  }
  b.put((uint32_t)runs.size() - 1, 8);
  for (auto r : runs) {
    b.put(r, 8);
  }
}

/// Lengths for a complete code of n symbols that mixes lengths: one symbol of
/// length 1, one of length 6, the rest of length 7 (n = 64).
std::vector<unsigned> mixed_lens64() {
  std::vector<unsigned> l(64, 7);
  l[0] = 1;
  l[5] = 6;
  return l;
}

}  // namespace

// ---- what the APPNOTE says ----------------------------------------------------------

/// The APPNOTE's own example of a tree of eight (3, 3, 3, 3, 3, 2, 4, 4),
/// through the construction here: the table it prints gives the reversed codes
/// 101, 001, 110, 010, 100, 11, 1000, 0000 for the values 0 to 7.
TEST(ImplodeFormat, TheAppnotesExampleTree) {
  const std::vector<unsigned> lens = {3, 3, 3, 3, 3, 2, 4, 4};
  const auto c = spec_codes(lens);
  const unsigned want[8] = {0b101, 0b001, 0b110, 0b010, 0b100, 0b11, 0b1000, 0};
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(c[i] & ((1u << lens[i]) - 1u), want[i]) << i;
  }
}

/// A stream by hand with three trees, two of them of mixed lengths: literals,
/// a match with its distance split into low bits and a tree code, and a match
/// whose length takes the extra byte.
TEST(ImplodeFormat, AStreamBuiltFromTheAppnote) {
  const std::vector<unsigned> lit_lens(256, 8);
  const auto len_lens = mixed_lens64();
  std::vector<unsigned> dist_lens(64, 6);
  const auto lc = spec_codes(lit_lens);
  const auto nc = spec_codes(len_lens);
  const auto dc = spec_codes(dist_lens);
  Bits b;
  put_tree(b, lit_lens);
  put_tree(b, len_lens);
  put_tree(b, dist_lens);
  Bytes want;
  auto lit = [&](uint8_t ch) {
    b.put(1, 1);
    b.put(lc[ch], 8);
    want.push_back(ch);
  };
  auto match = [&](unsigned dist, unsigned len) {
    const unsigned d = dist - 1;
    const unsigned sym = std::min(len - 3, 63u);
    b.put(0, 1);
    b.put(d & 127u, 7); // an 8K dictionary: seven low bits
    b.put(dc[d >> 7], dist_lens[d >> 7]);
    b.put(nc[sym], len_lens[sym]);
    if (sym == 63) {
      b.put(len - 3 - 63, 8);
    }
    for (unsigned i = 0; i < len; i++) {
      want.push_back(want[want.size() - dist]);
    }
  };
  for (char ch : std::string("hello, world")) {
    lit((uint8_t)ch);
  }
  match(12, 12);   // all of it again: length symbol 9
  match(5, 3);     // the shortest, length symbol 0
  match(20, 66);   // symbol 63 plus an extra byte of 0
  match(12, 300);  // symbol 63 plus 234
  Bytes out;
  ASSERT_EQ(decode_with("implode", b.finish(), out, cfg(8192, true, want.size())),
      GCOMP_OK);
  EXPECT_EQ(out, want);
}

/// Without the literal tree there are two trees, literals are eight bits, and
/// the shortest match is two.
TEST(ImplodeFormat, WithoutALiteralTreeLiteralsAreEightBitsAndMatchesStartAtTwo) {
  const auto len_lens = mixed_lens64();
  std::vector<unsigned> dist_lens(64, 6);
  const auto nc = spec_codes(len_lens);
  const auto dc = spec_codes(dist_lens);
  Bits b;
  put_tree(b, len_lens);
  put_tree(b, dist_lens);
  b.put(1, 1);
  b.put('a', 8);
  b.put(1, 1);
  b.put('b', 8);
  // a match of two at distance 2 (4K dictionary: six low bits)
  b.put(0, 1);
  b.put(1 & 63u, 6);
  b.put(dc[0], 6);
  b.put(nc[0], len_lens[0]);
  Bytes out;
  ASSERT_EQ(decode_with("implode", b.finish(), out, cfg(4096, false, 4)),
      GCOMP_OK);
  EXPECT_EQ(out, (Bytes{'a', 'b', 'a', 'b'}));
}

/// Before the start of the output the dictionary is zeros.
TEST(ImplodeFormat, ADistanceBeforeTheStartReadsZeros) {
  const auto len_lens = mixed_lens64();
  std::vector<unsigned> dist_lens(64, 6);
  const auto nc = spec_codes(len_lens);
  const auto dc = spec_codes(dist_lens);
  Bits b;
  put_tree(b, len_lens);
  put_tree(b, dist_lens);
  b.put(0, 1);
  b.put(9, 6); // distance 10, nothing written yet
  b.put(dc[0], 6);
  b.put(nc[2], len_lens[2]); // length 2 + 2
  Bytes out;
  ASSERT_EQ(decode_with("implode", b.finish(), out, cfg(4096, false, 4)),
      GCOMP_OK);
  EXPECT_EQ(out, Bytes(4, 0));
}

/// A stream that unzip 6.00 (Info-ZIP) and 7-Zip 25.01 both extract to "hello
/// hello hello hello world hello hello" as ZIP method 6 with flags 6 (an 8 KiB
/// dictionary, a literal tree). This encoder made it; the two readers are what
/// say it is a stream, which no reader of ours can.
TEST(ImplodeFormat, AStreamTwoOtherReadersAccept) {
  const Bytes z = from_hex(
      "1cf838b706f7f7f7f7271617062705170617063706f7f7f7f7f7f7f7f777073645044504"
      "f5f5f5050416f5f5f5c55fdfffffe7afc27fef9f1fff1f91ff01");
  const std::string text = "hello hello hello hello world hello hello";
  Bytes out;
  ASSERT_EQ(decode_with("implode", z, out, cfg(8192, true, text.size())),
      GCOMP_OK);
  EXPECT_EQ(std::string(out.begin(), out.end()), text);
}

// ---- round trips ---------------------------------------------------------------------

TEST(ImplodeRoundtrip, EveryConfigurationOnEveryKindOfInput) {
  Bytes far = noise(30000, 9);
  far.insert(far.end(), far.begin(), far.begin() + 20000);
  Bytes mixed = words(100000);
  const Bytes rnd = noise(60000, 5);
  mixed.insert(mixed.end(), rnd.begin(), rnd.end());
  mixed.insert(mixed.end(), 100000, 0);
  const std::vector<std::pair<const char *, Bytes>> inputs = {
      {"empty", {}},
      {"one", Bytes(1, 'x')},
      {"two", Bytes(2, 'x')},
      {"short", Bytes{97, 98, 99, 97, 98, 99}},
      {"words", words(300000)},
      {"noise", noise(100000, 6)},
      {"zeros", Bytes(500000, 0)},
      {"far", far},
      {"mixed", mixed},
  };
  for (unsigned dict : {4096u, 8192u}) {
    for (bool lit : {false, true}) {
      for (const auto & in : inputs) {
        const Bytes z = encode_with("implode", in.second, cfg(dict, lit));
        Bytes out;
        ASSERT_EQ(decode_with("implode", z, out, cfg(dict, lit, in.second.size())),
            GCOMP_OK)
            << in.first << " " << dict << " " << lit;
        EXPECT_EQ(out, in.second) << in.first << " " << dict << " " << lit;
      }
    }
  }
}

TEST(ImplodeRoundtrip, WithoutASizeTheStreamEndsWhereItsInputDoes) {
  const Bytes in = words(50000);
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  Bytes out;
  ASSERT_EQ(decode_with("implode", z, out, cfg(8192, true, 0)), GCOMP_OK);
  EXPECT_EQ(out, in);
}

TEST(ImplodeRoundtrip, EveryBufferingDecodesTheSame) {
  Bytes in = words(40000);
  const Bytes tail = noise(3000, 12);
  in.insert(in.end(), tail.begin(), tail.end());
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  for (size_t in_step : {size_t(1), size_t(7), size_t(4096), z.size()}) {
    for (size_t out_step : {size_t(1), size_t(13), size_t(100000)}) {
      if (in_step == 1 && out_step == 1) {
        continue;
      }
      Bytes out;
      ASSERT_EQ(decode_steps("implode", z, out, in_step, out_step,
                    cfg(8192, true, in.size())),
          GCOMP_OK)
          << "in " << in_step << " out " << out_step;
      EXPECT_EQ(out, in);
    }
  }
}

// ---- malformed streams and limits -------------------------------------------------------

TEST(ImplodeMalformed, ATreeThatDescribesTheWrongNumberOfCodes) {
  Bits b;
  put_tree(b, std::vector<unsigned>(200, 8)); // a literal tree wants 256
  Bytes out;
  EXPECT_EQ(decode_with("implode", b.finish(), out, cfg(8192, true, 10)),
      GCOMP_ERR_CORRUPT);
}

TEST(ImplodeMalformed, ATreeWithMoreCodesThanThereAreBitStrings) {
  Bits b;
  put_tree(b, std::vector<unsigned>(256, 7)); // 256 codes of seven bits
  Bytes out;
  EXPECT_EQ(decode_with("implode", b.finish(), out, cfg(8192, true, 10)),
      GCOMP_ERR_CORRUPT);
}

TEST(ImplodeMalformed, AStreamCutShortWithASizeGiven) {
  const Bytes in = words(20000);
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  for (size_t cut = 0; cut + 1 < z.size(); cut += 1 + z.size() / 60) {
    Bytes out;
    const Bytes part(z.begin(), z.begin() + cut);
    EXPECT_NE(decode_with("implode", part, out, cfg(8192, true, in.size())),
        GCOMP_OK)
        << cut;
  }
}

TEST(ImplodeMalformed, DamageNeverCrashes) {
  const Bytes in = words(20000);
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  g_seed = 99;
  for (int i = 0; i < 300; i++) {
    Bytes bad = z;
    bad[next_rand() % bad.size()] ^= (uint8_t)(1u << (next_rand() % 8));
    Bytes out;
    (void)decode_with("implode", bad, out, cfg(8192, true, in.size()));
  }
}

TEST(ImplodeMalformed, ADictionarySizeThatIsNotAllowed) {
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  gcomp_options_set_uint64(o, "implode.dictionary_size", 16384);
  gcomp_decoder_t * d = nullptr;
  EXPECT_NE(gcomp_decoder_create(gcomp_registry_default(), "implode", o, &d),
      GCOMP_OK);
  gcomp_encoder_t * e = nullptr;
  EXPECT_NE(gcomp_encoder_create(gcomp_registry_default(), "implode", o, &e),
      GCOMP_OK);
  gcomp_options_destroy(o);
}

TEST(ImplodeLimits, TheOutputLimitIsEnforced) {
  const Bytes in = words(100000);
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  Bytes out;
  EXPECT_EQ(decode_with("implode", z, out,
                [](gcomp_options_t * o) {
                  gcomp_options_set_uint64(o, "implode.uncompressed_size", 100000);
                  gcomp_options_set_uint64(o, "limits.max_output_bytes", 50000);
                }),
      GCOMP_ERR_LIMIT);
}

TEST(ImplodeLimits, TheExpansionRatioIsEnforced) {
  const Bytes in(1u << 22, 0);
  const Bytes z = encode_with("implode", in, cfg(8192, true));
  Bytes out;
  EXPECT_EQ(decode_with("implode", z, out,
                [&](gcomp_options_t * o) {
                  cfg(8192, true, in.size())(o);
                  gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 50);
                },
                1u << 23),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(decode_with("implode", z, out, cfg(8192, true, in.size()), 1u << 23),
      GCOMP_OK);
}

TEST(ImplodeLimits, ABoundThatHoldsForTheWorstInput) {
  for (size_t n : {size_t(0), size_t(1), size_t(100000)}) {
    const Bytes in = noise(n, 11);
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "implode", nullptr, n, &bound),
        GCOMP_OK);
    Bytes z(bound);
    size_t used = 0;
    EXPECT_EQ(gcomp_encode_buffer(nullptr, "implode", nullptr, in.data(), n,
                  z.data(), z.size(), &used),
        GCOMP_OK)
        << n;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
