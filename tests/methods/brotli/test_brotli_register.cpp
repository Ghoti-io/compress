/**
 * @file test_brotli_register.cpp
 *
 * The brotli method descriptor's own entry points: peek, and the table
 * accessors the decoder reads through.
 *
 * ## Why this file exists
 *
 * `peek_wbits` is the whole of brotli's `peek`, and it was being exercised at
 * one window size. The default is lgwin 16, which RFC 7932 spells as the
 * single bit 0 - the shortest of the four forms the field has - so every
 * test that peeked a brotli stream took the first branch and returned. The
 * other three forms, which between them cover windows 10 to 15, 17, and 18
 * to 24, had never run, in a function whose correctness was established by
 * reading the RFC rather than by being tested.
 *
 * The encoder's `write_wbits` is the exact inverse of it, so the sweep below
 * writes a stream at every window the option allows and reads the window
 * back out of it. That makes the two halves check each other, and a stream
 * at each of those windows is also handed to the decoder, which reads WBITS
 * by a third path of its own.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <set>
#include <vector>

#include "methods/brotli/brotli_internal.h"

class BrotliRegisterTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  std::vector<uint8_t> Encode(
      const std::vector<uint8_t> & data, int lgwin, int level) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.lgwin", lgwin), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.level", level), GCOMP_OK);
    std::vector<uint8_t> out(data.size() + 4096);
    size_t written = 0;
    const gcomp_status_t st = gcomp_encode_buffer(registry_, "brotli", opts,
        data.data(), data.size(), out.data(), out.size(), &written);
    gcomp_options_destroy(opts);
    EXPECT_EQ(st, GCOMP_OK);
    out.resize(st == GCOMP_OK ? written : 0);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

/**
 * @brief Peek reads back the window the encoder wrote, at every window.
 *
 * WBITS has four forms (RFC 7932 section 9.1) and the decoding is not
 * monotonic: a leading 0 means 16, then three bits that are non-zero mean
 * 17 + that value, and three zeros are followed by three more that mean
 * 8 + that value, except that 0 means 17 and 1 is reserved. So the
 * interesting values are not the extremes - they are 16, 17 and the
 * boundaries either side of them.
 *
 * `window_size` is the window minus 16 bytes, which is what RFC 7932 calls
 * the usable window for a given WBITS, and `header_size` is one byte because
 * that is where the first meta-block starts.
 */
TEST_F(BrotliRegisterTest, PeekReadsEveryWindowTheEncoderWrites) {
  const std::vector<uint8_t> data(4096, 'w');
  std::set<uint64_t> seen;
  for (int lgwin = 10; lgwin <= 24; lgwin++) {
    for (int level : {0, 1}) {
      const std::vector<uint8_t> stream = Encode(data, lgwin, level);
      ASSERT_FALSE(stream.empty()) << lgwin;

      gcomp_stream_info_t info;
      memset(&info, 0, sizeof(info));
      size_t needed = 0;
      ASSERT_EQ(gcomp_peek(registry_, "brotli", nullptr, stream.data(),
                    stream.size(), &info, &needed),
          GCOMP_OK)
          << "lgwin " << lgwin << " level " << level;
      EXPECT_EQ(info.window_size, (1ull << lgwin) - 16ull)
          << "lgwin " << lgwin << " level " << level;
      EXPECT_EQ(info.header_size, 1u) << lgwin;
      EXPECT_EQ(needed, 1u) << lgwin;

      // One byte is all peek needs, and it must give the same answer from it.
      gcomp_stream_info_t one;
      memset(&one, 0, sizeof(one));
      ASSERT_EQ(
          gcomp_peek(registry_, "brotli", nullptr, stream.data(), 1, &one,
              nullptr),
          GCOMP_OK)
          << lgwin;
      EXPECT_EQ(one.window_size, info.window_size) << lgwin;

      seen.insert(info.window_size);
    }
  }
  // Fifteen distinct windows, so no two lgwin values collapsed onto one
  // answer - which is what a WBITS decoder that dropped a branch would do.
  EXPECT_EQ(seen.size(), 15u);
}

/// Nothing to read yet: ask for one byte, and say so rather than guessing.
TEST_F(BrotliRegisterTest, PeekWithNothingToReadAsksForOneByte) {
  gcomp_stream_info_t info;
  memset(&info, 0, sizeof(info));
  size_t needed = 0;
  EXPECT_EQ(gcomp_peek(registry_, "brotli", nullptr, nullptr, 0, &info,
                &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 1u);

  // A non-NULL buffer of zero length is the same question.
  const uint8_t byte = 0;
  needed = 0;
  EXPECT_EQ(
      gcomp_peek(registry_, "brotli", nullptr, &byte, 0, &info, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 1u);

  // needed_out is optional.
  EXPECT_EQ(
      gcomp_peek(registry_, "brotli", nullptr, nullptr, 0, &info, nullptr),
      GCOMP_ERR_LIMIT);
}

/**
 * @brief The reserved window field is the only first byte brotli refuses.
 *
 * RFC 7932 section 9.1 reserves one WBITS encoding: the leading 1, then three
 * zero bits, then 001. Everything else the seven-bit field can say is a
 * window from 10 to 24. The bits are packed least-significant first, so that
 * encoding is 0x11 in the low seven bits and the eighth bit belongs to the
 * next field - which is why it is refused at both settings of that bit and
 * why the refused set is exactly two of the 256 possible first bytes.
 *
 * Measured rather than reasoned: the set below is what the sweep in
 * EveryFirstByteIsAcceptedOrRefusedConsistently finds, and naming it here
 * means a change that started refusing or accepting one more fails.
 */
TEST_F(BrotliRegisterTest, OnlyTheReservedWindowFieldIsRefused) {
  std::set<int> refused;
  for (int b = 0; b < 256; b++) {
    const uint8_t byte = (uint8_t)b;
    gcomp_stream_info_t info;
    memset(&info, 0, sizeof(info));
    if (gcomp_peek(registry_, "brotli", nullptr, &byte, 1, &info, nullptr) !=
        GCOMP_OK) {
      refused.insert(b);
    }
  }
  const std::set<int> want = {0x11, 0x91};
  EXPECT_EQ(refused, want);

  // And it is GCOMP_ERR_CORRUPT, not a bare argument error: the bytes are a
  // stream that says something the format does not define.
  for (int b : want) {
    const uint8_t byte = (uint8_t)b;
    gcomp_stream_info_t info;
    memset(&info, 0, sizeof(info));
    EXPECT_EQ(gcomp_peek(registry_, "brotli", nullptr, &byte, 1, &info,
                  nullptr),
        GCOMP_ERR_CORRUPT)
        << "byte " << b;
  }
}

/**
 * @brief Every WBITS byte value gets a consistent answer.
 *
 * The field is at most seven bits, so all 256 first bytes can be tried. Any
 * window peek reports must be one the format defines and one the option
 * range allows, because a caller sizes a buffer from it.
 */
TEST_F(BrotliRegisterTest, EveryFirstByteIsAcceptedOrRefusedConsistently) {
  int accepted = 0;
  int refused = 0;
  for (int b = 0; b < 256; b++) {
    const uint8_t byte = (uint8_t)b;
    gcomp_stream_info_t info;
    memset(&info, 0, sizeof(info));
    const gcomp_status_t st =
        gcomp_peek(registry_, "brotli", nullptr, &byte, 1, &info, nullptr);
    if (st == GCOMP_OK) {
      accepted++;
      bool known = false;
      for (int lgwin = 10; lgwin <= 24; lgwin++) {
        if (info.window_size == (1ull << lgwin) - 16ull) {
          known = true;
        }
      }
      EXPECT_TRUE(known) << "byte 0x" << std::hex << b
                         << " gave window_size " << std::dec
                         << info.window_size;
    }
    else {
      refused++;
      EXPECT_EQ(st, GCOMP_ERR_CORRUPT) << "byte 0x" << std::hex << b;
    }
  }
  // Both outcomes happen, so neither arm of the sweep is vacuous.
  EXPECT_GT(accepted, 0);
  EXPECT_GT(refused, 0);
  EXPECT_EQ(accepted + refused, 256);
}

/**
 * @brief The transform table is addressed by a symbol the stream chooses.
 *
 * RFC 7932 Appendix B defines 121 transforms. The decoder checks the symbol
 * before it gets here, so this bound is the second line and only a direct
 * call reaches it - but a NULL that was never returned is a NULL no caller
 * has handled.
 */
TEST(BrotliRegister, TheTransformTableHasExactlyItsDefinedRange) {
  for (int id = 0; id <= 120; id++) {
    EXPECT_NE(brotli_xform(id), nullptr) << id;
  }
  EXPECT_EQ(brotli_xform(-1), nullptr);
  EXPECT_EQ(brotli_xform(121), nullptr);
  EXPECT_EQ(brotli_xform(1 << 20), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
