/**
 * @file test_schema.cpp
 *
 * Unit tests for option schema introspection and validation helpers
 * in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

/**
 * Define a simple dummy method with a small option schema for testing.
 */

static const gcomp_option_schema_t kDummyOptionSchemas[] = {
    {
        "dummy.int",                             /* key */
        GCOMP_OPT_INT64,                         /* type */
        1,                                       /* has_default */
        {.i64 = 5},                              /* default_value */
        1,                                       /* has_min */
        1,                                       /* has_max */
        0,                                       /* min_int */
        10,                                      /* max_int */
        0,                                       /* min_uint (unused) */
        0,                                       /* max_uint (unused) */
        "Integer option with default and range", /* help */
        nullptr, /* allowed */
    },
    {
        "dummy.uint",                          /* key */
        GCOMP_OPT_UINT64,                      /* type */
        0,                                     /* has_default */
        {0},                                   /* default_value (unused) */
        0,                                     /* has_min */
        1,                                     /* has_max */
        0,                                     /* min_int (unused) */
        0,                                     /* max_int (unused) */
        0,                                     /* min_uint */
        100,                                   /* max_uint */
        "Unsigned option with max constraint", /* help */
        nullptr, /* allowed */
    },
    {
        "dummy.flag",          /* key */
        GCOMP_OPT_BOOL,        /* type */
        0,                     /* has_default */
        {0},                   /* default_value (unused) */
        0,                     /* has_min */
        0,                     /* has_max */
        0,                     /* min_int (unused) */
        0,                     /* max_int (unused) */
        0,                     /* min_uint (unused) */
        0,                     /* max_uint (unused) */
        "Boolean flag option", /* help */
        nullptr, /* allowed */
    },
};

static const char * const kDummyOptionKeys[] = {
    "dummy.int",
    "dummy.uint",
    "dummy.flag",
};

static const gcomp_method_schema_t kDummySchemaErrorPolicy = {
    kDummyOptionSchemas,
    sizeof(kDummyOptionSchemas) / sizeof(kDummyOptionSchemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    kDummyOptionKeys,
};

static const gcomp_method_schema_t kDummySchemaIgnorePolicy = {
    kDummyOptionSchemas,
    sizeof(kDummyOptionSchemas) / sizeof(kDummyOptionSchemas[0]),
    GCOMP_UNKNOWN_KEY_IGNORE,
    kDummyOptionKeys,
};

static const gcomp_method_schema_t * dummy_get_schema_error(void) {
  return &kDummySchemaErrorPolicy;
}

static const gcomp_method_schema_t * dummy_get_schema_ignore(void) {
  return &kDummySchemaIgnorePolicy;
}

static gcomp_method_t create_dummy_method(
    const gcomp_method_schema_t * (*get_schema_fn)(void), const char * name) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities = GCOMP_CAP_NONE;
  method.create_encoder = nullptr;
  method.create_decoder = nullptr;
  method.destroy_encoder = nullptr;
  method.destroy_decoder = nullptr;
  method.get_schema = get_schema_fn;
  return method;
}

class SchemaTest : public ::testing::Test {
protected:
  void SetUp() override {
    method_error_ = create_dummy_method(dummy_get_schema_error, "dummy_error");
    method_ignore_ =
        create_dummy_method(dummy_get_schema_ignore, "dummy_ignore");
  }

  gcomp_method_t method_error_;
  gcomp_method_t method_ignore_;
};

TEST_F(SchemaTest, GetAllSchemas_Success) {
  const gcomp_method_schema_t * schema = nullptr;
  gcomp_status_t status = gcomp_method_get_all_schemas(&method_error_, &schema);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(schema, nullptr);
  EXPECT_EQ(schema, &kDummySchemaErrorPolicy);
  EXPECT_EQ(schema->num_options,
      sizeof(kDummyOptionSchemas) / sizeof(kDummyOptionSchemas[0]));
  EXPECT_EQ(schema->unknown_key_policy, GCOMP_UNKNOWN_KEY_ERROR);
}

TEST_F(SchemaTest, GetOptionSchema_ValidKey) {
  const gcomp_option_schema_t * opt_schema = nullptr;
  gcomp_status_t status =
      gcomp_method_get_option_schema(&method_error_, "dummy.int", &opt_schema);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(opt_schema, nullptr);
  EXPECT_STREQ(opt_schema->key, "dummy.int");
  EXPECT_EQ(opt_schema->type, GCOMP_OPT_INT64);
  EXPECT_NE(opt_schema->help, nullptr);
}

TEST_F(SchemaTest, GetOptionSchema_InvalidKey) {
  const gcomp_option_schema_t * opt_schema = nullptr;
  gcomp_status_t status = gcomp_method_get_option_schema(
      &method_error_, "nonexistent", &opt_schema);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(SchemaTest, GetOptionKeys_Success) {
  const char * const * keys = nullptr;
  size_t count = 0;
  gcomp_status_t status =
      gcomp_method_get_option_keys(&method_error_, &keys, &count);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(count, sizeof(kDummyOptionKeys) / sizeof(kDummyOptionKeys[0]));
  ASSERT_NE(keys, nullptr);

  for (size_t i = 0; i < count; ++i) {
    EXPECT_STREQ(keys[i], kDummyOptionKeys[i]);
  }
}

TEST_F(SchemaTest, OptionsValidate_Success) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  EXPECT_EQ(gcomp_options_set_int64(opts, "dummy.int", 7), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(opts, "dummy.uint", 50U), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(opts, "dummy.flag", 1), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_OK);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidate_UnknownKey_ErrorPolicy) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  EXPECT_EQ(gcomp_options_set_int64(opts, "unknown.option", 1), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidate_UnknownKey_IgnorePolicy) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  EXPECT_EQ(gcomp_options_set_int64(opts, "unknown.option", 1), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_ignore_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_OK);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidate_OutOfRange_Int64) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  /* dummy.int has range [0,10] */
  EXPECT_EQ(gcomp_options_set_int64(opts, "dummy.int", -1), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidate_OutOfRange_Uint64) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  /* dummy.uint has max_uint = 100 */
  EXPECT_EQ(gcomp_options_set_uint64(opts, "dummy.uint", 101U), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidate_TypeMismatch) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  /* dummy.int expects INT64 but we set it as UINT64. */
  EXPECT_EQ(gcomp_options_set_uint64(opts, "dummy.int", 5U), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate(opts, method_ptr), GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidateKey_Success) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  EXPECT_EQ(gcomp_options_set_int64(opts, "dummy.int", 3), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(
      gcomp_options_validate_key(opts, method_ptr, "dummy.int"), GCOMP_OK);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidateKey_MissingKey) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate_key(opts, method_ptr, "dummy.int"),
      GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

TEST_F(SchemaTest, OptionsValidateKey_TypeMismatch) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  EXPECT_EQ(gcomp_options_set_bool(opts, "dummy.int", 1), GCOMP_OK);

  const struct gcomp_method_s * method_ptr =
      reinterpret_cast<const struct gcomp_method_s *>(&method_error_);
  EXPECT_EQ(gcomp_options_validate_key(opts, method_ptr, "dummy.int"),
      GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
}

//
// Deflate method schema tests
//
// These tests use custom registries for test isolation - each test gets a
// fresh registry that doesn't share state with other tests or the default
// registry. This requires explicit method registration.
//

TEST(SchemaDeflateTest, GetAllSchemas_DeflateMethod) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  const gcomp_method_t * deflate = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(deflate, nullptr);

  const gcomp_method_schema_t * schema = nullptr;
  gcomp_status_t status = gcomp_method_get_all_schemas(deflate, &schema);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(schema, nullptr);
  EXPECT_GE(schema->num_options, 1u);
  EXPECT_EQ(schema->unknown_key_policy, GCOMP_UNKNOWN_KEY_ERROR);

  gcomp_registry_destroy(reg);
}

TEST(SchemaDeflateTest, GetOptionSchema_DeflateLevel) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  const gcomp_method_t * deflate = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(deflate, nullptr);

  const gcomp_option_schema_t * opt_schema = nullptr;
  gcomp_status_t status =
      gcomp_method_get_option_schema(deflate, "deflate.level", &opt_schema);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(opt_schema, nullptr);
  EXPECT_STREQ(opt_schema->key, "deflate.level");
  EXPECT_EQ(opt_schema->type, GCOMP_OPT_INT64);
  EXPECT_NE(opt_schema->help, nullptr);

  gcomp_registry_destroy(reg);
}

TEST(SchemaDeflateTest, OptionsValidate_DeflateOptions) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  const gcomp_method_t * deflate = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(deflate, nullptr);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_uint64(opts, "deflate.window_bits", 15), GCOMP_OK);
  EXPECT_EQ(gcomp_options_validate(opts, deflate), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_registry_destroy(reg);
}

TEST(SchemaDeflateTest, OptionsValidate_DeflateLevelOutOfRange) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  const gcomp_method_t * deflate = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(deflate, nullptr);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(opts, "deflate.level", 99), GCOMP_OK);
  EXPECT_EQ(gcomp_options_validate(opts, deflate), GCOMP_ERR_INVALID_ARG);

  gcomp_options_destroy(opts);
  gcomp_registry_destroy(reg);
}

// Every method's own key array, against its own schema.
//
// gcomp_method_schema_s::keys is documented as "an array of num_options
// pointers, each matching the corresponding gcomp_option_schema_s::key", and
// nothing checked that for a real method: GetOptionKeys_Success above tests
// the dummy method defined in this file, whose two arrays this file wrote
// together and got right.  The methods keep the two lists by hand, metres
// apart in one file, and brotli's had drifted -- the keys array was missing
// brotli.level, so it held five pointers while num_options said six.  A
// caller enumerating brotli's options read one past the end of a static
// array, and every key from the second on named the wrong option.
//
// The sweep is over the default registry, which is what a caller gets from
// gcomp_registry_default(); each method is named so a failure says which.
TEST(SchemaAllMethodsTest, EveryKeyArrayMatchesItsSchema) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  size_t checked = 0;
  for (const char * name : {"deflate", "zlib", "gzip", "lz4", "zstd", "lzw",
           "rle", "brotli", "lzma", "lzma2", "bzip2", "delta", "bcj"}) {
    const gcomp_method_t * method = gcomp_registry_find(reg, name);
    ASSERT_NE(method, nullptr) << name;

    const gcomp_method_schema_t * schema = nullptr;
    ASSERT_EQ(gcomp_method_get_all_schemas(method, &schema), GCOMP_OK) << name;
    ASSERT_NE(schema, nullptr) << name;

    const char * const * keys = nullptr;
    size_t count = 0;
    ASSERT_EQ(gcomp_method_get_option_keys(method, &keys, &count), GCOMP_OK)
        << name;
    ASSERT_EQ(count, schema->num_options) << name;
    ASSERT_NE(keys, nullptr) << name;

    for (size_t i = 0; i < count; i++) {
      // Reading keys[i] for every i up to count is the point as much as the
      // comparison is: a short array is an out-of-bounds read here, which
      // ASan reports as a global-buffer-overflow.
      ASSERT_NE(keys[i], nullptr) << name << " key " << i;
      ASSERT_NE(schema->options[i].key, nullptr) << name << " schema " << i;
      EXPECT_STREQ(keys[i], schema->options[i].key)
          << name << ": keys[" << i << "] does not name options[" << i << "]";
    }
    checked += count;
  }
  // A registry that answered with nothing would pass every assertion above.
  EXPECT_GT(checked, 50u) << "the sweep saw too few options to mean anything";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
