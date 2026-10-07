/**
 * @file test_options.cpp
 *
 * Unit tests for the options API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "failing_allocator.h"
#include <cstdlib>
#include <vector>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

class OptionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh options object for each test
    gcomp_options_create(&options_);
  }

  void TearDown() override {
    if (options_ != nullptr) {
      gcomp_options_destroy(options_);
      options_ = nullptr;
    }
  }

  gcomp_options_t * options_ = nullptr;
};

// Test gcomp_options_create()
TEST_F(OptionsTest, CreateSuccess) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(opts, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(OptionsTest, CreateNullPointer) {
  gcomp_status_t status = gcomp_options_create(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_destroy()
TEST_F(OptionsTest, DestroyNullPointer) {
  // Should not crash
  gcomp_options_destroy(nullptr);
}

TEST_F(OptionsTest, DestroyCleanup) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set some values
  ASSERT_EQ(gcomp_options_set_int64(opts, "test.int", 42), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "test.str", "hello"), GCOMP_OK);

  // Destroy should clean up
  gcomp_options_destroy(opts);
  // If we get here without crashing, cleanup worked
}

// Test gcomp_options_set_int64() / gcomp_options_get_int64()
TEST_F(OptionsTest, SetGetInt64) {
  int64_t value = 0;

  // Set a value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);

  // Get it back
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);
}

TEST_F(OptionsTest, SetInt64Overwrite) {
  int64_t value = 0;

  // Set initial value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);

  // Overwrite with new value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 100), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 100);
}

TEST_F(OptionsTest, GetInt64NotFound) {
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetInt64NullPointer) {
  EXPECT_EQ(
      gcomp_options_set_int64(nullptr, "test.int", 42), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_options_set_int64(options_, nullptr, 42), GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetInt64NullPointer) {
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(nullptr, "test.int", &value),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_get_int64(options_, nullptr, &value),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetInt64TypeMismatch) {
  // Set as int64
  EXPECT_EQ(gcomp_options_set_int64(options_, "test", 42), GCOMP_OK);

  // Try to get as uint64 (should fail or return error)
  uint64_t uvalue = 0;
  EXPECT_NE(gcomp_options_get_uint64(options_, "test", &uvalue), GCOMP_OK);
}

TEST_F(OptionsTest, SetInt64MaxValues) {
  int64_t value = 0;

  // Test maximum positive value
  EXPECT_EQ(gcomp_options_set_int64(options_, "max", INT64_MAX), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "max", &value), GCOMP_OK);
  EXPECT_EQ(value, INT64_MAX);

  // Test minimum negative value
  EXPECT_EQ(gcomp_options_set_int64(options_, "min", INT64_MIN), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "min", &value), GCOMP_OK);
  EXPECT_EQ(value, INT64_MIN);
}

// Test gcomp_options_set_uint64() / gcomp_options_get_uint64()
TEST_F(OptionsTest, SetGetUint64) {
  uint64_t value = 0;

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 100), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 100U);
}

TEST_F(OptionsTest, SetUint64Overwrite) {
  uint64_t value = 0;

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 42U);

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 200), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 200U);
}

TEST_F(OptionsTest, GetUint64NotFound) {
  uint64_t value = 0;
  EXPECT_EQ(gcomp_options_get_uint64(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetUint64MaxValue) {
  uint64_t value = 0;
  EXPECT_EQ(gcomp_options_set_uint64(options_, "max", UINT64_MAX), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "max", &value), GCOMP_OK);
  EXPECT_EQ(value, UINT64_MAX);
}

// Test gcomp_options_set_bool() / gcomp_options_get_bool()
TEST_F(OptionsTest, SetGetBool) {
  int value = -1;

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 1);

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool2", 0), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool2", &value), GCOMP_OK);
  EXPECT_EQ(value, 0);
}

TEST_F(OptionsTest, SetBoolOverwrite) {
  int value = -1;

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 1);

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 0), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 0);
}

TEST_F(OptionsTest, GetBoolNotFound) {
  int value = -1;
  EXPECT_EQ(gcomp_options_get_bool(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_set_string() / gcomp_options_get_string()
TEST_F(OptionsTest, SetGetString) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, "hello");
}

TEST_F(OptionsTest, SetStringOverwrite) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_STREQ(value, "hello");

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "world"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_STREQ(value, "world");
}

TEST_F(OptionsTest, SetStringEmpty) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", ""), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, "");
}

TEST_F(OptionsTest, SetStringNullPointer) {
  EXPECT_EQ(gcomp_options_set_string(nullptr, "test.str", "hello"),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_set_string(options_, nullptr, "hello"),
      GCOMP_ERR_INVALID_ARG);
  // NULL value might be allowed or not - test both behaviors
  // For now, assume NULL value is invalid
  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetStringNotFound) {
  const char * value = nullptr;
  EXPECT_EQ(gcomp_options_get_string(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetStringLong) {
  const char * value = nullptr;
  std::string long_str(1000, 'a');

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", long_str.c_str()),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, long_str.c_str());
}

// Test gcomp_options_set_bytes() / gcomp_options_get_bytes()
TEST_F(OptionsTest, SetGetBytes) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};

  EXPECT_EQ(gcomp_options_set_bytes(
                options_, "test.bytes", test_data, sizeof(test_data)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_NE(data_out, nullptr);
  EXPECT_EQ(size_out, sizeof(test_data));
  EXPECT_BUFFERS_EQ(test_data, sizeof(test_data), data_out, size_out);
}

TEST_F(OptionsTest, SetBytesOverwrite) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  uint8_t data1[] = {0x01, 0x02};
  uint8_t data2[] = {0x03, 0x04, 0x05};

  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", data1, sizeof(data1)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", data2, sizeof(data2)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, sizeof(data2));
  EXPECT_BUFFERS_EQ(data2, sizeof(data2), data_out, size_out);
}

TEST_F(OptionsTest, SetBytesEmpty) {
  const void * data_out = nullptr;
  size_t size_out = 0;

  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", nullptr, 0), GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, 0U);
}

TEST_F(OptionsTest, SetBytesNullPointer) {
  uint8_t data[] = {0x01, 0x02};
  EXPECT_EQ(gcomp_options_set_bytes(nullptr, "test.bytes", data, sizeof(data)),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_set_bytes(options_, nullptr, data, sizeof(data)),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetBytesNotFound) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "nonexistent", &data_out, &size_out),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetBytesLarge) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  std::vector<uint8_t> large_data(10000);
  for (size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  EXPECT_EQ(gcomp_options_set_bytes(
                options_, "test.bytes", large_data.data(), large_data.size()),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, large_data.size());
  EXPECT_BUFFERS_EQ(large_data.data(), large_data.size(), data_out, size_out);
}

// Test gcomp_options_clone()
TEST_F(OptionsTest, Clone) {
  // Set various types of values
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 100U), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  uint8_t bytes[] = {0x01, 0x02, 0x03};
  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", bytes, sizeof(bytes)),
      GCOMP_OK);

  // Clone
  gcomp_options_t * cloned = nullptr;
  EXPECT_EQ(gcomp_options_clone(options_, &cloned), GCOMP_OK);
  EXPECT_NE(cloned, nullptr);

  // Verify all values are cloned correctly
  int64_t int_val = 0;
  EXPECT_EQ(gcomp_options_get_int64(cloned, "test.int", &int_val), GCOMP_OK);
  EXPECT_EQ(int_val, 42);

  uint64_t uint_val = 0;
  EXPECT_EQ(gcomp_options_get_uint64(cloned, "test.uint", &uint_val), GCOMP_OK);
  EXPECT_EQ(uint_val, 100U);

  int bool_val = 0;
  EXPECT_EQ(gcomp_options_get_bool(cloned, "test.bool", &bool_val), GCOMP_OK);
  EXPECT_EQ(bool_val, 1);

  const char * str_val = nullptr;
  EXPECT_EQ(gcomp_options_get_string(cloned, "test.str", &str_val), GCOMP_OK);
  EXPECT_STREQ(str_val, "hello");

  const void * bytes_data = nullptr;
  size_t bytes_size = 0;
  EXPECT_EQ(
      gcomp_options_get_bytes(cloned, "test.bytes", &bytes_data, &bytes_size),
      GCOMP_OK);
  EXPECT_EQ(bytes_size, sizeof(bytes));
  EXPECT_BUFFERS_EQ(bytes, sizeof(bytes), bytes_data, bytes_size);

  // Cleanup
  gcomp_options_destroy(cloned);
}

TEST_F(OptionsTest, CloneNullPointer) {
  gcomp_options_t * cloned = nullptr;
  EXPECT_EQ(gcomp_options_clone(nullptr, &cloned), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_clone(options_, nullptr), GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_freeze()
TEST_F(OptionsTest, Freeze) {
  // Set a value before freezing
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);

  // Freeze
  EXPECT_EQ(gcomp_options_freeze(options_), GCOMP_OK);

  // Should still be able to read
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);

  // Should not be able to set new values
  EXPECT_NE(gcomp_options_set_int64(options_, "test.int", 100), GCOMP_OK);
  EXPECT_NE(gcomp_options_set_int64(options_, "test.new", 200), GCOMP_OK);

  // Verify original value unchanged
  value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);
}

TEST_F(OptionsTest, FreezeNullPointer) {
  EXPECT_EQ(gcomp_options_freeze(nullptr), GCOMP_ERR_INVALID_ARG);
}

// Test memory cleanup with many values
TEST_F(OptionsTest, MemoryCleanupManyValues) {
  // Set many values
  for (int i = 0; i < 100; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key.%d", i);
    EXPECT_EQ(gcomp_options_set_int64(options_, key, i), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_string(options_, key, "test string"), GCOMP_OK);
  }

  // Destroy should clean up all memory
  gcomp_options_destroy(options_);
  options_ = nullptr;
  // If we get here without crashing, cleanup worked
}

//
// Option validation at create time
//
// The schemas have always said this happens -- "validate user-provided
// options at create time", "unknown keys cause GCOMP_UNKNOWN_KEY_ERROR at
// create time" -- but nothing called gcomp_options_validate(), so a
// misspelled key or a value set with the wrong type was silently ignored and
// the caller quietly got defaults it never asked for.
//

namespace {

gcomp_status_t createEncoderWith(const char * method,
    void (*setup)(gcomp_options_t *)) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GCOMP_ERR_INTERNAL;
  }
  setup(opts);
  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t st =
      gcomp_encoder_create(gcomp_registry_default(), method, opts, &enc);
  if (st == GCOMP_OK) {
    gcomp_encoder_destroy(enc);
  }
  gcomp_options_destroy(opts);
  return st;
}

gcomp_status_t createDecoderWith(const char * method,
    void (*setup)(gcomp_options_t *)) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GCOMP_ERR_INTERNAL;
  }
  setup(opts);
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t st =
      gcomp_decoder_create(gcomp_registry_default(), method, opts, &dec);
  if (st == GCOMP_OK) {
    gcomp_decoder_destroy(dec);
  }
  gcomp_options_destroy(opts);
  return st;
}

void setNothing(gcomp_options_t *) {}
void setMisspelled(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "lz4.blocksize", 65536);
}
void setForeignKey(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "zstd.level", 3);
}
void setWrongTypeBool(gcomp_options_t * o) {
  gcomp_options_set_bool(o, "lz4.content_size", 1); // uint64 in the schema
}
void setWrongTypeString(gcomp_options_t * o) {
  gcomp_options_set_string(o, "lz4.block_size", "65536");
}
void setOutOfRange(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "lz4.block_size", 99); // below the schema min
}
void setValid(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "lz4.block_size", 65536);
}
// brotli's window and level, at each boundary.  The method used to repeat
// these ranges in brotli_encoder_init() and the repetition was unreachable,
// because this validation runs first; the ranges live only in the schema now,
// so the boundary has to be asserted through the public entry point.
void setBrotliWindowBelowMin(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.lgwin", 9);
}
void setBrotliWindowAboveMax(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.lgwin", 25);
}
void setBrotliWindowAtMin(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.lgwin", 10);
}
void setBrotliWindowAtMax(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.lgwin", 24);
}
void setBrotliLevelBelowMin(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.level", -1);
}
void setBrotliLevelAboveMax(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "brotli.level", 2);
}
// lzma and lzma2 at each boundary of each range, for the same reason as
// brotli's: the encoder does not repeat them, so only the schema holds them.
template <const char * (*Key)(), int64_t V>
void setLzmaInt(gcomp_options_t * o) {
  gcomp_options_set_int64(o, Key(), V);
}
template <const char * (*Key)(), uint64_t V>
void setLzmaUint(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, Key(), V);
}
const char * kLzmaPreset() { return "lzma.preset"; }
const char * kLzmaLc() { return "lzma.lc"; }
const char * kLzmaLp() { return "lzma.lp"; }
const char * kLzmaPb() { return "lzma.pb"; }
const char * kLzmaDict() { return "lzma.dict_size"; }
const char * kLzma2Preset() { return "lzma2.preset"; }
const char * kLzma2Lc() { return "lzma2.lc"; }
const char * kLzma2Dict() { return "lzma2.dict_size"; }
const char * kBzip2Level() { return "bzip2.level"; }
void setZeroOutputAndRatio(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "limits.max_output_bytes", 0);
  gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0);
}
void setDeflateLevel(gcomp_options_t * o) {
  gcomp_options_set_int64(o, "deflate.level", 6); // int64, not uint64
}
void setDeflateLevelWrongType(gcomp_options_t * o) {
  gcomp_options_set_uint64(o, "deflate.level", 6);
}
void setGzipOwnAndPassthrough(gcomp_options_t * o) {
  gcomp_options_set_string(o, "gzip.name", "x.txt");
  gcomp_options_set_int64(o, "deflate.level", 3);
  gcomp_options_set_uint64(o, "limits.max_output_bytes", 1u << 20);
}

} // namespace

TEST(OptionValidationTest, MistakesAreRejectedAtCreateTime) {
  struct Case {
    const char * what;
    const char * method;
    void (*setup)(gcomp_options_t *);
    gcomp_status_t want;
  };
  static const Case kCases[] = {
      {"no options set", "lz4", setNothing, GCOMP_OK},
      {"a valid option", "lz4", setValid, GCOMP_OK},
      {"a misspelled key", "lz4", setMisspelled, GCOMP_ERR_INVALID_ARG},
      {"another method's key", "lz4", setForeignKey, GCOMP_ERR_INVALID_ARG},
      {"bool set on a uint64", "lz4", setWrongTypeBool,
          GCOMP_ERR_INVALID_ARG},
      {"string set on a uint64", "lz4", setWrongTypeString,
          GCOMP_ERR_INVALID_ARG},
      {"a value below the minimum", "lz4", setOutOfRange,
          GCOMP_ERR_INVALID_ARG},
      {"deflate.level as int64", "deflate", setDeflateLevel, GCOMP_OK},
      {"deflate.level as uint64", "deflate", setDeflateLevelWrongType,
          GCOMP_ERR_INVALID_ARG},
      {"brotli.lgwin below the minimum", "brotli", setBrotliWindowBelowMin,
          GCOMP_ERR_INVALID_ARG},
      {"brotli.lgwin above the maximum", "brotli", setBrotliWindowAboveMax,
          GCOMP_ERR_INVALID_ARG},
      {"brotli.lgwin at the minimum", "brotli", setBrotliWindowAtMin,
          GCOMP_OK},
      {"brotli.lgwin at the maximum", "brotli", setBrotliWindowAtMax,
          GCOMP_OK},
      {"brotli.level below the minimum", "brotli", setBrotliLevelBelowMin,
          GCOMP_ERR_INVALID_ARG},
      {"brotli.level above the maximum", "brotli", setBrotliLevelAboveMax,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.preset below the minimum", "lzma", setLzmaInt<kLzmaPreset, -1>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.preset at the minimum", "lzma", setLzmaInt<kLzmaPreset, 0>,
          GCOMP_OK},
      {"lzma.preset at the maximum", "lzma", setLzmaInt<kLzmaPreset, 9>,
          GCOMP_OK},
      {"lzma.preset above the maximum", "lzma", setLzmaInt<kLzmaPreset, 10>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.lc at the maximum", "lzma", setLzmaInt<kLzmaLc, 8>, GCOMP_OK},
      {"lzma.lc above the maximum", "lzma", setLzmaInt<kLzmaLc, 9>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.lp at the maximum", "lzma", setLzmaInt<kLzmaLp, 4>, GCOMP_OK},
      {"lzma.lp above the maximum", "lzma", setLzmaInt<kLzmaLp, 5>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.pb at the maximum", "lzma", setLzmaInt<kLzmaPb, 4>, GCOMP_OK},
      {"lzma.pb above the maximum", "lzma", setLzmaInt<kLzmaPb, 5>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma.dict_size below the minimum", "lzma",
          setLzmaUint<kLzmaDict, 4095>, GCOMP_ERR_INVALID_ARG},
      {"lzma.dict_size at the minimum", "lzma", setLzmaUint<kLzmaDict, 4096>,
          GCOMP_OK},
      // The schema's top, 4 GiB - 1, is a value the encoder then cannot
      // allocate for (it holds about six times the dictionary), so the
      // largest one that creates is a size that can be.
      {"lzma.dict_size at a large size", "lzma", setLzmaUint<kLzmaDict, 1u << 26>,
          GCOMP_OK},
      {"lzma.dict_size above the maximum", "lzma",
          setLzmaUint<kLzmaDict, 0x100000000ull>, GCOMP_ERR_INVALID_ARG},
      {"lzma2.preset above the maximum", "lzma2", setLzmaInt<kLzma2Preset, 10>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma2.lc above its maximum of 4", "lzma2", setLzmaInt<kLzma2Lc, 5>,
          GCOMP_ERR_INVALID_ARG},
      {"lzma2.dict_size below the minimum", "lzma2",
          setLzmaUint<kLzma2Dict, 4095>, GCOMP_ERR_INVALID_ARG},
      {"bzip2.level below the minimum", "bzip2", setLzmaInt<kBzip2Level, 0>,
          GCOMP_ERR_INVALID_ARG},
      {"bzip2.level at the minimum", "bzip2", setLzmaInt<kBzip2Level, 1>,
          GCOMP_OK},
      {"bzip2.level at the maximum", "bzip2", setLzmaInt<kBzip2Level, 9>,
          GCOMP_OK},
      {"bzip2.level above the maximum", "bzip2", setLzmaInt<kBzip2Level, 10>,
          GCOMP_ERR_INVALID_ARG},
  };

  for (const Case & c : kCases) {
    EXPECT_EQ(createEncoderWith(c.method, c.setup), c.want)
        << c.what << " (" << c.method << " encoder)";
    EXPECT_EQ(createDecoderWith(c.method, c.setup), c.want)
        << c.what << " (" << c.method << " decoder)";
  }
}

TEST(OptionValidationTest, ZeroIsAcceptedForTheLimitsThatMeanUnlimited) {
  // Zero means "unlimited" for these, and GzipLimitsTest asserts exactly that
  // for both -- so a schema declaring a minimum of 1 rejects a documented
  // value before the method ever sees it.  lz4 and zstd both declared one.
  for (const char * method :
      {"lz4", "zstd", "lzw", "rle", "deflate", "gzip", "brotli", "lzma",
          "lzma2", "bzip2"}) {
    EXPECT_EQ(createEncoderWith(method, setZeroOutputAndRatio), GCOMP_OK)
        << method << ": zero output/ratio limits must be accepted";
    EXPECT_EQ(createDecoderWith(method, setZeroOutputAndRatio), GCOMP_OK)
        << method;
  }
}

TEST(OptionValidationTest, ZeroMeansUnlimitedForEveryLimitOnEveryMethod) {
  // limits.h documents "0 means unlimited" for every limit option, in nine
  // places, and the core helpers implement it -- gcomp_limits_check_output(),
  // gcomp_memory_check_limit() and gcomp_limits_check_expansion_ratio() all
  // return GCOMP_OK on a zero limit.  Methods that open-coded the comparison
  // instead kept forgetting: lz4's output check and zstd's output and memory
  // checks each read 0 as "none permitted" and refused a frame they should
  // have decoded.
  //
  // This goes through a real decode rather than only creating a codec,
  // because two of those three were checks made while decoding -- creating
  // the decoder succeeded and the failure came later.
  std::vector<uint8_t> data(20000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)("the quick brown fox "[i % 20]);
  }

  static const char * kKeys[] = {"limits.max_output_bytes",
      "limits.max_memory_bytes", "limits.max_expansion_ratio"};

  for (const char * method :
      {"deflate", "gzip", "lz4", "lzw", "rle", "zstd", "brotli"}) {
    for (const char * key : kKeys) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      gcomp_options_set_uint64(opts, key, 0);

      // Encoding with the limit set matters too, and setting it only on the
      // decode side is how zstd's encoder memory check stayed uncaught: the
      // limits are read on both paths.
      std::vector<uint8_t> enc(data.size() * 2 + 65536);
      size_t enc_len = 0;
      ASSERT_EQ(gcomp_encode_buffer(nullptr, method, opts, data.data(),
                    data.size(), enc.data(), enc.size(), &enc_len),
          GCOMP_OK)
          << method << " encoding with " << key << " = 0 (means unlimited)";

      std::vector<uint8_t> dec(data.size() + 65536);
      size_t dec_len = 0;
      gcomp_status_t st = gcomp_decode_buffer(nullptr, method, opts,
          enc.data(), enc_len, dec.data(), dec.size(), &dec_len);
      gcomp_options_destroy(opts);

      ASSERT_EQ(st, GCOMP_OK)
          << method << " decoding with " << key << " = 0 (means unlimited)";
      dec.resize(dec_len);
      EXPECT_EQ(dec, data) << method << " with " << key << " = 0";
    }
  }
}

TEST(OptionValidationTest, LimitsAreStillEnforcedWhenTheyAreNotZero) {
  // The other half: making 0 mean unlimited must not have made every limit
  // toothless.
  std::vector<uint8_t> data(20000, 'q');
  for (const char * method :
      {"deflate", "gzip", "lz4", "lzw", "rle", "zstd", "brotli"}) {
    std::vector<uint8_t> enc(data.size() * 2 + 65536);
    size_t enc_len = 0;
    ASSERT_EQ(gcomp_encode_buffer(nullptr, method, nullptr, data.data(),
                  data.size(), enc.data(), enc.size(), &enc_len),
        GCOMP_OK)
        << method;

    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100);

    std::vector<uint8_t> dec(data.size() + 65536);
    size_t dec_len = 0;
    gcomp_status_t st = gcomp_decode_buffer(nullptr, method, opts, enc.data(),
        enc_len, dec.data(), dec.size(), &dec_len);
    gcomp_options_destroy(opts);

    EXPECT_EQ(st, GCOMP_ERR_LIMIT)
        << method << ": a 100-byte output limit must stop a 20,000-byte "
                     "decode";
  }
}

TEST(OptionValidationTest, EveryMethodDeclaresTheLimitsItHonours) {
  // A schema that omits an option the method reads cannot be used to validate
  // a caller's options -- the caller gets rejected for passing something the
  // method would have honoured.  deflate and gzip read the limits through the
  // gcomp_limits_* helpers and did not declare any of them.
  struct Expect {
    const char * method;
    const char * key;
  };
  static const Expect kExpected[] = {
      {"deflate", "limits.max_output_bytes"},
      {"deflate", "limits.max_memory_bytes"},
      {"deflate", "limits.max_expansion_ratio"},
      {"deflate", "limits.max_window_bytes"},
      {"gzip", "limits.max_output_bytes"},
      {"gzip", "limits.max_memory_bytes"},
      {"gzip", "limits.max_expansion_ratio"},
      {"lz4", "limits.max_block_bytes"},
      {"zstd", "limits.max_window_bytes"},
  };
  for (const Expect & e : kExpected) {
    const gcomp_method_t * m =
        gcomp_registry_find(gcomp_registry_default(), e.method);
    ASSERT_NE(m, nullptr) << e.method;
    const gcomp_option_schema_t * opt = nullptr;
    EXPECT_EQ(gcomp_method_get_option_schema(m, e.key, &opt), GCOMP_OK)
        << e.method << " does not declare " << e.key
        << ", which it honours at runtime";
  }
}

TEST(OptionValidationTest, WrapperMethodsStillTakeTheirPassthroughOptions) {
  // gzip wraps deflate and hands it the caller's options.  It used to hand
  // over all of them, on the stated grounds that deflate would ignore what it
  // did not know -- deflate's policy is GCOMP_UNKNOWN_KEY_ERROR, so that was
  // never true, and validating at create time is what made it visible.
  EXPECT_EQ(createEncoderWith("gzip", setGzipOwnAndPassthrough), GCOMP_OK);
  EXPECT_EQ(createDecoderWith("gzip", setGzipOwnAndPassthrough), GCOMP_OK);

  // gzip's own policy is to ignore keys it does not recognise, which a
  // wrapper needs; that must survive validation being switched on.
  EXPECT_EQ(createEncoderWith("gzip", setForeignKey), GCOMP_OK);
}

namespace {

/// One string option, the values it takes, and one it must not.
struct StringOptionCase {
  const char * method;
  const char * key;
  const char * const * good; ///< NULL-terminated
  const char * const * bad;  ///< NULL-terminated
};

const char * const k_rle_good[] = {"packbits", "tga", nullptr};
const char * const k_rle_bad[] = {"rle", "RLE", "packbits ", "", nullptr};
const char * const k_lzw_fmt_good[] = {"gif", "tiff", nullptr};
const char * const k_lzw_fmt_bad[] = {"lzw", "TIFF", "", nullptr};
const char * const k_lzw_lookup_good[] = {"linear", "hash", nullptr};
const char * const k_lzw_lookup_bad[] = {"hsah", "Hash", "", nullptr};
const char * const k_deflate_good[] = {
    "default", "lazy", "huffman_only", "rle", "fixed", nullptr};
const char * const k_deflate_bad[] = {"lasy", "Default", "", nullptr};

const StringOptionCase k_string_options[] = {
    {"rle", "rle.format", k_rle_good, k_rle_bad},
    {"lzw", "lzw.format", k_lzw_fmt_good, k_lzw_fmt_bad},
    {"lzw", "lzw.encoder_lookup", k_lzw_lookup_good, k_lzw_lookup_bad},
    {"deflate", "deflate.strategy", k_deflate_good, k_deflate_bad},
};

} // namespace

/**
 * @brief A string option with a fixed set refuses everything outside it.
 *
 * These were enforced but not declared, which put the refusal in the wrong
 * place or lost it entirely:
 *
 * - `rle.format` accepted any string and failed later as "RLE profile encode
 *   failed", which names nothing the caller set.
 * - `lzw.encoder_lookup` was worse than that: the encoder selects the linear
 *   scan on an exact "linear" and treats *everything else* as "hash", so a
 *   typo silently chose a mode. Both modes produce identical bytes, so no
 *   later check could ever have caught it.
 * - `deflate.strategy` already refused an unknown name, but from inside the
 *   encoder rather than from validation.
 *
 * The schema had no way to say any of it, because min and max are numeric.
 */
TEST(Options, AStringOptionRefusesValuesOutsideItsDeclaredSet) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  for (const StringOptionCase & c : k_string_options) {
    for (const char * const * v = c.good; *v; v++) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_string(o, c.key, *v), GCOMP_OK);
      gcomp_encoder_t * enc = nullptr;
      EXPECT_EQ(gcomp_encoder_create(reg, c.method, o, &enc), GCOMP_OK)
          << c.key << " = \"" << *v << "\" is valid and was refused";
      if (enc) {
        gcomp_encoder_destroy(enc);
      }
      gcomp_options_destroy(o);
    }

    for (const char * const * v = c.bad; *v; v++) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_string(o, c.key, *v), GCOMP_OK);
      gcomp_encoder_t * enc = nullptr;
      EXPECT_NE(gcomp_encoder_create(reg, c.method, o, &enc), GCOMP_OK)
          << c.key << " = \"" << *v << "\" is not a value this option takes "
          << "and an encoder was created for it";
      if (enc) {
        gcomp_encoder_destroy(enc);
      }
      gcomp_options_destroy(o);
    }
  }
}

/**
 * @brief The set is discoverable, not merely enforced.
 *
 * Half the point of declaring it: a caller can ask what a mode selector takes
 * instead of parsing the help text.
 */
TEST(Options, ADeclaredSetIsVisibleThroughIntrospection) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  for (const StringOptionCase & c : k_string_options) {
    const gcomp_method_t * method = gcomp_registry_find(reg, c.method);
    ASSERT_NE(method, nullptr) << c.method;
    const gcomp_option_schema_t * schema = nullptr;
    ASSERT_EQ(gcomp_method_get_option_schema(method, c.key, &schema), GCOMP_OK)
        << c.key;
    ASSERT_NE(schema, nullptr) << c.key;
    ASSERT_NE(schema->allowed, nullptr)
        << c.key << " enforces a set of values but does not declare it";

    // Every value the option accepts is named, and in the same order the
    // method documents, so the list can be printed as-is.
    size_t n = 0;
    for (const char * const * p = schema->allowed; *p; p++) {
      n++;
    }
    size_t expected = 0;
    for (const char * const * v = c.good; *v; v++) {
      expected++;
    }
    EXPECT_EQ(n, expected) << c.key;
    for (size_t i = 0; i < n && i < expected; i++) {
      EXPECT_STREQ(schema->allowed[i], c.good[i]) << c.key << " index " << i;
    }
  }
}

/**
 * @brief A free-form string option declares no set, and must not.
 *
 * RFC 1952 section 2.3.1 makes FNAME and FCOMMENT arbitrary text. A list here
 * would be wrong rather than missing, so this is the case that says the new
 * field means "these and no others" rather than "nobody filled this in yet".
 */
TEST(Options, AFreeFormStringOptionDeclaresNoSet) {
  gcomp_registry_t * reg = gcomp_registry_default();
  const gcomp_method_t * method = gcomp_registry_find(reg, "gzip");
  ASSERT_NE(method, nullptr);

  for (const char * key : {"gzip.name", "gzip.comment"}) {
    const gcomp_option_schema_t * schema = nullptr;
    ASSERT_EQ(gcomp_method_get_option_schema(method, key, &schema), GCOMP_OK)
        << key;
    ASSERT_NE(schema, nullptr) << key;
    EXPECT_EQ(schema->allowed, nullptr)
        << key << " declares a set of permitted values, but a gzip filename "
        << "or comment is arbitrary text";

    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_string(o, key, "anything at all.txt"),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(reg, "gzip", o, &enc), GCOMP_OK) << key;
    if (enc) {
      gcomp_encoder_destroy(enc);
    }
    gcomp_options_destroy(o);
  }
}

/**
 * @brief A set that fails leaves the options object exactly as it was.
 *
 * set_entry() used to link the entry, free the value it was replacing, and
 * only then allocate the new one. A failed allocation therefore reported
 * GCOMP_ERR_MEMORY - correctly - while having already destroyed the previous
 * value and left a string entry holding NULL.
 *
 * Nothing read that field, so it stayed invisible for as long as no string
 * option declared the values it accepts. Once one did, validation saw a string
 * entry with no string and refused the whole object with
 * GCOMP_ERR_INVALID_ARG, naming neither the real problem nor the memory that
 * caused it. tests/core/test_alloc_failure.cpp found it at allocation 12
 * of 24.
 */
TEST(Options, AFailedSetLeavesThePreviousValueIntact) {
  FailingAllocator fa;
  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create_with_allocator(fa.allocator(), &o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(o, "rle.format", "tga"), GCOMP_OK);

  // Fail the very next allocation, which is the copy of the new string.
  fa.fail_from(fa.calls() + 1);
  EXPECT_EQ(gcomp_options_set_string(o, "rle.format", "packbits"),
      GCOMP_ERR_MEMORY);
  fa.fail_from(0);

  const char * value = nullptr;
  ASSERT_EQ(gcomp_options_get_string(o, "rle.format", &value), GCOMP_OK)
      << "a failed set removed the option that was already there";
  ASSERT_NE(value, nullptr)
      << "a failed set left the option present but holding no string";
  EXPECT_STREQ(value, "tga")
      << "a failed set destroyed the value it was replacing";

  // And the object is still usable, which is the point of saying it is
  // unchanged rather than merely non-crashing.
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(
      gcomp_encoder_create(gcomp_registry_default(), "rle", o, &enc), GCOMP_OK);
  if (enc) {
    gcomp_encoder_destroy(enc);
  }
  gcomp_options_destroy(o);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
