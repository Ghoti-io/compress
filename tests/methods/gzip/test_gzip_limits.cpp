/**
 * @file test_gzip_limits.cpp
 *
 * Unit tests for gzip decoder limit enforcement.
 *
 * These tests verify:
 * - limits.max_output_bytes enforcement
 * - limits.max_expansion_ratio enforcement
 * - Header field size limits (FNAME, FCOMMENT, FEXTRA)
 * - Limits apply correctly across concatenated members
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class GzipLimitsTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data with options
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 1024);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return result;
  }

  // Helper: Decompress data with options, returns status
  gcomp_status_t decompress_with_status(const void * data, size_t len,
      gcomp_options_t * opts, std::vector<uint8_t> & output) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    output.resize(16 * 1024 * 1024); // 16 MB max

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      output.clear();
      return status;
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    gcomp_decoder_destroy(decoder);

    if (status == GCOMP_OK) {
      output.resize(out_buf.used);
    }
    else {
      output.clear();
    }
    return status;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Max Output Bytes Tests
//

TEST_F(GzipLimitsTest, MaxOutputBytesEnforced) {
  // Create compressible data that expands to 1000 bytes
  std::vector<uint8_t> original(1000, 'A'); // Highly compressible
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set limit to 500 bytes (less than uncompressed size)
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 500);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should fail with limit error
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(GzipLimitsTest, MaxOutputBytesAllowsWithinLimit) {
  // Create data that is exactly at the limit
  std::vector<uint8_t> original(500, 'B');
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set limit to 500 bytes (exactly the uncompressed size)
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 500);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should succeed
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.size(), 500u);

  gcomp_options_destroy(opts);
}

TEST_F(GzipLimitsTest, MaxOutputBytesZeroMeansUnlimited) {
  // Create some data
  std::vector<uint8_t> original(10000, 'C');
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set limit to 0 (unlimited)
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 0);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should succeed
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.size(), 10000u);

  gcomp_options_destroy(opts);
}

//
// Max Expansion Ratio Tests
//

TEST_F(GzipLimitsTest, ExpansionRatioEnforced) {
  // Create highly compressible data (1 MB of zeros compresses very small)
  std::vector<uint8_t> original(1024 * 1024, 0); // 1 MB of zeros
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // With zeros, compression ratio is typically > 1000x
  // Set a small expansion limit
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set very small expansion ratio (10x)
  status = gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should fail with limit error
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(GzipLimitsTest, ExpansionRatioAllowsReasonable) {
  // Create moderately compressible data
  std::vector<uint8_t> original(10000);
  test_helpers_generate_sequential(original.data(), original.size());

  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Sequential data compresses somewhat but not drastically
  // Allow a reasonable expansion ratio
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set generous expansion ratio (1000x - the default)
  status = gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should succeed
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.size(), original.size());

  gcomp_options_destroy(opts);
}

TEST_F(GzipLimitsTest, ExpansionRatioZeroMeansUnlimited) {
  // Create highly compressible data
  std::vector<uint8_t> original(100000, 0);
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set expansion ratio to 0 (unlimited)
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);
  ASSERT_EQ(status, GCOMP_OK);

  // Also need to set output limit to 0 to avoid default limit
  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 0);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should succeed despite high expansion
  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), opts, output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.size(), original.size());

  gcomp_options_destroy(opts);
}

//
// Header Field Limit Tests
//

TEST_F(GzipLimitsTest, FNAMELimitEnforced) {
  // Create encoder options with long filename
  gcomp_options_t * enc_opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&enc_opts);
  ASSERT_EQ(status, GCOMP_OK);

  std::string long_name(200, 'x');
  status = gcomp_options_set_string(enc_opts, "gzip.name", long_name.c_str());
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> original = {'H', 'i'};
  std::vector<uint8_t> compressed =
      compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Decode with small name limit
  gcomp_options_t * dec_opts = nullptr;
  status = gcomp_options_create(&dec_opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(dec_opts, "gzip.max_name_bytes", 50);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), dec_opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(dec_opts);
}

// A header field longer than the decoder's internal buffer, which the default
// limit allows and that buffer could not hold.
//
// FNAME and FCOMMENT carry no length, so they were accumulated a byte at a
// time into header_accum - 1024 bytes - while the bound was checked against
// gzip.max_name_bytes, a megabyte by default. Any gzip file with a filename
// over 1 KiB wrote past that array on default options; UBSan found it in the
// header of an ordinary file. The accumulator grows now.
//
// The member is built here rather than by this library's encoder, which
// refuses to emit a header larger than GZIP_MAX_HEADER_BUFFER and so cannot
// produce the input at all. That asymmetry is the point: a decoder has to read
// what other producers write.
TEST_F(GzipLimitsTest, AHeaderFieldLongerThanTheInternalBufferIsRead) {
  const std::vector<uint8_t> payload = {'H', 'i', ' ', 't', 'h', 'e', 'r', 'e'};

  // The payload as a raw deflate stream, which is what a gzip member carries.
  std::vector<uint8_t> deflated(1024);
  size_t deflated_size = 0;
  ASSERT_EQ(gcomp_encode_buffer(gcomp_registry_default(), "deflate", nullptr,
                payload.data(), payload.size(), deflated.data(),
                deflated.size(), &deflated_size),
      GCOMP_OK);
  deflated.resize(deflated_size);

  struct Field {
    const char * name;
    uint8_t flag;
    size_t length;
  };
  // Both fields, because they are separate cases in the same state machine and
  // only one of them being wrong is the shape this started as.
  const std::vector<Field> fields = {
      {"FNAME", 0x08, 4000},
      {"FCOMMENT", 0x10, 5000},
  };

  for (const auto & f : fields) {
    // Sized once, then filled: GCC 16 under TSan takes a fill appended to the
    // ten-byte initializer list (insert or resize) for an overrun of it, and
    // -Werror=array-bounds then stops test-tsan building at all.
    static const uint8_t header[10] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x03};
    std::vector<uint8_t> member(sizeof(header) + f.length, (uint8_t)'n');
    memcpy(member.data(), header, sizeof(header));
    member[3] = f.flag;
    member.push_back(0x00); // the field's terminator

    member.insert(member.end(), deflated.begin(), deflated.end());

    // gcomp_crc32() returns the unfinalized value; RFC 1952 wants the
    // finalized one, which is what gcomp_crc32_finalize() is for.
    uint32_t crc =
        gcomp_crc32_finalize(gcomp_crc32(payload.data(), payload.size()));
    uint32_t isize = (uint32_t)payload.size();
    for (int i = 0; i < 4; i++) {
      member.push_back((uint8_t)((crc >> (8 * i)) & 0xff));
    }
    for (int i = 0; i < 4; i++) {
      member.push_back((uint8_t)((isize >> (8 * i)) & 0xff));
    }

    std::vector<uint8_t> output;
    gcomp_status_t status =
        decompress_with_status(member.data(), member.size(), nullptr, output);
    EXPECT_EQ(status, GCOMP_OK) << f.name << " of " << f.length << " bytes";
    EXPECT_EQ(output, payload) << f.name;
  }
}

TEST_F(GzipLimitsTest, FCOMMENTLimitEnforced) {
  // Create encoder options with long comment
  gcomp_options_t * enc_opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&enc_opts);
  ASSERT_EQ(status, GCOMP_OK);

  std::string long_comment(200, 'y');
  status =
      gcomp_options_set_string(enc_opts, "gzip.comment", long_comment.c_str());
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> original = {'H', 'i'};
  std::vector<uint8_t> compressed =
      compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Decode with small comment limit
  gcomp_options_t * dec_opts = nullptr;
  status = gcomp_options_create(&dec_opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(dec_opts, "gzip.max_comment_bytes", 50);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), dec_opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(dec_opts);
}

TEST_F(GzipLimitsTest, FEXTRALimitEnforced) {
  // Create encoder options with extra data
  gcomp_options_t * enc_opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&enc_opts);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> extra_data(100, 0xAB);
  status = gcomp_options_set_bytes(
      enc_opts, "gzip.extra", extra_data.data(), extra_data.size());
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> original = {'H', 'i'};
  std::vector<uint8_t> compressed =
      compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Decode with small extra limit
  gcomp_options_t * dec_opts = nullptr;
  status = gcomp_options_create(&dec_opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(dec_opts, "gzip.max_extra_bytes", 10);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output;
  status = decompress_with_status(
      compressed.data(), compressed.size(), dec_opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(dec_opts);
}

//
// Limits with Concatenated Members
//

TEST_F(GzipLimitsTest, LimitsApplyAcrossConcatMembers) {
  // Create two members, each 500 bytes
  std::vector<uint8_t> data1(500, 'A');
  std::vector<uint8_t> data2(500, 'B');

  std::vector<uint8_t> comp1 = compress(data1.data(), data1.size());
  std::vector<uint8_t> comp2 = compress(data2.data(), data2.size());
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Concatenate
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), comp1.begin(), comp1.end());
  concat.insert(concat.end(), comp2.begin(), comp2.end());

  // Set limit that allows first member but not both
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 750);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should fail when trying to produce more than 750 bytes total
  std::vector<uint8_t> output;
  status = decompress_with_status(concat.data(), concat.size(), opts, output);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(GzipLimitsTest, LimitsAllowConcatWithinBounds) {
  // Create two small members
  std::vector<uint8_t> data1(200, 'A');
  std::vector<uint8_t> data2(200, 'B');

  std::vector<uint8_t> comp1 = compress(data1.data(), data1.size());
  std::vector<uint8_t> comp2 = compress(data2.data(), data2.size());
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Concatenate
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), comp1.begin(), comp1.end());
  concat.insert(concat.end(), comp2.begin(), comp2.end());

  // Set limit that allows both members
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 500);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode should succeed
  std::vector<uint8_t> output;
  status = decompress_with_status(concat.data(), concat.size(), opts, output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.size(), 400u);

  gcomp_options_destroy(opts);
}

//
// Error Message Tests
//

TEST_F(GzipLimitsTest, LimitErrorHasDetailedMessage) {
  std::vector<uint8_t> original(1000, 'X');
  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output(16 * 1024 * 1024);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  // Check that error detail message is set
  const char * detail = gcomp_decoder_get_error_detail(decoder);
  EXPECT_NE(detail, nullptr);
  EXPECT_NE(strlen(detail), 0u);
  // Message should mention "limit" or similar
  EXPECT_TRUE(strstr(detail, "limit") != nullptr ||
      strstr(detail, "exceeds") != nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
