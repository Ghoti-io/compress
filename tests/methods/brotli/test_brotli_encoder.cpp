/**
 * @file test_brotli_encoder.cpp
 *
 * Encoder-side tests for the Brotli method: the argument and sequence checks
 * its siblings have, reset, output back-pressure, and the two prefix-code
 * paths that ordinary text never reaches.
 *
 * ## Why this file exists
 *
 * `brotli_encode.c` sat at 82.1% of 251 lines and `brotli_lz.c` at 90.0% of
 * 851 while the decoder, after its own robustness file, was at 92.6%. The
 * uncovered encoder lines were not exotic either: `brotli_encoder_reset` had
 * never been called by any test, so an encoder reused for a second stream was
 * untested; no test had given the encoder an output buffer too small to drain
 * into, which is the whole `GCOMP_ERR_LIMIT` protocol; `output_ok`'s two
 * argument checks had never been reached; and two prefix-code shapes -
 * the depth-limit fallback in `assign_huffman` and the five-bit code-length
 * code - are reached by the shape of a frequency distribution rather than by
 * the size or kind of the input, so every input any test used missed them.
 *
 * ## What is asserted
 *
 * Behaviour, not coverage. A test written to colour a line in and asserting
 * only that nothing crashed would keep passing after the line stopped doing
 * its job. So the skewed-alphabet inputs assert a byte-exact round trip and
 * hand the stream to libbrotli as well, because a prefix code this library
 * wrote and this library read proves only that the two agree.
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
#include <string>
#include <vector>

#include "methods/brotli/brotli_internal.h"

namespace {

using BrotliDecompress = int (*)(size_t, const uint8_t *, size_t *, uint8_t *);

struct BrotliLib {
  BrotliDecompress decompress = nullptr;
  bool ok() const {
    return decompress != nullptr;
  }
};

const BrotliLib & brotli_lib() {
  static BrotliLib lib;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    void * dec = dlopen("libbrotlidec.so.1", RTLD_NOW);
    if (dec) {
      lib.decompress = reinterpret_cast<BrotliDecompress>(
          dlsym(dec, "BrotliDecoderDecompress"));
    }
  }
  return lib;
}

} // namespace

/**
 * @brief The reference this file compares against is really here.
 *
 * Without this, every `if (lib.ok())` below becomes a silent no-op on a
 * machine with no libbrotli, and the file would still report all green.
 * check-oracle-coverage requires a suite carrying this sentinel to be named
 * in ORACLE_TEST_NAMES, which is what puts it in `make check-oracle`.
 */
TEST(BrotliEncoder, LibbrotliIsActuallyAvailable) {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }
  }
  ASSERT_TRUE(brotli_lib().ok())
      << "libbrotlidec.so.1 could not be loaded. The pinned copy is in the "
         "oracle image; `make check-oracle` runs this binary there. Set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to skip this on a machine that has no "
         "libbrotli.";
}

class BrotliEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  gcomp_options_t * Options(int level, int lgwin = 0) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.level", level), GCOMP_OK);
    if (lgwin != 0) {
      EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.lgwin", lgwin), GCOMP_OK);
    }
    return opts;
  }

  std::vector<uint8_t> Encode(
      const std::vector<uint8_t> & data, int level, int lgwin = 0) {
    gcomp_options_t * opts = Options(level, lgwin);
    std::vector<uint8_t> out(data.size() + 4096);
    size_t written = 0;
    const gcomp_status_t st = gcomp_encode_buffer(registry_, "brotli", opts,
        data.data(), data.size(), out.data(), out.size(), &written);
    gcomp_options_destroy(opts);
    EXPECT_EQ(st, GCOMP_OK);
    out.resize(st == GCOMP_OK ? written : 0);
    return out;
  }

  /**
   * @brief Encode through the streaming API with buffers of a chosen size.
   *
   * `in_chunk` bytes offered per update and `out_room` bytes of output room
   * are both the point: the queue-draining protocol only runs when the
   * output buffer is smaller than what the encoder has to say, and that is
   * what GCOMP_ERR_LIMIT from finish() and flush() means.
   */
  std::vector<uint8_t> EncodeStreaming(const std::vector<uint8_t> & data,
      int level, size_t in_chunk, size_t out_room) {
    gcomp_options_t * opts = Options(level);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(registry_, "brotli", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);
    if (!enc) {
      return {};
    }

    std::vector<uint8_t> stream;
    std::vector<uint8_t> room(out_room ? out_room : 1);
    size_t offered = 0;
    // An update that neither consumes nor produces is the documented signal
    // to stop (stream.h); counting them bounds the loop so a protocol
    // mistake here fails rather than hangs.
    int idle = 0;
    while (offered < data.size() && idle < 4) {
      const size_t take =
          std::min(in_chunk, data.size() - offered);
      gcomp_buffer_t in = {(void *)(data.data() + offered), take, 0};
      gcomp_buffer_t ob = {room.data(), room.size(), 0};
      const gcomp_status_t st = gcomp_encoder_update(enc, &in, &ob);
      EXPECT_EQ(st, GCOMP_OK);
      if (st != GCOMP_OK) {
        break;
      }
      stream.insert(stream.end(), room.data(), room.data() + ob.used);
      idle = (in.used == 0 && ob.used == 0) ? idle + 1 : 0;
      offered += in.used;
    }
    EXPECT_EQ(offered, data.size()) << "the encoder stopped taking input";

    for (int guard = 0; guard < 1000000; guard++) {
      gcomp_buffer_t ob = {room.data(), room.size(), 0};
      const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
      stream.insert(stream.end(), room.data(), room.data() + ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      EXPECT_EQ(st, GCOMP_ERR_LIMIT)
          << "finish must either complete or ask for more room";
      if (st != GCOMP_ERR_LIMIT) {
        break;
      }
    }
    gcomp_encoder_destroy(enc);
    return stream;
  }

  gcomp_status_t Decode(const std::vector<uint8_t> & stream, size_t cap,
      std::vector<uint8_t> * out) {
    out->assign(cap, 0);
    size_t written = 0;
    const gcomp_status_t st = gcomp_decode_buffer(registry_, "brotli", nullptr,
        stream.data(), stream.size(), out->data(), out->size(), &written);
    out->resize(st == GCOMP_OK ? written : 0);
    return st;
  }

  /// libbrotli's verdict on a stream this library wrote.
  void ExpectLibbrotliReads(
      const std::vector<uint8_t> & stream, const std::vector<uint8_t> & want) {
    const BrotliLib & lib = brotli_lib();
    if (!lib.ok()) {
      return;
    }
    std::vector<uint8_t> out(want.size() + 64);
    size_t produced = out.size();
    const int ok = lib.decompress(
        stream.size(), stream.data(), &produced, out.data());
    ASSERT_EQ(ok, 1) << "libbrotli refused a stream we wrote";
    ASSERT_EQ(produced, want.size());
    if (produced != 0) {
      EXPECT_EQ(memcmp(out.data(), want.data(), produced), 0);
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Arguments and sequence
//

/*
 * A buffer whose fields contradict each other is refused by the core, for
 * every method at once: StreamBufferArguments in tests/core/test_stream.cpp
 * asserts the status, the detail naming the field, and that nothing was
 * written, on all eight. It lives there because the check does - brotli's
 * encoder is what took a heap-buffer-overflow for it, and seven other
 * encoders had the same hole.
 */

TEST_F(BrotliEncoderTest, EncoderRefusesNullHandles) {
  std::vector<uint8_t> room(16);
  gcomp_buffer_t in = {room.data(), 0, 0};
  gcomp_buffer_t ob = {room.data(), room.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(nullptr, &in, &ob), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_finish(nullptr, &ob), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_flush(nullptr, &ob, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_reset(nullptr), GCOMP_ERR_INVALID_ARG);
  // Destroying nothing is not an error, and must not be a crash either.
  gcomp_encoder_destroy(nullptr);
}

TEST_F(BrotliEncoderTest, UpdateAfterFinishIsRefused) {
  const std::vector<uint8_t> data(100, 'z');
  gcomp_options_t * opts = Options(1);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "brotli", opts, &enc), GCOMP_OK);
  gcomp_options_destroy(opts);

  std::vector<uint8_t> room(4096);
  gcomp_buffer_t in = {(void *)data.data(), data.size(), 0};
  gcomp_buffer_t ob = {room.data(), room.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  gcomp_buffer_t fb = {room.data(), room.size(), 0};
  ASSERT_EQ(gcomp_encoder_finish(enc, &fb), GCOMP_OK);

  // A second finish is idempotent: the stream is already terminated.
  gcomp_buffer_t again = {room.data(), room.size(), 0};
  EXPECT_EQ(gcomp_encoder_finish(enc, &again), GCOMP_OK);
  EXPECT_EQ(again.used, 0u);

  // An update is not: there is nowhere for the bytes to go.
  gcomp_buffer_t more_in = {(void *)data.data(), data.size(), 0};
  gcomp_buffer_t more_out = {room.data(), room.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &more_in, &more_out),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_STREQ(gcomp_encoder_get_error_detail(enc),
      "brotli: encoder update after finish");
  EXPECT_EQ(more_in.used, 0u);

  gcomp_encoder_destroy(enc);
}

//
// Reset
//

/**
 * @brief An encoder reset writes the next stream exactly as a new one would.
 *
 * `brotli_encoder_reset` had never been called by any test. It clears
 * `started`, `finished`, the hold and the output queue, and calls
 * `reset_dist` to put the distance ring buffer back to the four values a
 * decoder starts with - and the ring buffer is the one piece of state that
 * would otherwise survive and make the second stream depend on the first.
 */
TEST_F(BrotliEncoderTest, ResetWritesTheNextStreamAsAFreshEncoderWould) {
  // Two different inputs, so a reset that kept the first stream's distance
  // ring buffer would encode the second one differently.
  std::vector<uint8_t> first(9000);
  std::vector<uint8_t> second(7000);
  for (size_t i = 0; i < first.size(); i++) {
    first[i] = (uint8_t)("the quick brown fox jumps "[i % 26]);
  }
  for (size_t i = 0; i < second.size(); i++) {
    second[i] = (uint8_t)(i % 7 == 0 ? 'A' + (i % 23) : 'x');
  }

  for (int level : {0, 1}) {
    const std::vector<uint8_t> want_second = Encode(second, level);

    gcomp_options_t * opts = Options(level);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "brotli", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);

    std::vector<uint8_t> room(32768);
    for (int pass = 0; pass < 3; pass++) {
      const std::vector<uint8_t> & src = pass == 0 ? first : second;
      std::vector<uint8_t> got;
      gcomp_buffer_t in = {(void *)src.data(), src.size(), 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {room.data(), room.size(), 0};
        ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
        got.insert(got.end(), room.data(), room.data() + ob.used);
      }
      for (;;) {
        gcomp_buffer_t ob = {room.data(), room.size(), 0};
        const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
        got.insert(got.end(), room.data(), room.data() + ob.used);
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT);
      }

      std::vector<uint8_t> back;
      ASSERT_EQ(Decode(got, src.size() + 64, &back), GCOMP_OK)
          << "level " << level << " pass " << pass;
      EXPECT_EQ(back, src) << "level " << level << " pass " << pass;
      if (pass != 0) {
        // Byte-for-byte what a brand new encoder writes for this input.
        EXPECT_EQ(got, want_second)
            << "level " << level << " pass " << pass
            << ": reset left state behind";
      }
      ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
      EXPECT_EQ(gcomp_encoder_get_error_detail(enc)[0], '\0')
          << "reset must clear the last error";
    }
    gcomp_encoder_destroy(enc);
  }
}

/**
 * @brief Reset mid-stream throws away what was held, it does not flush it.
 *
 * The bytes offered before the reset are not in the next stream, and the
 * next stream is a whole stream - with its own window byte and terminator -
 * rather than a continuation.
 */
TEST_F(BrotliEncoderTest, ResetMidStreamDiscardsWhatWasHeld) {
  const std::vector<uint8_t> abandoned(5000, 'a');
  const std::vector<uint8_t> kept(5000, 'b');

  for (int level : {0, 1}) {
    gcomp_options_t * opts = Options(level);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "brotli", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);

    std::vector<uint8_t> room(32768);
    gcomp_buffer_t in = {(void *)abandoned.data(), abandoned.size(), 0};
    gcomp_buffer_t ob = {room.data(), room.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
    ASSERT_EQ(in.used, abandoned.size());
    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

    std::vector<uint8_t> got;
    gcomp_buffer_t in2 = {(void *)kept.data(), kept.size(), 0};
    while (in2.used < in2.size) {
      gcomp_buffer_t ob2 = {room.data(), room.size(), 0};
      ASSERT_EQ(gcomp_encoder_update(enc, &in2, &ob2), GCOMP_OK);
      got.insert(got.end(), room.data(), room.data() + ob2.used);
    }
    for (;;) {
      gcomp_buffer_t ob2 = {room.data(), room.size(), 0};
      const gcomp_status_t st = gcomp_encoder_finish(enc, &ob2);
      got.insert(got.end(), room.data(), room.data() + ob2.used);
      if (st == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(st, GCOMP_ERR_LIMIT);
    }
    gcomp_encoder_destroy(enc);

    std::vector<uint8_t> back;
    ASSERT_EQ(Decode(got, kept.size() + abandoned.size() + 64, &back), GCOMP_OK)
        << "level " << level;
    EXPECT_EQ(back, kept) << "level " << level;
    EXPECT_EQ(got, Encode(kept, level)) << "level " << level;
    ExpectLibbrotliReads(got, kept);
  }
}

//
// Output back-pressure
//

/**
 * @brief Every byte comes out through an output window of a few bytes.
 *
 * This is the whole `GCOMP_ERR_LIMIT` protocol on the encode side, and no
 * brotli test had run it: the encoder holds a block, writes it into an
 * internal queue, and drains that queue into whatever room the caller
 * offers. With a large output buffer the queue always empties in one go, so
 * the partial-drain arm of update(), the early break in its block loop and
 * the LIMIT return from finish() and flush() never ran.
 *
 * The input is larger than one block at both levels (level 0 stores 64 KiB
 * blocks, level 1 holds 256 KiB), so the loop runs more than once.
 */
TEST_F(BrotliEncoderTest, EveryByteComesOutThroughASmallWindow) {
  std::vector<uint8_t> data(300000);
  unsigned lcg = 0x1234567u;
  for (size_t i = 0; i < data.size(); i++) {
    // Compressible, but not so uniform that the whole thing is one match.
    lcg = lcg * 1103515245u + 12345u;
    data[i] = (uint8_t)(i % 97 == 0 ? (lcg >> 16) : ('a' + (i % 19)));
  }

  struct Shape {
    size_t in_chunk;
    size_t out_room;
  };
  static const Shape shapes[] = {
      {1, 1},        // one byte each way
      {data.size(), 1},  // whole input, one byte of room
      {7, 3},
      {1, 65536},    // one byte in, plenty of room
      {data.size(), 64},
  };

  for (int level : {0, 1}) {
    for (const Shape & s : shapes) {
      const std::vector<uint8_t> stream =
          EncodeStreaming(data, level, s.in_chunk, s.out_room);
      ASSERT_FALSE(stream.empty())
          << "level " << level << " in=" << s.in_chunk
          << " out=" << s.out_room;
      std::vector<uint8_t> back;
      ASSERT_EQ(Decode(stream, data.size() + 64, &back), GCOMP_OK)
          << "level " << level << " in=" << s.in_chunk
          << " out=" << s.out_room;
      EXPECT_EQ(back, data) << "level " << level << " in=" << s.in_chunk
                            << " out=" << s.out_room;
      ExpectLibbrotliReads(stream, data);
    }
  }
}

/**
 * @brief A flush with no room asks for more, and loses nothing.
 *
 * stream.h: flush is checked before anything is written, "so a caller that
 * gets this back knows its output buffer is untouched and the encoder is
 * where it left it". GCOMP_ERR_LIMIT from flush means drain and call again,
 * the same as everywhere else in this library.
 */
TEST_F(BrotliEncoderTest, AFlushWithNoRoomAsksForMore) {
  const std::vector<uint8_t> data(20000, 'k');
  for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
    gcomp_options_t * opts = Options(1);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "brotli", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);

    std::vector<uint8_t> stream;
    uint8_t one = 0;
    gcomp_buffer_t in = {(void *)data.data(), data.size(), 0};
    gcomp_buffer_t big = {&one, 0, 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &in, &big), GCOMP_OK);
    ASSERT_EQ(in.used, data.size());

    // Zero room: nothing is written and the status says so.
    gcomp_buffer_t none = {&one, 0, 0};
    gcomp_status_t st = gcomp_encoder_flush(enc, &none, mode);
    EXPECT_EQ(st, GCOMP_ERR_LIMIT) << mode;
    EXPECT_EQ(none.used, 0u) << mode;

    // One byte at a time, until it completes.
    int rounds = 0;
    do {
      gcomp_buffer_t ob = {&one, 1, 0};
      st = gcomp_encoder_flush(enc, &ob, mode);
      if (ob.used != 0) {
        stream.push_back(one);
      }
      ASSERT_LT(++rounds, 100000) << mode;
    } while (st == GCOMP_ERR_LIMIT);
    ASSERT_EQ(st, GCOMP_OK) << mode;

    do {
      gcomp_buffer_t ob = {&one, 1, 0};
      st = gcomp_encoder_finish(enc, &ob);
      if (ob.used != 0) {
        stream.push_back(one);
      }
      ASSERT_LT(++rounds, 100000) << mode;
    } while (st == GCOMP_ERR_LIMIT);
    ASSERT_EQ(st, GCOMP_OK) << mode;
    gcomp_encoder_destroy(enc);

    std::vector<uint8_t> back;
    ASSERT_EQ(Decode(stream, data.size() + 64, &back), GCOMP_OK) << mode;
    EXPECT_EQ(back, data) << mode;
    ExpectLibbrotliReads(stream, data);
  }
}

//
// The prefix-code shapes a frequency distribution selects
//

namespace {

/**
 * @brief A block whose literal frequencies are the first @p nsym Fibonacci
 * numbers.
 *
 * Fibonacci weights are the worst case for Huffman code depth: with n
 * symbols weighted F(1)..F(n) the tree is a chain and the longest code is
 * n-1 bits. That is the one property of an input that selects between
 * `assign_huffman`'s two outcomes - at 16 symbols the deepest code is 15
 * bits, which is the format's limit and uses every length from 1 to 15, and
 * at 17 it is 16 bits, which `kraft_complete` rejects and the balanced
 * fallback replaces wholesale. Neither depends on how much data there is, so
 * no amount of ordinary text reaches either.
 *
 * The chosen byte values leave a gap of one and a long gap, so the
 * code-length code has to spell both a single zero (symbol 0) and a run of
 * zeros (symbol 17) alongside the lengths themselves. That is what pushes
 * the code-length alphabet to 17 live symbols, where the static code the
 * header is written in needs its five-bit form.
 */
std::vector<uint8_t> FibonacciAlphabet(int nsym, unsigned seed) {
  std::vector<uint8_t> picks;
  for (int i = 0; i < nsym; i++) {
    // 0..nsym-2 contiguous, then a one-byte gap, then one more: a gap of
    // exactly one, with everything above it absent.
    picks.push_back((uint8_t)(i < nsym - 1 ? i : i + 1));
  }

  std::vector<uint8_t> out;
  uint64_t a = 1;
  uint64_t b = 1;
  for (int i = 0; i < nsym; i++) {
    for (uint64_t k = 0; k < a; k++) {
      out.push_back(picks[(size_t)i]);
    }
    const uint64_t next = a + b;
    a = b;
    b = next;
  }

  // Shuffled deterministically, so the bytes do not form long matches that
  // would turn literals into copies and flatten the histogram.
  unsigned lcg = seed;
  for (size_t i = out.size(); i > 1; i--) {
    lcg = lcg * 1103515245u + 12345u;
    const size_t j = (size_t)(lcg >> 8) % i;
    std::swap(out[i - 1], out[j]);
  }
  return out;
}

} // namespace

/**
 * @brief Both outcomes of the Huffman depth limit produce a readable stream.
 *
 * RFC 7932 section 3.5 caps a prefix code at 15 bits. `assign_huffman`
 * builds the real Huffman code and then checks it: if the deepest code is
 * longer than 15 bits, or the lengths do not satisfy the Kraft equality, it
 * throws the whole assignment away and assigns balanced lengths instead.
 * That fallback had never run. A code that violated the cap would be
 * refused by any decoder, so what is asserted is that every one of these
 * round-trips through this library and through libbrotli.
 *
 * The sweep runs each symbol count at both levels and at two shuffles. Level
 * 0 stores the block and is the control: it must produce the same bytes back
 * without going near a prefix code at all.
 */
TEST_F(BrotliEncoderTest, SkewedLiteralAlphabetsRoundTrip) {
  for (int nsym = 2; nsym <= 22; nsym++) {
    for (unsigned seed : {0x5EED5EEDu, 0xC0FFEEu}) {
      const std::vector<uint8_t> data = FibonacciAlphabet(nsym, seed);
      ASSERT_FALSE(data.empty()) << nsym;
      for (int level : {0, 1}) {
        const std::vector<uint8_t> stream = Encode(data, level);
        ASSERT_FALSE(stream.empty()) << "nsym=" << nsym << " level=" << level;
        std::vector<uint8_t> back;
        ASSERT_EQ(Decode(stream, data.size() + 64, &back), GCOMP_OK)
            << "nsym=" << nsym << " level=" << level
            << " bytes=" << data.size();
        EXPECT_EQ(back, data) << "nsym=" << nsym << " level=" << level;
        ExpectLibbrotliReads(stream, data);
      }
    }
  }
}

/**
 * @brief A skewed alphabet repeated until the block splits, at every window.
 *
 * The same distributions as above, but long enough to fill more than one
 * level-1 block, so the deep-code and fallback paths run on a block that is
 * not the first and with a distance ring buffer carried in from the previous
 * one.
 */
TEST_F(BrotliEncoderTest, SkewedAlphabetsAcrossBlockBoundaries) {
  for (int nsym : {14, 16, 17, 18}) {
    std::vector<uint8_t> data;
    unsigned seed = 0x13579BDFu;
    while (data.size() < 300000) {
      const std::vector<uint8_t> part = FibonacciAlphabet(nsym, seed);
      data.insert(data.end(), part.begin(), part.end());
      seed = seed * 69069u + 1u;
    }
    for (int lgwin : {10, 16, 24}) {
      const std::vector<uint8_t> stream = Encode(data, 1, lgwin);
      ASSERT_FALSE(stream.empty()) << "nsym=" << nsym << " lgwin=" << lgwin;
      std::vector<uint8_t> back;
      ASSERT_EQ(Decode(stream, data.size() + 64, &back), GCOMP_OK)
          << "nsym=" << nsym << " lgwin=" << lgwin;
      EXPECT_EQ(back, data) << "nsym=" << nsym << " lgwin=" << lgwin;
      ExpectLibbrotliReads(stream, data);
    }
  }
}

//
// The chunk writer's own contract
//

/**
 * @brief A meta-block long enough to need six MLEN nibbles.
 *
 * `write_mlen` picks four, five or six nibbles from the length, and the
 * framing layer never asks for six: `brotli_compress_chunk` is called with
 * at most BROTLI_BLOCK (256 KiB) bytes, and six nibbles start at 1048577.
 * The arm is correct and unreachable through the public API, which is the
 * shape that quietly stops being correct - so this calls the chunk writer
 * directly, at the length its own contract allows rather than the length the
 * encoder's block policy chooses.
 *
 * `brotli_compress_chunk` writes a compressed meta-block followed by an
 * empty metadata block, which leaves the stream byte-aligned; prefixing the
 * window byte for lgwin 16 and appending the 0x03 terminator makes it a
 * whole stream, which is how both decoders here can be asked about it.
 */
TEST_F(BrotliEncoderTest, AMetaBlockLongEnoughForSixNibbles) {
  // Just over 2^20 bytes, so the length needs a sixth nibble, and periodic
  // so that it compresses rather than falling back to a stored block.
  const size_t len = (1u << 20) + 12345u;
  std::vector<uint8_t> data(len);
  for (size_t i = 0; i < len; i++) {
    data[i] = (uint8_t)("meta-block lengths are nibble counted. "[i % 39]);
  }

  std::vector<uint8_t> scratch(len + 65536);
  size_t written = 0;
  uint32_t rb[4] = {4, 11, 15, 16};
  int fresh = 0;
  const int r = brotli_compress_chunk(nullptr, scratch.data(), scratch.size(),
      &written, data.data(), data.size(), (1u << 24) - 16u, rb, &fresh);
  ASSERT_EQ(r, 0) << "a periodic megabyte should compress, not store";
  ASSERT_GT(written, 0u);
  ASSERT_LT(written, len);

  std::vector<uint8_t> stream;
  stream.push_back(0x0c); // lgwin 16, then the empty metadata block
  stream.insert(stream.end(), scratch.data(), scratch.data() + written);
  stream.push_back(0x03); // the empty last meta-block

  std::vector<uint8_t> back;
  ASSERT_EQ(Decode(stream, len + 64, &back), GCOMP_OK);
  EXPECT_EQ(back, data);
  ExpectLibbrotliReads(stream, data);
}

/**
 * @brief The chunk writer refuses a length the format cannot express.
 *
 * MLEN tops out at 16777216 (RFC 7932 section 9.2). Asking for more is a
 * caller's mistake, and the answer is the store fallback rather than a
 * meta-block claiming a length it cannot write.
 */
TEST_F(BrotliEncoderTest, TheChunkWriterDeclinesWhatItCannotFrame) {
  std::vector<uint8_t> scratch(4096);
  size_t written = 123;
  uint32_t rb[4] = {4, 11, 15, 16};
  int fresh = 0;
  const std::vector<uint8_t> data(64, 'x');

  // No room at all: the compressed form cannot be written, so the caller is
  // told to store the block instead.
  EXPECT_EQ(brotli_compress_chunk(nullptr, scratch.data(), 0, &written,
                data.data(), data.size(), 65520u, rb, &fresh),
      1);
  // An empty block is not a block.
  EXPECT_NE(brotli_compress_chunk(nullptr, scratch.data(), scratch.size(),
                &written, data.data(), 0, 65520u, rb, &fresh),
      0);
  // The ring buffer is untouched whenever the answer is not zero.
  EXPECT_EQ(rb[0], 4u);
  EXPECT_EQ(rb[1], 11u);
  EXPECT_EQ(rb[2], 15u);
  EXPECT_EQ(rb[3], 16u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
