/**
 * @file test_bzip2_bwt.cpp
 *
 * The Burrows-Wheeler transform against the definition: sort the n rotations
 * of the block, take the last byte of each, and note which row the unrotated
 * block landed in. The suffix sorter under it is linear-time and recursive and
 * easy to get subtly wrong, so it is checked on every short string over a
 * small alphabet, which is where the corner cases of the algorithm are (the
 * LMS substrings that tie, the names that are not yet distinct, the recursion
 * that bottoms out), and on random, periodic and constant strings of every
 * length up to a few hundred.
 *
 * Equal rotations (a block that repeats something shorter) can sort in any
 * order, and every order gives the same last column, so the column is what is
 * compared, and the row is checked by the one property a decoder uses: the
 * rotation sitting in it is the block.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "methods/bzip2/bzip2_internal.h"
#include "test_helpers.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/allocator.h>
#include <gtest/gtest.h>
#include <numeric>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct Naive {
  Bytes last;
  Bytes rotation_at_orig_row;
};

Naive naive_bwt(const Bytes & b, uint32_t orig_row_from_sut) {
  const size_t n = b.size();
  std::vector<size_t> rot(n);
  std::iota(rot.begin(), rot.end(), 0);
  std::stable_sort(rot.begin(), rot.end(), [&](size_t x, size_t y) {
    for (size_t i = 0; i < n; i++) {
      uint8_t cx = b[(x + i) % n], cy = b[(y + i) % n];
      if (cx != cy) {
        return cx < cy;
      }
    }
    return false;
  });
  Naive r;
  for (size_t i = 0; i < n; i++) {
    r.last.push_back(b[(rot[i] + n - 1) % n]);
  }
  /* The rotation the implementation put in its origin row, as bytes, which
   * needs the implementation's own order: rebuilt from its last column. */
  (void)orig_row_from_sut;
  return r;
}

/// Undo the transform from a last column and a row, the way a decoder does.
Bytes inverse(const Bytes & last, uint32_t orig) {
  const size_t n = last.size();
  std::vector<uint32_t> cnt(256, 0), start(256, 0), next(n);
  for (uint8_t c : last) {
    cnt[c]++;
  }
  uint32_t sum = 0;
  for (int c = 0; c < 256; c++) {
    start[c] = sum;
    sum += cnt[c];
  }
  for (size_t i = 0; i < n; i++) {
    next[start[last[i]]++] = (uint32_t)i;
  }
  Bytes out;
  uint32_t p = next[orig];
  for (size_t i = 0; i < n; i++) {
    out.push_back(last[p]);
    p = next[p];
  }
  return out;
}

class Bwt {
public:
  explicit Bwt(size_t max_n)
      : s_(2 * max_n + 8), sa_(2 * max_n + 8) {
    scratch_.s = s_.data();
    scratch_.sa = sa_.data();
  }
  bool run(const Bytes & b, Bytes & last, uint32_t & orig) {
    last.assign(b.size(), 0);
    return bzip2_bwt(gcomp_allocator_default(), &scratch_, b.data(),
               (uint32_t)b.size(), last.data(), &orig) == 0;
  }

private:
  std::vector<uint16_t> s_;
  std::vector<int32_t> sa_;
  bzip2_bwt_scratch_t scratch_;
};

void check(Bwt & bwt, const Bytes & b) {
  Bytes last;
  uint32_t orig = 0;
  ASSERT_TRUE(bwt.run(b, last, orig));
  Naive want = naive_bwt(b, orig);
  ASSERT_EQ(last, want.last) << "length " << b.size();
  ASSERT_LT(orig, b.size());
  /* What a decoder does with the pair must give the block back. */
  ASSERT_EQ(inverse(last, orig), b) << "length " << b.size();
}

TEST(Bzip2Bwt, EveryShortStringOverTwoAndThreeSymbols) {
  Bwt bwt(16);
  for (int alpha : {2, 3}) {
    const int max_len = alpha == 2 ? 13 : 8;
    for (int len = 1; len <= max_len; len++) {
      uint64_t total = 1;
      for (int i = 0; i < len; i++) {
        total *= (uint64_t)alpha;
      }
      for (uint64_t code = 0; code < total; code++) {
        Bytes b(len);
        uint64_t c = code;
        for (int i = 0; i < len; i++) {
          b[i] = (uint8_t)('a' + c % alpha);
          c /= alpha;
        }
        check(bwt, b);
        if (::testing::Test::HasFatalFailure()) {
          return;
        }
      }
    }
  }
}

TEST(Bzip2Bwt, RandomPeriodicAndConstantStringsOfEveryLengthToThreeHundred) {
  Bwt bwt(512);
  uint32_t seed = 12345;
  auto rnd = [&]() {
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
  };
  for (size_t len = 1; len <= 300; len++) {
    for (unsigned alpha : {1u, 2u, 4u, 256u}) {
      Bytes b(len);
      for (auto & c : b) {
        c = (uint8_t)(rnd() % alpha);
      }
      check(bwt, b);
      /* A short random block repeated: every rotation has a twin. */
      Bytes unit(1 + rnd() % 5);
      for (auto & c : unit) {
        c = (uint8_t)(rnd() % alpha);
      }
      Bytes per(len);
      for (size_t i = 0; i < len; i++) {
        per[i] = unit[i % unit.size()];
      }
      check(bwt, per);
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
    check(bwt, Bytes(len, 0x00));
    check(bwt, Bytes(len, 0xFF));
  }
}

/// A full-size block: the largest the encoder sorts. The check is the inverse,
/// since sorting 900000 rotations naively is not something to wait for.
TEST(Bzip2Bwt, AFullBlockInvertsToItself) {
  const size_t n = 900000 - 19;
  Bwt bwt(n);
  uint32_t seed = 99;
  for (int shape = 0; shape < 4; shape++) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) {
      seed = seed * 1103515245u + 12345u;
      uint32_t r = seed >> 16;
      switch (shape) {
      case 0: b[i] = (uint8_t)r; break;                 /* noise */
      case 1: b[i] = (uint8_t)("abcab"[i % 5]); break; /* periodic */
      case 2: b[i] = (uint8_t)(r % 3); break;           /* three symbols */
      default: b[i] = (uint8_t)(i * 7 / 3000);          /* long constant runs */
      }
    }
    Bytes last;
    uint32_t orig = 0;
    ASSERT_TRUE(bwt.run(b, last, orig)) << "shape " << shape;
    ASSERT_LT(orig, n);
    EXPECT_EQ(inverse(last, orig), b) << "shape " << shape;
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
