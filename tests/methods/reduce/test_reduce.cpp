/**
 * @file test_reduce.cpp
 *
 * The reduce method (ZIP methods 2 to 5). Streams built here bit by bit from
 * PKWARE's APPNOTE section 5.2, with the formulas it gives for each compression
 * factor, hold the format to the text; real members written by other programs
 * (test_legacy_real_vectors.cpp) hold it to the files, and found that the text,
 * read plainly, was misread once (the follower index takes at least one bit).
 * No reader on hand writes or decodes reduce: unzip 6.00 is built without it,
 * 7-Zip 25.01 and libarchive refuse the method.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../legacy_zip_helpers.h"
#include <ghoti.io/compress/reduce.h>
#include <map>

namespace {

auto cfg(int factor, uint64_t size = 0) {
  return [=](gcomp_options_t * o) {
    gcomp_options_set_int64(o, "reduce.factor", factor);
    gcomp_options_set_uint64(o, "reduce.uncompressed_size", size);
  };
}

using Sets = std::map<unsigned, Bytes>;

/// The follower sets, S(255) first (5.2.2, 5.2.3): six bits of size, then the
/// members as bytes.
void put_sets(Bits & b, const Sets & sets) {
  for (int j = 255; j >= 0; j--) {
    const auto it = sets.find((unsigned)j);
    const size_t n = it == sets.end() ? 0 : it->second.size();
    b.put((uint32_t)n, 6);
    for (size_t k = 0; k < n; k++) {
      b.put(it->second[k], 8);
    }
  }
}

/// B(N): the bits that hold N - 1, at least one (5.2.4; found from real files).
unsigned b_of(size_t n) {
  unsigned b = 1; // writing 0 takes a bit
  while ((1u << b) < n) {
    b++;
  }
  return b;
}

/// One byte of the outer layer, after `last` (5.2.4).
void put_byte(Bits & b, const Sets & sets, unsigned last, uint8_t v) {
  const auto it = sets.find(last);
  if (it == sets.end() || it->second.empty()) {
    b.put(v, 8);
    return;
  }
  const Bytes & s = it->second;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == v) {
      b.put(0, 1);
      b.put((uint32_t)i, b_of(s.size()));
      return;
    }
  }
  b.put(1, 1);
  b.put(v, 8);
}

/// The outer layer for a run of inner bytes, starting after Last-Character 0.
Bytes outer(const Sets & sets, const Bytes & inner) {
  Bits b;
  put_sets(b, sets);
  unsigned last = 0;
  for (uint8_t v : inner) {
    put_byte(b, sets, last, v);
    last = v;
  }
  return b.finish();
}

void copy_back(Bytes & out, unsigned dist, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    out.push_back(out.size() >= dist ? out[out.size() - dist] : 0);
  }
}

}  // namespace

// ---- what the APPNOTE says ---------------------------------------------------------

/// Each factor splits the byte after the marker into a length and the top of a
/// distance in its own proportion (5.2.5).
TEST(ReduceFormat, EachFactorsLengthAndDistanceFormulas) {
  for (int f = 1; f <= 4; f++) {
    const unsigned shift = 8u - (unsigned)f;
    const unsigned mask = (1u << shift) - 1u;
    Bytes inner, want;
    for (int i = 0; i < 400; i++) { // a history of 400 bytes, no marker in it
      const uint8_t v = (uint8_t)(1 + (i * 7) % 100);
      inner.push_back(v);
      want.push_back(v);
    }
    // a short repeat: length field 2 (copy 5), distance 4
    inner.insert(inner.end(), {144, 2, 3});
    copy_back(want, 4, 2 + 3);
    // a long one: the length field is all ones and a byte follows; distance
    // 300, which needs the top bit(s) of the byte after the marker
    const unsigned dist = 300 - 1;
    inner.insert(inner.end(), {144,
        (uint8_t)(((dist >> 8) << shift) | mask), 10, (uint8_t)(dist & 255)});
    copy_back(want, 300, mask + 10 + 3);
    // the marker as itself
    inner.insert(inner.end(), {144, 0});
    want.push_back(144);
    Bytes out;
    ASSERT_EQ(decode_with("reduce", outer({}, inner), out, cfg(f, want.size())),
        GCOMP_OK)
        << "factor " << f;
    EXPECT_EQ(out, want) << "factor " << f;
  }
}

TEST(ReduceFormat, ADistanceBeforeTheStartReadsZeros) {
  Bytes inner = {'a', 144, 1, 49}, want = {'a'};
  copy_back(want, 50, 1 + 3);
  Bytes out;
  ASSERT_EQ(decode_with("reduce", outer({}, inner), out, cfg(4, want.size())),
      GCOMP_OK);
  EXPECT_EQ(out, want);
}

/// A follower set makes a likely byte cost a flag bit and B(N) bits of index;
/// any other byte costs the flag and eight bits; with no set, eight bits.
TEST(ReduceFormat, FollowerSets) {
  Sets sets;
  sets[0] = {'A', 'B'};           // after Last-Character 0, which starts it
  sets['A'] = {'B'};              // one member: still one bit of index
  sets['B'] = {'C', 'A', 'Z'};    // three: two bits of index
  const Bytes inner = {'A', 'B', 'A', 'B', 'Z', 'Q', 'A', 'B', 'C'};
  Bits direct;
  put_sets(direct, sets);
  // by hand, each byte a flag bit and then its index (B(N) bits, at least one):
  direct.put(0, 1); direct.put(0, 1);  // A: S(0)[0], two members, one bit
  direct.put(0, 1); direct.put(0, 1);  // B: S(A)[0], one member, still one bit
  direct.put(0, 1); direct.put(1, 2);  // A: S(B)[1], three members, two bits
  direct.put(0, 1); direct.put(0, 1);  // B: S(A)[0]
  direct.put(0, 1); direct.put(2, 2);  // Z: S(B)[2]
  direct.put('Q', 8);                  // S(Z) is empty: the byte, no flag
  direct.put('A', 8);                  // S(Q) is empty
  direct.put(0, 1); direct.put(0, 1);  // B: S(A)[0]
  direct.put(0, 1); direct.put(0, 2);  // C: S(B)[0]
  const Bytes z = direct.finish();
  EXPECT_EQ(z, outer(sets, inner));
  Bytes out;
  ASSERT_EQ(decode_with("reduce", z, out, cfg(4, inner.size())), GCOMP_OK);
  EXPECT_EQ(out, inner);
}

TEST(ReduceFormat, AByteNotInTheSetIsSentWhole) {
  Sets sets;
  sets[0] = {'x'};
  const Bytes inner = {'y', 'z'};
  Bytes out;
  ASSERT_EQ(decode_with("reduce", outer(sets, inner), out, cfg(2, 2)), GCOMP_OK);
  EXPECT_EQ(out, inner);
}

// ---- round trips ------------------------------------------------------------------

TEST(ReduceRoundtrip, EveryFactorOnEveryKindOfInput) {
  Bytes far = noise(30000, 9);
  far.insert(far.end(), far.begin(), far.begin() + 3000);
  Bytes marker(5000, 144);
  Bytes mixed = words(60000);
  const Bytes rnd = noise(30000, 5);
  mixed.insert(mixed.end(), rnd.begin(), rnd.end());
  mixed.insert(mixed.end(), 100000, 0);
  const std::vector<std::pair<const char *, Bytes>> inputs = {
      {"empty", {}},
      {"one", Bytes(1, 'x')},
      {"marker", Bytes(1, 144)},
      {"markers", marker},
      {"short", Bytes{97, 98, 99, 97, 98, 99, 97, 98, 99, 97, 98, 99}},
      {"words", words(300000)},
      {"noise", noise(100000, 6)},
      {"zeros", Bytes(500000, 0)},
      {"far", far},
      {"mixed", mixed},
  };
  for (int f = 1; f <= 4; f++) {
    for (const auto & in : inputs) {
      const Bytes z = encode_with("reduce", in.second, cfg(f));
      Bytes out;
      ASSERT_EQ(decode_with("reduce", z, out, cfg(f, in.second.size())), GCOMP_OK)
          << in.first << " factor " << f;
      EXPECT_EQ(out, in.second) << in.first << " factor " << f;
    }
  }
}

TEST(ReduceRoundtrip, TextShrinksAndMoreSoWithMoreFactor) {
  const Bytes in = words(300000);
  const size_t f1 = encode_with("reduce", in, cfg(1)).size();
  const size_t f4 = encode_with("reduce", in, cfg(4)).size();
  EXPECT_LT(f1, in.size());
  EXPECT_LT(f4, f1);
}

/// With no size given nothing says where the data stops, so the padding to the
/// byte may read as a byte or two more: what comes out starts with the input.
TEST(ReduceRoundtrip, WithoutASizeTheOutputStartsWithTheInput) {
  const Bytes in = words(50000);
  const Bytes z = encode_with("reduce", in, cfg(4));
  Bytes out;
  ASSERT_EQ(decode_with("reduce", z, out, cfg(4, 0)), GCOMP_OK);
  ASSERT_GE(out.size(), in.size());
  EXPECT_LE(out.size(), in.size() + 8);
  EXPECT_TRUE(std::equal(in.begin(), in.end(), out.begin()));
}

TEST(ReduceRoundtrip, EveryBufferingDecodesTheSame) {
  Bytes in = words(40000);
  const Bytes tail = noise(3000, 12);
  in.insert(in.end(), tail.begin(), tail.end());
  const Bytes z = encode_with("reduce", in, cfg(3));
  for (size_t in_step : {size_t(1), size_t(7), size_t(4096), z.size()}) {
    for (size_t out_step : {size_t(1), size_t(13), size_t(100000)}) {
      if (in_step == 1 && out_step == 1) {
        continue;
      }
      Bytes out;
      ASSERT_EQ(decode_steps("reduce", z, out, in_step, out_step, cfg(3, in.size())),
          GCOMP_OK)
          << "in " << in_step << " out " << out_step;
      EXPECT_EQ(out, in);
    }
  }
}

// ---- malformed streams and limits ---------------------------------------------------

TEST(ReduceMalformed, AFollowerSetOfMoreThanThirtyTwo) {
  Bits b;
  b.put(40, 6); // S(255) says forty members
  Bytes out;
  EXPECT_EQ(decode_with("reduce", b.finish(), out, cfg(4, 10)), GCOMP_ERR_CORRUPT);
}

TEST(ReduceMalformed, TheSetsCutShort) {
  Bits b;
  b.put(3, 6);
  b.put(1, 8); // two of the three members
  b.put(2, 8);
  Bytes out;
  EXPECT_EQ(decode_with("reduce", b.finish(), out, cfg(4, 10)), GCOMP_ERR_CORRUPT);
}

TEST(ReduceMalformed, AnIndexPastTheEndOfASet) {
  Sets sets;
  sets[0] = {'a', 'b', 'c'}; // three: two bits, so index 3 is out
  Bits b;
  put_sets(b, sets);
  b.put(0, 1);
  b.put(3, 2);
  Bytes out;
  EXPECT_EQ(decode_with("reduce", b.finish(), out, cfg(4, 1)), GCOMP_ERR_CORRUPT);
}

TEST(ReduceMalformed, EveryTruncationWithASizeGivenIsAnError) {
  const Bytes in = words(20000);
  const Bytes z = encode_with("reduce", in, cfg(4));
  for (size_t cut = 0; cut + 1 < z.size(); cut += 1 + z.size() / 60) {
    Bytes out;
    const Bytes part(z.begin(), z.begin() + cut);
    EXPECT_NE(decode_with("reduce", part, out, cfg(4, in.size())), GCOMP_OK) << cut;
  }
}

TEST(ReduceMalformed, DamageNeverCrashes) {
  const Bytes in = words(20000);
  const Bytes z = encode_with("reduce", in, cfg(4));
  g_seed = 99;
  for (int i = 0; i < 300; i++) {
    Bytes bad = z;
    bad[next_rand() % bad.size()] ^= (uint8_t)(1u << (next_rand() % 8));
    Bytes out;
    (void)decode_with("reduce", bad, out, cfg(4, in.size()));
  }
}

TEST(ReduceMalformed, AFactorOutOfRangeIsRefused) {
  for (int f : {0, 5}) {
    gcomp_options_t * o = nullptr;
    gcomp_options_create(&o);
    gcomp_options_set_int64(o, "reduce.factor", f);
    gcomp_encoder_t * e = nullptr;
    EXPECT_NE(gcomp_encoder_create(gcomp_registry_default(), "reduce", o, &e),
        GCOMP_OK)
        << f;
    gcomp_decoder_t * d = nullptr;
    EXPECT_NE(gcomp_decoder_create(gcomp_registry_default(), "reduce", o, &d),
        GCOMP_OK)
        << f;
    gcomp_options_destroy(o);
  }
}

TEST(ReduceLimits, TheOutputLimitIsEnforced) {
  const Bytes in = words(100000);
  const Bytes z = encode_with("reduce", in, cfg(4));
  Bytes out;
  EXPECT_EQ(decode_with("reduce", z, out,
                [&](gcomp_options_t * o) {
                  cfg(4, in.size())(o);
                  gcomp_options_set_uint64(o, "limits.max_output_bytes", 50000);
                }),
      GCOMP_ERR_LIMIT);
}

TEST(ReduceLimits, TheExpansionRatioIsEnforced) {
  const Bytes in(1u << 22, 0);
  const Bytes z = encode_with("reduce", in, cfg(1));
  Bytes out;
  EXPECT_EQ(decode_with("reduce", z, out,
                [&](gcomp_options_t * o) {
                  cfg(1, in.size())(o);
                  gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 100);
                },
                1u << 23),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(decode_with("reduce", z, out, cfg(1, in.size()), 1u << 23), GCOMP_OK);
}

TEST(ReduceLimits, ABoundThatHoldsForTheWorstInput) {
  for (size_t n : {size_t(0), size_t(1), size_t(100000)}) {
    for (int f = 1; f <= 4; f++) {
      const Bytes in = noise(n, 11);
      gcomp_options_t * o = nullptr;
      gcomp_options_create(&o);
      cfg(f)(o);
      size_t bound = 0;
      ASSERT_EQ(gcomp_encode_bound(nullptr, "reduce", o, n, &bound), GCOMP_OK);
      Bytes z(bound);
      size_t used = 0;
      EXPECT_EQ(gcomp_encode_buffer(nullptr, "reduce", o, in.data(), n, z.data(),
                    z.size(), &used),
          GCOMP_OK)
          << n;
      gcomp_options_destroy(o);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
