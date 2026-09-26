/**
 * @file test_zstd_window.cpp
 *
 * The match window buffer: how history leaves it, and what that costs.
 *
 * The encoder keeps one window of history followed by the block it is
 * compressing, and history that falls out of the window has to go.  Moving the
 * bytes back is the obvious way to drop them and it cost a whole window every
 * block: at window_log 25, 93% of the encode was that one memmove, 24.6 GiB
 * moved to compress 128 MiB.  The buffer is now allocated with room past one
 * window (zstd_window_slack()) so that dropping history advances where the
 * history starts and the bytes move only when the room runs out.
 *
 * Two things therefore need testing, and one of them is not a ratio:
 *
 * - The cost is amortised.  @ref WindowCompactionIsAmortised asserts the
 *   *count*, not a rate: the count is exact and the same on any machine, where
 *   a throughput assertion on a loaded box is a coin toss.
 * - The history is still there and still at the right distance.  The bookkeeping
 *   that moved is what tells the match finder where a match is, so a test that
 *   only round-trips would pass against a window that had quietly stopped
 *   matching.  @ref ACompactedWindowStillMatchesAcrossItsWholeReach puts the
 *   only redundancy in the input beyond what a smaller window can reach, and
 *   carries the smaller window as the control.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include "../../../src/methods/zstd/zstd_internal.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

/// ZSTD_BLOCK_SIZE_MAX: what the old slide moved the window once per.
constexpr size_t kEncoderBlock = 128u * 1024u;

/// Incompressible bytes, deterministically, so a size is comparable run to run.
std::vector<uint8_t> noise(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(seed);
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<uint8_t>(rng() >> 24);
  }
  return v;
}

/**
 * @brief Bytes whose only redundancy sits exactly @p distance back.
 *
 * One incompressible block, then compressible filler, repeated: every copy of
 * the block is `block + filler` bytes behind the last, and nothing else in the
 * input is worth finding.  The output size then says one thing only - how many
 * copies of the block the encoder managed to match - so a window shorter than
 * the distance is usable as a control.
 *
 * The filler is a repeated pattern rather than more noise for the reason
 * test_zstd_ldm.cpp gives: megabytes of noise have to be searched position by
 * position and found wanting, which is seconds per run to test nothing extra.
 */
std::vector<uint8_t> farRepeats(
    size_t block, size_t filler, size_t copies, size_t * distance_out) {
  const std::vector<uint8_t> subject = noise(block, 11u);
  const std::vector<uint8_t> pattern = noise(8u * 1024u, 12u);
  std::vector<uint8_t> out;
  out.reserve((block + filler) * copies);
  for (size_t c = 0; c < copies; c++) {
    out.insert(out.end(), subject.begin(), subject.end());
    const size_t upto = out.size() + filler;
    while (out.size() < upto) {
      size_t take = pattern.size();
      if (out.size() + take > upto) {
        take = upto - out.size();
      }
      out.insert(out.end(), pattern.begin(), pattern.begin() + (long)take);
    }
  }
  if (distance_out) {
    *distance_out = block + filler;
  }
  return out;
}

/// A pattern repeated: compressible, and every match is local, so it exercises
/// the window machinery without the match finder's own cost dominating.
std::vector<uint8_t> repeated(size_t n) {
  const std::vector<uint8_t> pattern = noise(8u * 1024u, 3u);
  std::vector<uint8_t> v;
  v.reserve(n);
  while (v.size() < n) {
    size_t take = pattern.size();
    if (v.size() + take > n) {
      take = n - v.size();
    }
    v.insert(v.end(), pattern.begin(), pattern.begin() + (long)take);
  }
  return v;
}

/// Encode through the streaming API, which is the only way to a handle the
/// diagnostics can be read from.
gcomp_status_t streamEncode(gcomp_registry_t * registry,
    gcomp_options_t * options, const std::vector<uint8_t> & input,
    std::vector<uint8_t> * out, uint64_t * compactions_out) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "zstd", options, &encoder);
  if (s != GCOMP_OK) {
    return s;
  }
  out->assign(input.size() + input.size() / 4u + 64u * 1024u, 0u);
  gcomp_buffer_t in_buf = {(void *)input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {out->data(), out->size(), 0};
  while (in_buf.used < in_buf.size && s == GCOMP_OK) {
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  }
  if (s == GCOMP_OK) {
    s = gcomp_encoder_finish(encoder, &out_buf);
  }
  if (compactions_out) {
    *compactions_out = gcomp_zstd_encoder_window_compactions(encoder);
  }
  out->resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  return s;
}

::testing::AssertionResult decodesTo(gcomp_registry_t * registry,
    const std::vector<uint8_t> & stream, const std::vector<uint8_t> & want) {
  std::vector<uint8_t> back(want.size() + 64u);
  size_t used = 0;
  gcomp_status_t s = gcomp_decode_buffer(registry, "zstd", nullptr,
      stream.data(), stream.size(), back.data(), back.size(), &used);
  if (s != GCOMP_OK) {
    return ::testing::AssertionFailure()
        << "decode: " << gcomp_status_to_string(s);
  }
  if (used != want.size()) {
    return ::testing::AssertionFailure()
        << "decoded " << used << " bytes, wanted " << want.size();
  }
  if (!want.empty() && memcmp(back.data(), want.data(), want.size()) != 0) {
    return ::testing::AssertionFailure() << "decoded bytes differ";
  }
  return ::testing::AssertionSuccess();
}

gcomp_options_t * windowOptions(uint64_t window_log, uint64_t threads) {
  gcomp_options_t * options = nullptr;
  EXPECT_EQ(gcomp_options_create(&options), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(options, "zstd.window_log", window_log),
      GCOMP_OK);
  // The windows here are larger than the default memory ceiling allows.
  EXPECT_EQ(gcomp_options_set_uint64(options, "limits.max_memory_bytes",
                4096ull * 1024u * 1024u),
      GCOMP_OK);
  if (threads > 0) {
    EXPECT_EQ(
        gcomp_options_set_uint64(options, "threads.count", threads), GCOMP_OK);
  }
  return options;
}

class ZstdWindowTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }
  gcomp_registry_t * registry_ = nullptr;
};

/**
 * @brief Dropping history must not cost a window every block.
 *
 * 24 MB through an 8 MB window.  Sixteen megabytes of it arrive after the
 * window is full, so the old slide would have moved the window once per 128 KB
 * block - 128 times.  With a quarter window of slack the bytes move once per
 * 2 MB instead, so eight or so times.
 *
 * The bound is the point of the test and the lower bound is what keeps it
 * honest: a count of zero would also satisfy "not many", and would mean the
 * window never filled and the test measured nothing.
 */
TEST_F(ZstdWindowTest, WindowCompactionIsAmortised) {
  constexpr size_t kWindowLog = 23;                    // 8 MiB
  constexpr size_t kWindow = 1u << kWindowLog;
  constexpr size_t kSlack = kWindow / 4u;              // zstd_window_slack()
  const std::vector<uint8_t> input = repeated(24u * 1024u * 1024u);

  gcomp_options_t * options = windowOptions(kWindowLog, 0);
  std::vector<uint8_t> stream;
  uint64_t compactions = 0;
  ASSERT_EQ(streamEncode(registry_, options, input, &stream, &compactions),
      GCOMP_OK);
  gcomp_options_destroy(options);

  const uint64_t past_the_window = (input.size() - kWindow);
  const uint64_t amortised = past_the_window / kSlack;
  const uint64_t per_block = past_the_window / kEncoderBlock;

  EXPECT_GT(compactions, 0u)
      << "nothing was compacted, so the bound below asserted nothing: either "
         "the window never filled, or history is being dropped by moving the "
         "bytes rather than by moving where they start";
  // Two windows of headroom over the amortised figure, and still an order of
  // magnitude below moving the window every block.
  EXPECT_LE(compactions, amortised * 2u + 4u)
      << "compacted " << compactions << " times for " << past_the_window
      << " bytes past the window; amortised would be about " << amortised
      << " and once per block would be " << per_block;
  EXPECT_TRUE(decodesTo(registry_, stream, input));
}

/**
 * @brief A reset starts the count again, like every other per-stream diagnostic.
 */
TEST_F(ZstdWindowTest, ResetStartsTheCompactionCountAgain) {
  constexpr size_t kWindowLog = 23;
  const std::vector<uint8_t> input = repeated(24u * 1024u * 1024u);

  gcomp_options_t * options = windowOptions(kWindowLog, 0);
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", options, &encoder),
      GCOMP_OK);

  std::vector<uint8_t> out(input.size() + 1024u * 1024u);
  for (int pass = 0; pass < 2; pass++) {
    gcomp_buffer_t in_buf = {(void *)input.data(), input.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    while (in_buf.used < in_buf.size) {
      ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
    }
    ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);
    const uint64_t first = gcomp_zstd_encoder_window_compactions(encoder);
    EXPECT_GT(first, 0u) << "pass " << pass;
    ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
    EXPECT_EQ(gcomp_zstd_encoder_window_compactions(encoder), 0u)
        << "pass " << pass << ": the count survived a reset";
  }

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(options);
}

/**
 * @brief History must still be findable at the far edge of the window.
 *
 * The distance from where the history starts inside the buffer to where a match
 * is is what the match finder is told, and that is exactly the bookkeeping the
 * amortised slide changed.  So the only thing worth finding in this input sits
 * 6 MB back, an 8 MB window is asked to find it four times over 24 MB - eight
 * compactions or so, enough that an error cannot hide in the first one - and a
 * 2 MB window is carried as the control.
 *
 * The control is what makes the size mean something: it cannot reach 6 MB, so
 * it has to keep every copy of the block, and if it did not then the input was
 * never testing reach.
 */
TEST_F(ZstdWindowTest, ACompactedWindowStillMatchesAcrossItsWholeReach) {
  constexpr size_t kSubject = 1024u * 1024u;
  constexpr size_t kFiller = 5u * 1024u * 1024u;
  constexpr size_t kCopies = 4u;
  size_t distance = 0;
  const std::vector<uint8_t> input =
      farRepeats(kSubject, kFiller, kCopies, &distance);
  ASSERT_GT(distance, size_t{1} << 21) << "the control could reach the repeat";
  ASSERT_LT(distance, size_t{1} << 23) << "the window under test cannot reach it";

  std::vector<uint8_t> wide;
  std::vector<uint8_t> narrow;
  uint64_t compactions = 0;

  gcomp_options_t * options = windowOptions(23, 0);
  ASSERT_EQ(streamEncode(registry_, options, input, &wide, &compactions),
      GCOMP_OK);
  gcomp_options_destroy(options);

  options = windowOptions(21, 0);
  ASSERT_EQ(streamEncode(registry_, options, input, &narrow, nullptr),
      GCOMP_OK);
  gcomp_options_destroy(options);

  EXPECT_GT(compactions, 0u)
      << "nothing was compacted, so this input never made the window drop any "
         "history and the reach it proves is not the reach after a compaction";
  // The control: a window that cannot reach the repeat keeps every copy.
  EXPECT_GT(narrow.size(), kSubject * (kCopies - 2u))
      << "the 2 MB control matched repeats it cannot reach, so its size is not "
         "evidence that reach is what the wide window is being asked for";
  // The subject: one copy kept, the other three matched.
  EXPECT_LT(wide.size(), kSubject + kSubject / 2u)
      << "the 8 MB window kept " << wide.size() << " bytes where one copy of "
         "the block is " << kSubject << "; the control kept " << narrow.size();
  EXPECT_TRUE(decodesTo(registry_, wide, input));
  EXPECT_TRUE(decodesTo(registry_, narrow, input));
}

/**
 * @brief The same, above one thread.
 *
 * A parallel job slides its own window with the same code, so the reach a job
 * offers is the same bookkeeping.  The job is smaller than this stream, so more
 * than one job runs and the overlap each one is seeded with is slid too.
 */
TEST_F(ZstdWindowTest, AParallelJobKeepsItsReachAcrossACompaction) {
  constexpr size_t kSubject = 1024u * 1024u;
  constexpr size_t kFiller = 5u * 1024u * 1024u;
  constexpr size_t kCopies = 4u;
  size_t distance = 0;
  const std::vector<uint8_t> input =
      farRepeats(kSubject, kFiller, kCopies, &distance);

  std::vector<uint8_t> wide;
  std::vector<uint8_t> narrow;
  gcomp_options_t * options = windowOptions(23, 4);
  ASSERT_EQ(streamEncode(registry_, options, input, &wide, nullptr), GCOMP_OK);
  gcomp_options_destroy(options);

  options = windowOptions(21, 4);
  ASSERT_EQ(streamEncode(registry_, options, input, &narrow, nullptr),
      GCOMP_OK);
  gcomp_options_destroy(options);

  EXPECT_GT(narrow.size(), kSubject * (kCopies - 2u))
      << "the 2 MB control matched repeats it cannot reach";
  EXPECT_LT(wide.size(), kSubject + kSubject / 2u)
      << "four threads at an 8 MB window kept " << wide.size()
      << " bytes; the control kept " << narrow.size();
  EXPECT_TRUE(decodesTo(registry_, wide, input));
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
