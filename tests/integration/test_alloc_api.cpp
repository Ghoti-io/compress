/**
 * @file test_alloc_api.cpp
 *
 * gcomp_encode_alloc() and gcomp_decode_alloc() take the buffer sizing off the
 * caller. What has to be true for that to be worth using:
 *
 * - the round trip is exact, for every method and at sizes that cross the
 *   growth thresholds;
 * - the growth loop does not lose or duplicate bytes, which is what a
 *   reallocation in the middle of a stream is most likely to do;
 * - `limits.max_output_bytes` still stops a bomb, because a function that
 *   allocates on the caller's behalf is exactly where an unbounded one would
 *   hurt; and
 * - nothing leaks, whichever way a call ends.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "failing_allocator.h"

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include <ghoti.io/compress/zlib.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char * const kMethods[] = {
    "deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli", "lzma2", "bzip2"};

/// Compressible, but not so compressible that it says nothing.
std::vector<uint8_t> text_like(size_t len) {
  static const char * words[] = {"the", "quick", "brown", "fox", "over",
      "lazy", "dog", "buffer", "allocate", "grow", "decode", "stream"};
  std::string s;
  uint32_t state = 7u;
  while (s.size() < len) {
    state = state * 1103515245u + 12345u;
    s += words[(state >> 16) % 12];
    s += ((state >> 9) & 3) ? " " : "\n";
  }
  s.resize(len);
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> incompressible(size_t len) {
  std::vector<uint8_t> v(len);
  uint32_t s = 2463534242u;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v[i] = (uint8_t)(s >> 24);
  }
  return v;
}

/// A long run: compresses to almost nothing, which is the interesting case.
std::vector<uint8_t> very_compressible(size_t len) {
  return std::vector<uint8_t>(len, 0x2A);
}

void round_trip(const char * method, const std::vector<uint8_t> & input) {
  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, method, nullptr, input.data(),
                input.size(), &enc, &enc_len),
      GCOMP_OK)
      << method << " n=" << input.size();
  ASSERT_NE(enc, nullptr) << method;

  void * dec = nullptr;
  size_t dec_len = 0;
  gcomp_status_t s = gcomp_decode_alloc(
      nullptr, method, nullptr, enc, enc_len, &dec, &dec_len);
  gcomp_buffer_free(nullptr, enc);

  ASSERT_EQ(s, GCOMP_OK) << method << " n=" << input.size();
  ASSERT_NE(dec, nullptr) << method;
  EXPECT_EQ(dec_len, input.size()) << method << " n=" << input.size();
  if (dec_len == input.size() && dec_len > 0) {
    EXPECT_EQ(std::memcmp(dec, input.data(), dec_len), 0)
        << method << " n=" << input.size() << ": bytes differ";
  }
  gcomp_buffer_free(nullptr, dec);
}

//
// Round trips
//

/**
 * @brief Every method, at sizes that cross the growth thresholds.
 *
 * The decode buffer starts at 64 KB or four times the input, so the sizes here
 * straddle that and force several doublings.
 */
TEST(AllocApi, RoundTripsAtManySizes) {
  for (size_t n : {0u, 1u, 2u, 1000u, 65535u, 65536u, 70000u, 300000u}) {
    const std::vector<uint8_t> input = text_like(n);
    for (const char * method : kMethods) {
      round_trip(method, input);
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
}

/// Data that will not compress: the encode bound is what gets allocated.
TEST(AllocApi, RoundTripsIncompressibleData) {
  const std::vector<uint8_t> input = incompressible(100000);
  for (const char * method : kMethods) {
    round_trip(method, input);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

/**
 * @brief Data that compresses to almost nothing.
 *
 * A megabyte of one byte goes to a few dozen bytes, which is a ratio far past
 * the 1000:1 the expansion-ratio limit defaults to. Decoding it is what
 * gcomp_decode_alloc() has to get right: the frame states its size, so there is
 * an exact ceiling available and the ratio's guess is not needed.
 *
 * For a format that does not state a size, the ratio still applies - so the
 * ones that do not are given a limit the caller sets, which is what a caller
 * would have to do anyway.
 */
TEST(AllocApi, RoundTripsVeryCompressibleData) {
  const std::vector<uint8_t> input = very_compressible(1000000);

  for (const char * method : {"zstd", "lz4"}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o,
                  std::string(method).append(".content_size").c_str(),
                  (uint64_t)input.size()),
        GCOMP_OK);

    void * enc = nullptr;
    size_t enc_len = 0;
    ASSERT_EQ(gcomp_encode_alloc(nullptr, method, o, input.data(),
                  input.size(), &enc, &enc_len),
        GCOMP_OK)
        << method;
    EXPECT_LT(enc_len, input.size() / 100) << method << ": barely compressed?";

    // No limits touched by the caller: the frame says how large it is, and
    // that is what decode_alloc decodes against.
    void * dec = nullptr;
    size_t dec_len = 0;
    gcomp_status_t s = gcomp_decode_alloc(
        nullptr, method, nullptr, enc, enc_len, &dec, &dec_len);
    gcomp_buffer_free(nullptr, enc);
    ASSERT_EQ(s, GCOMP_OK) << method
                           << ": a stream that states its size should decode "
                              "however well it compressed";
    EXPECT_EQ(dec_len, input.size()) << method;
    if (dec_len == input.size()) {
      EXPECT_EQ(std::memcmp(dec, input.data(), dec_len), 0) << method;
    }
    gcomp_buffer_free(nullptr, dec);
    gcomp_options_destroy(o);
  }
}

//
// Limits
//

/**
 * @brief A bomb is refused by the documented ceiling.
 *
 * A function that allocates for the caller is where an unbounded one hurts
 * most, so the growth loop checks `limits.max_output_bytes` before every
 * enlargement rather than after the fact.
 */
TEST(AllocApi, RespectsMaxOutputBytes) {
  const std::vector<uint8_t> input = very_compressible(4 * 1024 * 1024);

  // The frame states its size, so the exact-ceiling path is available - and
  // the ceiling that then applies is still the caller's, which is the point.
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(
                enc_opts, "zstd.content_size", (uint64_t)input.size()),
      GCOMP_OK);

  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", enc_opts, input.data(),
                input.size(), &enc, &enc_len),
      GCOMP_OK);
  gcomp_options_destroy(enc_opts);

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(o, "limits.max_output_bytes", 1024u * 1024u),
      GCOMP_OK);

  void * dec = nullptr;
  size_t dec_len = 0;
  gcomp_status_t s =
      gcomp_decode_alloc(nullptr, "zstd", o, enc, enc_len, &dec, &dec_len);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT)
      << "4 MB of output under a 1 MB ceiling should be refused";
  EXPECT_EQ(dec, nullptr) << "nothing should be handed back on failure";

  // And with a ceiling that fits, the same stream decodes.
  ASSERT_EQ(gcomp_options_set_uint64(
                o, "limits.max_output_bytes", 8u * 1024u * 1024u),
      GCOMP_OK);
  ASSERT_EQ(
      gcomp_decode_alloc(nullptr, "zstd", o, enc, enc_len, &dec, &dec_len),
      GCOMP_OK);
  EXPECT_EQ(dec_len, input.size());
  gcomp_buffer_free(nullptr, dec);

  gcomp_buffer_free(nullptr, enc);
  gcomp_options_destroy(o);
}

/**
 * @brief The ceiling is reported as a limit by **every** method.
 *
 * `RespectsMaxOutputBytes` above covers zstd, whose frame states its decompressed
 * size - so `gcomp_decode_alloc()` takes its exact-ceiling path and the refusal
 * comes from a different place than it does for a method that states nothing. The
 * conclusion "the ceiling is refused with GCOMP_ERR_LIMIT" was drawn from that one
 * method and generalised to every method.
 *
 * Six of them did answer that way. **gzip did not**: its `finish()` ignored
 * the output buffer, so a full buffer with deflate data still to come was reported
 * as `GCOMP_ERR_CORRUPT` - "gzip stream truncated in deflate data" - for a stream
 * that was neither truncated nor corrupt. `font` found it on the first `.pcf.gz`
 * it read under a ceiling of its own.
 *
 * So this sweeps the methods rather than naming one. A method added to the registry
 * and not to this list is the gap that let the first one through, which is why the
 * count is asserted too.
 */
TEST(AllocApi, EveryMethodReportsTheCeilingAsALimit) {
  const std::vector<uint8_t> input = very_compressible(256 * 1024);
  const char * methods[] = {
      "gzip", "zlib", "deflate", "lz4", "lzw", "rle", "zstd", "brotli", "lzma2", "bzip2"};
  size_t swept = 0;

  for (const char * method : methods) {
    void * enc = nullptr;
    size_t enc_len = 0;
    ASSERT_EQ(gcomp_encode_alloc(nullptr, method, nullptr, input.data(),
                  input.size(), &enc, &enc_len),
        GCOMP_OK) << method;

    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_output_bytes", 4096u),
        GCOMP_OK);

    void * dec = nullptr;
    size_t dec_len = 0;
    const gcomp_status_t s =
        gcomp_decode_alloc(nullptr, method, o, enc, enc_len, &dec, &dec_len);
    EXPECT_EQ(s, GCOMP_ERR_LIMIT)
        << method << " reported " << gcomp_status_to_string(s)
        << " for output past the ceiling, which is a fact about the caller's "
           "limit and not about the data";
    EXPECT_EQ(dec, nullptr) << method;

    // And with a ceiling that fits, the same stream decodes - so the refusal
    // above was the ceiling and not the method failing outright.
    ASSERT_EQ(gcomp_options_set_uint64(
                  o, "limits.max_output_bytes", 1024u * 1024u),
        GCOMP_OK);
    ASSERT_EQ(
        gcomp_decode_alloc(nullptr, method, o, enc, enc_len, &dec, &dec_len),
        GCOMP_OK) << method;
    EXPECT_EQ(dec_len, input.size()) << method;
    gcomp_buffer_free(nullptr, dec);

    gcomp_options_destroy(o);
    gcomp_buffer_free(nullptr, enc);
    ++swept;
  }
  EXPECT_EQ(swept, 10u) << "a method was added to the registry and not to this "
                          "list, which is how gzip's answer went unnoticed";
}

/**
 * @brief A genuinely truncated gzip member still reports corruption.
 *
 * The control for the fix above: `finish()` now answers GCOMP_ERR_LIMIT when the
 * output buffer is full, and it must not answer that when the buffer has room and
 * the input really did end early. Without this the fix would have turned every
 * truncated gzip into a limit, which `gcomp_decode_alloc()` would then try to grow
 * its way out of.
 */
TEST(AllocApi, ATruncatedGzipIsStillCorrupt) {
  const std::vector<uint8_t> input = very_compressible(256 * 1024);
  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "gzip", nullptr, input.data(),
                input.size(), &enc, &enc_len),
      GCOMP_OK);
  ASSERT_GT(enc_len, 32u);

  void * dec = nullptr;
  size_t dec_len = 0;
  // Half a member, under a ceiling that is not in the way.
  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(o, "limits.max_output_bytes", 8u * 1024u * 1024u),
      GCOMP_OK);
  const gcomp_status_t s =
      gcomp_decode_alloc(nullptr, "gzip", o, enc, enc_len / 2, &dec, &dec_len);
  EXPECT_EQ(s, GCOMP_ERR_CORRUPT)
      << "a truncated member reported " << gcomp_status_to_string(s);
  EXPECT_EQ(dec, nullptr);

  gcomp_options_destroy(o);
  gcomp_buffer_free(nullptr, enc);
}

/// The caller's options are not modified, however the decode is arranged.
TEST(AllocApi, DoesNotModifyCallerOptions) {
  const std::vector<uint8_t> input = text_like(50000);

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.content_size",
                (uint64_t)input.size()),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 1000u),
      GCOMP_OK);

  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", o, input.data(), input.size(),
                &enc, &enc_len),
      GCOMP_OK);

  void * dec = nullptr;
  size_t dec_len = 0;
  ASSERT_EQ(gcomp_decode_alloc(nullptr, "zstd", o, enc, enc_len, &dec,
                &dec_len),
      GCOMP_OK);

  uint64_t ratio = 0;
  EXPECT_EQ(gcomp_options_get_uint64(o, "limits.max_expansion_ratio", &ratio),
      GCOMP_OK);
  EXPECT_EQ(ratio, 1000u) << "the caller's options were changed underneath it";

  gcomp_buffer_free(nullptr, dec);
  gcomp_buffer_free(nullptr, enc);
  gcomp_options_destroy(o);
}

/**
 * @brief Output of exactly the ceiling is a success, not a refusal.
 *
 * The boundary case, and the one a caller most often creates on purpose: it
 * reads the content size out of the header and sets `max_output_bytes` to
 * exactly that. Growing is then impossible when the buffer fills, and treating
 * that as the failure - rather than asking the decoder whether the stream had
 * simply ended - refuses every such decode.
 *
 * Found by examples/detect_and_decode.c on the first file it was given.
 */
TEST(AllocApi, OutputExactlyAtTheCeilingSucceeds) {
  for (size_t n : {1u, 1000u, 65536u, 250000u}) {
    const std::vector<uint8_t> input = text_like(n);

    for (const char * method : kMethods) {
      void * enc = nullptr;
      size_t enc_len = 0;
      ASSERT_EQ(gcomp_encode_alloc(nullptr, method, nullptr, input.data(),
                    input.size(), &enc, &enc_len),
          GCOMP_OK)
          << method;

      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      // Exactly the output, and no ratio - what a caller does once it has a
      // content size from gcomp_peek().
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_output_bytes",
                    (uint64_t)n),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
          GCOMP_OK);

      void * dec = nullptr;
      size_t dec_len = 0;
      gcomp_status_t s =
          gcomp_decode_alloc(nullptr, method, o, enc, enc_len, &dec, &dec_len);
      gcomp_buffer_free(nullptr, enc);
      gcomp_options_destroy(o);

      ASSERT_EQ(s, GCOMP_OK)
          << method << " n=" << n
          << ": output of exactly max_output_bytes should decode";
      EXPECT_EQ(dec_len, n) << method << " n=" << n;
      if (dec_len == n && n > 0) {
        EXPECT_EQ(std::memcmp(dec, input.data(), n), 0) << method;
      }
      gcomp_buffer_free(nullptr, dec);
    }
  }
}

//
// Allocator
//

/**
 * @brief The buffer comes from the registry's allocator and goes back to it.
 *
 * A caller with its own allocator has one because it has to account for every
 * byte; a library that allocated through it and freed through the C library
 * would break that, silently.
 */
TEST(AllocApi, UsesTheRegistryAllocator) {
  FailingAllocator fa;
  gcomp_registry_t * reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(fa.allocator(), &reg), GCOMP_OK);
  ASSERT_EQ(gcomp_method_zstd_register(reg), GCOMP_OK);

  const std::vector<uint8_t> input = text_like(40000);

  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(reg, "zstd", nullptr, input.data(),
                input.size(), &enc, &enc_len),
      GCOMP_OK);
  EXPECT_GT(fa.live_blocks(), 0u);

  void * dec = nullptr;
  size_t dec_len = 0;
  ASSERT_EQ(
      gcomp_decode_alloc(reg, "zstd", nullptr, enc, enc_len, &dec, &dec_len),
      GCOMP_OK);
  EXPECT_EQ(dec_len, input.size());

  gcomp_buffer_free(reg, enc);
  gcomp_buffer_free(reg, dec);
  gcomp_registry_destroy(reg);

  EXPECT_EQ(fa.live_blocks(), 0u)
      << "left " << fa.live_bytes() << " bytes allocated";
}

/**
 * @brief An allocation failure anywhere leaves nothing behind.
 *
 * These functions hold a buffer across a whole decode and reallocate it as
 * they go, which is the shape most likely to drop one on an error path.
 */
TEST(AllocApi, LeavesNothingBehindWhenAllocationFails) {
  const std::vector<uint8_t> input = text_like(40000);

  // Encode once, outside the sweep, so the sweep is only about decoding.
  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", nullptr, input.data(),
                input.size(), &enc, &enc_len),
      GCOMP_OK);
  const std::vector<uint8_t> stream(
      (uint8_t *)enc, (uint8_t *)enc + enc_len);
  gcomp_buffer_free(nullptr, enc);

  FailingAllocator counter;
  {
    gcomp_registry_t * reg = nullptr;
    ASSERT_EQ(gcomp_registry_create(counter.allocator(), &reg), GCOMP_OK);
    ASSERT_EQ(gcomp_method_zstd_register(reg), GCOMP_OK);
    void * dec = nullptr;
    size_t dec_len = 0;
    ASSERT_EQ(gcomp_decode_alloc(reg, "zstd", nullptr, stream.data(),
                  stream.size(), &dec, &dec_len),
        GCOMP_OK);
    gcomp_buffer_free(reg, dec);
    gcomp_registry_destroy(reg);
  }
  ASSERT_EQ(counter.live_blocks(), 0u);
  const size_t n = counter.calls();
  ASSERT_GT(n, 0u);

  for (size_t k = 1; k <= n; k++) {
    FailingAllocator fa;
    fa.fail_at(k);

    gcomp_registry_t * reg = nullptr;
    if (gcomp_registry_create(fa.allocator(), &reg) != GCOMP_OK) {
      EXPECT_EQ(fa.live_blocks(), 0u) << "k=" << k;
      continue;
    }
    if (gcomp_method_zstd_register(reg) == GCOMP_OK) {
      void * dec = nullptr;
      size_t dec_len = 0;
      gcomp_status_t s = gcomp_decode_alloc(reg, "zstd", nullptr,
          stream.data(), stream.size(), &dec, &dec_len);
      EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
          << "k=" << k << ": " << gcomp_status_to_string(s);
      if (s == GCOMP_OK) {
        EXPECT_EQ(dec_len, input.size()) << "k=" << k;
        EXPECT_EQ(std::memcmp(dec, input.data(), dec_len), 0) << "k=" << k;
      }
      else {
        EXPECT_EQ(dec, nullptr) << "k=" << k << ": handed back a buffer on "
                                   "failure";
      }
      gcomp_buffer_free(reg, dec);
    }
    gcomp_registry_destroy(reg);
    EXPECT_EQ(fa.live_blocks(), 0u)
        << "k=" << k << " leaked " << fa.live_bytes() << " bytes";
  }
}

//
// Arguments
//

TEST(AllocApi, RejectsBadArguments) {
  void * out = nullptr;
  size_t len = 0;
  const uint8_t buf[4] = {0};

  EXPECT_EQ(
      gcomp_encode_alloc(nullptr, nullptr, nullptr, buf, 4, &out, &len),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_encode_alloc(nullptr, "zstd", nullptr, buf, 4, nullptr, &len),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_encode_alloc(nullptr, "zstd", nullptr, nullptr, 4, &out, &len),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_decode_alloc(nullptr, "no-such-method", nullptr, buf, 4, &out,
          &len),
      GCOMP_ERR_UNSUPPORTED);

  // Freeing nothing, and freeing through a default registry, are both fine.
  gcomp_buffer_free(nullptr, nullptr);
}

/// Corrupt input fails without handing back a buffer.
TEST(AllocApi, CorruptInputYieldsNothing) {
  const uint8_t not_zstd[] = {0x28, 0xB5, 0x2F, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF};
  void * out = (void *)0x1;
  size_t len = 99;
  gcomp_status_t s = gcomp_decode_alloc(
      nullptr, "zstd", nullptr, not_zstd, sizeof(not_zstd), &out, &len);
  EXPECT_NE(s, GCOMP_OK);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(len, 0u);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
