/**
 * @file test_detect.cpp
 *
 * gcomp_detect() has to be right about two different things: that it
 * recognises what our own encoders write, and that it does not recognise
 * things it has not been shown.
 *
 * The second is the harder one. A detector that answers confidently on random
 * bytes is worse than no detector, because the caller hands those bytes to a
 * decoder and gets a corruption error from somewhere deep inside it. So the
 * false-positive rate is measured here rather than assumed: zlib's two-byte
 * header check should fire on about one random pair in 31, and nothing else
 * should fire at all.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> sample_input(size_t len = 4096) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    v[i] = (uint8_t)('a' + (i % 23));
  }
  return v;
}

/// Encode `input` with `method` and return the stream.
std::vector<uint8_t> encode_with(
    const char * method, const std::vector<uint8_t> & input) {
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, method, nullptr, input.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, method, nullptr, input.data(),
                input.size(), out.data(), out.size(), &written),
      GCOMP_OK);
  out.resize(written);
  return out;
}

/// A skippable frame with `payload` bytes, magic variant `variant`.
std::vector<uint8_t> skippable_frame(uint8_t variant, size_t payload) {
  std::vector<uint8_t> f;
  uint32_t magic = 0x184D2A50u + (variant & 0x0Fu);
  for (int i = 0; i < 4; i++) {
    f.push_back((uint8_t)((magic >> (8 * i)) & 0xFF));
  }
  for (int i = 0; i < 4; i++) {
    f.push_back((uint8_t)((payload >> (8 * i)) & 0xFF));
  }
  f.insert(f.end(), payload, 0x7E);
  return f;
}

//
// What it must recognise
//

TEST(Detect, RecognisesOurOwnStreams) {
  const std::vector<uint8_t> input = sample_input();
  for (const char * method : {"gzip", "zstd", "lz4", "zlib"}) {
    std::vector<uint8_t> stream = encode_with(method, input);
    const char * name = nullptr;
    size_t needed = 0;
    ASSERT_EQ(gcomp_detect(stream.data(), stream.size(), &name, &needed),
        GCOMP_OK)
        << method;
    ASSERT_NE(name, nullptr) << method;
    EXPECT_STREQ(name, method);
  }
}

/// And what it detects must then actually decode, which is the point of it.
TEST(Detect, DetectedMethodDecodes) {
  const std::vector<uint8_t> input = sample_input();
  for (const char * method : {"gzip", "zstd", "lz4", "zlib"}) {
    std::vector<uint8_t> stream = encode_with(method, input);
    const char * name = nullptr;
    ASSERT_EQ(gcomp_detect(stream.data(), stream.size(), &name, nullptr),
        GCOMP_OK);

    std::vector<uint8_t> out(input.size() + 64);
    size_t produced = 0;
    ASSERT_EQ(gcomp_decode_buffer(nullptr, name, nullptr, stream.data(),
                  stream.size(), out.data(), out.size(), &produced),
        GCOMP_OK)
        << name;
    out.resize(produced);
    EXPECT_EQ(out, input) << name;
  }
}

//
// What it must not claim to recognise
//

/**
 * @brief The formats that begin with data cannot be detected, and must not be.
 *
 * A deflate, LZW, RLE or brotli stream starts with whatever the first block
 * happens to be. Brotli's first bits are a window size, not a magic number.
 * The only honest answer is that it is not known - except that
 * a two-byte prefix can coincidentally satisfy zlib's header check, which the
 * zlib case below measures.
 */
TEST(Detect, DoesNotGuessAtHeaderlessFormats) {
  const std::vector<uint8_t> input = sample_input();
  for (const char * method : {"deflate", "lzw", "rle", "brotli"}) {
    std::vector<uint8_t> stream = encode_with(method, input);
    const char * name = nullptr;
    gcomp_status_t s =
        gcomp_detect(stream.data(), stream.size(), &name, nullptr);
    // Either "not known", or the zlib coincidence - never one of the three
    // magic formats, which would be a real false positive.
    if (s == GCOMP_OK) {
      EXPECT_STREQ(name, "zlib")
          << method << " stream detected as " << name;
    }
    else {
      EXPECT_EQ(s, GCOMP_ERR_UNSUPPORTED) << method;
    }
  }
}

/**
 * @brief The false-positive rate on random bytes, measured rather than assumed.
 *
 * The three magic formats must never fire: 32 bits of magic against uniform
 * bytes is one chance in four billion, and 20,000 trials would not hit it.
 *
 * zlib's is a real rate rather than zero, and it is worth knowing what it is.
 * RFC 1950 section 2.2 constrains three things independently: CM must be 8
 * (one byte value in sixteen), CINFO must be at most 7 (one in two), and the
 * two bytes together must be a multiple of 31 (one in 31). That is about one
 * random pair in 992, so roughly 20 hits in 20,000 - not the one in 31 that
 * the divisibility test alone would suggest, which is what this test was first
 * written to expect and what measuring it corrected.
 *
 * The interval is wide enough not to flake on the Poisson spread at that rate,
 * and narrow enough to notice a check that has stopped checking: were the CM
 * and CINFO tests dropped, this would fire about 645 times.
 */
TEST(Detect, FalsePositiveRateOnRandomBytes) {
  const int kTrials = 20000;
  int magic_hits = 0;
  int zlib_hits = 0;

  uint32_t s = 2463534242u;
  for (int i = 0; i < kTrials; i++) {
    uint8_t buf[16];
    for (unsigned char & b : buf) {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      b = (uint8_t)(s >> 24);
    }
    const char * name = nullptr;
    if (gcomp_detect(buf, sizeof(buf), &name, nullptr) == GCOMP_OK) {
      if (std::strcmp(name, "zlib") == 0) {
        zlib_hits++;
      }
      else {
        magic_hits++;
      }
    }
  }

  EXPECT_EQ(magic_hits, 0)
      << "a magic-number format matched random bytes " << magic_hits
      << " times in " << kTrials;
  EXPECT_GT(zlib_hits, 5) << "zlib header check fired " << zlib_hits
                          << " times in " << kTrials
                          << "; about 1 in 992, so near 20, was expected";
  EXPECT_LT(zlib_hits, 60) << "zlib header check fired " << zlib_hits
                           << " times in " << kTrials
                           << "; about 1 in 992, so near 20, was expected";
}

//
// Not enough input yet
//

TEST(Detect, ShortInputAsksForMore) {
  const std::vector<uint8_t> input = sample_input();
  std::vector<uint8_t> stream = encode_with("zstd", input);

  for (size_t n = 0; n < 4; n++) {
    const char * name = nullptr;
    size_t needed = 0;
    EXPECT_EQ(gcomp_detect(stream.data(), n, &name, &needed), GCOMP_ERR_LIMIT)
        << "with " << n << " bytes";
    EXPECT_GE(needed, 4u) << "with " << n << " bytes";
  }

  // And with the bytes it asked for, it answers.
  const char * name = nullptr;
  EXPECT_EQ(gcomp_detect(stream.data(), 4, &name, nullptr), GCOMP_OK);
  EXPECT_STREQ(name, "zstd");
}

TEST(Detect, EmptyInput) {
  const char * name = nullptr;
  size_t needed = 0;
  EXPECT_EQ(gcomp_detect(nullptr, 0, &name, &needed), GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 4u);
}

TEST(Detect, RejectsBadArguments) {
  uint8_t buf[8] = {0};
  EXPECT_EQ(gcomp_detect(buf, sizeof(buf), nullptr, nullptr),
      GCOMP_ERR_INVALID_ARG);
  const char * name = nullptr;
  EXPECT_EQ(gcomp_detect(nullptr, 10, &name, nullptr), GCOMP_ERR_INVALID_ARG);
}

//
// Skippable frames
//

/**
 * @brief A skippable frame is not an answer, because it belongs to both formats.
 *
 * LZ4 and Zstandard define it on the same magic range with the same layout, so
 * a stream that opens with one is not identifiable from that frame. It has to
 * be stepped over.
 */
TEST(Detect, StepsOverSkippableFrames) {
  const std::vector<uint8_t> input = sample_input();

  for (const char * method : {"zstd", "lz4"}) {
    std::vector<uint8_t> stream;
    std::vector<uint8_t> pre = skippable_frame(0, 100);
    stream.insert(stream.end(), pre.begin(), pre.end());
    std::vector<uint8_t> body = encode_with(method, input);
    stream.insert(stream.end(), body.begin(), body.end());

    const char * name = nullptr;
    ASSERT_EQ(gcomp_detect(stream.data(), stream.size(), &name, nullptr),
        GCOMP_OK)
        << method;
    EXPECT_STREQ(name, method);
  }
}

TEST(Detect, StepsOverSeveralSkippableFrames) {
  const std::vector<uint8_t> input = sample_input();
  std::vector<uint8_t> stream;
  for (uint8_t variant = 0; variant < 4; variant++) {
    std::vector<uint8_t> f = skippable_frame(variant, 16u * variant);
    stream.insert(stream.end(), f.begin(), f.end());
  }
  std::vector<uint8_t> body = encode_with("zstd", input);
  stream.insert(stream.end(), body.begin(), body.end());

  const char * name = nullptr;
  ASSERT_EQ(gcomp_detect(stream.data(), stream.size(), &name, nullptr),
      GCOMP_OK);
  EXPECT_STREQ(name, "zstd");
}

/// A stream that is only skippable frames has nothing to identify.
TEST(Detect, OnlySkippableFramesAsksForMore) {
  std::vector<uint8_t> stream = skippable_frame(5, 32);
  const char * name = nullptr;
  size_t needed = 0;
  EXPECT_EQ(gcomp_detect(stream.data(), stream.size(), &name, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_GE(needed, stream.size() + 4u);
}

/// A skippable frame cut short asks for the rest of it, not for four bytes.
TEST(Detect, TruncatedSkippableFrameAsksForTheRest) {
  const std::vector<uint8_t> input = sample_input();
  std::vector<uint8_t> stream;
  std::vector<uint8_t> pre = skippable_frame(1, 5000);
  stream.insert(stream.end(), pre.begin(), pre.end());
  std::vector<uint8_t> body = encode_with("zstd", input);
  stream.insert(stream.end(), body.begin(), body.end());

  const char * name = nullptr;
  size_t needed = 0;
  EXPECT_EQ(gcomp_detect(stream.data(), 100, &name, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_GE(needed, 5000u) << "should have asked past the skippable payload";

  // Given that much, it answers.
  ASSERT_LE(needed, stream.size());
  EXPECT_EQ(gcomp_detect(stream.data(), needed, &name, nullptr), GCOMP_OK);
  EXPECT_STREQ(name, "zstd");
}

/**
 * @brief A declared payload size that runs off the end must not wrap.
 *
 * The size comes from the stream, so it is chosen by whoever wrote it.
 */
TEST(Detect, AbsurdSkippableSizeDoesNotWrap) {
  std::vector<uint8_t> stream = skippable_frame(0, 0);
  // Rewrite the declared size to 4 GB - 1 while supplying almost nothing.
  stream[4] = 0xFF;
  stream[5] = 0xFF;
  stream[6] = 0xFF;
  stream[7] = 0xFF;

  const char * name = nullptr;
  size_t needed = 0;
  EXPECT_EQ(gcomp_detect(stream.data(), stream.size(), &name, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_GE(needed, 0xFFFFFFFFull) << "needed wrapped";
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
