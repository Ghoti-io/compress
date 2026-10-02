/**
 * @file test_brotli_robustness.cpp
 *
 * Robustness tests for the Brotli decoder, the shape gzip, lz4 and zstd
 * already have and brotli did not.
 *
 * The decoder sat at 78.4% of 1120 lines while the rest of the module was
 * above 82%, and the uncovered lines were not exotic: `brotli_decoder_reset`
 * had never been called by any test, none of `buffers_ok`'s six argument
 * checks had been reached, no stream had more than one block type, no
 * metadata block had ever carried content, and about half the remainder were
 * the `GCOMP_ERR_CORRUPT` arms - the decoder's whole security surface, driven
 * by the fuzzers and asserted about by nothing.
 *
 * Four instruments here, in the order they were worth writing:
 *
 * - API sequences and argument checks, as the sibling files do.
 * - Streams built bit by bit, for the parts of RFC 7932 no encoder in this
 *   library writes: metadata blocks with content, the skip-length forms, and
 *   the five- and six-nibble meta-block lengths. Each legal one is handed to
 *   libbrotli as well, because a stream this file invented is worth nothing
 *   as a test until something other than the code under test agrees it is
 *   a stream.
 * - Every stream decoded one byte in and one byte out, which is what reaches
 *   the suspend-and-resume arm of each field rather than only the fields a
 *   43-byte stream happened to split on.
 * - A corruption sweep that counts how many distinct refusals it provoked.
 *   A sweep asserting only "no crash" passes just as well when it stops
 *   reaching anything, so the floor is on the number of distinct messages.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using BrotliCompress = int (*)(int, int, int, size_t, const uint8_t *, size_t *,
    uint8_t *);
using BrotliDecompress = int (*)(size_t, const uint8_t *, size_t *, uint8_t *);
using BrotliMaxSize = size_t (*)(size_t);

struct BrotliLib {
  BrotliCompress compress = nullptr;
  BrotliDecompress decompress = nullptr;
  BrotliMaxSize max_size = nullptr;
  bool ok() const {
    return compress && decompress && max_size;
  }
};

const BrotliLib & brotli_lib() {
  static BrotliLib lib;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    void * enc = dlopen("libbrotlienc.so.1", RTLD_NOW);
    void * dec = dlopen("libbrotlidec.so.1", RTLD_NOW);
    if (enc) {
      lib.compress =
          reinterpret_cast<BrotliCompress>(dlsym(enc, "BrotliEncoderCompress"));
      lib.max_size = reinterpret_cast<BrotliMaxSize>(
          dlsym(enc, "BrotliEncoderMaxCompressedSize"));
    }
    if (dec) {
      lib.decompress = reinterpret_cast<BrotliDecompress>(
          dlsym(dec, "BrotliDecoderDecompress"));
    }
  }
  return lib;
}

/// LSB-first, the order RFC 7932 packs bits in. Only the test needs it; the
/// library's own writer is not reachable from here and should not be - a
/// stream built with the code under test cannot contradict it.
class BitWriter {
public:
  void put(uint32_t value, int bits) {
    for (int i = 0; i < bits; i++) {
      if (nbits_ == 0) {
        out_.push_back(0);
      }
      if ((value >> i) & 1u) {
        out_.back() = (uint8_t)(out_.back() | (1u << nbits_));
      }
      nbits_ = (nbits_ + 1) & 7;
    }
  }

  void align() {
    nbits_ = 0;
  }

  void bytes(const uint8_t * data, size_t n) {
    EXPECT_EQ(nbits_, 0) << "raw bytes must start on a byte boundary";
    out_.insert(out_.end(), data, data + n);
  }

  /// WBITS for a 16-bit window is the single bit 0.
  void window16() {
    put(0, 1);
  }

  /// A metadata meta-block carrying `n` bytes nothing reads, which is the
  /// only part of the format that makes the decoder skip input rather than
  /// turn it into output.
  void metadata(const uint8_t * data, size_t n, int skip_bytes) {
    put(0, 1); // ISLAST
    put(3, 2); // MNIBBLES == 3 means metadata
    put(0, 1); // reserved
    put((uint32_t)skip_bytes, 2);
    if (n != 0) {
      const uint32_t len = (uint32_t)n - 1u;
      for (int i = 0; i < skip_bytes; i++) {
        put((len >> (i * 8)) & 0xffu, 8);
      }
    }
    align();
    if (n != 0) {
      bytes(data, n);
    }
  }

  /// An uncompressed meta-block. `nibbles` is 4, 5 or 6; the format requires
  /// the top nibble to be non-zero for 5 and 6, so a caller asking for more
  /// nibbles than the length needs is asking for a stream the decoder must
  /// refuse.
  void stored(const uint8_t * data, size_t n, int nibbles) {
    const uint32_t len = (uint32_t)n - 1u;
    put(0, 1);                            // ISLAST
    put((uint32_t)(nibbles - 4), 2);      // MNIBBLES
    for (int i = 0; i < nibbles; i++) {
      put((len >> (i * 4)) & 15u, 4);
    }
    put(1, 1); // ISUNCOMPRESSED
    align();
    bytes(data, n);
  }

  void last_empty() {
    put(1, 1); // ISLAST
    put(1, 1); // ISLASTEMPTY
    align();
  }

  const std::vector<uint8_t> & done() {
    return out_;
  }

private:
  std::vector<uint8_t> out_;
  int nbits_ = 0;
};

} // namespace

class BrotliRobustnessTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  std::vector<uint8_t> Compress(const uint8_t * data, size_t len, int level) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.level", level), GCOMP_OK);
    std::vector<uint8_t> out(len + 1024);
    size_t written = 0;
    const gcomp_status_t st = gcomp_encode_buffer(
        registry_, "brotli", opts, data, len, out.data(), out.size(), &written);
    gcomp_options_destroy(opts);
    EXPECT_EQ(st, GCOMP_OK);
    out.resize(st == GCOMP_OK ? written : 0);
    return out;
  }

  /// Whole-buffer decode. Returns the status and fills `out`.
  gcomp_status_t Decode(const std::vector<uint8_t> & stream, size_t cap,
      std::vector<uint8_t> * out) {
    out->assign(cap, 0);
    size_t written = 0;
    const gcomp_status_t st = gcomp_decode_buffer(registry_, "brotli", nullptr,
        stream.data(), stream.size(), out->data(), out->size(), &written);
    out->resize(st == GCOMP_OK ? written : 0);
    return st;
  }

  /// One input byte per update, with room to write all of it. This is what
  /// reaches the arm where a field runs out of input mid-read and has to
  /// resume where it stopped.
  gcomp_status_t DecodeByteAtATime(const std::vector<uint8_t> & stream,
      size_t cap, std::vector<uint8_t> * out) {
    gcomp_decoder_t * dec = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
    std::vector<uint8_t> buf(cap, 0);
    size_t filled = 0;
    gcomp_status_t st = GCOMP_OK;
    for (size_t i = 0; i < stream.size(); i++) {
      gcomp_buffer_t in = {stream.data() + i, 1, 0};
      // Keep going until this byte is taken: a decoder is allowed to return
      // without consuming it when it has filled the output, and dropping it
      // would corrupt the stream the test is feeding.
      while (in.used < in.size) {
        gcomp_buffer_t ob = {buf.data() + filled, cap - filled, 0};
        st = gcomp_decoder_update(dec, &in, &ob);
        filled += ob.used;
        if (st != GCOMP_OK) {
          break;
        }
        if (in.used == 0 && ob.used == 0) {
          st = GCOMP_ERR_LIMIT; // no progress with room to write: give up
          break;
        }
      }
      if (st != GCOMP_OK) {
        break;
      }
    }
    if (st == GCOMP_OK) {
      gcomp_buffer_t ob = {buf.data() + filled, cap - filled, 0};
      st = gcomp_decoder_finish(dec, &ob);
      filled += ob.used;
    }
    gcomp_decoder_destroy(dec);
    buf.resize(st == GCOMP_OK ? filled : 0);
    *out = buf;
    return st;
  }

  /// The whole stream at once, but one output byte at a time, which is the
  /// other half: every point where the decoder has to stop mid-copy or
  /// mid-literal-run because there is nowhere to put the next byte.
  gcomp_status_t DecodeOneByteOut(const std::vector<uint8_t> & stream,
      size_t cap, std::vector<uint8_t> * out) {
    gcomp_decoder_t * dec = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
    std::vector<uint8_t> buf(cap, 0);
    size_t filled = 0;
    gcomp_buffer_t in = {stream.data(), stream.size(), 0};
    gcomp_status_t st = GCOMP_OK;
    for (;;) {
      gcomp_buffer_t ob = {buf.data() + filled, filled < cap ? 1u : 0u, 0};
      st = gcomp_decoder_update(dec, &in, &ob);
      filled += ob.used;
      if (st != GCOMP_OK) {
        break;
      }
      if (ob.used == 0) {
        break; // nothing more to write with a byte of room: the input is done
      }
    }
    if (st == GCOMP_OK) {
      for (;;) {
        gcomp_buffer_t ob = {buf.data() + filled, filled < cap ? 1u : 0u, 0};
        st = gcomp_decoder_finish(dec, &ob);
        filled += ob.used;
        if (st != GCOMP_ERR_LIMIT || ob.used == 0) {
          break;
        }
      }
    }
    gcomp_decoder_destroy(dec);
    buf.resize(st == GCOMP_OK ? filled : 0);
    *out = buf;
    return st;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// API sequences and argument checks
//

TEST_F(BrotliRobustnessTest, DecoderFinishBeforeUpdate) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(16);
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  // Nothing has been read, so the stream is not complete. The contract says
  // this is an error, not a silent empty success.
  EXPECT_NE(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderUpdateAfterFinish) {
  const uint8_t text[] = "update after finish";
  const std::vector<uint8_t> stream = Compress(text, sizeof(text) - 1, 1);
  ASSERT_FALSE(stream.empty());

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(64);
  gcomp_buffer_t in = {stream.data(), stream.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);

  // A completed stream is not an error to keep feeding: the decoder reports
  // success and consumes nothing, which is how a container tells where the
  // stream ended. What it must not do is decode the bytes again.
  gcomp_buffer_t again = {stream.data(), stream.size(), 0};
  gcomp_buffer_t ob2 = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &again, &ob2), GCOMP_OK);
  EXPECT_EQ(again.used, 0u) << "bytes after the stream were consumed";
  EXPECT_EQ(ob2.used, 0u) << "bytes after the stream produced output";
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderMultipleFinishCalls) {
  const uint8_t text[] = "finish twice";
  const std::vector<uint8_t> stream = Compress(text, sizeof(text) - 1, 1);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(64);
  gcomp_buffer_t in = {stream.data(), stream.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  // A second finish on a complete stream is idempotent, not an error.
  EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderDestroyWithoutFinish) {
  const uint8_t text[] = "destroyed mid-stream";
  const std::vector<uint8_t> stream = Compress(text, sizeof(text) - 1, 1);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(64);
  gcomp_buffer_t in = {stream.data(), stream.size() / 2, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_decoder_update(dec, &in, &ob);
  gcomp_decoder_destroy(dec); // the window and every tree have to go with it
}

TEST_F(BrotliRobustnessTest, DecoderDestroyImmediately) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderDestroyIsSafeOnNull) {
  gcomp_decoder_destroy(nullptr);
}

TEST_F(BrotliRobustnessTest, DecoderUpdateZeroSizeInput) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(16);
  gcomp_buffer_t in = {nullptr, 0, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
  EXPECT_EQ(ob.used, 0u);
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderUpdateZeroSizeOutput) {
  const uint8_t text[] = "no room at all";
  const std::vector<uint8_t> stream = Compress(text, sizeof(text) - 1, 1);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  gcomp_buffer_t in = {stream.data(), stream.size(), 0};
  gcomp_buffer_t ob = {nullptr, 0, 0};
  // No room is not an error; the decoder consumes what it can and stops.
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
  EXPECT_EQ(ob.used, 0u);
  gcomp_decoder_destroy(dec);
}

/**
 * The six argument checks in `buffers_ok`, none of which any test reached.
 * Each one has to be refused with GCOMP_ERR_INVALID_ARG and leave a detail
 * string behind, because a decoder that returns a bare code here is a decoder
 * a caller cannot debug.
 */
TEST_F(BrotliRobustnessTest, DecoderRefusesInconsistentBuffers) {
  const uint8_t text[] = "argument checks";
  const std::vector<uint8_t> stream = Compress(text, sizeof(text) - 1, 1);
  std::vector<uint8_t> out(64);

  struct Case {
    const char * what;
    bool null_input;
    bool null_output;
    bool input_overused;
    bool output_overused;
    bool input_data_null;
    bool output_data_null;
  };
  static const Case cases[] = {
      {"input buffer pointer is NULL", true, false, false, false, false, false},
      {"output buffer pointer is NULL", false, true, false, false, false, false},
      {"input used exceeds size", false, false, true, false, false, false},
      {"output used exceeds size", false, false, false, true, false, false},
      {"input data is NULL with a size", false, false, false, false, true,
          false},
      {"output data is NULL with a size", false, false, false, false, false,
          true},
  };

  for (const Case & c : cases) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(
        gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
    gcomp_buffer_t in = {stream.data(), stream.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (c.input_overused) {
      in.used = in.size + 1;
    }
    if (c.output_overused) {
      ob.used = ob.size + 1;
    }
    if (c.input_data_null) {
      in.data = nullptr;
    }
    if (c.output_data_null) {
      ob.data = nullptr;
    }
    const gcomp_status_t st = gcomp_decoder_update(dec,
        c.null_input ? nullptr : &in, c.null_output ? nullptr : &ob);
    EXPECT_EQ(st, GCOMP_ERR_INVALID_ARG) << c.what;
    const char * detail = gcomp_decoder_get_error_detail(dec);
    // A NULL buffer pointer is refused by gcomp_decoder_update itself, before
    // any method sees it, and that layer sets no detail - which is why the
    // two arms brotli used to spell for this case were unreachable and are
    // gone. Everything the method does answer says what was wrong.
    if (c.null_input || c.null_output) {
      EXPECT_TRUE(detail == nullptr || detail[0] == '\0')
          << c.what << ": the core layer grew a detail string";
    }
    else {
      EXPECT_TRUE(detail != nullptr && detail[0] != '\0')
          << c.what << ": refused with no detail string";
    }
    gcomp_decoder_destroy(dec);
  }
}

TEST_F(BrotliRobustnessTest, DecoderRefusesNullHandles) {
  std::vector<uint8_t> out(16);
  gcomp_buffer_t in = {out.data(), 0, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(nullptr, &in, &ob), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_decoder_finish(nullptr, &ob), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_decoder_reset(nullptr), GCOMP_ERR_INVALID_ARG);
}

/**
 * `brotli_decoder_reset` had never been called. It frees the window and every
 * tree, zeroes the state, and then has to put back the four limits and the
 * distance ring buffer - so a reset that forgets one of them shows up as the
 * second stream decoding differently from the first, which is what this
 * compares.
 */
TEST_F(BrotliRobustnessTest, DecoderResetDecodesTheNextStreamIdentically) {
  std::vector<uint8_t> text(4000);
  test_helpers_generate_pattern(text.data(), text.size(),
      reinterpret_cast<const uint8_t *>("reset me "), 9);
  const std::vector<uint8_t> stream = Compress(text.data(), text.size(), 1);
  ASSERT_FALSE(stream.empty());

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  for (int pass = 0; pass < 3; pass++) {
    std::vector<uint8_t> out(text.size() + 64);
    gcomp_buffer_t in = {stream.data(), stream.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK) << pass;
    ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK) << pass;
    out.resize(ob.used);
    if (pass == 0) {
      first = out;
    }
    else {
      second = out;
      EXPECT_EQ(second, first) << "pass " << pass << " differs from the first";
    }
    ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK) << pass;
  }
  EXPECT_EQ(first.size(), text.size());
  EXPECT_EQ(memcmp(first.data(), text.data(), text.size()), 0);
  gcomp_decoder_destroy(dec);
}

TEST_F(BrotliRobustnessTest, DecoderResetMidStreamDiscardsTheStream) {
  std::vector<uint8_t> text(4000);
  test_helpers_generate_pattern(text.data(), text.size(),
      reinterpret_cast<const uint8_t *>("abandon "), 8);
  const std::vector<uint8_t> stream = Compress(text.data(), text.size(), 1);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(text.size() + 64);
  gcomp_buffer_t in = {stream.data(), stream.size() / 2, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_decoder_update(dec, &in, &ob);
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // The half-read stream is gone: the whole one decodes from the start.
  gcomp_buffer_t in2 = {stream.data(), stream.size(), 0};
  gcomp_buffer_t ob2 = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in2, &ob2), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob2), GCOMP_OK);
  ASSERT_EQ(ob2.used, text.size());
  EXPECT_EQ(memcmp(out.data(), text.data(), text.size()), 0);
  gcomp_decoder_destroy(dec);
}

/**
 * A refusal the decoder latched, and what a reset does to it.
 *
 * The two kinds of failure are not the same and the test used to assume they
 * were. A `dec_fail` - a Huffman code that does not exist, a length past the
 * meta-block - sets a flag, and every later call returns that code without
 * looking at the input again. A `finish` on a stream that simply stopped early
 * reports GCOMP_ERR_CORRUPT with "truncated stream" and latches nothing,
 * because more input could still complete it; feeding the same bytes again
 * gets GCOMP_OK, meaning "still waiting". Both are asserted here so that a
 * change to either is visible.
 */
TEST_F(BrotliRobustnessTest, ALatchedRefusalStaysUntilResetAndATruncationDoesNot) {
  std::vector<uint8_t> text(2000);
  test_helpers_generate_pattern(text.data(), text.size(),
      reinterpret_cast<const uint8_t *>("latch this "), 11);
  const std::vector<uint8_t> good = Compress(text.data(), text.size(), 1);
  ASSERT_GT(good.size(), 16u);

  // Corrupt a byte inside the compressed body, past the header, so the failure
  // comes from a code the decoder read rather than from running out of input.
  std::vector<uint8_t> broken = good;
  gcomp_status_t latched = GCOMP_OK;
  for (size_t pos = good.size() / 2; pos < good.size() && latched == GCOMP_OK;
      pos++) {
    broken = good;
    broken[pos] = (uint8_t)(broken[pos] ^ 0xffu);
    std::vector<uint8_t> out;
    const gcomp_status_t st = Decode(broken, text.size() + 256, &out);
    if (st == GCOMP_ERR_CORRUPT) {
      latched = st;
    }
  }
  ASSERT_EQ(latched, GCOMP_ERR_CORRUPT)
      << "no single-byte change to the body of this stream was refused, so "
         "there is nothing latched to test";

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(text.size() + 256);
  gcomp_buffer_t in = {broken.data(), broken.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
  if (st == GCOMP_OK) {
    st = gcomp_decoder_finish(dec, &ob);
  }
  ASSERT_EQ(st, GCOMP_ERR_CORRUPT);
  const std::string first_detail = gcomp_decoder_get_error_detail(dec);
  EXPECT_FALSE(first_detail.empty());

  // Latched: a fresh update returns the same code and reads nothing.
  gcomp_buffer_t in2 = {good.data(), good.size(), 0};
  gcomp_buffer_t ob2 = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in2, &ob2), GCOMP_ERR_CORRUPT);
  EXPECT_EQ(in2.used, 0u) << "a latched decoder consumed input";

  // Reset clears it, and the next stream decodes as if nothing happened.
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_get_error_detail(dec)[0], '\0');
  gcomp_buffer_t in3 = {good.data(), good.size(), 0};
  gcomp_buffer_t ob3 = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in3, &ob3), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob3), GCOMP_OK);
  ASSERT_EQ(ob3.used, text.size());
  EXPECT_EQ(memcmp(out.data(), text.data(), text.size()), 0);
  gcomp_decoder_destroy(dec);

  // The other kind: a stream cut short. finish calls it corrupt and latches
  // nothing, so the same bytes offered again are merely incomplete.
  const std::vector<uint8_t> cut(good.begin(), good.begin() + (long)(good.size() / 2));
  gcomp_decoder_t * dec2 = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", nullptr, &dec2), GCOMP_OK);
  gcomp_buffer_t in4 = {cut.data(), cut.size(), 0};
  gcomp_buffer_t ob4 = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec2, &in4, &ob4), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(dec2, &ob4), GCOMP_ERR_CORRUPT);
  EXPECT_STREQ(gcomp_decoder_get_error_detail(dec2), "brotli: truncated stream");
  gcomp_buffer_t in5 = {good.data() + cut.size(), good.size() - cut.size(), 0};
  gcomp_buffer_t ob5 = {out.data() + ob4.used, out.size() - ob4.used, 0};
  EXPECT_EQ(gcomp_decoder_update(dec2, &in5, &ob5), GCOMP_OK)
      << "a truncation was latched, so the rest of the stream cannot be fed";
  gcomp_decoder_destroy(dec2);
}

//
// Streams built bit by bit, for the parts of the format no encoder here writes
//

/**
 * A metadata meta-block carrying content. Nothing in this library writes one -
 * the encoder emits only the empty form, to get back on a byte boundary - so
 * the decoder's skip phase had never run. The payload must not reach the
 * output, and the data around it must.
 */
TEST_F(BrotliRobustnessTest, MetadataContentIsSkippedNotEmitted) {
  const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0xff, 0x41, 0x42};
  const uint8_t text[] = "metadata sits between these";
  const size_t text_len = sizeof(text) - 1;

  // skip_bytes 1, 2 and 3 are the three lengths MSKIPBYTES can ask for.
  for (int skip_bytes = 1; skip_bytes <= 3; skip_bytes++) {
    const size_t n = skip_bytes == 1 ? 8u : (skip_bytes == 2 ? 300u : 70000u);
    std::vector<uint8_t> big(n);
    for (size_t i = 0; i < n; i++) {
      big[i] = payload[i % sizeof(payload)];
    }
    BitWriter bw;
    bw.window16();
    bw.metadata(big.data(), big.size(), skip_bytes);
    bw.stored(text, text_len, 4);
    bw.last_empty();
    const std::vector<uint8_t> stream = bw.done();

    std::vector<uint8_t> out;
    ASSERT_EQ(Decode(stream, text_len + 64, &out), GCOMP_OK)
        << "skip_bytes " << skip_bytes;
    ASSERT_EQ(out.size(), text_len) << "skip_bytes " << skip_bytes;
    EXPECT_EQ(memcmp(out.data(), text, text_len), 0)
        << "skip_bytes " << skip_bytes;

    // One byte at a time, which is what suspends inside the skip.
    std::vector<uint8_t> slow;
    ASSERT_EQ(DecodeByteAtATime(stream, text_len + 64, &slow), GCOMP_OK)
        << "skip_bytes " << skip_bytes;
    EXPECT_EQ(slow, out) << "skip_bytes " << skip_bytes;

    // And libbrotli has to agree this is a stream, or the test above is
    // only asserting that we accept something we invented.
    const BrotliLib & lib = brotli_lib();
    if (lib.ok()) {
      std::vector<uint8_t> theirs(text_len + 64);
      size_t written = theirs.size();
      ASSERT_EQ(lib.decompress(stream.size(), stream.data(), &written,
                    theirs.data()),
          1)
          << "skip_bytes " << skip_bytes;
      ASSERT_EQ(written, text_len) << "skip_bytes " << skip_bytes;
      EXPECT_EQ(memcmp(theirs.data(), text, text_len), 0)
          << "skip_bytes " << skip_bytes;
    }
  }
}

TEST_F(BrotliRobustnessTest, AnEmptyMetadataBlockIsAllowedAnywhere) {
  const uint8_t text[] = "empty metadata before and after";
  const size_t text_len = sizeof(text) - 1;
  BitWriter bw;
  bw.window16();
  bw.metadata(nullptr, 0, 0);
  bw.stored(text, text_len, 4);
  bw.metadata(nullptr, 0, 0);
  bw.last_empty();
  const std::vector<uint8_t> stream = bw.done();
  std::vector<uint8_t> out;
  ASSERT_EQ(Decode(stream, text_len + 64, &out), GCOMP_OK);
  EXPECT_EQ(out.size(), text_len);
  EXPECT_EQ(memcmp(out.data(), text, text_len), 0);
}

/**
 * Five- and six-nibble meta-block lengths. Our encoder writes four nibbles
 * for everything it stores, because it stores at most 65536 bytes at a time,
 * so the wider forms came in only from libbrotli and only when it chose to
 * use them. A stored block of more than 65536 bytes needs five.
 */
TEST_F(BrotliRobustnessTest, WiderMetaBlockLengthsAreRead) {
  for (int nibbles = 4; nibbles <= 6; nibbles++) {
    // The format requires the top nibble to be non-zero, so each width needs
    // a length that actually uses it.
    const size_t n = nibbles == 4 ? 60000u
                                  : (nibbles == 5 ? 100000u : 1100000u);
    std::vector<uint8_t> data(n);
    test_helpers_generate_random(data.data(), data.size(), 37 + nibbles);
    BitWriter bw;
    bw.window16();
    bw.stored(data.data(), data.size(), nibbles);
    bw.last_empty();
    const std::vector<uint8_t> stream = bw.done();

    std::vector<uint8_t> out;
    ASSERT_EQ(Decode(stream, n + 64, &out), GCOMP_OK) << nibbles;
    ASSERT_EQ(out.size(), n) << nibbles;
    EXPECT_EQ(memcmp(out.data(), data.data(), n), 0) << nibbles;

    const BrotliLib & lib = brotli_lib();
    if (lib.ok()) {
      std::vector<uint8_t> theirs(n + 64);
      size_t written = theirs.size();
      ASSERT_EQ(lib.decompress(stream.size(), stream.data(), &written,
                    theirs.data()),
          1)
          << nibbles;
      ASSERT_EQ(written, n) << nibbles;
      EXPECT_EQ(memcmp(theirs.data(), data.data(), n), 0) << nibbles;
    }
  }
}

/**
 * The malformed headers the decoder names, each built deliberately rather
 * than stumbled on. Every one has to be GCOMP_ERR_CORRUPT with a message, and
 * the message is checked because a decoder that refuses everything with one
 * string is not telling a caller anything.
 */
TEST_F(BrotliRobustnessTest, MalformedHeadersAreNamed) {
  const uint8_t text[] = "payload";
  const size_t text_len = sizeof(text) - 1;
  std::set<std::string> reasons;

  struct Build {
    const char * what;
    void (*build)(BitWriter &, const uint8_t *, size_t);
  };
  static const Build builds[] = {
      {"reserved metadata bit set",
          [](BitWriter & bw, const uint8_t * t, size_t n) {
            bw.window16();
            bw.put(0, 1); // ISLAST
            bw.put(3, 2); // metadata
            bw.put(1, 1); // reserved, which must be zero
            bw.put(0, 2);
            bw.align();
            bw.stored(t, n, 4);
            bw.last_empty();
          }},
      {"metadata length with a zero top byte",
          [](BitWriter & bw, const uint8_t *, size_t) {
            bw.window16();
            bw.put(0, 1);
            bw.put(3, 2);
            bw.put(0, 1);
            bw.put(2, 2); // two length bytes...
            bw.put(5, 8);
            bw.put(0, 8); // ...and the top one is zero, which is not canonical
            bw.align();
          }},
      {"five-nibble length with a zero top nibble",
          [](BitWriter & bw, const uint8_t * t, size_t n) {
            bw.window16();
            bw.put(0, 1);
            bw.put(1, 2); // MNIBBLES == 5
            const uint32_t len = (uint32_t)n - 1u;
            for (int i = 0; i < 4; i++) {
              bw.put((len >> (i * 4)) & 15u, 4);
            }
            bw.put(0, 4); // the fifth nibble, which must not be zero
            bw.put(1, 1);
            bw.align();
            bw.bytes(t, n);
            bw.last_empty();
          }},
      {"non-zero padding after the last empty block",
          [](BitWriter & bw, const uint8_t *, size_t) {
            bw.window16();
            bw.put(1, 1); // ISLAST
            bw.put(1, 1); // ISLASTEMPTY
            bw.put(1, 1); // and then a one where the padding must be zero
            bw.align();
          }},
      {"non-zero padding before stored bytes",
          [](BitWriter & bw, const uint8_t * t, size_t n) {
            bw.window16();
            const uint32_t len = (uint32_t)n - 1u;
            bw.put(0, 1);
            bw.put(0, 2);
            for (int i = 0; i < 4; i++) {
              bw.put((len >> (i * 4)) & 15u, 4);
            }
            bw.put(1, 1); // ISUNCOMPRESSED
            bw.put(1, 1); // a one in the padding to the byte boundary
            bw.align();
            bw.bytes(t, n);
            bw.last_empty();
          }},
  };

  for (const Build & b : builds) {
    BitWriter bw;
    b.build(bw, text, text_len);
    const std::vector<uint8_t> stream = bw.done();

    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(
        gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
    std::vector<uint8_t> out(text_len + 256);
    gcomp_buffer_t in = {stream.data(), stream.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
    if (st == GCOMP_OK) {
      st = gcomp_decoder_finish(dec, &ob);
    }
    EXPECT_EQ(st, GCOMP_ERR_CORRUPT) << b.what;
    const char * detail = gcomp_decoder_get_error_detail(dec);
    if (st == GCOMP_ERR_CORRUPT && detail && detail[0]) {
      reasons.insert(detail);
    }
    EXPECT_TRUE(detail != nullptr && detail[0] != '\0')
        << b.what << ": refused with no detail string";
    gcomp_decoder_destroy(dec);

    // libbrotli has to refuse it too. A stream only we reject is a stream we
    // may be rejecting for the wrong reason.
    const BrotliLib & lib = brotli_lib();
    if (lib.ok()) {
      std::vector<uint8_t> theirs(text_len + 256);
      size_t written = theirs.size();
      EXPECT_EQ(lib.decompress(stream.size(), stream.data(), &written,
                    theirs.data()),
          0)
          << b.what << ": libbrotli accepted it";
    }
  }

  // Four distinct messages for five builds: the two padding cases share one.
  EXPECT_GE(reasons.size(), 4u)
      << "the malformed headers collapsed into " << reasons.size()
      << " distinct messages, so they are not reaching distinct arms";
}

//
// A corpus, fed byte at a time, and then corrupted
//

namespace {

/// Inputs chosen so that between them libbrotli's quality 11 has reason to
/// split blocks, build context maps and use the static dictionary - the parts
/// of the format our own encoder never writes and so never tested.
std::vector<std::pair<const char *, std::vector<uint8_t>>> DecoderCorpus() {
  std::vector<std::pair<const char *, std::vector<uint8_t>>> all;

  const char * prose =
      "The quick brown fox jumps over the lazy dog. In the beginning the "
      "Universe was created; this has made a lot of people very angry and "
      "been widely regarded as a bad move. ";
  std::vector<uint8_t> text;
  while (text.size() < 40000) {
    text.insert(text.end(), prose, prose + strlen(prose));
  }
  text.resize(40000);
  all.emplace_back("English prose", text);

  // Words from the static dictionary, which is what makes quality 11 emit
  // dictionary references and their transforms.
  const char * webby =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<title>index</title></head><body><div class=\"content\">"
      "<a href=\"https://example.com/index.html\">link</a></div>"
      "<script src=\"/static/app.js\"></script></body></html>";
  std::vector<uint8_t> html;
  while (html.size() < 30000) {
    html.insert(html.end(), webby, webby + strlen(webby));
  }
  html.resize(30000);
  all.emplace_back("HTML, which the static dictionary is built for", html);

  // Heterogeneous: prose, then noise, then a long run, then prose again.
  // Block splitting exists for exactly this shape.
  std::vector<uint8_t> mixed;
  mixed.insert(mixed.end(), text.begin(), text.begin() + 12000);
  std::vector<uint8_t> noise(12000);
  test_helpers_generate_random(noise.data(), noise.size(), 5);
  mixed.insert(mixed.end(), noise.begin(), noise.end());
  mixed.insert(mixed.end(), 12000, 0x7e);
  mixed.insert(mixed.end(), html.begin(), html.begin() + 12000);
  all.emplace_back("prose, noise, a run and markup in one stream", mixed);

  // Every byte value, which forces the full literal alphabet and, at the top
  // of the range, the context modes that look at the previous two bytes.
  std::vector<uint8_t> every(16384);
  for (size_t i = 0; i < every.size(); i++) {
    every[i] = (uint8_t)((i * 31u + (i >> 8)) & 0xffu);
  }
  all.emplace_back("every byte value", every);

  std::vector<uint8_t> tiny(3);
  tiny[0] = 'a';
  tiny[1] = 'b';
  tiny[2] = 'c';
  all.emplace_back("three bytes", tiny);

  all.emplace_back("one byte", std::vector<uint8_t>(1, 'z'));
  all.emplace_back("empty", std::vector<uint8_t>());
  return all;
}

} // namespace

/**
 * Every stream in the corpus, from both encoders, decoded one byte in and one
 * byte out.
 *
 * Each field in this decoder can suspend when the input runs out mid-read and
 * has to resume where it stopped; those resume arms are most of what a
 * whole-buffer decode never reaches. There was one byte-at-a-time test, over a
 * 43-byte stream, which split on the handful of fields that stream contained.
 */
TEST_F(BrotliRobustnessTest, EveryStreamSurvivesOneByteAtATime) {
  const BrotliLib & lib = brotli_lib();
  for (const auto & entry : DecoderCorpus()) {
    std::vector<std::pair<const char *, std::vector<uint8_t>>> streams;
    streams.emplace_back("ours, level 0",
        Compress(entry.second.data(), entry.second.size(), 0));
    streams.emplace_back("ours, level 1",
        Compress(entry.second.data(), entry.second.size(), 1));
    if (lib.ok()) {
      for (int q : {0, 5, 11}) {
        const size_t cap = lib.max_size(entry.second.size());
        std::vector<uint8_t> enc(cap ? cap : 64);
        size_t written = enc.size();
        if (lib.compress(q, 16, 0, entry.second.size(), entry.second.data(),
                &written, enc.data())
            == 1) {
          enc.resize(written);
          streams.emplace_back(
              q == 0 ? "libbrotli q0" : (q == 5 ? "libbrotli q5" : "libbrotli q11"),
              enc);
        }
      }
    }

    for (const auto & s : streams) {
      ASSERT_FALSE(s.second.empty()) << entry.first << " / " << s.first;
      std::vector<uint8_t> slow_in;
      ASSERT_EQ(
          DecodeByteAtATime(s.second, entry.second.size() + 64, &slow_in),
          GCOMP_OK)
          << entry.first << " / " << s.first << " / one byte in";
      ASSERT_EQ(slow_in.size(), entry.second.size())
          << entry.first << " / " << s.first << " / one byte in";

      std::vector<uint8_t> slow_out;
      ASSERT_EQ(
          DecodeOneByteOut(s.second, entry.second.size() + 64, &slow_out),
          GCOMP_OK)
          << entry.first << " / " << s.first << " / one byte out";
      ASSERT_EQ(slow_out.size(), entry.second.size())
          << entry.first << " / " << s.first << " / one byte out";

      if (!entry.second.empty()) {
        EXPECT_EQ(memcmp(slow_in.data(), entry.second.data(), slow_in.size()), 0)
            << entry.first << " / " << s.first << " / one byte in";
        EXPECT_EQ(
            memcmp(slow_out.data(), entry.second.data(), slow_out.size()), 0)
            << entry.first << " / " << s.first << " / one byte out";
      }
    }
  }
}

/**
 * The corruption sweep: every byte of a stream, replaced by each of four
 * values, and every truncation of it.
 *
 * What a sweep like this must not be is an assertion that nothing crashed,
 * because that stays true as it stops reaching anything - a stream whose first
 * byte is broken is refused before the interesting code runs. So two things
 * are measured instead. The output, when the decoder returns GCOMP_OK, has to
 * be a prefix-correct decode or a refusal and never a wrong answer reported as
 * success. And the number of *distinct* refusal messages is floored: these are
 * the decoder's own names for the things it checks, so a sweep that stops
 * provoking them has stopped testing the error paths whatever its exit status
 * says.
 */
/* Measured on 2026-10-02, not guessed: the sweep below provoked these
 * fourteen of the decoder's own refusals, out of 34907 trials, of which 8759
 * were refused and 26148 still decoded - a single changed byte in a format
 * this dense usually yields a different legal stream rather than an illegal
 * one, which is why the count of distinct refusals is the thing worth
 * flooring. Raise it when the sweep reaches more. */
static const size_t kDistinctRefusals = 14;

TEST_F(BrotliRobustnessTest, CorruptionIsRefusedAndTheRefusalsAreCounted) {
  std::vector<uint8_t> text(6000);
  test_helpers_generate_pattern(text.data(), text.size(),
      reinterpret_cast<const uint8_t *>("corrupt this stream, carefully. "), 32);
  for (size_t i = 3000; i < 4000; i++) {
    text[i] = (uint8_t)(i * 17u);
  }

  std::vector<std::vector<uint8_t>> bases;
  bases.push_back(Compress(text.data(), text.size(), 1));
  bases.push_back(Compress(text.data(), text.size(), 0));
  const BrotliLib & lib = brotli_lib();
  if (lib.ok()) {
    for (int q : {5, 11}) {
      const size_t cap = lib.max_size(text.size());
      std::vector<uint8_t> enc(cap);
      size_t written = enc.size();
      if (lib.compress(q, 16, 0, text.size(), text.data(), &written, enc.data())
          == 1) {
        enc.resize(written);
        bases.push_back(enc);
      }
    }
  }

  std::set<std::string> reasons;
  size_t refused = 0;
  size_t accepted = 0;
  size_t trials = 0;

  for (const std::vector<uint8_t> & base : bases) {
    ASSERT_FALSE(base.empty());
    for (size_t pos = 0; pos < base.size(); pos++) {
      for (uint8_t replacement : {(uint8_t)0x00, (uint8_t)0xff, (uint8_t)0x55,
               (uint8_t)(base[pos] ^ 0x80u)}) {
        if (replacement == base[pos]) {
          continue;
        }
        std::vector<uint8_t> bad = base;
        bad[pos] = replacement;
        trials++;

        gcomp_decoder_t * dec = nullptr;
        ASSERT_EQ(
            gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
        std::vector<uint8_t> out(text.size() + 1024, 0xa5);
        gcomp_buffer_t in = {bad.data(), bad.size(), 0};
        gcomp_buffer_t ob = {out.data(), out.size(), 0};
        gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
        if (st == GCOMP_OK) {
          st = gcomp_decoder_finish(dec, &ob);
        }
        if (st == GCOMP_OK) {
          accepted++;
          // A one-byte change usually produces a different but entirely legal
          // stream, so the bytes cannot be compared against anything. What
          // the decoder must not do is write past the length it reports: the
          // buffer is painted, and the byte after the reported end has to
          // still be paint. A sanitizer cannot see this one, because the
          // write would be inside the buffer the caller handed over.
          ASSERT_LE(ob.used, out.size());
          if (ob.used < out.size()) {
            EXPECT_EQ(out[ob.used], 0xa5)
                << "wrote past the " << ob.used << " bytes it reported";
          }
        }
        else {
          refused++;
          const char * detail = gcomp_decoder_get_error_detail(dec);
          if (detail && detail[0]) {
            reasons.insert(detail);
          }
        }
        gcomp_decoder_destroy(dec);
      }
    }

    // Every truncation, which is the other half: a stream that stops inside
    // a field must come back as incomplete rather than as a short decode.
    for (size_t keep = 0; keep < base.size(); keep++) {
      std::vector<uint8_t> cut(base.begin(), base.begin() + (long)keep);
      trials++;
      gcomp_decoder_t * dec = nullptr;
      ASSERT_EQ(
          gcomp_decoder_create(registry_, "brotli", nullptr, &dec), GCOMP_OK);
      std::vector<uint8_t> out(text.size() + 1024, 0);
      gcomp_buffer_t in = {cut.empty() ? nullptr : cut.data(), cut.size(), 0};
      gcomp_buffer_t ob = {out.data(), out.size(), 0};
      gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
      if (st == GCOMP_OK) {
        st = gcomp_decoder_finish(dec, &ob);
      }
      EXPECT_NE(st, GCOMP_OK)
          << "a stream truncated to " << keep << " of " << base.size()
          << " bytes decoded as complete";
      if (st != GCOMP_OK) {
        refused++;
        const char * detail = gcomp_decoder_get_error_detail(dec);
        if (detail && detail[0]) {
          reasons.insert(detail);
        }
      }
      gcomp_decoder_destroy(dec);
    }
  }

  EXPECT_GT(trials, 20000u) << "the sweep got smaller than it was written to be";
  EXPECT_GT(refused, 0u);
  EXPECT_GT(accepted, 0u)
      << "no corrupted stream decoded at all, so the sweep is only reaching "
         "the header and not the body";
  // Printed, not just counted: the floor below is only maintainable by
  // someone who can see what it is a floor on.
  RecordProperty("distinct_refusals", (int)reasons.size());
  std::cerr << "    " << reasons.size() << " distinct refusals over " << trials
            << " trials (" << refused << " refused, " << accepted
            << " still decoded):\n";
  for (const std::string & r : reasons) {
    std::cerr << "      " << r << "\n";
  }
  EXPECT_GE(reasons.size(), kDistinctRefusals)
      << "the sweep provoked only " << reasons.size()
      << " distinct refusals across " << trials
      << " trials; it has stopped reaching the decoder's checks";
}

/**
 * The two limits that refuse a stream before its window is allocated.
 *
 * `limits.max_window_bytes` and `limits.max_memory_bytes` are the only things
 * standing between a one-byte header and a 16MiB allocation, and neither
 * refusal had ever been reached: the default window limit accepts every window
 * RFC 7932 defines, so nothing that did not set the option could reach them.
 * Each is checked against the window the *stream* declares, which is the point
 * - the decision is made from the header, before the memory is taken.
 */
TEST_F(BrotliRobustnessTest, WindowLimitsRefuseBeforeAllocating) {
  const uint8_t text[] = "a large window for a small payload";
  const size_t text_len = sizeof(text) - 1;

  // lgwin 24 is the largest the format defines: a window of 16777200 bytes.
  gcomp_options_t * eopts = nullptr;
  ASSERT_EQ(gcomp_options_create(&eopts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(eopts, "brotli.lgwin", 24), GCOMP_OK);
  std::vector<uint8_t> wide(text_len + 1024);
  size_t written = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "brotli", eopts, text, text_len,
                wide.data(), wide.size(), &written),
      GCOMP_OK);
  gcomp_options_destroy(eopts);
  wide.resize(written);

  struct Case {
    const char * key;
    uint64_t value;
    const char * expect;
  };
  static const Case cases[] = {
      {"limits.max_window_bytes", 65536,
          "brotli: window exceeds limits.max_window_bytes"},
      {"limits.max_memory_bytes", 65536,
          "brotli: window exceeds limits.max_memory_bytes"},
  };

  for (const Case & c : cases) {
    gcomp_options_t * dopts = nullptr;
    ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(dopts, c.key, c.value), GCOMP_OK)
        << c.key;
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "brotli", dopts, &dec), GCOMP_OK)
        << c.key;
    gcomp_options_destroy(dopts);
    std::vector<uint8_t> out(text_len + 64);
    gcomp_buffer_t in = {wide.data(), wide.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
    if (st == GCOMP_OK) {
      st = gcomp_decoder_finish(dec, &ob);
    }
    EXPECT_EQ(st, GCOMP_ERR_LIMIT) << c.key;
    EXPECT_STREQ(gcomp_decoder_get_error_detail(dec), c.expect) << c.key;
    gcomp_decoder_destroy(dec);
  }

  // The same stream with the limits left alone has to decode, or the test
  // above is only showing that a 24-bit window is refused outright.
  std::vector<uint8_t> out;
  ASSERT_EQ(Decode(wide, text_len + 64, &out), GCOMP_OK);
  ASSERT_EQ(out.size(), text_len);
  EXPECT_EQ(memcmp(out.data(), text, text_len), 0);
}

/**
 * A stream long enough and varied enough that libbrotli's block counts run
 * out inside a meta-block, so the decoder has to perform real block switches
 * rather than only reading the counts.
 *
 * The switch path reads a block type from the ring buffer of the last two -
 * code 0 means the previous type, code 1 means the current plus one, anything
 * else is the type itself minus two, and the result wraps - and none of it had
 * run, because the counts it depends on were never read at all. Reading them
 * was the fix; exercising the switch is how that fix is more than a bit
 * position.
 */
TEST_F(BrotliRobustnessTest, BlockCountsRunOutAndTheTypesSwitch) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  // Many short segments of unlike material, so a block split is worth making
  // repeatedly rather than once.
  const char * prose = "and so it went on, much as before, for some while. ";
  const char * markup = "<li class=\"row\"><span id=\"x\">cell</span></li>";
  std::vector<uint8_t> data;
  unsigned lcg = 0x5EED5EEDu;
  for (int segment = 0; segment < 40; segment++) {
    const int kind = segment % 4;
    for (int i = 0; i < 5000; i++) {
      if (kind == 0) {
        data.push_back((uint8_t)prose[(size_t)i % strlen(prose)]);
      }
      else if (kind == 1) {
        lcg = lcg * 1103515245u + 12345u;
        data.push_back((uint8_t)(lcg >> 16));
      }
      else if (kind == 2) {
        data.push_back((uint8_t)(0x30 + (i % 10)));
      }
      else {
        data.push_back((uint8_t)markup[(size_t)i % strlen(markup)]);
      }
    }
  }
  ASSERT_EQ(data.size(), 200000u);

  for (int quality : {4, 5, 9, 11}) {
    const size_t cap = lib.max_size(data.size());
    std::vector<uint8_t> enc(cap);
    size_t written = enc.size();
    ASSERT_EQ(lib.compress(quality, 22, 0, data.size(), data.data(), &written,
                  enc.data()),
        1)
        << quality;
    enc.resize(written);

    std::vector<uint8_t> out;
    ASSERT_EQ(Decode(enc, data.size() + 64, &out), GCOMP_OK) << quality;
    ASSERT_EQ(out.size(), data.size()) << quality;
    EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0) << quality;

    // And the same stream with the output metered one byte at a time, which
    // is where a switch has to survive being suspended in the middle.
    std::vector<uint8_t> slow;
    ASSERT_EQ(DecodeOneByteOut(enc, data.size() + 64, &slow), GCOMP_OK)
        << quality;
    EXPECT_EQ(slow.size(), data.size()) << quality;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
