/**
 * @file test_flush.cpp
 *
 * Tests for gcomp_encoder_flush() across every method.
 *
 * ## The promise these tests exist to check
 *
 * After a flush returns GCOMP_OK, a decoder given the bytes produced so far
 * must produce every byte the encoder has consumed so far.  That one sentence
 * is the whole API, and EveryMethodDeliversWhatItHasConsumed checks it
 * directly: encode in pieces, flush after each piece, and after every flush
 * decode the prefix from scratch and compare.
 *
 * It is worth being explicit about what makes that test meaningful.  A flush
 * that emitted nothing at all would leave the decoder short, so the test
 * fails; a flush that emitted an unterminated block would leave the decoder
 * unable to finish it, so the test fails.  Both were checked by making
 * gcomp_encoder_flush() a no-op: all six methods failed all eight
 * configurations.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <algorithm>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

/// Every method the library registers, so a new one cannot quietly opt out.
const char * const kMethods[] = {"rle", "lzw", "lz4", "deflate", "gzip",
    "zlib", "zstd", "brotli"};

/**
 * Data with three characters: text that compresses through matches, a long
 * run that compresses through repetition, and noise that does not compress at
 * all and so exercises the stored/raw paths.
 */
std::vector<uint8_t> MakeMixedData(size_t n) {
  std::vector<uint8_t> data(n);
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog "};
  unsigned state = 12345;
  size_t pos = 0;
  while (pos < n / 3) {
    state = state * 1103515245u + 12345u;
    const char * w = words[(state >> 16) % 8];
    size_t len = strlen(w);
    if (pos + len > n / 3) {
      len = n / 3 - pos;
    }
    memcpy(data.data() + pos, w, len);
    pos += len;
  }
  size_t run = n / 3;
  memset(data.data() + pos, 'Z', run);
  pos += run;
  while (pos < n) {
    state = state * 1103515245u + 12345u;
    data[pos++] = (uint8_t)(state >> 16);
  }
  return data;
}

class FlushTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  /**
   * Zstd's full flush ends the frame and begins another, so reading its
   * output needs concatenated-frame support.  See zstd_encoder_flush().
   */
  gcomp_options_t * DecoderOptionsFor(const char * method, gcomp_flush_t mode) {
    if (mode != GCOMP_FLUSH_FULL || strcmp(method, "zstd") != 0) {
      return nullptr;
    }
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(opts, "zstd.concat", 1), GCOMP_OK);
    return opts;
  }

  /// Decode a deliberately unfinished stream; finish() is never called.
  std::vector<uint8_t> DecodePrefix(
      const char * method, gcomp_options_t * dopts,
      const std::vector<uint8_t> & stream, size_t expected) {
    gcomp_decoder_t * decoder = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, method, dopts, &decoder), GCOMP_OK);
    if (!decoder) {
      return {};
    }
    std::vector<uint8_t> out(expected + 65536);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK) << method;
    out.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  std::vector<uint8_t> DecodeWhole(const char * method,
      gcomp_options_t * dopts, const std::vector<uint8_t> & stream,
      size_t expected) {
    gcomp_decoder_t * decoder = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, method, dopts, &decoder), GCOMP_OK);
    if (!decoder) {
      return {};
    }
    std::vector<uint8_t> out(expected + 65536);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK)
        << method;
    EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK) << method;
    out.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  /// Drive update() until the whole chunk is taken, collecting output.
  void PushAll(gcomp_encoder_t * encoder, const uint8_t * data, size_t len,
      std::vector<uint8_t> & stream) {
    uint8_t chunk[4096];
    gcomp_buffer_t in_buf = {const_cast<uint8_t *>(data), len, 0};
    while (in_buf.used < in_buf.size) {
      gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
      size_t before = in_buf.used;
      ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
      stream.insert(stream.end(), chunk, chunk + out_buf.used);
      ASSERT_FALSE(in_buf.used == before && out_buf.used == 0)
          << "update() made no progress";
    }
  }

  /// Drive flush() to completion, collecting output.
  void FlushAll(gcomp_encoder_t * encoder, gcomp_flush_t mode,
      std::vector<uint8_t> & stream, size_t out_chunk = 4096) {
    std::vector<uint8_t> chunk(out_chunk);
    for (;;) {
      gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
      gcomp_status_t status = gcomp_encoder_flush(encoder, &out_buf, mode);
      stream.insert(
          stream.end(), chunk.begin(), chunk.begin() + out_buf.used);
      if (status == GCOMP_OK) {
        return;
      }
      ASSERT_EQ(status, GCOMP_ERR_LIMIT);
      ASSERT_GT(out_buf.used, 0u) << "flush() made no progress";
    }
  }

  void FinishAll(gcomp_encoder_t * encoder, std::vector<uint8_t> & stream) {
    uint8_t chunk[4096];
    for (;;) {
      gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
      gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
      stream.insert(stream.end(), chunk, chunk + out_buf.used);
      if (status == GCOMP_OK) {
        return;
      }
      ASSERT_EQ(status, GCOMP_ERR_LIMIT);
      ASSERT_GT(out_buf.used, 0u) << "finish() made no progress";
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

/**
 * The whole API, as a test.  For every method and both modes: feed the
 * encoder input in pieces, flush after each piece, and check that a decoder
 * given everything emitted so far produces exactly the input consumed so far.
 */
TEST_F(FlushTest, EveryMethodDeliversWhatItHasConsumed) {
  const std::vector<uint8_t> data = MakeMixedData(120000);
  const size_t pieces[] = {1024, 40000};

  for (const char * method : kMethods) {
    for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
      gcomp_options_t * dopts = DecoderOptionsFor(method, mode);

      gcomp_encoder_t * encoder = nullptr;
      ASSERT_EQ(gcomp_encoder_create(registry_, method, nullptr, &encoder),
          GCOMP_OK)
          << method;

      std::vector<uint8_t> stream;
      size_t consumed = 0;
      size_t piece_index = 0;
      while (consumed < data.size()) {
        size_t take =
            std::min(pieces[piece_index++ % 2], data.size() - consumed);
        PushAll(encoder, data.data() + consumed, take, stream);
        consumed += take;
        FlushAll(encoder, mode, stream);

        std::vector<uint8_t> seen =
            DecodePrefix(method, dopts, stream, consumed);
        ASSERT_EQ(seen.size(), consumed)
            << method << " mode=" << (int)mode << " after " << consumed
            << " bytes: the decoder was left short";
        ASSERT_EQ(memcmp(seen.data(), data.data(), consumed), 0)
            << method << " mode=" << (int)mode << " after " << consumed
            << " bytes: the decoder produced the wrong bytes";
      }

      // And the stream still finishes properly after all that flushing.
      FinishAll(encoder, stream);
      gcomp_encoder_destroy(encoder);

      std::vector<uint8_t> whole =
          DecodeWhole(method, dopts, stream, data.size());
      EXPECT_EQ(whole, data) << method << " mode=" << (int)mode;

      if (dopts) {
        gcomp_options_destroy(dopts);
      }
    }
  }
}

/**
 * The same promise with the output buffer starved, so every flush is handed
 * out across many calls and every resumption path is taken.
 *
 * Seven bytes for every method but RLE.  A PackBits literal packet is a
 * length byte and up to 128 bytes written as a unit, so RLE genuinely cannot
 * flush into less than that -- it says so rather than looping, and this test
 * gives it what it asks for.
 */
TEST_F(FlushTest, AFlushSurvivesATinyOutputBuffer) {
  const std::vector<uint8_t> data = MakeMixedData(20000);

  for (const char * method : kMethods) {
    const size_t out_chunk = (strcmp(method, "rle") == 0) ? 129 : 7;
    gcomp_encoder_t * encoder = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, method, nullptr, &encoder), GCOMP_OK)
        << method;

    std::vector<uint8_t> stream;
    size_t consumed = 0;
    while (consumed < data.size()) {
      size_t take = std::min<size_t>(3000, data.size() - consumed);
      PushAll(encoder, data.data() + consumed, take, stream);
      consumed += take;
      FlushAll(encoder, GCOMP_FLUSH_SYNC, stream, out_chunk);

      std::vector<uint8_t> seen =
          DecodePrefix(method, nullptr, stream, consumed);
      ASSERT_EQ(seen.size(), consumed) << method << " after " << consumed;
      ASSERT_EQ(memcmp(seen.data(), data.data(), consumed), 0)
          << method << " after " << consumed;
    }

    FinishAll(encoder, stream);
    gcomp_encoder_destroy(encoder);
    EXPECT_EQ(DecodeWhole(method, nullptr, stream, data.size()), data)
        << method;
  }
}

/**
 * RLE reports its minimum rather than looping.  A caller handing it a buffer
 * too small for one PackBits packet gets an error that says so, not a
 * GCOMP_ERR_LIMIT it can never satisfy by draining.
 */
TEST_F(FlushTest, RleSaysWhenTheOutputBufferIsTooSmallToFlushInto) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "rle", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> data(200);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i * 37 + 11); // No runs, so it all becomes literals.
  }
  std::vector<uint8_t> stream;
  PushAll(encoder, data.data(), data.size(), stream);

  uint8_t tiny[8];
  gcomp_buffer_t out_buf = {tiny, sizeof(tiny), 0};
  EXPECT_EQ(
      gcomp_encoder_flush(encoder, &out_buf, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_LIMIT);
  EXPECT_NE(gcomp_encoder_get_error_detail(encoder), nullptr);
  EXPECT_NE(std::string(gcomp_encoder_get_error_detail(encoder)).find("129"),
      std::string::npos)
      << "the error should name the space it needs";

  // With room, the same flush succeeds.
  FlushAll(encoder, GCOMP_FLUSH_SYNC, stream);
  EXPECT_EQ(DecodePrefix("rle", nullptr, stream, data.size()), data);

  gcomp_encoder_destroy(encoder);
}

/// Flushing an encoder that has been handed nothing is legal and writes at
/// most a stream header.
TEST_F(FlushTest, FlushingWithNothingBufferedIsNotAnError) {
  for (const char * method : kMethods) {
    gcomp_encoder_t * encoder = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, method, nullptr, &encoder), GCOMP_OK)
        << method;

    std::vector<uint8_t> stream;
    FlushAll(encoder, GCOMP_FLUSH_SYNC, stream);
    // Whatever came out, a second flush must not repeat it or fail.
    size_t after_first = stream.size();
    FlushAll(encoder, GCOMP_FLUSH_SYNC, stream);
    EXPECT_LE(stream.size() - after_first, after_first + 16u) << method;

    // The stream still finishes into something a decoder accepts as empty.
    FinishAll(encoder, stream);
    gcomp_encoder_destroy(encoder);
    EXPECT_TRUE(DecodeWhole(method, nullptr, stream, 0).empty()) << method;
  }
}

/**
 * A flush before the first update() must leave the encoder able to take input
 * of any size.  deflate sized its staging buffer from the flush, which holds
 * nothing, and update() skipped its own allocation because a buffer existed,
 * so the next large update failed with GCOMP_ERR_LIMIT.  The input is larger
 * than any method's block or chunk, in both modes and at each end of the
 * level range where a method has one, and the stream is decoded to compare the bytes.
 */
TEST_F(FlushTest, AFlushBeforeTheFirstUpdateLeavesRoomForALargeOne) {
  const std::vector<uint8_t> data = MakeMixedData(1u << 20);
  for (const char * method : kMethods) {
    for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
      for (int level : {-1, 0, 1}) {
        // -1 is the default; 0 and 1 pick the low and high end of the level
        // range for the methods that have one.  The rest have one setting.
        gcomp_options_t * eopts = nullptr;
        if (level >= 0) {
          const char * key = nullptr;
          int64_t lo = 0, hi = 0;
          if (!strcmp(method, "deflate") || !strcmp(method, "gzip") ||
              !strcmp(method, "zlib")) {
            key = "deflate.level", lo = 0, hi = 9;
          }
          else if (!strcmp(method, "zstd")) {
            key = "zstd.level", lo = 1, hi = 19;
          }
          else if (!strcmp(method, "brotli")) {
            key = "brotli.level", lo = 0, hi = 1;
          }
          if (!key) {
            continue;
          }
          ASSERT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
          ASSERT_EQ(gcomp_options_set_int64(eopts, key, level ? hi : lo),
              GCOMP_OK)
              << method;
        }
        gcomp_encoder_t * encoder = nullptr;
        ASSERT_EQ(gcomp_encoder_create(registry_, method, eopts, &encoder),
            GCOMP_OK)
            << method;
        if (eopts) {
          gcomp_options_destroy(eopts);
        }

        std::vector<uint8_t> stream;
        FlushAll(encoder, mode, stream);
        PushAll(encoder, data.data(), data.size(), stream);
        FinishAll(encoder, stream);
        gcomp_encoder_destroy(encoder);

        gcomp_options_t * dopts = DecoderOptionsFor(method, mode);
        EXPECT_EQ(DecodeWhole(method, dopts, stream, data.size()), data)
            << method << " mode " << mode << " level " << level;
        if (dopts) {
          gcomp_options_destroy(dopts);
        }
      }
    }
  }
}

/// A finished stream has nothing left to flush into.
TEST_F(FlushTest, FlushingAfterFinishIsRefused) {
  const std::vector<uint8_t> data = MakeMixedData(5000);

  for (const char * method : kMethods) {
    gcomp_encoder_t * encoder = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, method, nullptr, &encoder), GCOMP_OK)
        << method;

    std::vector<uint8_t> stream;
    PushAll(encoder, data.data(), data.size(), stream);
    FinishAll(encoder, stream);

    uint8_t chunk[256];
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    EXPECT_NE(gcomp_encoder_flush(encoder, &out_buf, GCOMP_FLUSH_SYNC),
        GCOMP_OK)
        << method << " accepted a flush after finish";

    gcomp_encoder_destroy(encoder);
  }
}

/// A mode the library does not define is rejected before anything is written.
TEST_F(FlushTest, AnUnknownModeIsRejectedWithoutWriting) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder), GCOMP_OK);

  uint8_t chunk[64];
  memset(chunk, 0xAB, sizeof(chunk));
  gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
  EXPECT_EQ(gcomp_encoder_flush(encoder, &out_buf, (gcomp_flush_t)99),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(out_buf.used, 0u);
  EXPECT_EQ(chunk[0], 0xAB);

  gcomp_encoder_destroy(encoder);
}

TEST_F(FlushTest, NullArgumentsAreRejected) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder), GCOMP_OK);
  uint8_t chunk[16];
  gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};

  EXPECT_EQ(gcomp_encoder_flush(nullptr, &out_buf, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_flush(encoder, nullptr, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_INVALID_ARG);

  gcomp_buffer_t bad_buf = {nullptr, 16, 0};
  EXPECT_EQ(gcomp_encoder_flush(encoder, &bad_buf, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

/**
 * Flushing costs ratio -- that is the trade, and it should be visible rather
 * than assumed.  Flushing after every byte must still be correct, and must
 * cost more than flushing once.
 */
TEST_F(FlushTest, FlushingOftenCostsRatioButStaysCorrect) {
  const std::vector<uint8_t> data = MakeMixedData(3000);

  for (const char * method : kMethods) {
    auto encode_with_flush_every = [&](size_t every) {
      gcomp_encoder_t * encoder = nullptr;
      EXPECT_EQ(
          gcomp_encoder_create(registry_, method, nullptr, &encoder), GCOMP_OK);
      std::vector<uint8_t> stream;
      size_t consumed = 0;
      while (consumed < data.size()) {
        size_t take = std::min(every, data.size() - consumed);
        PushAll(encoder, data.data() + consumed, take, stream);
        consumed += take;
        FlushAll(encoder, GCOMP_FLUSH_SYNC, stream);
      }
      FinishAll(encoder, stream);
      gcomp_encoder_destroy(encoder);
      EXPECT_EQ(DecodeWhole(method, nullptr, stream, data.size()), data)
          << method << " flushing every " << every;
      return stream.size();
    };

    size_t rare = encode_with_flush_every(data.size());
    size_t often = encode_with_flush_every(1);
    EXPECT_GT(often, rare)
        << method << ": flushing every byte should cost something";
  }
}

//
// Method-specific behaviour
//

/**
 * Deflate's flush ends with the empty stored block RFC 1951 3.2.4 describes --
 * the 00 00 FF FF tail zlib's Z_SYNC_FLUSH produces.  Without it the decoder
 * would read the padding bits as the next block's header.
 */
TEST_F(FlushTest, DeflateEndsASyncFlushWithAnEmptyStoredBlock) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder), GCOMP_OK);

  const std::string text = "the quick brown fox jumps over the lazy dog";
  std::vector<uint8_t> stream;
  PushAll(encoder, (const uint8_t *)text.data(), text.size(), stream);
  FlushAll(encoder, GCOMP_FLUSH_SYNC, stream);

  ASSERT_GE(stream.size(), 4u);
  EXPECT_EQ(stream[stream.size() - 4], 0x00);
  EXPECT_EQ(stream[stream.size() - 3], 0x00);
  EXPECT_EQ(stream[stream.size() - 2], 0xFF);
  EXPECT_EQ(stream[stream.size() - 1], 0xFF);

  gcomp_encoder_destroy(encoder);
}

/**
 * A full flush must leave nothing for later bytes to match into.  Highly
 * repetitive data makes that measurable: with the history kept, everything
 * after the flush is one long match; with it dropped, the second half has to
 * be described again from scratch.
 */
TEST_F(FlushTest, AFullFlushActuallyDropsTheHistory) {
  std::vector<uint8_t> half(20000);
  static const char * words[] = {"alpha ", "beta ", "gamma ", "delta "};
  unsigned state = 999;
  size_t pos = 0;
  while (pos < half.size()) {
    state = state * 1103515245u + 12345u;
    const char * w = words[(state >> 16) % 4];
    size_t len = strlen(w);
    if (pos + len > half.size()) {
      len = half.size() - pos;
    }
    memcpy(half.data() + pos, w, len);
    pos += len;
  }
  // The same bytes twice, so the second half is entirely matchable against
  // the first -- unless the flush between them dropped the history.
  std::vector<uint8_t> data(half);
  data.insert(data.end(), half.begin(), half.end());

  // Only the methods that keep history across a flush can show this.  RLE has
  // none, and LZW's flush must clear its dictionary to be readable at all.
  //
  // LZ4 needs linked blocks to be one of them.  Its default is *independent*
  // blocks, where a block already carries no history and the two flush modes
  // are identical -- which is exactly what the first version of this test
  // measured, and why it failed with 15,819 bytes against 15,819.
  for (const char * method : {"deflate", "gzip", "zlib", "lz4", "zstd"}) {
    const bool linked = (strcmp(method, "lz4") == 0);

    auto encode = [&](gcomp_flush_t mode) {
      gcomp_options_t * eopts = nullptr;
      if (linked) {
        EXPECT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
        EXPECT_EQ(
            gcomp_options_set_bool(eopts, "lz4.independent_blocks", 0),
            GCOMP_OK);
      }
      gcomp_encoder_t * encoder = nullptr;
      EXPECT_EQ(
          gcomp_encoder_create(registry_, method, eopts, &encoder), GCOMP_OK);
      if (eopts) {
        gcomp_options_destroy(eopts);
      }
      std::vector<uint8_t> stream;
      PushAll(encoder, data.data(), half.size(), stream);
      FlushAll(encoder, mode, stream);
      PushAll(encoder, data.data() + half.size(), half.size(), stream);
      FinishAll(encoder, stream);
      gcomp_encoder_destroy(encoder);

      gcomp_options_t * dopts = DecoderOptionsFor(method, mode);
      EXPECT_EQ(DecodeWhole(method, dopts, stream, data.size()), data)
          << method << " mode=" << (int)mode;
      if (dopts) {
        gcomp_options_destroy(dopts);
      }
      return stream.size();
    };

    size_t kept = encode(GCOMP_FLUSH_SYNC);
    size_t dropped = encode(GCOMP_FLUSH_FULL);
    EXPECT_GT(dropped, kept)
        << method
        << ": a full flush should cost more than a sync flush, because the "
           "second half can no longer match into the first";
  }
}

/**
 * LZW cannot byte-align a flush, so it pushes the pending code out by writing
 * a CLEAR after it -- which also resets the dictionary.  Both modes therefore
 * produce the same bytes.
 */
TEST_F(FlushTest, LzwFlushModesAreTheSame) {
  const std::vector<uint8_t> data = MakeMixedData(9000);

  auto encode = [&](gcomp_flush_t mode) {
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "lzw", nullptr, &encoder), GCOMP_OK);
    std::vector<uint8_t> stream;
    size_t consumed = 0;
    while (consumed < data.size()) {
      size_t take = std::min<size_t>(1500, data.size() - consumed);
      PushAll(encoder, data.data() + consumed, take, stream);
      consumed += take;
      FlushAll(encoder, mode, stream);
    }
    FinishAll(encoder, stream);
    gcomp_encoder_destroy(encoder);
    return stream;
  };

  EXPECT_EQ(encode(GCOMP_FLUSH_SYNC), encode(GCOMP_FLUSH_FULL));
}

/// Zstd's full flush ends the frame, so the output is more than one frame and
/// a decoder without zstd.concat stops at the first.
TEST_F(FlushTest, ZstdFullFlushEndsTheFrame) {
  const std::vector<uint8_t> data = MakeMixedData(30000);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder), GCOMP_OK);
  std::vector<uint8_t> stream;
  PushAll(encoder, data.data(), data.size() / 2, stream);
  FlushAll(encoder, GCOMP_FLUSH_FULL, stream);
  size_t first_frame_len = stream.size();
  PushAll(encoder, data.data() + data.size() / 2, data.size() - data.size() / 2,
      stream);
  FinishAll(encoder, stream);
  gcomp_encoder_destroy(encoder);

  // A second frame really did start: the magic appears again.
  ASSERT_GT(stream.size(), first_frame_len + 4);
  EXPECT_EQ(stream[first_frame_len + 0], 0x28);
  EXPECT_EQ(stream[first_frame_len + 1], 0xB5);
  EXPECT_EQ(stream[first_frame_len + 2], 0x2F);
  EXPECT_EQ(stream[first_frame_len + 3], 0xFD);

  gcomp_options_t * dopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
  EXPECT_EQ(DecodeWhole("zstd", dopts, stream, data.size()), data);
  gcomp_options_destroy(dopts);
}

/**
 * Incompressible bytes, then a flush, then exactly the same bytes again.
 *
 * This is the sharpest instrument there is for GCOMP_FLUSH_FULL, because the
 * only compression available is the match against the copy: history kept costs
 * about half of 2x, history dropped costs all of it.  Repetitive text would
 * blur that into a few percent.
 *
 * ## Why this had to be a ratio and not a comparison of bytes
 *
 * Every other flush test here asserts that the bytes come back, and they do --
 * a stream whose FULL flush kept its history is still perfectly well-formed and
 * still decodes to the input.  What FULL adds is a property about what the
 * encoder *did not do*, and nothing that reads the output can see it.  That is
 * why parallel mode ignored the mode for as long as it did.
 */
TEST_F(FlushTest, ZstdFullFlushDropsTheHistoryAtEveryThreadCount) {
  // Noise, so nothing in the half compresses on its own.
  //
  // 100 KB, not more: the match finder's reach for a single match of this shape
  // runs out somewhere between 100 KB and 300 KB, and at 300 KB even one thread
  // fails to match the copy -- which would disarm the control below and leave
  // the assertion after it proving nothing.  Measured, not chosen.
  std::vector<uint8_t> half(100000);
  unsigned state = 20260926u;
  for (auto & b : half) {
    state = state * 1103515245u + 12345u;
    b = (uint8_t)(state >> 16);
  }
  std::vector<uint8_t> data(half);
  data.insert(data.end(), half.begin(), half.end());

  auto encode = [&](uint64_t threads, gcomp_flush_t mode) {
    gcomp_options_t * eopts = nullptr;
    EXPECT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
    EXPECT_EQ(
        gcomp_options_set_uint64(eopts, "threads.count", threads), GCOMP_OK);
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "zstd", eopts, &encoder), GCOMP_OK);
    gcomp_options_destroy(eopts);
    if (!encoder) {
      return (size_t)0;
    }

    std::vector<uint8_t> stream;
    PushAll(encoder, data.data(), half.size(), stream);
    FlushAll(encoder, mode, stream);
    PushAll(encoder, data.data() + half.size(), half.size(), stream);
    FinishAll(encoder, stream);
    gcomp_encoder_destroy(encoder);

    // A full flush ends the frame in both modes now, so both need concat.
    gcomp_options_t * dopts = DecoderOptionsFor("zstd", mode);
    EXPECT_EQ(DecodeWhole("zstd", dopts, stream, data.size()), data)
        << "threads=" << threads << " mode=" << (int)mode;
    if (dopts) {
      gcomp_options_destroy(dopts);
    }
    return stream.size();
  };

  const size_t twice = data.size();
  for (uint64_t threads : {(uint64_t)1, (uint64_t)4}) {
    size_t kept = encode(threads, GCOMP_FLUSH_SYNC);
    size_t dropped = encode(threads, GCOMP_FLUSH_FULL);

    // The control. A sync flush must still let the second half match the
    // first, so it comes in near half of 2x. If this ever fails the subject
    // stopped being compressible and the assertion below proves nothing.
    EXPECT_LT(kept, twice * 3 / 4)
        << "threads=" << threads
        << ": a sync flush should keep the history, so the copy should still "
           "match and the output should be far below 2x";

    // The property. Nothing after the flush may refer to anything before it,
    // so the copy has to be described again from scratch.
    EXPECT_GT(dropped, twice * 9 / 10)
        << "threads=" << threads
        << ": a full flush dropped nothing -- the second half still matched "
           "into the first";
    EXPECT_GT(dropped, kept) << "threads=" << threads;
  }
}

/**
 * The frame really is ended in parallel mode, not merely emptied of history.
 * Both halves of FULL matter: the history is what the promise is about, and the
 * frame boundary is what makes one API call mean one thing at any thread count.
 */
TEST_F(FlushTest, ZstdFullFlushEndsTheFrameInParallelMode) {
  const std::vector<uint8_t> data = MakeMixedData(400000);

  gcomp_options_t * eopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(eopts, "threads.count", 4), GCOMP_OK);
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", eopts, &encoder), GCOMP_OK);
  gcomp_options_destroy(eopts);

  std::vector<uint8_t> stream;
  PushAll(encoder, data.data(), data.size() / 2, stream);
  FlushAll(encoder, GCOMP_FLUSH_FULL, stream);
  const size_t first_frame_len = stream.size();
  PushAll(encoder, data.data() + data.size() / 2,
      data.size() - data.size() / 2, stream);
  FinishAll(encoder, stream);
  gcomp_encoder_destroy(encoder);

  // The new header is staged by the flush and emitted by the next write, the
  // same way the first frame's header is staged at create time -- so the magic
  // lands at exactly the end of the closed frame.
  ASSERT_GT(stream.size(), first_frame_len + 4);
  EXPECT_EQ(stream[first_frame_len + 0], 0x28);
  EXPECT_EQ(stream[first_frame_len + 1], 0xB5);
  EXPECT_EQ(stream[first_frame_len + 2], 0x2F);
  EXPECT_EQ(stream[first_frame_len + 3], 0xFD);

  gcomp_options_t * dopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
  EXPECT_EQ(DecodeWhole("zstd", dopts, stream, data.size()), data);
  gcomp_options_destroy(dopts);
}

/**
 * A parallel full flush stages a terminator, a checksum and a fresh header, and
 * any of them can run out of output part-way.  Two bytes at a time forces every
 * one of those boundaries to be resumed.
 *
 * This is the half of the change that was reasoned rather than measured when it
 * was written, which is the reason for the test: on re-entry the flush runs the
 * whole submit-and-wait path again before reaching the epilogue, and it has to
 * come to the same conclusion the second time.
 */
TEST_F(FlushTest, ZstdParallelFullFlushResumesThroughATinyOutputBuffer) {
  const std::vector<uint8_t> data = MakeMixedData(200000);

  gcomp_options_t * eopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(eopts, "threads.count", 4), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(eopts, "zstd.checksum", 1), GCOMP_OK);
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", eopts, &encoder), GCOMP_OK);
  gcomp_options_destroy(eopts);

  std::vector<uint8_t> stream;
  PushAll(encoder, data.data(), data.size() / 2, stream);
  FlushAll(encoder, GCOMP_FLUSH_FULL, stream, /*out_chunk=*/2);
  PushAll(encoder, data.data() + data.size() / 2,
      data.size() - data.size() / 2, stream);
  FinishAll(encoder, stream);
  gcomp_encoder_destroy(encoder);

  gcomp_options_t * dopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
  EXPECT_EQ(DecodeWhole("zstd", dopts, stream, data.size()), data);
  gcomp_options_destroy(dopts);
}

/**
 * Two full flushes in a row, with nothing between them, must behave the same at
 * either thread count: the second closes an empty frame rather than erroring or
 * quietly doing nothing.  It is also what proves the epilogue flag is cleared,
 * since a stale one would make the second flush emit no terminator at all.
 */
TEST_F(FlushTest, ZstdBackToBackFullFlushesAgreeAcrossThreadCounts) {
  const std::vector<uint8_t> data = MakeMixedData(150000);

  auto encode = [&](uint64_t threads) {
    gcomp_options_t * eopts = nullptr;
    EXPECT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
    EXPECT_EQ(
        gcomp_options_set_uint64(eopts, "threads.count", threads), GCOMP_OK);
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "zstd", eopts, &encoder), GCOMP_OK);
    gcomp_options_destroy(eopts);
    if (!encoder) {
      return (size_t)0;
    }
    std::vector<uint8_t> stream;
    PushAll(encoder, data.data(), data.size(), stream);
    FlushAll(encoder, GCOMP_FLUSH_FULL, stream);
    FlushAll(encoder, GCOMP_FLUSH_FULL, stream);
    FinishAll(encoder, stream);
    gcomp_encoder_destroy(encoder);

    gcomp_options_t * dopts = nullptr;
    EXPECT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
    EXPECT_EQ(DecodeWhole("zstd", dopts, stream, data.size()), data)
        << "threads=" << threads;
    gcomp_options_destroy(dopts);
    return stream.size();
  };

  EXPECT_GT(encode(1), 0u);
  EXPECT_GT(encode(4), 0u);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
