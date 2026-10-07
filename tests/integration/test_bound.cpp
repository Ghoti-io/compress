/**
 * @file test_bound.cpp
 *
 * gcomp_encode_bound() promises an output buffer that cannot be too small.
 * This is the half of that promise the library has to keep.
 *
 * A bound is only useful if it holds for the input that compresses worst, so
 * the inputs here are chosen to defeat each method rather than to flatter it:
 * random bytes, which nothing can compress; data whose every byte is distinct
 * from its neighbours, which is LZW's worst case; and runs just too short to
 * pay for a run packet, which is RLE's.
 *
 * Two things are asserted for every combination:
 *
 * - encoding into a buffer of exactly the bound succeeds, and
 * - what it wrote is no larger than the bound.
 *
 * The first is the one that matters. A bound that is never reached proves
 * nothing about a bound that is too small, and `GCOMP_ERR_LIMIT` from an
 * encode at exactly the bound is the failure this exists to catch.
 *
 * The sizes are not random for their own sake: each format's block boundary is
 * tested at the boundary and on either side of it, because that is where a
 * per-block overhead gets counted once too few.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

//
// Inputs that defeat compression
//

/// Bytes with no structure at all: every method must store these verbatim.
std::vector<uint8_t> incompressible(size_t len, uint32_t seed = 2463534242u) {
  std::vector<uint8_t> v(len);
  uint32_t s = seed;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v[i] = (uint8_t)(s >> 24);
  }
  return v;
}

/**
 * @brief A counter widened to three bytes per step.
 *
 * Every three-byte window is distinct, so LZW's dictionary never gets a second
 * use out of an entry and each byte costs a full-width code - which is how LZW
 * expands, and the case its bound has to cover.
 */
std::vector<uint8_t> never_repeats(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    size_t k = i / 3;
    switch (i % 3) {
    case 0: v[i] = (uint8_t)(k & 0xFF); break;
    case 1: v[i] = (uint8_t)((k >> 8) & 0xFF); break;
    default: v[i] = (uint8_t)((k >> 16) & 0xFF); break;
    }
  }
  return v;
}

/**
 * @brief Pairs of equal bytes, then a change.
 *
 * A run of two is the shortest PackBits will encode as a run, and alternating
 * like this keeps the encoder switching between literal and run packets, which
 * is where its per-packet control byte is spent most often.
 */
std::vector<uint8_t> alternating_runs(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    v[i] = (uint8_t)((i / 2) & 0xFF);
  }
  return v;
}

/// One byte repeated: the case every method compresses best.
std::vector<uint8_t> all_one(size_t len) {
  return std::vector<uint8_t>(len, 0x5A);
}

/**
 * @brief RLE's worst case exactly: one byte that cannot run, then two that can.
 *
 * Every three bytes cost four - a one-byte literal packet and a two-byte run
 * packet - which is the ratio rle_encode_bound() is derived from.  The first
 * byte of each triple is never zero, so it can never join the run beside it and
 * the pattern never lets two triples merge.
 */
std::vector<uint8_t> literal_run_alternating(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    size_t k = i / 3;
    v[i] = (i % 3 == 0) ? (uint8_t)(1u + (k % 255u)) : (uint8_t)0;
  }
  return v;
}

using Generator = std::vector<uint8_t> (*)(size_t);
using OptionSetter = std::function<void(gcomp_options_t *)>;

struct Config {
  std::string name;
  const char * method;
  OptionSetter set_options;
};

/**
 * @brief Whether this run is under Memcheck.
 *
 * The sweep below is thousands of encodes, some of them a third of a megabyte.
 * Outside valgrind that is under a minute; under it, where every byte carries
 * validity and addressability bits and every instruction is instrumented, it
 * runs for hours - long enough that the valgrind gate stops being something
 * anyone waits for, which is the same as not having it.
 *
 * So under valgrind the sweep keeps every method and every option set and
 * trims the sizes: the point of running this here is that the bound arithmetic
 * and the encoders' framing are watched for uninitialised reads and overruns,
 * and that happens on the first few encodes as well as the ten-thousandth.
 * The full sweep runs in the ordinary build and under ASan.
 *
 * The Makefile's valgrind targets set this.
 */
static bool UnderValgrind() {
  const char * vg = std::getenv("GCOMP_UNDER_VALGRIND");
  if (vg && vg[0] == '1') {
    return true;
  }
  // ThreadSanitizer sets GCOMP_UNDER_SLOW_BUILD, and for the same reason: this
  // sweep measures what a bound arithmetic says about a format, and a race
  // detector has nothing to add to that while making it twenty times slower.
  // The full grid still runs in the ordinary build and under ASan.
  const char * slow = std::getenv("GCOMP_UNDER_SLOW_BUILD");
  return slow && slow[0] == '1';
}

/// Sizes to try: the boundaries every format counts blocks at, and a spread.
std::vector<size_t> sizes_to_try() {
  std::vector<size_t> v;
  if (UnderValgrind()) {
    // Small, but still across a block boundary and either side of one.
    for (size_t n : {0u, 1u, 129u, 1000u, 65534u, 65535u, 65536u, 70000u}) {
      v.push_back(n);
    }
    return v;
  }
  // Degenerate and tiny.
  for (size_t n : {0u, 1u, 2u, 3u, 127u, 128u, 129u, 130u, 255u, 256u, 257u}) {
    v.push_back(n);
  }
  // DEFLATE's stored block is 65535 bytes; LZ4's smallest frame block 65536;
  // zstd's largest 131072.  Each is counted at the boundary and either side.
  for (size_t b : {65535u, 65536u, 131072u, 262144u}) {
    v.push_back(b - 1);
    v.push_back(b);
    v.push_back(b + 1);
  }
  // A spread through the middle, deterministic so a failure reproduces.
  uint32_t s = 22695477u;
  for (int i = 0; i < 50; i++) {
    s = s * 1103515245u + 12345u;
    v.push_back((size_t)(s % 300000u));
  }
  return v;
}

std::vector<Config> configurations() {
  std::vector<Config> c;

  c.push_back({"deflate/default", "deflate", nullptr});
  for (int lvl : {1, 6, 9}) {
    c.push_back({"deflate/L" + std::to_string(lvl), "deflate",
        [lvl](gcomp_options_t * o) {
          gcomp_options_set_int64(o, "deflate.level", lvl);
        }});
  }
  for (uint64_t wb : {8u, 15u}) {
    c.push_back({"deflate/win" + std::to_string(wb), "deflate",
        [wb](gcomp_options_t * o) {
          gcomp_options_set_uint64(o, "deflate.window_bits", wb);
        }});
  }

  c.push_back({"zlib/default", "zlib", nullptr});
  for (uint64_t wb : {8u, 15u}) {
    c.push_back({"zlib/win" + std::to_string(wb), "zlib",
        [wb](gcomp_options_t * o) {
          gcomp_options_set_uint64(o, "deflate.window_bits", wb);
        }});
  }
  // A preset dictionary makes the encoder write FDICT and four DICTID bytes,
  // so it changes the bound. No zlib configuration here set one, which is why
  // the sweep could not see the bound failing to count them.
  //
  // One length, not three. What varies with the length is the DICTID
  // arithmetic, and EncodeBound.ZlibCountsTheDictid pins that at 1, 64 and 4096
  // bytes directly and in microseconds. What the sweep adds is "a buffer of
  // exactly the bound is enough, with a dictionary in the stream", and that does
  // not depend on how long the dictionary is - so three lengths here tripled the
  // most expensive test in the suite for nothing.
  c.push_back({"zlib/dict", "zlib", [](gcomp_options_t * o) {
                 std::vector<uint8_t> dict(64, uint8_t{'Q'});
                 gcomp_options_set_bytes(
                     o, "zlib.dictionary", dict.data(), dict.size());
               }});
  c.push_back({"zlib/dict_tight", "zlib", [](gcomp_options_t * o) {
                 std::vector<uint8_t> dict(64, uint8_t{'Q'});
                 gcomp_options_set_bytes(
                     o, "zlib.dictionary", dict.data(), dict.size());
                 gcomp_options_set_uint64(o, "deflate.window_bits", 8u);
                 gcomp_options_set_int64(o, "deflate.level", 1);
               }});
  c.push_back({"gzip/default", "gzip", nullptr});
  c.push_back({"gzip/fields", "gzip", [](gcomp_options_t * o) {
                 gcomp_options_set_string(
                     o, "gzip.name", "a-file-with-a-fairly-long-name.bin");
                 gcomp_options_set_string(o, "gzip.comment",
                     "a comment that is also not especially short");
                 gcomp_options_set_bool(o, "gzip.header_crc", 1);
               }});

  c.push_back({"lz4/default", "lz4", nullptr});
  for (uint64_t bs : {65536u, 262144u, 1048576u, 4194304u}) {
    c.push_back({"lz4/block" + std::to_string(bs), "lz4",
        [bs](gcomp_options_t * o) {
          gcomp_options_set_uint64(o, "lz4.block_size", bs);
        }});
  }
  c.push_back({"lz4/checksums", "lz4", [](gcomp_options_t * o) {
                 gcomp_options_set_uint64(o, "lz4.block_size", 65536);
                 gcomp_options_set_bool(o, "lz4.block_checksum", 1);
                 gcomp_options_set_bool(o, "lz4.content_checksum", 1);
               }});
  c.push_back({"lz4/linked", "lz4", [](gcomp_options_t * o) {
                 gcomp_options_set_bool(o, "lz4.independent_blocks", 0);
               }});

  c.push_back({"zstd/default", "zstd", nullptr});
  for (int64_t lvl : {1, 3, 9, 11, 19}) {
    c.push_back({"zstd/L" + std::to_string(lvl), "zstd",
        [lvl](gcomp_options_t * o) {
          gcomp_options_set_int64(o, "zstd.level", lvl);
        }});
  }
  for (uint64_t wl : {10u, 17u, 24u}) {
    c.push_back({"zstd/win" + std::to_string(wl), "zstd",
        [wl](gcomp_options_t * o) {
          gcomp_options_set_uint64(o, "zstd.window_log", wl);
        }});
  }
  c.push_back({"zstd/checksum", "zstd", [](gcomp_options_t * o) {
                 gcomp_options_set_bool(o, "zstd.checksum", 1);
               }});

  c.push_back({"lzw/default", "lzw", nullptr});
  for (const char * fmt : {"tiff", "gif"}) {
    std::string f = fmt;
    c.push_back({"lzw/" + f, "lzw", [f](gcomp_options_t * o) {
                   gcomp_options_set_string(o, "lzw.format", f.c_str());
                 }});
  }
  // lzw_core.h caps the code width at LZW_CORE_MAX_CODE_BITS (12).
  for (uint64_t bits : {9u, 10u, 12u}) {
    c.push_back({"lzw/bits" + std::to_string(bits), "lzw",
        [bits](gcomp_options_t * o) {
          gcomp_options_set_uint64(o, "lzw.max_code_bits", bits);
        }});
  }

  c.push_back({"lzma2/default", "lzma2", nullptr});
  for (int64_t preset : {0, 6}) {
    c.push_back({"lzma2/preset" + std::to_string(preset), "lzma2",
        [preset](gcomp_options_t * o) {
          gcomp_options_set_int64(o, "lzma2.preset", preset);
        }});
  }

  c.push_back({"brotli/default", "brotli", nullptr});
  for (int64_t lgwin : {10, 24}) {
    c.push_back({"brotli/lgwin" + std::to_string(lgwin), "brotli",
        [lgwin](gcomp_options_t * o) {
          gcomp_options_set_int64(o, "brotli.lgwin", lgwin);
        }});
  }

  c.push_back({"rle/default", "rle", nullptr});
  for (const char * fmt : {"packbits", "tga"}) {
    std::string f = fmt;
    c.push_back({"rle/" + f, "rle", [f](gcomp_options_t * o) {
                   gcomp_options_set_string(o, "rle.format", f.c_str());
                 }});
  }

  return c;
}

/// Build the options object a configuration describes.
gcomp_options_t * make_options(const Config & c) {
  gcomp_options_t * o = nullptr;
  if (gcomp_options_create(&o) != GCOMP_OK) {
    return nullptr;
  }
  if (c.set_options) {
    c.set_options(o);
  }
  return o;
}

/// Check one method, one option set, one input.
void check(const Config & c, const std::vector<uint8_t> & input,
    const char * shape) {
  gcomp_options_t * opts = make_options(c);
  ASSERT_NE(opts, nullptr);

  size_t bound = 0;
  gcomp_status_t s = gcomp_encode_bound(
      nullptr, c.method, opts, input.size(), &bound);
  ASSERT_EQ(s, GCOMP_OK) << c.name << "/" << shape << " n=" << input.size()
                         << ": bound failed: " << gcomp_status_to_string(s);

  // A buffer of exactly the bound must be enough.  gcomp_encode_buffer()
  // rejects a zero capacity outright, so an empty input still gets one byte.
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  s = gcomp_encode_buffer(nullptr, c.method, opts, input.data(), input.size(),
      out.data(), out.size(), &written);
  EXPECT_EQ(s, GCOMP_OK) << c.name << "/" << shape << " n=" << input.size()
                         << ": encoding into a buffer of exactly the bound ("
                         << bound << ") returned "
                         << gcomp_status_to_string(s);
  if (s == GCOMP_OK) {
    EXPECT_LE(written, bound)
        << c.name << "/" << shape << " n=" << input.size() << ": wrote "
        << written << " bytes into a bound of " << bound;
  }

  gcomp_options_destroy(opts);
}

void sweep_method(const char * method) {
  const std::vector<size_t> sizes = sizes_to_try();
  const struct {
    const char * name;
    Generator gen;
  } shapes[] = {
      {"incompressible", nullptr}, // handled below, it takes a seed
      {"never_repeats", &never_repeats},
      {"alternating_runs", &alternating_runs},
      {"literal_run_alternating", &literal_run_alternating},
      {"all_one", &all_one},
  };

  for (const Config & c : configurations()) {
    if (std::strcmp(c.method, method) != 0) {
      continue;
    }
    for (size_t n : sizes) {
      for (const auto & shape : shapes) {
        std::vector<uint8_t> input =
            shape.gen ? shape.gen(n) : incompressible(n);
        check(c, input, shape.name);
        if (::testing::Test::HasFatalFailure()) {
          return;
        }
      }
    }
  }
}

TEST(EncodeBound, Deflate) {
  sweep_method("deflate");
}
TEST(EncodeBound, Zlib) {
  sweep_method("zlib");
}
TEST(EncodeBound, Gzip) {
  sweep_method("gzip");
}
TEST(EncodeBound, Lz4) {
  sweep_method("lz4");
}
TEST(EncodeBound, Zstd) {
  sweep_method("zstd");
}
TEST(EncodeBound, Lzw) {
  sweep_method("lzw");
}
TEST(EncodeBound, Rle) {
  sweep_method("rle");
}
TEST(EncodeBound, Brotli) {
  sweep_method("brotli");
}

TEST(EncodeBound, Lzma2) {
  sweep_method("lzma2");
}

/**
 * @brief The parallel encoders must sit under the same bound.
 *
 * They frame differently - jobs, an overlap, an epilogue - and a bound that
 * only held for one thread would be a trap for exactly the caller who most
 * wants a buffer sized in advance.
 */
TEST(EncodeBound, Threaded) {
  for (const char * method : {"zstd", "lz4"}) {
    for (uint64_t threads : {2u, 4u}) {
      for (size_t n : {1000u, 70000u, 200000u, 600000u}) {
        gcomp_options_t * o = nullptr;
        ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_uint64(o, "threads.count", threads),
            GCOMP_OK);

        size_t bound = 0;
        ASSERT_EQ(gcomp_encode_bound(nullptr, method, o, n, &bound), GCOMP_OK);

        std::vector<uint8_t> input = incompressible(n);
        std::vector<uint8_t> out(bound ? bound : 1);
        size_t written = 0;
        gcomp_status_t s = gcomp_encode_buffer(nullptr, method, o,
            input.data(), input.size(), out.data(), out.size(), &written);
        EXPECT_EQ(s, GCOMP_OK)
            << method << " threads=" << threads << " n=" << n
            << ": encoding into exactly the bound (" << bound << ") returned "
            << gcomp_status_to_string(s);
        EXPECT_LE(written, bound) << method << " threads=" << threads
                                  << " n=" << n;
        gcomp_options_destroy(o);
      }
    }
  }
}

/**
 * @brief zstd.seekable writes many frames, and the bound has to know that.
 *
 * `EncodeBound.Threaded` is the same idea one option over, and this is the cell
 * neither it nor the seekable tests covered: the bound was computed for a single
 * frame and never read `zstd.seekable` at all, so it came out byte-identical
 * whether the option was set or not. Seekable writes one complete frame per
 * `zstd.seekable_frame_size` of input, each owing its own header and checksum,
 * plus a seek table with an entry per frame.
 *
 * With compressible input the single-frame slack absorbed the difference, which
 * is why nothing noticed. With incompressible input the documented pattern -
 * ask for the bound, allocate exactly that, encode - returned GCOMP_ERR_LIMIT at
 * every size tried: 300,000 bytes wanted 300,048 against a bound of 300,030, and
 * 2,160,000 wanted 2,160,137 against 2,160,072.
 *
 * The frame sizes below straddle the default 1 MiB frame span so the count is 1,
 * 2 and 3, because an overhead counted once instead of per frame is invisible
 * when there is only one frame.
 */
TEST(EncodeBound, ZstdSeekableCountsEveryFrame) {
  for (int checksum = 0; checksum < 2; checksum++) {
    for (int seek_checksum = 0; seek_checksum < 2; seek_checksum++) {
      for (size_t n : {300000u, 1000000u, 1100000u, 2160000u}) {
        gcomp_options_t * o = nullptr;
        ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_bool(o, "zstd.checksum", checksum),
            GCOMP_OK);
        ASSERT_EQ(
            gcomp_options_set_bool(o, "zstd.seekable_checksum", seek_checksum),
            GCOMP_OK);

        size_t bound = 0;
        ASSERT_EQ(gcomp_encode_bound(nullptr, "zstd", o, n, &bound), GCOMP_OK);

        const std::vector<uint8_t> input = incompressible(n);
        std::vector<uint8_t> out(bound ? bound : 1);
        size_t written = 0;
        const gcomp_status_t s = gcomp_encode_buffer(nullptr, "zstd", o,
            input.data(), input.size(), out.data(), out.size(), &written);
        EXPECT_EQ(s, GCOMP_OK)
            << "n=" << n << " checksum=" << checksum
            << " seekable_checksum=" << seek_checksum
            << ": encoding into exactly the bound (" << bound << ") returned "
            << gcomp_status_to_string(s);
        EXPECT_LE(written, bound)
            << "n=" << n << " checksum=" << checksum
            << " seekable_checksum=" << seek_checksum;
        gcomp_options_destroy(o);
      }
    }
  }
}

/**
 * @brief A seekable bound must exceed the same input's single-frame bound.
 *
 * The sweep above would pass if the bound were simply enormous. This is the
 * cheap statement of what makes it right: more frames cost more, so asking for
 * a seekable file must produce a larger figure than asking for one frame, and
 * asking for smaller frames must produce a larger figure still.
 *
 * It is also the assertion that fails if somebody stops reading the option
 * again, which is the actual regression to guard - the arithmetic was correct
 * for one frame all along.
 */
TEST(EncodeBound, ZstdSeekableBoundGrowsWithTheFrameCount) {
  const size_t n = 4000000;

  size_t plain = 0;
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_encode_bound(nullptr, "zstd", o, n, &plain), GCOMP_OK);
    gcomp_options_destroy(o);
  }

  size_t previous = plain;
  // Smaller frames, so more of them: each step must cost more than the last.
  for (uint64_t frame : {(uint64_t)2097152, (uint64_t)1048576,
           (uint64_t)262144, (uint64_t)65536}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_uint64(o, "zstd.seekable_frame_size", frame),
        GCOMP_OK);
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "zstd", o, n, &bound), GCOMP_OK);
    gcomp_options_destroy(o);

    EXPECT_GT(bound, previous)
        << "frame_size=" << frame << ": " << bound << " is not more than "
        << previous << ", so the frame count is not reaching the bound";
    previous = bound;
  }
}

/**
 * @brief The bound must be close to what the worst input actually produces.
 *
 * Otherwise the sweep above proves only that the bound is large, not that it is
 * the right size - a bound of SIZE_MAX would pass every test here.
 *
 * "Worst input" is per method and is not the random one: random bytes are the
 * worst case for the LZ77 family, but RLE's worst case is data that alternates
 * between a byte that cannot run and a pair that can, and LZW's is data whose
 * every substring is new. So this takes the largest output any of the shapes
 * produces and asks the bound to be near it.
 */
TEST(EncodeBound, IsNotWildlyLoose) {
  const size_t n = UnderValgrind() ? 20000 : 100000;
  const std::vector<std::vector<uint8_t>> shapes = {incompressible(n),
      never_repeats(n), alternating_runs(n), literal_run_alternating(n),
      all_one(n)};

  for (const char * method :
      {"deflate", "zlib", "gzip", "zstd", "lz4", "rle", "lzw", "brotli",
          "lzma2"}) {
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, method, nullptr, n, &bound),
        GCOMP_OK);

    size_t worst = 0;
    for (const std::vector<uint8_t> & input : shapes) {
      std::vector<uint8_t> out(bound);
      size_t written = 0;
      ASSERT_EQ(gcomp_encode_buffer(nullptr, method, nullptr, input.data(), n,
                    out.data(), out.size(), &written),
          GCOMP_OK)
          << method;
      if (written > worst) {
        worst = written;
      }
    }

    // Within a fifth of the worst these inputs achieve, plus a kilobyte of
    // framing. Generous for two reasons: no fixed set of inputs is guaranteed
    // to reach a method's true worst case, and LZW's bound is honestly loose -
    // it charges the widest code for every byte, where a real stream starts at
    // nine bits and widens as the table fills, so about a tenth of the gap
    // there is the ramp rather than slack. Tight enough that a bound which had
    // stopped tracking the format would show.
    //
    // For reference, at the time of writing: deflate, zlib, gzip, lz4 and zstd
    // land within a fraction of a percent, RLE within one byte of the
    // literal/run worst case, and LZW about 10% above what any of these shapes
    // reaches.
    EXPECT_LE(bound, worst + worst / 5 + 1024)
        << method << ": bound " << bound << " against " << worst
        << " bytes for the worst of " << shapes.size() << " input shapes";
  }
}

/**
 * @brief zlib's bound must count the DICTID a dictionary makes it write.
 *
 * RFC 1950 section 2.2: FDICT in FLG means four DICTID bytes follow the two
 * header bytes. The encoder writes them; the bound has to charge for them.
 *
 * This is asserted as an exact difference rather than as "the buffer was big
 * enough", because "big enough" could not see the defect this pins. The bound
 * was four bytes short, and deflate's own bound happened to carry exactly four
 * bytes of slack at its tightest point, so every encode still fit -- with zero
 * margin. Measured across 305,760 configurations the tightest case with a
 * dictionary had 0 bytes spare and the same case without one had 4. A test
 * that only checked the encode succeeded would have passed throughout.
 *
 * An empty dictionary is the boundary: the encoder treats zero bytes as no
 * dictionary and does not set FDICT, so the bound must not charge for one.
 */
TEST(EncodeBound, ZlibCountsTheDictid) {
  const size_t n = 52; // The corner where deflate's bound is tightest.

  gcomp_options_t * plain = nullptr;
  ASSERT_EQ(gcomp_options_create(&plain), GCOMP_OK);
  gcomp_options_set_uint64(plain, "deflate.window_bits", 8u);
  gcomp_options_set_int64(plain, "deflate.level", 1);

  size_t bound_plain = 0;
  ASSERT_EQ(gcomp_encode_bound(nullptr, "zlib", plain, n, &bound_plain),
      GCOMP_OK);

  for (size_t dict_len : {size_t{1}, size_t{64}, size_t{4096}}) {
    std::vector<uint8_t> dict(dict_len, uint8_t{'Q'});
    gcomp_options_t * with = nullptr;
    ASSERT_EQ(gcomp_options_create(&with), GCOMP_OK);
    gcomp_options_set_uint64(with, "deflate.window_bits", 8u);
    gcomp_options_set_int64(with, "deflate.level", 1);
    gcomp_options_set_bytes(with, "zlib.dictionary", dict.data(), dict.size());

    size_t bound_with = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "zlib", with, n, &bound_with),
        GCOMP_OK);

    // DICTID is four bytes whatever the dictionary's length, because it is an
    // Adler-32 of it and not a copy of it.
    EXPECT_EQ(bound_with, bound_plain + 4)
        << "a " << dict_len << "-byte dictionary moved the bound from "
        << bound_plain << " to " << bound_with
        << "; RFC 1950 adds exactly four DICTID bytes";

    // And the margin is really there: encode into exactly the bound and check
    // what it took, so the figure is measured rather than only asserted.
    std::vector<uint8_t> input = incompressible(n);
    std::vector<uint8_t> out(bound_with);
    size_t written = 0;
    ASSERT_EQ(gcomp_encode_buffer(nullptr, "zlib", with, input.data(), n,
                  out.data(), out.size(), &written),
        GCOMP_OK);
    EXPECT_LE(written, bound_with);
    EXPECT_GE(bound_with - written, size_t{4})
        << "the bound should keep deflate's own slack once the DICTID is paid "
           "for; wrote "
        << written << " into " << bound_with;

    gcomp_options_destroy(with);
  }

  // Zero bytes is not a dictionary, and must not be charged for.
  gcomp_options_t * empty = nullptr;
  ASSERT_EQ(gcomp_options_create(&empty), GCOMP_OK);
  gcomp_options_set_uint64(empty, "deflate.window_bits", 8u);
  gcomp_options_set_int64(empty, "deflate.level", 1);
  gcomp_options_set_bytes(empty, "zlib.dictionary", nullptr, 0);
  size_t bound_empty = 0;
  ASSERT_EQ(gcomp_encode_bound(nullptr, "zlib", empty, n, &bound_empty),
      GCOMP_OK);
  EXPECT_EQ(bound_empty, bound_plain)
      << "an empty zlib.dictionary does not set FDICT, so it adds no DICTID";
  gcomp_options_destroy(empty);

  gcomp_options_destroy(plain);
}

TEST(EncodeBound, RejectsBadArguments) {
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, nullptr, nullptr, 10, &bound),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encode_bound(nullptr, "deflate", nullptr, 10, nullptr),
      GCOMP_ERR_INVALID_ARG);
  // An unregistered name is GCOMP_ERR_UNSUPPORTED, which is what
  // gcomp_encoder_create() answers for the same mistake.
  EXPECT_EQ(gcomp_encode_bound(nullptr, "no-such-method", nullptr, 10, &bound),
      GCOMP_ERR_UNSUPPORTED);
}

/**
 * @brief An input size whose bound cannot be represented is refused.
 *
 * bound_internal.h: "A bound that silently wrapped would be worse than no
 * bound at all: the caller would allocate a small buffer and believe it was
 * big enough. Overflow is reported as GCOMP_ERR_LIMIT."
 *
 * No buffer this large exists, which is exactly why this has to be asserted
 * rather than observed: the arithmetic is reached by a caller passing a size
 * it computed, and a size computed wrongly is how an attacker gets a short
 * buffer. The sizes below are chosen to overflow at a different step for
 * different formats - near SIZE_MAX the input itself does not fit, and a
 * little below it the per-block framing is what does not.
 *
 * Returning GCOMP_OK here is the failure being guarded against, so the
 * assertion is on the status and not merely on "did not crash". brotli
 * computed its block count by hand instead of through
 * gcomp_bound_block_count(), and that addition wrapped: for SIZE_MAX - 1000
 * it reported a block count of zero, charged nothing for the block headers,
 * and answered GCOMP_OK with a bound smaller than the input.
 */
TEST(EncodeBound, AnUnrepresentableBoundIsRefused) {
  const size_t kMax = (size_t)-1;
  for (const char * method :
      {"deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli",
          "lzma2"}) {
    // Each of these is larger than SIZE_MAX minus the framing every format
    // here charges for an input that long, so none of their bounds fits in a
    // size_t. Three quarters of SIZE_MAX does fit for several of them, which
    // is why the sizes stop where they do.
    for (size_t n : {kMax, kMax - 1, kMax - 1000, kMax - 65536}) {
      size_t bound = 12345;
      gcomp_status_t st =
          gcomp_encode_bound(nullptr, method, nullptr, n, &bound);
      // Non-fatal, so one method's wrap does not hide the others'.
      EXPECT_NE(st, GCOMP_OK)
          << method << ": bound for " << n << " reported OK as " << bound;
      if (st != GCOMP_OK) {
        EXPECT_EQ(st, GCOMP_ERR_LIMIT)
            << method << ": overflow is GCOMP_ERR_LIMIT (bound_internal.h)";
      }
    }
  }
}

/// Zero in, something out: every framed format still writes its frame.
TEST(EncodeBound, EmptyInput) {
  for (const char * method :
      {"deflate", "zlib", "gzip", "lz4", "zstd", "lzw", "rle", "brotli",
          "lzma2"}) {
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, method, nullptr, 0, &bound),
        GCOMP_OK)
        << method;
    std::vector<uint8_t> out(bound ? bound : 1);
    size_t written = 0;
    EXPECT_EQ(gcomp_encode_buffer(nullptr, method, nullptr, nullptr, 0,
                  out.data(), out.size(), &written),
        GCOMP_OK)
        << method << ": bound " << bound;
    EXPECT_LE(written, bound ? bound : 1) << method;
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
