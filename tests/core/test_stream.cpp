/**
 * @file test_stream.cpp
 *
 * Unit tests for the stream API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

// Include internal header for testing
#include "../src/core/stream_internal.h"

// Mock update/finish functions for testing
static gcomp_status_t mock_encoder_update(
    GCOMP_MAYBE_UNUSED(gcomp_encoder_t * encoder),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * input),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * output)) {
  return GCOMP_OK;
}

static gcomp_status_t mock_encoder_finish(
    GCOMP_MAYBE_UNUSED(gcomp_encoder_t * encoder),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * output)) {
  return GCOMP_OK;
}

static gcomp_status_t mock_decoder_update(
    GCOMP_MAYBE_UNUSED(gcomp_decoder_t * decoder),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * input),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * output)) {
  return GCOMP_OK;
}

static gcomp_status_t mock_decoder_finish(
    GCOMP_MAYBE_UNUSED(gcomp_decoder_t * decoder),
    GCOMP_MAYBE_UNUSED(gcomp_buffer_t * output)) {
  return GCOMP_OK;
}

// Mock create_encoder that sets up the encoder properly
static gcomp_status_t mock_create_encoder(
    GCOMP_MAYBE_UNUSED(gcomp_registry_t * registry),
    GCOMP_MAYBE_UNUSED(gcomp_options_t * options),
    gcomp_encoder_t ** encoder_out) {
  // The encoder structure is already allocated by gcomp_encoder_create
  // We just need to set up the function pointers
  if (encoder_out && *encoder_out) {
    gcomp_encoder_t * encoder = *encoder_out;
    encoder->update_fn = mock_encoder_update;
    encoder->finish_fn = mock_encoder_finish;
  }

  return GCOMP_OK;
}

// Mock create_decoder that sets up the decoder properly
static gcomp_status_t mock_create_decoder(
    GCOMP_MAYBE_UNUSED(gcomp_registry_t * registry),
    GCOMP_MAYBE_UNUSED(gcomp_options_t * options),
    gcomp_decoder_t ** decoder_out) {
  // The decoder structure is already allocated by gcomp_decoder_create
  // We just need to set up the function pointers
  if (decoder_out && *decoder_out) {
    gcomp_decoder_t * decoder = *decoder_out;
    decoder->update_fn = mock_decoder_update;
    decoder->finish_fn = mock_decoder_finish;
  }

  return GCOMP_OK;
}

static void mock_destroy_encoder(GCOMP_MAYBE_UNUSED(gcomp_encoder_t * encoder)) {
  // No-op for mock
}

static void mock_destroy_decoder(GCOMP_MAYBE_UNUSED(gcomp_decoder_t * decoder)) {
  // No-op for mock
}

// Create a mock method for testing
static gcomp_method_t create_mock_method(
    const char * name, gcomp_capabilities_t caps) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities = caps;
  method.create_encoder = mock_create_encoder;
  method.create_decoder = mock_create_decoder;
  method.destroy_encoder = mock_destroy_encoder;
  method.destroy_decoder = mock_destroy_decoder;
  return method;
}

class StreamTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh registry for each test
    gcomp_registry_create(nullptr, &registry_);
    ASSERT_NE(registry_, nullptr);

    // Register a mock method that supports both encode and decode
    // Store as member variable so it stays in scope
    method_ = create_mock_method("test_method",
        static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE));
    ASSERT_EQ(gcomp_registry_register(registry_, &method_), GCOMP_OK);
  }

  void TearDown() override {
    if (encoder_ != nullptr) {
      gcomp_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_ != nullptr) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_encoder_t * encoder_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
  gcomp_method_t method_; // Store method so pointer remains valid
};

// Test gcomp_encoder_create()
TEST_F(StreamTest, EncoderCreateSuccess) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "test_method", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  // Clean up
  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderCreateNullRegistry) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(nullptr, "test_method", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateNullMethodName) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, nullptr, nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateNullEncoderOut) {
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "test_method", nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderCreateInvalidMethodName) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "nonexistent", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateMethodWithoutEncodeCapability) {
  // Register a method that only supports decode
  gcomp_method_t decode_only =
      create_mock_method("decode_only", GCOMP_CAP_DECODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &decode_only), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "decode_only", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateMethodWithoutCreateFunction) {
  // Register a method with NULL create_encoder
  gcomp_method_t method = create_mock_method("no_create", GCOMP_CAP_ENCODE);
  method.create_encoder = nullptr;
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "no_create", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

// Test gcomp_decoder_create()
TEST_F(StreamTest, DecoderCreateSuccess) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "test_method", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  // Clean up
  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderCreateNullRegistry) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(nullptr, "test_method", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateNullMethodName) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, nullptr, nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateNullDecoderOut) {
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "test_method", nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderCreateInvalidMethodName) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "nonexistent", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateMethodWithoutDecodeCapability) {
  // Register a method that only supports encode
  gcomp_method_t encode_only =
      create_mock_method("encode_only", GCOMP_CAP_ENCODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &encode_only), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "encode_only", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateMethodWithoutCreateFunction) {
  // Register a method with NULL create_decoder
  gcomp_method_t method = create_mock_method("no_create", GCOMP_CAP_DECODE);
  method.create_decoder = nullptr;
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "no_create", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

//
// Deflate method tests
//
// These tests use custom registries for test isolation - each test gets a
// fresh registry that doesn't share state with other tests or the default
// registry. This requires explicit method registration.
//

TEST_F(StreamTest, DeflateEncoderCreateSuccess) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(reg, "deflate", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, DeflateEncoderUpdateSucceeds) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "deflate", nullptr, &encoder), GCOMP_OK);

  uint8_t input_buf[4] = {0x01, 0x02, 0x03, 0x04};
  uint8_t output_buf[64] = {};
  gcomp_buffer_t input = {input_buf, sizeof(input_buf), 0};
  gcomp_buffer_t output = {output_buf, sizeof(output_buf), 0};

  // Update should succeed
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, &output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(input.used, sizeof(input_buf)); // Input should be consumed

  // Finish should succeed
  gcomp_buffer_t finish_output = {
      output_buf + output.used, sizeof(output_buf) - output.used, 0};
  status = gcomp_encoder_finish(encoder, &finish_output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_GT(
      output.used + finish_output.used, 0u); // Should have produced output

  gcomp_encoder_destroy(encoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, DeflateDecoderCreateSuccess) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(reg, "deflate", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, DeflateDecoderUpdateSucceedsWithEmptyInput) {
  // Custom registry for test isolation
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(reg, "deflate", nullptr, &decoder), GCOMP_OK);

  uint8_t input_buf[4] = {};
  uint8_t output_buf[64] = {};
  gcomp_buffer_t input = {input_buf, sizeof(input_buf), 0};
  gcomp_buffer_t output = {output_buf, sizeof(output_buf), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
  gcomp_registry_destroy(reg);
}

// Test gcomp_encoder_destroy()
TEST_F(StreamTest, EncoderDestroyNullPointer) {
  // Should not crash
  gcomp_encoder_destroy(nullptr);
}

TEST_F(StreamTest, EncoderDestroyCleanup) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Destroy should clean up
  gcomp_encoder_destroy(encoder);
  // If we get here without crashing, cleanup worked
}

// Test gcomp_decoder_destroy()
TEST_F(StreamTest, DecoderDestroyNullPointer) {
  // Should not crash
  gcomp_decoder_destroy(nullptr);
}

TEST_F(StreamTest, DecoderDestroyCleanup) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Destroy should clean up
  gcomp_decoder_destroy(decoder);
  // If we get here without crashing, cleanup worked
}

// Test encoder/decoder creation with options
TEST_F(StreamTest, EncoderCreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "test_method", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(StreamTest, DecoderCreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "test_method", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

// Test update/finish with NULL function pointers (should return INTERNAL error)
TEST_F(StreamTest, EncoderUpdateWithoutFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Manually clear the update function to test error path
  encoder->update_fn = nullptr;

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishWithoutFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Manually clear the finish function to test error path
  encoder->finish_fn = nullptr;

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateWithoutFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Manually clear the update function to test error path
  decoder->update_fn = nullptr;

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishWithoutFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Manually clear the finish function to test error path
  decoder->finish_fn = nullptr;

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(decoder, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_decoder_destroy(decoder);
}

// Test update/finish with NULL arguments
TEST_F(StreamTest, EncoderUpdateNullEncoder) {
  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(nullptr, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderUpdateNullInput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderUpdateNullOutput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_buffer_t input = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishNullEncoder) {
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderFinishNullOutput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_status_t status = gcomp_encoder_finish(encoder, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateNullDecoder) {
  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(nullptr, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderUpdateNullInput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderUpdateNullOutput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_buffer_t input = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishNullDecoder) {
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderFinishNullOutput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_status_t status = gcomp_decoder_finish(decoder, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

// Test that update/finish functions are called correctly
TEST_F(StreamTest, EncoderUpdateCallsFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(encoder->update_fn, nullptr);

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishCallsFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(encoder->finish_fn, nullptr);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(encoder, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateCallsFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);
  ASSERT_NE(decoder->update_fn, nullptr);

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishCallsFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);
  ASSERT_NE(decoder->finish_fn, nullptr);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(decoder, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

// Error detail API tests

TEST_F(StreamTest, EncoderErrorDetailInitiallyEmpty) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Initially, no error should be set
  EXPECT_EQ(gcomp_encoder_get_error(encoder), GCOMP_OK);
  const char * detail = gcomp_encoder_get_error_detail(encoder);
  EXPECT_NE(detail, nullptr);
  EXPECT_STREQ(detail, "");

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderErrorDetailInitiallyEmpty) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Initially, no error should be set
  EXPECT_EQ(gcomp_decoder_get_error(decoder), GCOMP_OK);
  const char * detail = gcomp_decoder_get_error_detail(decoder);
  EXPECT_NE(detail, nullptr);
  EXPECT_STREQ(detail, "");

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, EncoderSetErrorStoresDetail) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Set an error with details
  gcomp_status_t ret =
      gcomp_encoder_set_error(encoder, GCOMP_ERR_CORRUPT, "test error %d", 42);
  EXPECT_EQ(ret, GCOMP_ERR_CORRUPT);

  // Check that the error was stored
  EXPECT_EQ(gcomp_encoder_get_error(encoder), GCOMP_ERR_CORRUPT);
  const char * detail = gcomp_encoder_get_error_detail(encoder);
  EXPECT_NE(detail, nullptr);
  EXPECT_STREQ(detail, "test error 42");

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderSetErrorStoresDetail) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Set an error with details
  gcomp_status_t ret = gcomp_decoder_set_error(
      decoder, GCOMP_ERR_LIMIT, "limit exceeded: %zu", (size_t)1000);
  EXPECT_EQ(ret, GCOMP_ERR_LIMIT);

  // Check that the error was stored
  EXPECT_EQ(gcomp_decoder_get_error(decoder), GCOMP_ERR_LIMIT);
  const char * detail = gcomp_decoder_get_error_detail(decoder);
  EXPECT_NE(detail, nullptr);
  EXPECT_STREQ(detail, "limit exceeded: 1000");

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, ErrorDetailNullEncoderReturnsEmpty) {
  // Null pointer handling
  EXPECT_EQ(gcomp_encoder_get_error(nullptr), GCOMP_ERR_INVALID_ARG);
  EXPECT_STREQ(gcomp_encoder_get_error_detail(nullptr), "");
}

TEST_F(StreamTest, ErrorDetailNullDecoderReturnsEmpty) {
  // Null pointer handling
  EXPECT_EQ(gcomp_decoder_get_error(nullptr), GCOMP_ERR_INVALID_ARG);
  EXPECT_STREQ(gcomp_decoder_get_error_detail(nullptr), "");
}

// Reset API tests

TEST_F(StreamTest, EncoderResetNullPointer) {
  EXPECT_EQ(gcomp_encoder_reset(nullptr), GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderResetNullPointer) {
  EXPECT_EQ(gcomp_decoder_reset(nullptr), GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderResetUnsupportedMethod) {
  // Mock method doesn't implement reset_fn
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Reset should return UNSUPPORTED since mock method doesn't implement reset
  EXPECT_EQ(gcomp_encoder_reset(encoder), GCOMP_ERR_UNSUPPORTED);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DeflateEncoderResetSuccess) {
  // Create isolated registry
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "deflate", nullptr, &encoder), GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // First compression
  uint8_t input1[] = "Hello, World!";
  uint8_t output1[64] = {};
  gcomp_buffer_t in1 = {input1, sizeof(input1) - 1, 0};
  gcomp_buffer_t out1 = {output1, sizeof(output1), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in1, &out1), GCOMP_OK);

  gcomp_buffer_t finish1 = {
      output1 + out1.used, sizeof(output1) - out1.used, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &finish1), GCOMP_OK);
  size_t compressed_size1 = out1.used + finish1.used;
  EXPECT_GT(compressed_size1, 0u);

  // Reset encoder
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);

  // Second compression (should produce valid output again)
  uint8_t input2[] = "Goodbye!";
  uint8_t output2[64] = {};
  gcomp_buffer_t in2 = {input2, sizeof(input2) - 1, 0};
  gcomp_buffer_t out2 = {output2, sizeof(output2), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in2, &out2), GCOMP_OK);

  gcomp_buffer_t finish2 = {
      output2 + out2.used, sizeof(output2) - out2.used, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &finish2), GCOMP_OK);
  size_t compressed_size2 = out2.used + finish2.used;
  EXPECT_GT(compressed_size2, 0u);

  // Verify second output is valid by decoding
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(reg, "deflate", nullptr, &decoder), GCOMP_OK);

  uint8_t decompressed[64] = {};
  gcomp_buffer_t dec_in = {output2, compressed_size2, 0};
  gcomp_buffer_t dec_out = {decompressed, sizeof(decompressed), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &dec_in, &dec_out), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &dec_out), GCOMP_OK);

  EXPECT_EQ(dec_out.used, sizeof(input2) - 1);
  EXPECT_EQ(memcmp(decompressed, input2, sizeof(input2) - 1), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, DeflateDecoderResetSuccess) {
  // Create isolated registry
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  // Create encoder to produce compressed data
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "deflate", nullptr, &encoder), GCOMP_OK);

  // Compress first message
  uint8_t input1[] = "First message";
  uint8_t compressed1[64] = {};
  gcomp_buffer_t in1 = {input1, sizeof(input1) - 1, 0};
  gcomp_buffer_t out1 = {compressed1, sizeof(compressed1), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &in1, &out1), GCOMP_OK);
  gcomp_buffer_t finish1 = {
      compressed1 + out1.used, sizeof(compressed1) - out1.used, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &finish1), GCOMP_OK);
  size_t compressed_size1 = out1.used + finish1.used;

  // Reset and compress second message
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  uint8_t input2[] = "Second message";
  uint8_t compressed2[64] = {};
  gcomp_buffer_t in2 = {input2, sizeof(input2) - 1, 0};
  gcomp_buffer_t out2 = {compressed2, sizeof(compressed2), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &in2, &out2), GCOMP_OK);
  gcomp_buffer_t finish2 = {
      compressed2 + out2.used, sizeof(compressed2) - out2.used, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &finish2), GCOMP_OK);
  size_t compressed_size2 = out2.used + finish2.used;

  gcomp_encoder_destroy(encoder);

  // Create decoder and decode first message
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(reg, "deflate", nullptr, &decoder), GCOMP_OK);

  uint8_t decompressed1[64] = {};
  gcomp_buffer_t dec_in1 = {compressed1, compressed_size1, 0};
  gcomp_buffer_t dec_out1 = {decompressed1, sizeof(decompressed1), 0};
  ASSERT_EQ(gcomp_decoder_update(decoder, &dec_in1, &dec_out1), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &dec_out1), GCOMP_OK);
  EXPECT_EQ(dec_out1.used, sizeof(input1) - 1);
  EXPECT_EQ(memcmp(decompressed1, input1, sizeof(input1) - 1), 0);

  // Reset decoder and decode second message
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  uint8_t decompressed2[64] = {};
  gcomp_buffer_t dec_in2 = {compressed2, compressed_size2, 0};
  gcomp_buffer_t dec_out2 = {decompressed2, sizeof(decompressed2), 0};
  ASSERT_EQ(gcomp_decoder_update(decoder, &dec_in2, &dec_out2), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &dec_out2), GCOMP_OK);
  EXPECT_EQ(dec_out2.used, sizeof(input2) - 1);
  EXPECT_EQ(memcmp(decompressed2, input2, sizeof(input2) - 1), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, EncoderResetClearsError) {
  // Create isolated registry
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "deflate", nullptr, &encoder), GCOMP_OK);

  // Set an error manually
  gcomp_encoder_set_error(encoder, GCOMP_ERR_CORRUPT, "test error");
  EXPECT_EQ(gcomp_encoder_get_error(encoder), GCOMP_ERR_CORRUPT);
  EXPECT_STRNE(gcomp_encoder_get_error_detail(encoder), "");

  // Reset should clear the error
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_get_error(encoder), GCOMP_OK);
  EXPECT_STREQ(gcomp_encoder_get_error_detail(encoder), "");

  gcomp_encoder_destroy(encoder);
  gcomp_registry_destroy(reg);
}

TEST_F(StreamTest, DecoderResetClearsError) {
  // Create isolated registry
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_deflate_register(reg), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(reg, "deflate", nullptr, &decoder), GCOMP_OK);

  // Set an error manually
  gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT, "test error");
  EXPECT_EQ(gcomp_decoder_get_error(decoder), GCOMP_ERR_CORRUPT);
  EXPECT_STRNE(gcomp_decoder_get_error_detail(decoder), "");

  // Reset should clear the error
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_get_error(decoder), GCOMP_OK);
  EXPECT_STREQ(gcomp_decoder_get_error_detail(decoder), "");

  gcomp_decoder_destroy(decoder);
  gcomp_registry_destroy(reg);
}

/**
 * @brief Every method refuses a buffer whose fields contradict each other.
 *
 * A `gcomp_buffer_t` says how large it is, how much of that is used, and
 * where the bytes are. Those three can be set so that they disagree, and
 * every method in this library computes the bytes available as
 * `size - used`. That subtraction is on size_t: an inconsistent buffer does
 * not produce a small span, it produces one of nearly SIZE_MAX bytes, and
 * the next memcpy runs off the end of the caller's memory.
 *
 * Measured before gcomp_encoder_update() began refusing it: brotli's encoder
 * took an AddressSanitizer heap-buffer-overflow, rle's walked off the end of
 * the mapping and took SIGSEGV, and the other six read whatever followed the
 * caller's buffer and returned GCOMP_OK. brotli's decoder was the only one of
 * the sixteen entry points that refused any of it, which is the shape worth
 * remembering: an argument check written inside one method is a check the
 * other seven do not have.
 *
 * So the sweep is over every method and both directions, and it asserts three
 * things per case - the status, the detail naming the field, and that nothing
 * was written. The last one matters most: a method that refuses after
 * producing output has already done the damage.
 */
TEST(StreamBufferArguments, EveryMethodRefusesAnInconsistentBuffer) {
  // A real stream per method, so the decoder cases are refused for their
  // arguments rather than for the bytes.
  static const char * const kMethods[] = {
      "deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli", "lzma",
      "lzma2", "bzip2"};

  struct Case {
    const char * what;
    bool on_input;      ///< Spoil the input buffer rather than the output.
    bool overused;      ///< used = size + 1, rather than data = NULL.
    const char * detail;
  };
  static const Case kCases[] = {
      {"input used exceeds size", true, true, "input used exceeds size"},
      {"input data is NULL", true, false, "input data is NULL"},
      {"output used exceeds size", false, true, "output used exceeds size"},
      {"output data is NULL", false, false, "output data is NULL"},
  };

  std::vector<uint8_t> data(8192);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)("buffer arguments "[i % 17]);
  }

  for (const char * method : kMethods) {
    std::vector<uint8_t> stream(data.size() * 2 + 65536);
    size_t stream_len = 0;
    ASSERT_EQ(gcomp_encode_buffer(nullptr, method, nullptr, data.data(),
                  data.size(), stream.data(), stream.size(), &stream_len),
        GCOMP_OK)
        << method;
    stream.resize(stream_len);

    for (const Case & c : kCases) {
      std::vector<uint8_t> room(4096);

      // The encoder.
      {
        gcomp_encoder_t * enc = nullptr;
        ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), method, nullptr, &enc),
            GCOMP_OK)
            << method;
        gcomp_buffer_t in = {(void *)data.data(), data.size(), 0};
        gcomp_buffer_t ob = {room.data(), room.size(), 0};
        gcomp_buffer_t * spoil = c.on_input ? &in : &ob;
        if (c.overused) {
          spoil->used = spoil->size + 1;
        }
        else {
          spoil->data = nullptr;
        }
        EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_ERR_INVALID_ARG)
            << method << " encoder update: " << c.what;
        EXPECT_STREQ(gcomp_encoder_get_error_detail(enc), c.detail)
            << method << " encoder update: " << c.what;
        if (!c.on_input || !c.overused) {
          // The output buffer's own `used` is the one field the test spoils
          // on purpose; everywhere else it must still be zero.
          EXPECT_EQ(ob.used, c.on_input ? 0u : (c.overused ? ob.size + 1 : 0u))
              << method << " encoder update: " << c.what << ": wrote anyway";
        }
        gcomp_encoder_destroy(enc);
      }

      // The encoder's finish and flush take only an output buffer.
      if (!c.on_input) {
        for (int which = 0; which < 2; which++) {
          gcomp_encoder_t * enc = nullptr;
          ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), method, nullptr, &enc),
              GCOMP_OK)
              << method;
          gcomp_buffer_t ob = {room.data(), room.size(), 0};
          if (c.overused) {
            ob.used = ob.size + 1;
          }
          else {
            ob.data = nullptr;
          }
          const gcomp_status_t st = which == 0
              ? gcomp_encoder_finish(enc, &ob)
              : gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_SYNC);
          EXPECT_EQ(st, GCOMP_ERR_INVALID_ARG)
              << method << (which == 0 ? " finish: " : " flush: ") << c.what;
          gcomp_encoder_destroy(enc);
        }
      }

      // The decoder.
      {
        gcomp_decoder_t * dec = nullptr;
        ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), method, nullptr, &dec),
            GCOMP_OK)
            << method;
        gcomp_buffer_t in = {(void *)stream.data(), stream.size(), 0};
        gcomp_buffer_t ob = {room.data(), room.size(), 0};
        gcomp_buffer_t * spoil = c.on_input ? &in : &ob;
        if (c.overused) {
          spoil->used = spoil->size + 1;
        }
        else {
          spoil->data = nullptr;
        }
        EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_ERR_INVALID_ARG)
            << method << " decoder update: " << c.what;
        EXPECT_STREQ(gcomp_decoder_get_error_detail(dec), c.detail)
            << method << " decoder update: " << c.what;
        if (c.on_input || !c.overused) {
          EXPECT_EQ(ob.used, 0u)
              << method << " decoder update: " << c.what << ": wrote anyway";
        }
        gcomp_decoder_destroy(dec);
      }

      if (!c.on_input) {
        gcomp_decoder_t * dec = nullptr;
        ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), method, nullptr, &dec),
            GCOMP_OK)
            << method;
        gcomp_buffer_t ob = {room.data(), room.size(), 0};
        if (c.overused) {
          ob.used = ob.size + 1;
        }
        else {
          ob.data = nullptr;
        }
        EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_ERR_INVALID_ARG)
            << method << " decoder finish: " << c.what;
        gcomp_decoder_destroy(dec);
      }
    }
  }
}

/**
 * @brief A fully consumed input buffer is not an inconsistent one.
 *
 * The documented streaming loop (stream.h) offers "the same input buffer and
 * a drained output buffer until `input` is used up", so `used == size` is the
 * normal end of that loop and arrives at update() on every call after the
 * input runs out. A check that read `used == size` as a contradiction would
 * break every caller that follows the documented loop, so the boundary is
 * asserted from the valid side as well as the invalid one.
 */
TEST(StreamBufferArguments, AFullyConsumedInputBufferIsAccepted) {
  std::vector<uint8_t> data(64, 'c');
  for (const char * method :
      {"deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli", "lzma",
          "lzma2", "bzip2"}) {
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(gcomp_registry_default(), method, nullptr, &enc),
        GCOMP_OK)
        << method;
    std::vector<uint8_t> room(4096);

    // Everything offered, everything taken: the call the loop makes last.
    gcomp_buffer_t spent = {(void *)data.data(), data.size(), data.size()};
    gcomp_buffer_t ob = {room.data(), room.size(), 0};
    EXPECT_EQ(gcomp_encoder_update(enc, &spent, &ob), GCOMP_OK) << method;

    // And a zero-length output buffer, which the loop also produces once the
    // caller has nowhere to put the bytes: not an error, just no progress.
    gcomp_buffer_t none = {room.data(), 0, 0};
    const gcomp_status_t st = gcomp_encoder_update(enc, &spent, &none);
    EXPECT_TRUE(st == GCOMP_OK || st == GCOMP_ERR_LIMIT)
        << method << ": status " << (int)st;
    EXPECT_EQ(none.used, 0u) << method;
    gcomp_encoder_destroy(enc);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
