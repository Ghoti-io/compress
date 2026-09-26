/**
 * @file test_zstd_reset.cpp
 *
 * Tests for zstd encoder and decoder reset functionality.
 *
 * Verifies:
 * - Reset clears all state correctly
 * - Buffers are retained (not reallocated) after reset
 * - Multiple streams can be processed without reallocation
 * - Reset after error recovers correctly
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdResetTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  /// Drive update() until the whole chunk is taken, collecting output.
  void PushAll(gcomp_encoder_t * enc, const uint8_t * data, size_t len,
      std::vector<uint8_t> & out) {
    uint8_t win[8192];
    gcomp_buffer_t in = {const_cast<uint8_t *>(data), len, 0};
    while (in.used < in.size) {
      gcomp_buffer_t ob = {win, sizeof(win), 0};
      const size_t before = in.used;
      ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
      out.insert(out.end(), win, win + ob.used);
      ASSERT_FALSE(in.used == before && ob.used == 0) << "no progress";
    }
  }

  void FlushAll(gcomp_encoder_t * enc, gcomp_flush_t mode,
      std::vector<uint8_t> & out) {
    uint8_t win[8192];
    for (;;) {
      gcomp_buffer_t ob = {win, sizeof(win), 0};
      const gcomp_status_t st = gcomp_encoder_flush(enc, &ob, mode);
      out.insert(out.end(), win, win + ob.used);
      if (st == GCOMP_OK) {
        return;
      }
      ASSERT_EQ(st, GCOMP_ERR_LIMIT);
    }
  }

  void FinishAll(gcomp_encoder_t * enc, std::vector<uint8_t> & out) {
    uint8_t win[8192];
    for (;;) {
      gcomp_buffer_t ob = {win, sizeof(win), 0};
      const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
      out.insert(out.end(), win, win + ob.used);
      if (st == GCOMP_OK) {
        return;
      }
      ASSERT_EQ(st, GCOMP_ERR_LIMIT);
    }
  }

  /// Decode a whole stream, allowing concatenated frames.
  std::vector<uint8_t> Decode(
      const std::vector<uint8_t> & stream, size_t expected) {
    gcomp_options_t * dopts = nullptr;
    EXPECT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
    gcomp_decoder_t * dec = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "zstd", dopts, &dec), GCOMP_OK);
    gcomp_options_destroy(dopts);
    if (!dec) {
      return {};
    }
    std::vector<uint8_t> out(expected + 65536);
    gcomp_buffer_t in = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
    out.resize(ob.used);
    gcomp_decoder_destroy(dec);
    return out;
  }

  /// Decode a deliberately unfinished stream: update() only, never finish().
  /// A SYNC flush leaves the frame open, so finish() would rightly call it
  /// truncated - the flush promise is about what update() can already produce.
  size_t DecodePrefixSize(
      const std::vector<uint8_t> & stream, size_t expected) {
    gcomp_options_t * dopts = nullptr;
    EXPECT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
    gcomp_decoder_t * dec = nullptr;
    EXPECT_EQ(gcomp_decoder_create(registry_, "zstd", dopts, &dec), GCOMP_OK);
    gcomp_options_destroy(dopts);
    if (!dec) {
      return 0;
    }
    std::vector<uint8_t> out(expected + 65536);
    gcomp_buffer_t in = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);
    const size_t got = ob.used;
    gcomp_decoder_destroy(dec);
    return got;
  }

  /// Compressible-but-not-trivial bytes, so a stale history is visible as a
  /// wrong decode rather than absorbed by a stored block.
  static std::vector<uint8_t> Prose(size_t n, unsigned seed) {
    static const char * words[] = {"alpha ", "beta ", "gamma ", "delta ",
        "epsilon ", "zeta ", "eta ", "theta "};
    std::vector<uint8_t> v(n);
    unsigned st = seed;
    size_t pos = 0;
    while (pos < n) {
      st = st * 1103515245u + 12345u;
      const char * w = words[(st >> 16) % 8];
      size_t l = strlen(w);
      if (pos + l > n) {
        l = n - pos;
      }
      memcpy(v.data() + pos, w, l);
      pos += l;
    }
    return v;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Parallel encoder reset
//
// Until 2026-09-26 the whole of this was one test, `ParallelEncodeReset` in
// test_zstd_encoder.cpp: 21 bytes in, one update, one finish, threads=2. That
// is how `a726438` hid - a reset that left the held job's overlap in place, so
// the first block of the new stream matched into the previous one and decoded
// to nothing. It was invisible until short blocks with history started being
// compressed at all, because every input below MIN_COMPRESSION_SIZE used to be
// stored raw.
//
// What each of these varies is the thing that test could not: enough data to
// cross a job boundary, a reset while work is still in flight, a reset after a
// flush has staged a new frame header, and the option that adds a second kind
// of history (zstd.long). The previous stream is deliberately *similar* to the
// next one in every case, because identical-looking data is what makes a stale
// history compress well and decode wrong.
//

namespace {

/**
 * Destroys the encoder however the test leaves.
 *
 * Not tidiness. A gtest ASSERT returns from the test body, so a plain
 * `gcomp_encoder_destroy()` at the end of the function is skipped on failure -
 * and a leaked parallel encoder leaves its pool workers parked on a semaphore,
 * which makes cutil's thread destructor block in pthread_join at process exit.
 * The suite then hangs *after* printing its failures, and with stdout
 * block-buffered to a file it looks like a hang with no output at all rather
 * than a test that failed. Found exactly that way.
 */
class EncGuard {
public:
  explicit EncGuard(gcomp_encoder_t * e) : e_(e) {}
  ~EncGuard() {
    if (e_) {
      gcomp_encoder_destroy(e_);
    }
  }
  EncGuard(const EncGuard &) = delete;
  EncGuard & operator=(const EncGuard &) = delete;

private:
  gcomp_encoder_t * e_;
};

/// Build a parallel encoder, optionally with long-distance matching.
gcomp_encoder_t * MakeParallel(gcomp_registry_t * registry, uint64_t threads,
    bool longmode, bool checksum) {
  gcomp_options_t * o = nullptr;
  if (gcomp_options_create(&o) != GCOMP_OK) {
    return nullptr;
  }
  if (gcomp_options_set_uint64(o, "threads.count", threads) != GCOMP_OK) {
    gcomp_options_destroy(o);
    return nullptr;
  }
  if (longmode &&
      gcomp_options_set_bool(o, "zstd.long", 1) != GCOMP_OK) {
    gcomp_options_destroy(o);
    return nullptr;
  }
  if (checksum &&
      gcomp_options_set_bool(o, "zstd.checksum", 1) != GCOMP_OK) {
    gcomp_options_destroy(o);
    return nullptr;
  }
  gcomp_encoder_t * enc = nullptr;
  const gcomp_status_t st = gcomp_encoder_create(registry, "zstd", o, &enc);
  gcomp_options_destroy(o);
  return (st == GCOMP_OK) ? enc : nullptr;
}

} // namespace

/**
 * Two full streams through one parallel encoder, each big enough to be several
 * jobs, and the second stream's bytes chosen to look like the first's.
 *
 * The 21-byte test cannot reach this: one job, one block, and a payload short
 * enough that a stale window has almost nothing to offer.
 */
TEST_F(ZstdResetTest, ParallelResetAcrossJobBoundaries) {
  // Comfortably several default 512 KB jobs.
  const std::vector<uint8_t> s1 = Prose(1200000, 11u);
  const std::vector<uint8_t> s2 = Prose(1200000, 12u);

  for (uint64_t threads : {(uint64_t)2, (uint64_t)4}) {
    gcomp_encoder_t * enc = MakeParallel(registry_, threads, false, false);
    EncGuard guard(enc);
    ASSERT_NE(enc, nullptr) << "threads=" << threads;

    std::vector<uint8_t> out1;
    PushAll(enc, s1.data(), s1.size(), out1);
    FinishAll(enc, out1);
    EXPECT_EQ(Decode(out1, s1.size()), s1) << "threads=" << threads;

    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK) << "threads=" << threads;

    std::vector<uint8_t> out2;
    PushAll(enc, s2.data(), s2.size(), out2);
    FinishAll(enc, out2);
    EXPECT_EQ(Decode(out2, s2.size()), s2)
        << "threads=" << threads
        << ": the second stream did not decode to itself, which is what a "
           "history carried across the reset looks like";

  }
}

/**
 * Reset part-way through a stream, with jobs submitted and results very likely
 * still in flight. The abandoned stream's output is thrown away, which is what
 * a caller doing this means; what must not survive is any of its state.
 */
TEST_F(ZstdResetTest, ParallelResetMidStreamAbandonsWorkInFlight) {
  const std::vector<uint8_t> partial = Prose(900000, 21u);
  const std::vector<uint8_t> whole = Prose(900000, 22u);

  for (uint64_t threads : {(uint64_t)2, (uint64_t)4}) {
    gcomp_encoder_t * enc = MakeParallel(registry_, threads, false, false);
    EncGuard guard(enc);
    ASSERT_NE(enc, nullptr) << "threads=" << threads;

    // Hand over most of a stream and never finish it.
    std::vector<uint8_t> discarded;
    PushAll(enc, partial.data(), partial.size(), discarded);

    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK) << "threads=" << threads;

    std::vector<uint8_t> out;
    PushAll(enc, whole.data(), whole.size(), out);
    FinishAll(enc, out);
    EXPECT_EQ(Decode(out, whole.size()), whole)
        << "threads=" << threads
        << ": a stream begun after an abandoned one did not decode to itself";

  }
}

/**
 * Reset immediately after a full flush, which is the one case where the reset
 * finds a frame header already staged in `parallel_output_buf` by the flush.
 * Writing the new stream's header without clearing that would emit two.
 */
TEST_F(ZstdResetTest, ParallelResetAfterAFullFlush) {
  const std::vector<uint8_t> s1 = Prose(700000, 31u);
  const std::vector<uint8_t> s2 = Prose(700000, 32u);

  for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
    gcomp_encoder_t * enc = MakeParallel(registry_, 4, false, false);
    EncGuard guard(enc);
    ASSERT_NE(enc, nullptr);

    std::vector<uint8_t> out1;
    PushAll(enc, s1.data(), s1.size() / 2, out1);
    FlushAll(enc, mode, out1);
    // Everything consumed so far must already be readable - that is the flush
    // contract, and it has to hold on the stream we are about to abandon.
    // update() only: a SYNC flush leaves the frame open, so asking finish()
    // about it reports GCOMP_ERR_CORRUPT correctly, which is what the first
    // version of this test got wrong.
    EXPECT_EQ(DecodePrefixSize(out1, s1.size() / 2), s1.size() / 2)
        << "mode=" << (int)mode;

    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK) << "mode=" << (int)mode;

    std::vector<uint8_t> out2;
    PushAll(enc, s2.data(), s2.size(), out2);
    FinishAll(enc, out2);
    EXPECT_EQ(Decode(out2, s2.size()), s2)
        << "mode=" << (int)mode
        << ": the stream after a flush-then-reset did not decode to itself";

  }
}

/**
 * The same, with long-distance matching on, because `zstd.long` adds a second
 * kind of history: the LDM table holds absolute positions and its scan keeps a
 * cursor. Both are per-job state in a pool the reset clears through
 * `zstd_parallel_reset()`, and a carried cursor is what section 8 of the notes
 * records going wrong once already.
 *
 * The payload repeats a block far enough back that only a long match can reach
 * it, so the option is doing something rather than merely being set.
 */
TEST_F(ZstdResetTest, ParallelResetWithLongDistanceMatching) {
  // A shape a long match can find: a distinctive run, a megabyte of filler,
  // then the run again.
  auto build = [](unsigned seed) {
    std::vector<uint8_t> run = Prose(80000, seed);
    std::vector<uint8_t> filler = Prose(1400000, seed + 500u);
    std::vector<uint8_t> v(run);
    v.insert(v.end(), filler.begin(), filler.end());
    v.insert(v.end(), run.begin(), run.end());
    return v;
  };
  const std::vector<uint8_t> s1 = build(41u);
  const std::vector<uint8_t> s2 = build(42u);

  gcomp_encoder_t * enc = MakeParallel(registry_, 4, true, true);
  EncGuard guard(enc);
  ASSERT_NE(enc, nullptr) << "zstd.long with threads should be supported";

  std::vector<uint8_t> out1;
  PushAll(enc, s1.data(), s1.size(), out1);
  FinishAll(enc, out1);
  EXPECT_EQ(Decode(out1, s1.size()), s1);

  ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  std::vector<uint8_t> out2;
  PushAll(enc, s2.data(), s2.size(), out2);
  FinishAll(enc, out2);
  EXPECT_EQ(Decode(out2, s2.size()), s2)
      << "the second long-mode stream did not decode to itself";

  // And the checksum is per stream: a hash carried across the reset would make
  // the second frame's trailer describe the first frame's content, which the
  // decode above validates because zstd.checksum is on.
}

/**
 * Many short streams through one parallel encoder. Each is below the job size,
 * so every one exercises the held-job path that `a726438` fixed, and doing it
 * repeatedly is what would expose state that accumulates rather than state
 * that is merely stale.
 */
TEST_F(ZstdResetTest, ParallelManyResetsWithShortStreams) {
  gcomp_encoder_t * enc = MakeParallel(registry_, 4, false, true);
  EncGuard guard(enc);
  ASSERT_NE(enc, nullptr);

  for (unsigned i = 0; i < 12; i++) {
    const std::vector<uint8_t> data = Prose(3000 + i * 250, 100u + i);
    std::vector<uint8_t> out;
    PushAll(enc, data.data(), data.size(), out);
    FinishAll(enc, out);
    EXPECT_EQ(Decode(out, data.size()), data) << "stream " << i;
    ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK) << "stream " << i;
  }
}

//
// Encoder Reset Tests
//

TEST_F(ZstdResetTest, EncoderResetAllowsReuse) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // First stream
  const char d1[] = "First stream data";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);
  size_t out1_size = b1.used;
  EXPECT_GT(out1_size, 0u);

  // Reset
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream
  const char d2[] = "Second stream different";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);
  size_t out2_size = b2.used;
  EXPECT_GT(out2_size, 0u);

  // Verify second stream is valid by decoding
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(256);
  gcomp_buffer_t din = {o2.data(), out2_size, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(decoded.data(), d2, strlen(d2)), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, EncoderResetClearsStateCompletely) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // First stream with partial data (no finish)
  std::vector<uint8_t> data1(1000, 'A');
  std::vector<uint8_t> out1(2048);
  gcomp_buffer_t in1 = {data1.data(), data1.size(), 0};
  gcomp_buffer_t ob1 = {out1.data(), out1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in1, &ob1), GCOMP_OK);
  // Don't call finish - simulate interrupted stream

  // Reset should clear partial state
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream should work independently
  std::vector<uint8_t> data2(500, 'B');
  std::vector<uint8_t> out2(1024);
  gcomp_buffer_t in2 = {data2.data(), data2.size(), 0};
  gcomp_buffer_t ob2 = {out2.data(), out2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in2, &ob2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob2), GCOMP_OK);
  EXPECT_GT(ob2.used, 0u);

  // Verify second stream decodes correctly
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(1024);
  gcomp_buffer_t din = {out2.data(), ob2.used, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(dob.used, 500u);
  EXPECT_EQ(memcmp(decoded.data(), data2.data(), 500), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, EncoderResetWithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  // First stream
  const char d1[] = "First with checksum";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);

  // Reset
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Second stream - checksum should be recomputed fresh
  const char d2[] = "Second with fresh checksum";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);

  // Verify second stream decodes with checksum validation
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> decoded(256);
  gcomp_buffer_t din = {o2.data(), b2.used, 0};
  gcomp_buffer_t dob = {decoded.data(), decoded.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(decoded.data(), d2, strlen(d2)), 0);
  gcomp_decoder_destroy(dec);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdResetTest, EncoderMultipleResets) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Process 5 streams with resets between each
  for (int i = 0; i < 5; i++) {
    std::string data = "Stream number " + std::to_string(i);
    std::vector<uint8_t> out(256);
    gcomp_buffer_t in = {(void *)data.data(), data.size(), 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};

    EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
    EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    EXPECT_GT(ob.used, 0u) << "Stream " << i << " should produce output";

    if (i < 4) {
      EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
    }
  }

  gcomp_encoder_destroy(enc);
}

//
// Decoder Reset Tests
//

TEST_F(ZstdResetTest, DecoderResetAllowsReuse) {
  // Encode two different streams
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char d1[] = "First decoder test";
  std::vector<uint8_t> c1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t o1 = {c1.data(), c1.size(), 0};
  gcomp_encoder_update(enc, &i1, &o1);
  gcomp_encoder_finish(enc, &o1);
  size_t c1_size = o1.used;

  gcomp_encoder_reset(enc);

  const char d2[] = "Second decoder test";
  std::vector<uint8_t> c2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t o2 = {c2.data(), c2.size(), 0};
  gcomp_encoder_update(enc, &i2, &o2);
  gcomp_encoder_finish(enc, &o2);
  size_t c2_size = o2.used;

  gcomp_encoder_destroy(enc);

  // Decode both streams with reset
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // First stream
  std::vector<uint8_t> dec1(256);
  gcomp_buffer_t din1 = {c1.data(), c1_size, 0};
  gcomp_buffer_t dob1 = {dec1.data(), dec1.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din1, &dob1), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob1), GCOMP_OK);
  EXPECT_EQ(memcmp(dec1.data(), d1, strlen(d1)), 0);

  // Reset
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Second stream
  std::vector<uint8_t> dec2(256);
  gcomp_buffer_t din2 = {c2.data(), c2_size, 0};
  gcomp_buffer_t dob2 = {dec2.data(), dec2.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(memcmp(dec2.data(), d2, strlen(d2)), 0);

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderResetClearsRepeatOffsets) {
  // Encode data that uses repeat offsets
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Pattern that will create repeat offsets
  std::string pattern = "ABCDABCDABCDABCD";
  std::vector<uint8_t> data;
  for (int i = 0; i < 50; i++) {
    data.insert(data.end(), pattern.begin(), pattern.end());
  }

  std::vector<uint8_t> compressed(data.size() + 1024);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode twice with reset
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  for (int round = 0; round < 2; round++) {
    std::vector<uint8_t> output(data.size() + 256);
    gcomp_buffer_t din = {compressed.data(), comp_size, 0};
    gcomp_buffer_t dob = {output.data(), output.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
    EXPECT_EQ(dob.used, data.size());
    EXPECT_EQ(memcmp(output.data(), data.data(), data.size()), 0)
        << "Round " << round << " should decode correctly";

    if (round < 1) {
      EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
    }
  }

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderResetClearsWindowBuffer) {
  // Encode data larger than typical small test
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  std::vector<uint8_t> data(10000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i % 256);
  }

  std::vector<uint8_t> compressed(data.size() + 1024);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode, reset, decode again
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // First decode
  std::vector<uint8_t> output1(data.size() + 256);
  gcomp_buffer_t din1 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob1 = {output1.data(), output1.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din1, &dob1), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob1), GCOMP_OK);
  EXPECT_EQ(dob1.used, data.size());

  // Reset
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Second decode should not be affected by first decode's window content
  std::vector<uint8_t> output2(data.size() + 256);
  gcomp_buffer_t din2 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob2 = {output2.data(), output2.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(dob2.used, data.size());
  EXPECT_EQ(memcmp(output2.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdResetTest, DecoderMultipleResets) {
  // Encode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char test_data[] = "Multiple reset test data";
  std::vector<uint8_t> compressed(256);
  gcomp_buffer_t in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Decode 5 times with resets
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  for (int i = 0; i < 5; i++) {
    std::vector<uint8_t> output(256);
    gcomp_buffer_t din = {compressed.data(), comp_size, 0};
    gcomp_buffer_t dob = {output.data(), output.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
    EXPECT_EQ(memcmp(output.data(), test_data, strlen(test_data)), 0)
        << "Decode " << i << " should match original";

    if (i < 4) {
      EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
    }
  }

  gcomp_decoder_destroy(dec);
}

//
// Reset After Partial Operations
//

TEST_F(ZstdResetTest, EncoderResetAfterPartialUpdate) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Partial update with small output buffer (forces buffering)
  std::vector<uint8_t> data(5000, 'X');
  std::vector<uint8_t> out(100); // Intentionally small
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  // Don't call finish

  // Reset should clear all buffered state
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // New stream should work
  const char new_data[] = "Fresh start";
  std::vector<uint8_t> new_out(256);
  gcomp_buffer_t new_in = {(void *)new_data, strlen(new_data), 0};
  gcomp_buffer_t new_ob = {new_out.data(), new_out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &new_in, &new_ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &new_ob), GCOMP_OK);
  EXPECT_GT(new_ob.used, 0u);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, DecoderResetAfterPartialDecode) {
  // Encode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  std::vector<uint8_t> data(1000, 'Y');
  std::vector<uint8_t> compressed(2048);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  // Start decoding with small output buffer
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  std::vector<uint8_t> output(100); // Intentionally small
  gcomp_buffer_t din = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob = {output.data(), output.size(), 0};
  gcomp_decoder_update(dec, &din, &dob);
  // Don't call finish

  // Reset should clear partial decode state
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // New decode should work from fresh state
  std::vector<uint8_t> full_output(2048);
  gcomp_buffer_t din2 = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob2 = {full_output.data(), full_output.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din2, &dob2), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob2), GCOMP_OK);
  EXPECT_EQ(dob2.used, 1000u);

  gcomp_decoder_destroy(dec);
}

//
// Reset Immediately After Create
//

TEST_F(ZstdResetTest, EncoderResetImmediatelyAfterCreate) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Reset right away (should be no-op but valid)
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);

  // Should still work
  const char data[] = "After immediate reset";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdResetTest, DecoderResetImmediatelyAfterCreate) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);

  // Reset right away (should be no-op but valid)
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);

  // Should still work - encode and decode test data
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  const char data[] = "Decode after immediate reset";
  std::vector<uint8_t> compressed(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_finish(enc, &ob);
  size_t comp_size = ob.used;
  gcomp_encoder_destroy(enc);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t din = {compressed.data(), comp_size, 0};
  gcomp_buffer_t dob = {output.data(), output.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &din, &dob), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dob), GCOMP_OK);
  EXPECT_EQ(memcmp(output.data(), data, strlen(data)), 0);

  gcomp_decoder_destroy(dec);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
