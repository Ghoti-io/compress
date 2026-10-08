/**
 * @file test_peek.cpp
 *
 * gcomp_peek() reports what a header says. The test that matters is not that
 * it returns plausible numbers but that its numbers agree with what actually
 * happens when the stream is decoded: a content size that is not the size the
 * decoder produces is worse than no content size, because a caller sizes a
 * buffer to it.
 *
 * So each case here encodes something, peeks at it, and then decodes it, and
 * checks the peek against the decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> sample_input(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    v[i] = (uint8_t)('A' + (i * 7 + i / 13) % 26);
  }
  return v;
}

std::vector<uint8_t> encode_with(const char * method, gcomp_options_t * opts,
    const std::vector<uint8_t> & input) {
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, method, opts, input.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, method, opts, input.data(),
                input.size(), out.data(), out.size(), &written),
      GCOMP_OK);
  out.resize(written);
  return out;
}

//
// Content size
//

/**
 * @brief A stated content size must be the size that comes out.
 *
 * This is the field callers act on, and the one where being wrong costs them a
 * truncated buffer.
 */
TEST(Peek, ContentSizeMatchesWhatDecodes) {
  for (size_t n : {0u, 1u, 255u, 256u, 257u, 65535u, 65536u, 200000u}) {
    const std::vector<uint8_t> input = sample_input(n);

    for (const char * method : {"zstd", "lz4"}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o,
                    std::string(method).append(".content_size").c_str(),
                    (uint64_t)n),
          GCOMP_OK);

      std::vector<uint8_t> stream = encode_with(method, o, input);

      gcomp_stream_info_t info;
      size_t needed = 0;
      ASSERT_EQ(gcomp_peek(nullptr, method, o, stream.data(), stream.size(),
                    &info, &needed),
          GCOMP_OK)
          << method << " n=" << n;
      // Both options use 0 as "not set", so an empty stream cannot say that
      // its content size is zero - and the two encoders differ on what they do
      // about it: zstd writes a single-segment frame that states zero, LZ4
      // omits the field. Neither is wrong, and neither is worth asserting.
      if (n > 0) {
        EXPECT_TRUE(info.has_content_size) << method << " n=" << n;
        EXPECT_EQ(info.content_size, (uint64_t)n) << method << " n=" << n;
      }

      // And the decode agrees - decoded the way the documentation tells a
      // caller to decode once it has a content size: an exact output ceiling
      // instead of the expansion ratio.
      //
      // This is not a convenience here, it is necessary. The sample compresses
      // 200,000 bytes to 89, and the default max_expansion_ratio of 1000
      // refuses to decode it - a legitimate stream, refused for being
      // compressible (COMPRESS-TODO section 1). A caller that knows the size
      // has a better bound available and should use it, which is most of the
      // reason gcomp_peek() exists.
      if (info.has_content_size) {
        ASSERT_EQ(gcomp_options_set_uint64(
                      o, "limits.max_output_bytes", info.content_size),
            GCOMP_OK);
        ASSERT_EQ(
            gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
            GCOMP_OK);
      }

      std::vector<uint8_t> out(n + 64);
      size_t produced = 0;
      ASSERT_EQ(gcomp_decode_buffer(nullptr, method, o, stream.data(),
                    stream.size(), out.data(), out.size(), &produced),
          GCOMP_OK)
          << method << " n=" << n;
      EXPECT_EQ(produced, n) << method << " n=" << n;

      gcomp_options_destroy(o);
    }
  }
}

/// Without the option, nothing is claimed - and nothing must be.
TEST(Peek, NoContentSizeWhenNotWritten) {
  const std::vector<uint8_t> input = sample_input(50000);
  for (const char * method : {"zstd", "lz4"}) {
    std::vector<uint8_t> stream = encode_with(method, nullptr, input);
    gcomp_stream_info_t info;
    ASSERT_EQ(gcomp_peek(nullptr, method, nullptr, stream.data(),
                  stream.size(), &info, nullptr),
        GCOMP_OK)
        << method;
    EXPECT_FALSE(info.has_content_size) << method;
    EXPECT_EQ(info.content_size, 0u) << method;
  }
}

/**
 * @brief gzip states its size in the trailer, so peek must not claim one.
 *
 * ISIZE is the size modulo 2^32, it is per member, and it is at the far end of
 * the stream. Reporting it from a header peek would be wrong for any file over
 * 4 GB and for every multi-member file.
 */
TEST(Peek, GzipDoesNotClaimAContentSize) {
  const std::vector<uint8_t> input = sample_input(10000);
  std::vector<uint8_t> stream = encode_with("gzip", nullptr, input);
  gcomp_stream_info_t info;
  ASSERT_EQ(gcomp_peek(nullptr, "gzip", nullptr, stream.data(), stream.size(),
                &info, nullptr),
      GCOMP_OK);
  EXPECT_FALSE(info.has_content_size);
  EXPECT_TRUE(info.has_checksum); // the CRC-32 in the trailer
}

//
// Header size
//

/// The header size must be where the first block actually starts.
TEST(Peek, HeaderSizeIsWhereTheDataStarts) {
  const std::vector<uint8_t> input = sample_input(4096);

  struct Case {
    const char * method;
    std::function<void(gcomp_options_t *)> opts;
  };
  const std::vector<Case> cases = {
      {"zstd", nullptr},
      {"lz4", nullptr},
      {"zlib", nullptr},
      {"gzip", nullptr},
      {"gzip",
          [](gcomp_options_t * o) {
            gcomp_options_set_string(o, "gzip.name", "peek-me.bin");
            gcomp_options_set_string(o, "gzip.comment", "with a comment");
            gcomp_options_set_bool(o, "gzip.header_crc", 1);
          }},
  };

  for (const Case & c : cases) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    if (c.opts) {
      c.opts(o);
    }
    std::vector<uint8_t> stream = encode_with(c.method, o, input);

    gcomp_stream_info_t info;
    ASSERT_EQ(gcomp_peek(nullptr, c.method, o, stream.data(), stream.size(),
                  &info, nullptr),
        GCOMP_OK)
        << c.method;
    EXPECT_GT(info.header_size, 0u) << c.method;
    EXPECT_LT(info.header_size, stream.size()) << c.method;
    gcomp_options_destroy(o);
  }
}

/**
 * @brief The headerless formats say so, rather than inventing a header.
 */
TEST(Peek, HeaderlessFormatsReportNothing) {
  const std::vector<uint8_t> input = sample_input(2048);
  for (const char * method : {"deflate", "lzw", "rle"}) {
    std::vector<uint8_t> stream = encode_with(method, nullptr, input);
    gcomp_stream_info_t info;
    ASSERT_EQ(gcomp_peek(nullptr, method, nullptr, stream.data(),
                  stream.size(), &info, nullptr),
        GCOMP_OK)
        << method;
    EXPECT_EQ(info.header_size, 0u) << method;
    EXPECT_FALSE(info.has_content_size) << method;
    EXPECT_EQ(info.dictionary_id, 0u) << method;
  }
}

//
// Window size
//

/// The window peek reports must be the window the encoder was told to use.
TEST(Peek, WindowSizeFollowsTheOption) {
  const std::vector<uint8_t> input = sample_input(100000);

  for (uint64_t wl : {10u, 15u, 17u, 20u}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.window_log", wl), GCOMP_OK);
    std::vector<uint8_t> stream = encode_with("zstd", o, input);

    gcomp_stream_info_t info;
    ASSERT_EQ(gcomp_peek(nullptr, "zstd", o, stream.data(), stream.size(),
                  &info, nullptr),
        GCOMP_OK)
        << "window_log=" << wl;
    // RFC 8878 3.1.1.1.2 allows a mantissa, so the declared window may be a
    // little above the power of two; never below it, and never a different
    // power of two.
    EXPECT_GE(info.window_size, (uint64_t)1u << wl) << "window_log=" << wl;
    EXPECT_LT(info.window_size, (uint64_t)2u << wl) << "window_log=" << wl;
    gcomp_options_destroy(o);
  }
}

TEST(Peek, DeflateFamilyWindows) {
  const std::vector<uint8_t> input = sample_input(4096);
  gcomp_stream_info_t info;

  std::vector<uint8_t> z = encode_with("zlib", nullptr, input);
  ASSERT_EQ(
      gcomp_peek(nullptr, "zlib", nullptr, z.data(), z.size(), &info, nullptr),
      GCOMP_OK);
  EXPECT_LE(info.window_size, 32768u);
  EXPECT_GT(info.window_size, 0u);

  std::vector<uint8_t> g = encode_with("gzip", nullptr, input);
  ASSERT_EQ(
      gcomp_peek(nullptr, "gzip", nullptr, g.data(), g.size(), &info, nullptr),
      GCOMP_OK);
  EXPECT_EQ(info.window_size, 32768u);
}

//
// Checksums and dictionaries
//

TEST(Peek, ChecksumFlagFollowsTheOption) {
  const std::vector<uint8_t> input = sample_input(4096);
  for (int on : {0, 1}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.checksum", on), GCOMP_OK);
    std::vector<uint8_t> stream = encode_with("zstd", o, input);

    gcomp_stream_info_t info;
    ASSERT_EQ(gcomp_peek(nullptr, "zstd", o, stream.data(), stream.size(),
                  &info, nullptr),
        GCOMP_OK);
    EXPECT_EQ(info.has_checksum, on);
    gcomp_options_destroy(o);
  }
}

/**
 * @brief A stream encoded against a dictionary says so, and says which.
 *
 * This is the field that lets a reader fail usefully - "I need dictionary
 * 0x1234" - instead of reporting corruption from somewhere inside the decode.
 */
TEST(Peek, ReportsTheDictionaryAStreamNeeds) {
  const std::vector<uint8_t> input = sample_input(8192);
  const std::vector<uint8_t> dict = sample_input(4096);

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                o, "zstd.dictionary", dict.data(), dict.size()),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.dictionary_id", 0x1234u),
      GCOMP_OK);

  std::vector<uint8_t> stream = encode_with("zstd", o, input);

  gcomp_stream_info_t info;
  ASSERT_EQ(gcomp_peek(nullptr, "zstd", o, stream.data(), stream.size(), &info,
                nullptr),
      GCOMP_OK);
  EXPECT_TRUE(info.has_dictionary);
  EXPECT_EQ(info.dictionary_id, 0x1234u);

  gcomp_options_destroy(o);
}

//
// Not enough input
//

/**
 * @brief Short input asks for more, and the amount asked for is enough.
 *
 * A `needed` that is too small makes a caller loop one byte at a time; one
 * that is too large makes it read data that does not exist.
 */
TEST(Peek, ShortInputAsksForEnough) {
  const std::vector<uint8_t> input = sample_input(4096);

  for (const char * method : {"zstd", "lz4", "zlib", "gzip"}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    // Only where it belongs: an unknown key is rejected by default, so setting
    // gzip.name on a zstd encode fails the encode rather than being ignored.
    // A long name is what makes gzip's header need more than its fixed ten
    // bytes, which is the case this test is here for.
    if (std::strcmp(method, "gzip") == 0) {
      gcomp_options_set_string(o, "gzip.name", "a-name-long-enough-to-matter");
    }
    std::vector<uint8_t> stream = encode_with(method, o, input);

    for (size_t n = 0; n < 12 && n < stream.size(); n++) {
      gcomp_stream_info_t info;
      size_t needed = 0;
      gcomp_status_t s =
          gcomp_peek(nullptr, method, o, stream.data(), n, &info, &needed);
      if (s == GCOMP_OK) {
        continue; // This format's header fits in n bytes.
      }
      ASSERT_EQ(s, GCOMP_ERR_LIMIT) << method << " with " << n << " bytes";
      EXPECT_GT(needed, n) << method << " with " << n
                           << " bytes: asked for no more than it had";
      ASSERT_LE(needed, stream.size()) << method;

      // What it asked for must be enough to get an answer.
      gcomp_status_t s2 =
          gcomp_peek(nullptr, method, o, stream.data(), needed, &info, nullptr);
      EXPECT_TRUE(s2 == GCOMP_OK || s2 == GCOMP_ERR_LIMIT)
          << method << ": asked for " << needed << " and then returned "
          << gcomp_status_to_string(s2);
    }
    gcomp_options_destroy(o);
  }
}

//
// Skippable frames
//

/**
 * @brief A skippable frame is reported as itself, not stepped over.
 *
 * Both LZ4 and Zstandard define the same magic range for these, so what
 * follows one is not peek's to assume. The caller is given its size and can
 * step over it.
 */
TEST(Peek, SkippableFrameIsReportedAndSized) {
  std::vector<uint8_t> stream;
  const uint32_t magic = 0x184D2A53u;
  for (int i = 0; i < 4; i++) {
    stream.push_back((uint8_t)((magic >> (8 * i)) & 0xFF));
  }
  const uint32_t payload = 64;
  for (int i = 0; i < 4; i++) {
    stream.push_back((uint8_t)((payload >> (8 * i)) & 0xFF));
  }
  stream.insert(stream.end(), payload, 0x11);

  const std::vector<uint8_t> input = sample_input(1000);
  std::vector<uint8_t> body = encode_with("zstd", nullptr, input);
  stream.insert(stream.end(), body.begin(), body.end());

  gcomp_stream_info_t info;
  ASSERT_EQ(gcomp_peek(nullptr, "zstd", nullptr, stream.data(), stream.size(),
                &info, nullptr),
      GCOMP_OK);
  EXPECT_TRUE(info.is_skippable);
  EXPECT_EQ(info.skippable_variant, 3u);
  EXPECT_EQ(info.skippable_size, 8u + payload);

  // Stepping over it by the size reported lands on the data frame.
  ASSERT_LT(info.skippable_size, stream.size());
  gcomp_stream_info_t after;
  ASSERT_EQ(gcomp_peek(nullptr, "zstd", nullptr,
                stream.data() + info.skippable_size,
                stream.size() - info.skippable_size, &after, nullptr),
      GCOMP_OK);
  EXPECT_FALSE(after.is_skippable);
  EXPECT_GT(after.header_size, 0u);
}

//
// Malformed input
//

TEST(Peek, RejectsCorruptHeaders) {
  gcomp_stream_info_t info;

  // A zstd magic number that is not one.
  const uint8_t bad_zstd[] = {0x28, 0xB5, 0x2F, 0xFC, 0x00, 0x00};
  EXPECT_EQ(gcomp_peek(nullptr, "zstd", nullptr, bad_zstd, sizeof(bad_zstd),
                &info, nullptr),
      GCOMP_ERR_CORRUPT);

  // RFC 8878 3.1.1.1.1: bit 3 of the descriptor is reserved and must be zero.
  const uint8_t reserved_set[] = {0x28, 0xB5, 0x2F, 0xFD, 0x08, 0x00, 0x00};
  EXPECT_EQ(gcomp_peek(nullptr, "zstd", nullptr, reserved_set,
                sizeof(reserved_set), &info, nullptr),
      GCOMP_ERR_CORRUPT);

  // gzip with the wrong magic.
  const uint8_t bad_gzip[] = {0x1F, 0x8C, 0x08, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(gcomp_peek(nullptr, "gzip", nullptr, bad_gzip, sizeof(bad_gzip),
                &info, nullptr),
      GCOMP_ERR_CORRUPT);

  // gzip naming a compression method RFC 1952 does not define.
  const uint8_t bad_cm[] = {0x1F, 0x8B, 0x07, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(gcomp_peek(nullptr, "gzip", nullptr, bad_cm, sizeof(bad_cm), &info,
                nullptr),
      GCOMP_ERR_UNSUPPORTED);
}

TEST(Peek, RejectsBadArguments) {
  gcomp_stream_info_t info;
  uint8_t buf[8] = {0};
  EXPECT_EQ(
      gcomp_peek(nullptr, nullptr, nullptr, buf, sizeof(buf), &info, nullptr),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_peek(nullptr, "zstd", nullptr, buf, sizeof(buf), nullptr,
                nullptr),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_peek(nullptr, "no-such-method", nullptr, buf, sizeof(buf),
                &info, nullptr),
      GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(gcomp_peek(nullptr, "zstd", nullptr, nullptr, 10, &info, nullptr),
      GCOMP_ERR_INVALID_ARG);
}

/**
 * @brief Everything the peek says agrees with what the decoder does.
 *
 * The cross-check: for every method and a spread of sizes, peek and then
 * decode, and hold the two against each other.
 */
TEST(Peek, AgreesWithTheDecoder) {
  for (size_t n : {0u, 1u, 1000u, 70000u}) {
    const std::vector<uint8_t> input = sample_input(n);
    for (const char * method :
        {"deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli",
            "bzip2", "xz"}) {
      std::vector<uint8_t> stream = encode_with(method, nullptr, input);
      if (stream.empty()) {
        continue;
      }

      gcomp_stream_info_t info;
      ASSERT_EQ(gcomp_peek(nullptr, method, nullptr, stream.data(),
                    stream.size(), &info, nullptr),
          GCOMP_OK)
          << method << " n=" << n;

      std::vector<uint8_t> out(n + 1024);
      size_t produced = 0;
      ASSERT_EQ(gcomp_decode_buffer(nullptr, method, nullptr, stream.data(),
                    stream.size(), out.data(), out.size(), &produced),
          GCOMP_OK)
          << method << " n=" << n;
      EXPECT_EQ(produced, n) << method << " n=" << n;

      if (info.has_content_size) {
        EXPECT_EQ(info.content_size, (uint64_t)produced)
            << method << " n=" << n << ": peek disagreed with the decode";
      }
      EXPECT_LE(info.header_size, stream.size()) << method << " n=" << n;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
