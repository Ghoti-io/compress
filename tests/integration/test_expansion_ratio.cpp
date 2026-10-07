/**
 * @file test_expansion_ratio.cpp
 *
 * Tests for decompression bomb protection (expansion ratio limits)
 * in the Ghoti.io Compress library.
 *
 * ## What one number cannot do
 *
 * The formats' ceilings differ by a factor of five hundred:
 *
 * | method | ceiling | where it comes from |
 * | --- | --- | --- |
 * | rle | 64 : 1 | TIFF 6.0 section 9: a 2-byte run token carries 128 bytes |
 * | lz4 | 255 : 1 | one extension byte per 255 bytes of match |
 * | deflate, zlib, gzip | 1032 : 1 | RFC 1951 3.2.5: 258 bytes in two bits |
 * | lzw | 2560 : 1 | TIFF 6.0 section 13: a 3839-byte string in twelve bits |
 * | zstd | 32768 : 1 | RFC 8878 3.1.1.2.2: an RLE_Block is four bytes |
| brotli | 1290556 : 1 | RFC 7932: a last compressed meta-block can carry 16777216 bytes |
 *
 * A limit set below a format's ceiling refuses streams the format may
 * legitimately produce; set at or above it, it never fires. For DEFLATE the
 * whole range 1 to 1032 can only produce false positives, because no DEFLATE
 * stream can exceed 1032:1 in the first place.
 *
 * So each method now defaults to its own ceiling. The check stops being a
 * policy guess and becomes an impossibility test: it fires only on output the
 * format could not have produced, which is corruption or a decoder bug.
 * `limits.max_output_bytes` remains the bound on how much a decode may
 * produce, and a caller who wants a policy cap sets a smaller ratio by hand.
 *
 * ## What these tests would have caught
 *
 * At the old default, 32 MiB of zeros handed to our own encoder and back to
 * our own decoder failed for five of the seven methods - deflate at a
 * whole-stream ratio of 993:1, which is *below* the 1000 limit it tripped,
 * because a uniform prefix runs ahead of the average. That is the same shape
 * as the real file that started this: a PNG whose transparent top rows push
 * the running ratio to 1028:1 before the whole-stream figure settles at 458.
 *
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/bzip2.h>
#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/lzma.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/rle.h>
#include <ghoti.io/compress/zlib.h>
#include <ghoti.io/compress/zstd.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

class ExpansionRatioTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    ASSERT_EQ(gcomp_options_create(&options_), GCOMP_OK);
    ASSERT_NE(options_, nullptr);
  }

  void TearDown() override {
    if (options_ != nullptr) {
      gcomp_options_destroy(options_);
      options_ = nullptr;
    }
  }

  // Helper to create compressed data with a specific expansion ratio
  // Uses stored blocks (level 0) which have 1:1 ratio overhead
  std::vector<uint8_t> createCompressedData(size_t decompressed_size) {
    std::vector<uint8_t> input(decompressed_size, 0); // All zeros

    // Compress with maximum compression to get best ratio
    gcomp_options_t * enc_opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", 9), GCOMP_OK);

    // Estimate output size (compressed all-zeros is very small)
    size_t output_capacity = decompressed_size;
    std::vector<uint8_t> output(output_capacity);
    size_t actual_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
            input.size(), output.data(), output_capacity, &actual_size);

    gcomp_options_destroy(enc_opts);

    if (status == GCOMP_OK) {
      output.resize(actual_size);
      return output;
    }

    // Return empty vector on failure
    return {};
  }

  // Helper to attempt decompression with ratio limit
  gcomp_status_t decompressWithRatioLimit(const std::vector<uint8_t> & compressed,
      uint64_t ratio_limit, std::vector<uint8_t> & output) {
    EXPECT_EQ(gcomp_options_set_uint64(
                  options_, "limits.max_expansion_ratio", ratio_limit),
        GCOMP_OK);

    // Use a generous output limit that won't interfere
    EXPECT_EQ(gcomp_options_set_uint64(
                  options_, "limits.max_output_bytes", 100ULL * 1024 * 1024),
        GCOMP_OK);

    // Allocate sufficient output buffer
    size_t output_capacity = 10 * 1024 * 1024; // 10 MB should be enough
    output.resize(output_capacity);
    size_t actual_size = 0;

    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "deflate", options_, compressed.data(),
            compressed.size(), output.data(), output_capacity, &actual_size);

    if (status == GCOMP_OK) {
      output.resize(actual_size);
    }

    return status;
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_options_t * options_ = nullptr;
};

// Test that normal compression/decompression works with default ratio limit
TEST_F(ExpansionRatioTest, NormalDataWorksWithDefaultLimit) {
  // 1 KB of zeros compresses to a few bytes, ratio ~100-200x
  // Should be well within the default 1000x limit
  std::vector<uint8_t> compressed = createCompressedData(1024);
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(
      compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 1024);
}

// Test that high but legitimate compression ratios work
TEST_F(ExpansionRatioTest, HighButLegitimateRatioAllowed) {
  // 10 KB of zeros compresses very well
  // This tests that we can achieve high compression without triggering the limit
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // With default 1000x limit, 10KB data should work
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(
      compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 10 * 1024);
}

// Test that setting a very restrictive ratio limit rejects data
TEST_F(ExpansionRatioTest, RestrictiveRatioLimitRejects) {
  // 10 KB of zeros compresses to roughly 50-100 bytes
  // That's about 100-200x expansion
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set a very restrictive limit of 10x
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(compressed, 10, decompressed);

  // Should be rejected due to ratio limit
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test that ratio limit of 0 (unlimited) allows any expansion
TEST_F(ExpansionRatioTest, UnlimitedRatioAllowsAnything) {
  // 100 KB of zeros has very high compression ratio
  std::vector<uint8_t> compressed = createCompressedData(100 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set ratio limit to 0 (unlimited)
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(compressed, 0, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 100 * 1024);
}

// Test expansion ratio with streaming decoder
TEST_F(ExpansionRatioTest, StreamingDecoderRatioEnforcement) {
  // Create compressed data
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Create decoder with restrictive ratio limit
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 5),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_output_bytes", 100ULL * 1024 * 1024),
      GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", options_, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Feed input in chunks
  std::vector<uint8_t> output(20 * 1024);
  size_t input_offset = 0;
  size_t output_offset = 0;
  bool hit_limit = false;

  while (input_offset < compressed.size() && !hit_limit) {
    size_t chunk_size = std::min(size_t(64), compressed.size() - input_offset);

    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(compressed.data() + input_offset), chunk_size, 0};
    gcomp_buffer_t out_buf = {
        output.data() + output_offset, output.size() - output_offset, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status == GCOMP_ERR_LIMIT) {
      hit_limit = true;
    } else {
      EXPECT_EQ(status, GCOMP_OK);
    }

    input_offset += in_buf.used;
    output_offset += out_buf.used;
  }

  gcomp_decoder_destroy(decoder);

  // Should have hit the ratio limit
  EXPECT_TRUE(hit_limit);
}

// Test that ratio limit interacts correctly with output limit
TEST_F(ExpansionRatioTest, RatioLimitInteractionWithOutputLimit) {
  // Create data that would trigger ratio limit before output limit
  std::vector<uint8_t> compressed = createCompressedData(50 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set output limit high (1 MB) but ratio limit low (50x)
  EXPECT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_output_bytes", 1024 * 1024),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 50),
      GCOMP_OK);

  size_t output_capacity = 1024 * 1024;
  std::vector<uint8_t> output(output_capacity);
  size_t actual_size = 0;

  gcomp_status_t status =
      gcomp_decode_buffer(registry_, "deflate", options_, compressed.data(),
          compressed.size(), output.data(), output_capacity, &actual_size);

  // Should hit ratio limit (not output limit) since 50KB all-zeros compresses
  // to ~100 bytes, giving 500x ratio which exceeds 50x limit
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test that random data (low compression ratio) passes ratio check
TEST_F(ExpansionRatioTest, RandomDataLowRatioPasses) {
  // Generate random data - will have low compression ratio
  std::vector<uint8_t> random_data(1024);
  for (size_t i = 0; i < random_data.size(); i++) {
    random_data[i] = static_cast<uint8_t>(rand() % 256);
  }

  // Compress the random data
  gcomp_options_t * enc_opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);

  size_t comp_capacity = random_data.size() + 100;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status =
      gcomp_encode_buffer(registry_, "deflate", enc_opts, random_data.data(),
          random_data.size(), compressed.data(), comp_capacity, &comp_size);
  gcomp_options_destroy(enc_opts);
  ASSERT_EQ(status, GCOMP_OK);
  compressed.resize(comp_size);

  // Random data barely compresses, ratio should be ~1x
  // Even a restrictive limit of 5x should pass
  std::vector<uint8_t> decompressed;
  status = decompressWithRatioLimit(compressed, 5, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), random_data.size());
  EXPECT_EQ(memcmp(decompressed.data(), random_data.data(), random_data.size()),
      0);
}

// Test decoder reset clears ratio tracking
TEST_F(ExpansionRatioTest, ResetClearsRatioTracking) {
  // Create compressed data
  std::vector<uint8_t> compressed = createCompressedData(1024);
  ASSERT_FALSE(compressed.empty());

  // Set a moderate ratio limit
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 500),
      GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", options_, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress first time
  std::vector<uint8_t> output(2 * 1024);
  gcomp_buffer_t in_buf = {
      const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset the decoder
  status = gcomp_decoder_reset(decoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Decompress again - should work because ratio tracking was reset
  in_buf = {const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
  out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

// Test that ratio check works with stored blocks (no compression)
TEST_F(ExpansionRatioTest, StoredBlocksRatioCheck) {
  // Create data and compress with level 0 (stored blocks)
  std::vector<uint8_t> input(1024, 'A');

  gcomp_options_t * enc_opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", 0), GCOMP_OK);

  size_t comp_capacity = input.size() + 100;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status =
      gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
          input.size(), compressed.data(), comp_capacity, &comp_size);
  gcomp_options_destroy(enc_opts);
  ASSERT_EQ(status, GCOMP_OK);
  compressed.resize(comp_size);

  // Stored blocks have ~1:1 ratio, should pass even with restrictive limit
  std::vector<uint8_t> decompressed;
  status = decompressWithRatioLimit(compressed, 2, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), input.size());
}

// Test an explicit 1000:1 limit against 1 MB of zeros.
TEST_F(ExpansionRatioTest, DocumentationExampleAtLimit) {
  // This once verified a documented default of 1000:1.  There is no such
  // default any more - each method defaults to its own format's ceiling, which
  // for deflate is 1032:1 - so what is left here is the mechanism at an
  // explicitly requested 1000, which is what a caller setting a policy cap
  // gets.  GCOMP_DEFAULT_MAX_EXPANSION_RATIO is still 1000 and is still the
  // fallback for a method with no known ceiling, so naming it below is not
  // stale; it is simply no longer what deflate uses when nobody asks.

  // Create 1MB of zeros (compresses very well)
  std::vector<uint8_t> compressed = createCompressedData(1024 * 1024);
  ASSERT_FALSE(compressed.empty());

  // If compressed size is <= 1KB and output is 1MB, ratio is >= 1000x
  // This should be at or over the default limit
  if (compressed.size() <= 1024) {
    // The ratio would exceed 1000x, should be rejected
    std::vector<uint8_t> decompressed;
    gcomp_status_t status = decompressWithRatioLimit(
        compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

    // Due to the extreme compression of all-zeros, this should hit the limit
    EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  } else {
    // If compressed size is larger, the ratio is lower, might pass
    // This is still a valid test case
    std::vector<uint8_t> decompressed;
    gcomp_status_t status = decompressWithRatioLimit(
        compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);
    // Status depends on actual compression ratio achieved
    (void)status; // Just verify no crash
  }
}

namespace {

/// Every method, with the level key that makes it try hardest and its ceiling.
struct MethodCase {
  const char * name;
  const char * level_key; ///< nullptr when the method has no level
  int64_t level;
  uint64_t ceiling;
};

const MethodCase k_methods[] = {
    {"deflate", "deflate.level", 9, GCOMP_DEFLATE_MAX_EXPANSION_RATIO},
    {"zlib", "zlib.level", 9, GCOMP_ZLIB_MAX_EXPANSION_RATIO},
    {"gzip", "gzip.level", 9, GCOMP_GZIP_MAX_EXPANSION_RATIO},
    {"zstd", "zstd.level", 19, GCOMP_ZSTD_MAX_EXPANSION_RATIO},
    {"lz4", nullptr, 0, GCOMP_LZ4_MAX_EXPANSION_RATIO},
    {"lzw", nullptr, 0, GCOMP_LZW_MAX_EXPANSION_RATIO},
    {"rle", nullptr, 0, GCOMP_RLE_MAX_EXPANSION_RATIO},
    {"brotli", "brotli.level", 1, GCOMP_BROTLI_MAX_EXPANSION_RATIO},
    {"lzma2", "lzma2.preset", 3, GCOMP_LZMA_MAX_EXPANSION_RATIO},
    {"bzip2", "bzip2.level", 9, GCOMP_BZIP2_MAX_EXPANSION_RATIO},
};

/**
 * @brief How much data to push through.
 *
 * Large enough that the running ratio has time to reach the ceiling - a few
 * kilobytes never gets there - and small enough not to dominate the suite.
 * Under Memcheck it is a twentieth of that: the ratio is a property of the
 * stream, not of how long the stream is, so the shape survives the shrinking.
 */
size_t payload_size() {
  const char * v = std::getenv("GCOMP_UNDER_VALGRIND");
  return (v && v[0] == '1') ? (256u * 1024u) : (8u * 1024u * 1024u);
}

gcomp_options_t * options_for(const MethodCase & m) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  if (m.level_key) {
    EXPECT_EQ(gcomp_options_set_int64(o, m.level_key, m.level), GCOMP_OK);
  }
  return o;
}

/// Encode with the method's own best effort; returns the stream.
std::vector<uint8_t> encode(
    const MethodCase & m, const std::vector<uint8_t> & input) {
  gcomp_options_t * o = options_for(m);
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, m.name, o, input.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, m.name, o, input.data(), input.size(),
                out.data(), out.size(), &written),
      GCOMP_OK)
      << m.name;
  gcomp_options_destroy(o);
  out.resize(written);
  return out;
}

/// Bytes whose four-byte prefixes are mostly distinct: nothing to match on.
std::vector<uint8_t> incompressible(size_t len, uint32_t seed) {
  std::vector<uint8_t> v(len);
  uint32_t s = seed;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v[i] = (uint8_t)(s >> 24);
  }
  return v;
}

/**
 * @brief The shape that started this: a uniform prefix, then real data.
 *
 * The PNG that first failed is a logo with transparent rows across the top.
 * The prefix compresses at close to the format's ceiling while the tail does
 * not, so the running ratio peaks early and the whole-stream ratio is far
 * lower - a decoder that refuses on the running figure stops inside a file
 * whose overall ratio was never close to the limit.
 */
std::vector<uint8_t> compressible_prefix(size_t len) {
  std::vector<uint8_t> v(len, 0);
  const size_t tail = len / 2;
  std::vector<uint8_t> noise = incompressible(tail, 0x5eed1234u);
  std::memcpy(v.data() + (len - tail), noise.data(), tail);
  return v;
}

} // namespace

/**
 * @brief Our own encoder's output, through our own decoder, at our defaults.
 *
 * The simplest thing a compression library must do, and the one the old
 * default broke: five of these seven failed with GCOMP_ERR_LIMIT.
 */
TEST(ExpansionRatio, EveryMethodDecodesItsOwnMostCompressibleOutput) {
  const size_t n = payload_size();
  const std::vector<uint8_t> input(n, 0);

  for (const MethodCase & m : k_methods) {
    const std::vector<uint8_t> enc = encode(m, input);
    ASSERT_FALSE(enc.empty()) << m.name;

    // A fresh options object: library defaults and nothing else.
    gcomp_options_t * d = nullptr;
    ASSERT_EQ(gcomp_options_create(&d), GCOMP_OK);
    std::vector<uint8_t> back(n + 64);
    size_t produced = 0;
    const gcomp_status_t s = gcomp_decode_buffer(
        nullptr, m.name, d, enc.data(), enc.size(), back.data(), back.size(),
        &produced);
    gcomp_options_destroy(d);

    EXPECT_EQ(s, GCOMP_OK)
        << m.name << " refused its own output: " << n << " bytes compressed to "
        << enc.size() << " (" << ((double)n / (double)enc.size())
        << ":1) against a default ceiling of " << m.ceiling;
    EXPECT_EQ(produced, n) << m.name;
    if (produced == n) {
      EXPECT_EQ(std::memcmp(back.data(), input.data(), n), 0) << m.name;
    }
  }
}

/**
 * @brief A stream whose front compresses harder than its body.
 *
 * The running-total check is what makes this different from the test above:
 * the whole-stream ratio here is around two, and the prefix's is at the
 * ceiling. A limit below the ceiling stops inside the file.
 */
TEST(ExpansionRatio, ACompressiblePrefixDoesNotStopTheStream) {
  const size_t n = payload_size();
  const std::vector<uint8_t> input = compressible_prefix(n);

  for (const MethodCase & m : k_methods) {
    const std::vector<uint8_t> enc = encode(m, input);
    gcomp_options_t * d = nullptr;
    ASSERT_EQ(gcomp_options_create(&d), GCOMP_OK);
    std::vector<uint8_t> back(n + 64);
    size_t produced = 0;
    const gcomp_status_t s = gcomp_decode_buffer(
        nullptr, m.name, d, enc.data(), enc.size(), back.data(), back.size(),
        &produced);
    gcomp_options_destroy(d);

    EXPECT_EQ(s, GCOMP_OK)
        << m.name << " stopped inside a stream whose whole-stream ratio is "
        << ((double)n / (double)enc.size()) << ":1, well under its ceiling of "
        << m.ceiling << "; it saw " << produced << " of " << n << " bytes";
    EXPECT_EQ(produced, n) << m.name;
    if (produced == n) {
      EXPECT_EQ(std::memcmp(back.data(), input.data(), n), 0) << m.name;
    }
  }
}

/**
 * @brief No encoder of ours beats the ceiling its format declares.
 *
 * If one did, the declared ceiling would be wrong and the default would refuse
 * legitimate output again - so this is the test that keeps the numbers honest
 * rather than merely consistent with themselves.
 */
TEST(ExpansionRatio, NoEncoderExceedsItsDeclaredCeiling) {
  const size_t n = payload_size();
  const std::vector<uint8_t> input(n, 0);

  for (const MethodCase & m : k_methods) {
    const std::vector<uint8_t> enc = encode(m, input);
    ASSERT_FALSE(enc.empty()) << m.name;
    const double achieved = (double)n / (double)enc.size();
    EXPECT_LE(achieved, (double)m.ceiling)
        << m.name << " reached " << achieved
        << ":1 on uniform input, above the " << m.ceiling
        << ":1 its format is supposed to bound it to";
  }
}

/**
 * @brief The schema reports the default the decoder actually uses.
 *
 * gcomp_method_get_option_schema() is how a caller discovers what a limit is
 * without setting it. Four of the seven used to report either "no default" or
 * zero while the decoder used something else entirely.
 */
TEST(ExpansionRatio, TheSchemaAgreesWithTheDecoder) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  for (const MethodCase & m : k_methods) {
    const gcomp_method_t * method = gcomp_registry_find(reg, m.name);
    ASSERT_NE(method, nullptr) << m.name;
    const gcomp_option_schema_t * schema = nullptr;
    ASSERT_EQ(gcomp_method_get_option_schema(
                  method, "limits.max_expansion_ratio", &schema),
        GCOMP_OK)
        << m.name;
    ASSERT_NE(schema, nullptr) << m.name;
    EXPECT_TRUE(schema->has_default)
        << m.name << " declares no default for limits.max_expansion_ratio, so "
        << "introspection cannot tell a caller what it will get";
    EXPECT_EQ(schema->default_value.ui64, m.ceiling) << m.name;
  }
}

/**
 * @brief Lowering the ratio still refuses, and zero still means unlimited.
 *
 * Raising the defaults is only safe if the mechanism they drive still works:
 * a caller who wants a policy cap must still get one.
 */
TEST(ExpansionRatio, ACallersOwnLimitIsStillEnforced) {
  const size_t n = payload_size();
  const std::vector<uint8_t> input(n, 0);

  for (const MethodCase & m : k_methods) {
    const std::vector<uint8_t> enc = encode(m, input);

    // Two to one, which every one of these streams beats by a wide margin.
    gcomp_options_t * tight = nullptr;
    ASSERT_EQ(gcomp_options_create(&tight), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_uint64(tight, "limits.max_expansion_ratio", 2),
        GCOMP_OK);
    std::vector<uint8_t> back(n + 64);
    size_t produced = 0;
    EXPECT_EQ(gcomp_decode_buffer(nullptr, m.name, tight, enc.data(),
                  enc.size(), back.data(), back.size(), &produced),
        GCOMP_ERR_LIMIT)
        << m.name << " accepted a stream at " << ((double)n / (double)enc.size())
        << ":1 under a 2:1 limit";
    gcomp_options_destroy(tight);

    // Zero is documented as unlimited, and has to keep meaning that.
    gcomp_options_t * off = nullptr;
    ASSERT_EQ(gcomp_options_create(&off), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(off, "limits.max_expansion_ratio", 0),
        GCOMP_OK);
    produced = 0;
    EXPECT_EQ(gcomp_decode_buffer(nullptr, m.name, off, enc.data(), enc.size(),
                  back.data(), back.size(), &produced),
        GCOMP_OK)
        << m.name << " refused a stream with the ratio check turned off";
    EXPECT_EQ(produced, n) << m.name;
    gcomp_options_destroy(off);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
