/**
 * @file test_brotli.cpp
 *
 * Brotli (RFC 7932): level 0's stored bytes, level 1's compressed blocks, and
 * a decode of streams produced by libbrotli. The pinned copy of
 * that library is the one in the oracle image (libbrotli 1.1.0). These tests
 * load it by soname, which is the image's copy when `make check-oracle` runs
 * them and this machine's copy when `make test` does.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "methods/brotli/brotli_internal.h"

namespace {

gcomp_options_t * lgwin_options(int lgwin, int level) {
  gcomp_options_t * opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  if (lgwin != 0) {
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.lgwin", lgwin), GCOMP_OK);
  }
  if (level >= 0) {
    EXPECT_EQ(gcomp_options_set_int64(opts, "brotli.level", level), GCOMP_OK);
  }
  return opts;
}

std::vector<uint8_t> encode_bytes(const uint8_t * data, size_t len, int lgwin,
    int level = -1) {
  gcomp_options_t * opts =
      (lgwin == 0 && level < 0) ? nullptr : lgwin_options(lgwin, level);
  std::vector<uint8_t> out(len + 64);
  size_t written = 0;
  gcomp_status_t st = gcomp_encode_buffer(nullptr, "brotli", opts, data, len,
      out.data(), out.size(), &written);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  EXPECT_EQ(st, GCOMP_OK);
  out.resize(st == GCOMP_OK ? written : 0);
  return out;
}

std::vector<uint8_t> decode_bytes(const uint8_t * data, size_t len,
    size_t cap) {
  std::vector<uint8_t> out(cap);
  size_t written = 0;
  gcomp_status_t st = gcomp_decode_buffer(nullptr, "brotli", nullptr, data, len,
      out.data(), out.size(), &written);
  EXPECT_EQ(st, GCOMP_OK);
  out.resize(st == GCOMP_OK ? written : 0);
  return out;
}

void expect_roundtrip(const uint8_t * data, size_t len, int lgwin) {
  std::vector<uint8_t> enc = encode_bytes(data, len, lgwin);
  ASSERT_FALSE(enc.empty() && len != 0);
  std::vector<uint8_t> dec = decode_bytes(enc.data(), enc.size(), len + 8);
  ASSERT_EQ(dec.size(), len);
  if (len) {
    EXPECT_EQ(memcmp(dec.data(), data, len), 0);
  }
}

using BrotliCompress = int (*)(int, int, int, size_t, const uint8_t *, size_t *,
    uint8_t *);
using BrotliDecompress = int (*)(size_t, const uint8_t *, size_t *, uint8_t *);
using BrotliMaxSize = size_t (*)(size_t);

struct BrotliLib {
  void * enc = nullptr;
  void * dec = nullptr;
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
    lib.enc = dlopen("libbrotlienc.so.1", RTLD_NOW);
    lib.dec = dlopen("libbrotlidec.so.1", RTLD_NOW);
    if (lib.enc) {
      lib.compress = reinterpret_cast<BrotliCompress>(
          dlsym(lib.enc, "BrotliEncoderCompress"));
      lib.max_size = reinterpret_cast<BrotliMaxSize>(
          dlsym(lib.enc, "BrotliEncoderMaxCompressedSize"));
    }
    if (lib.dec) {
      lib.decompress = reinterpret_cast<BrotliDecompress>(
          dlsym(lib.dec, "BrotliDecoderDecompress"));
    }
  }
  return lib;
}

std::vector<uint8_t> lib_compress(const uint8_t * data, size_t len, int quality,
    int lgwin) {
  const BrotliLib & lib = brotli_lib();
  size_t cap = lib.max_size(len);
  std::vector<uint8_t> out(cap ? cap : 16);
  size_t written = out.size();
  int ok = lib.compress(quality, lgwin, 0, len, data, &written, out.data());
  EXPECT_EQ(ok, 1);
  out.resize(ok == 1 ? written : 0);
  return out;
}

} // namespace

TEST(Brotli, EmptyStreamIsOneByte) {
  std::vector<uint8_t> enc = encode_bytes(nullptr, 0, 0);
  ASSERT_EQ(enc.size(), 1u);
  EXPECT_EQ(enc[0], 0x06);
  std::vector<uint8_t> dec = decode_bytes(enc.data(), enc.size(), 8);
  EXPECT_TRUE(dec.empty());
}

TEST(Brotli, EmptyWindowBits) {
  struct {
    int lgwin;
    uint8_t bytes[2];
    size_t n;
  } cases[] = {
      {10, {0xa1, 0x01}, 2},
      {16, {0x06, 0x00}, 1},
      {17, {0x81, 0x01}, 2},
      {24, {0x3f, 0x00}, 1},
  };
  for (const auto & c : cases) {
    std::vector<uint8_t> enc = encode_bytes(nullptr, 0, c.lgwin);
    ASSERT_EQ(enc.size(), c.n) << c.lgwin;
    EXPECT_EQ(memcmp(enc.data(), c.bytes, c.n), 0) << c.lgwin;
    std::vector<uint8_t> dec = decode_bytes(enc.data(), enc.size(), 4);
    EXPECT_TRUE(dec.empty()) << c.lgwin;
  }
}

TEST(Brotli, SingleByteIsTheTrivialLayout) {
  const uint8_t a = 'a';
  std::vector<uint8_t> enc = encode_bytes(&a, 1, 16, 0);
  const uint8_t want[] = {0x0c, 0x00, 0x00, 0x08, 'a', 0x03};
  ASSERT_EQ(enc.size(), sizeof(want));
  EXPECT_EQ(memcmp(enc.data(), want, sizeof(want)), 0);
  std::vector<uint8_t> dec = decode_bytes(enc.data(), enc.size(), 4);
  ASSERT_EQ(dec.size(), 1u);
  EXPECT_EQ(dec[0], 'a');
}

TEST(Brotli, ChunkOf65536ThenTerminator) {
  std::vector<uint8_t> data(65536, 0);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i * 3);
  }
  std::vector<uint8_t> enc = encode_bytes(data.data(), data.size(), 16, 0);
  ASSERT_GE(enc.size(), 5u);
  EXPECT_EQ(enc[0], 0x0c);
  EXPECT_EQ(enc[1], 248);
  EXPECT_EQ(enc[2], 255);
  EXPECT_EQ(enc[3], 15);
  EXPECT_EQ(enc.back(), 0x03);
  std::vector<uint8_t> dec =
      decode_bytes(enc.data(), enc.size(), data.size() + 8);
  ASSERT_EQ(dec.size(), data.size());
  EXPECT_EQ(memcmp(dec.data(), data.data(), data.size()), 0);
}

TEST(Brotli, ChunkBoundarySplits65537) {
  std::vector<uint8_t> data(65537, 0x5a);
  data[65536] = 0x11;
  std::vector<uint8_t> enc = encode_bytes(data.data(), data.size(), 16, 0);
  ASSERT_EQ(enc.size(), 1u + 3u + 65536u + 3u + 1u + 1u);
  EXPECT_EQ(enc[0], 0x0c);
  EXPECT_EQ(enc[65540], 0x00);
  EXPECT_EQ(enc[65541], 0x00);
  EXPECT_EQ(enc[65542], 0x08);
  EXPECT_EQ(enc[65543], 0x11);
  EXPECT_EQ(enc[65544], 0x03);
  std::vector<uint8_t> dec =
      decode_bytes(enc.data(), enc.size(), data.size() + 8);
  ASSERT_EQ(dec, data);
}

TEST(Brotli, LevelOneShrinksARun) {
  std::vector<uint8_t> data(8000, 'a');
  std::vector<uint8_t> stored = encode_bytes(data.data(), data.size(), 16, 0);
  std::vector<uint8_t> comp = encode_bytes(data.data(), data.size(), 16, 1);
  ASSERT_LT(comp.size(), stored.size() / 8);
  std::vector<uint8_t> dec = decode_bytes(comp.data(), comp.size(), data.size() + 8);
  ASSERT_EQ(dec, data);

  const BrotliLib & lib = brotli_lib();
  if (lib.ok()) {
    std::vector<uint8_t> out(data.size() + 8);
    size_t written = out.size();
    ASSERT_EQ(lib.decompress(comp.size(), comp.data(), &written, out.data()), 1);
    ASSERT_EQ(written, data.size());
    EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0);
  }
}

TEST(Brotli, LevelOneBlockCanExceed65536) {
  std::vector<uint8_t> data(200000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)("abcd"[i % 4]);
  }
  std::vector<uint8_t> stored = encode_bytes(data.data(), data.size(), 16, 0);
  std::vector<uint8_t> comp = encode_bytes(data.data(), data.size(), 16, 1);
  EXPECT_LT(comp.size(), stored.size() / 8);
  std::vector<uint8_t> dec =
      decode_bytes(comp.data(), comp.size(), data.size() + 8);
  ASSERT_EQ(dec, data);
}

/**
 * A match further back than 131068 bytes, which is where the distance
 * alphabet's extra-bit widths used to stop.
 *
 * The far block can only be matched at a distance of 232000, so the window has
 * to be at least lgwin 18 for the matcher to offer it at all - and an offered
 * distance the emitter could not spell failed the whole meta-block, not just
 * that command. Asking for a bigger window therefore made the output fourteen
 * times larger: 250015 bytes at lgwin 20 against 17061 at lgwin 16, where the
 * window is too small to find the match in the first place. The assertion is
 * that a wider window is never worse than a narrower one, which is the
 * property the cliff broke.
 */
TEST(Brotli, AFarMatchDoesNotCostTheWholeBlock) {
  std::vector<uint8_t> data(250000);
  const char * phrase = "lorem ipsum dolor sit amet consectetur ";
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)phrase[i % strlen(phrase)];
  }
  std::vector<uint8_t> block(8192);
  test_helpers_generate_random(block.data(), block.size(), 23);
  memcpy(data.data(), block.data(), block.size());
  memcpy(data.data() + 232000, block.data(), block.size());

  const std::vector<uint8_t> narrow = encode_bytes(data.data(), data.size(), 16);
  ASSERT_LT(narrow.size(), data.size());
  for (int lgwin : {18, 20, 22, 24}) {
    const std::vector<uint8_t> wide =
        encode_bytes(data.data(), data.size(), lgwin);
    EXPECT_LE(wide.size(), narrow.size()) << "lgwin " << lgwin
        << " produced more output than lgwin 16, so a distance it could reach "
           "was one it could not spell";
    std::vector<uint8_t> dec =
        decode_bytes(wide.data(), wide.size(), data.size() + 8);
    ASSERT_EQ(dec, data) << lgwin;
    const BrotliLib & lib = brotli_lib();
    if (lib.ok()) {
      std::vector<uint8_t> out(data.size() + 8);
      size_t written = out.size();
      ASSERT_EQ(lib.decompress(wide.size(), wide.data(), &written, out.data()), 1)
          << lgwin;
      ASSERT_EQ(written, data.size()) << lgwin;
      EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0) << lgwin;
    }
  }
}

TEST(Brotli, LevelOneRoundTripsTextAndNoise) {
  std::vector<uint8_t> text;
  const char * phrase = "the quick brown fox jumps over the lazy dog ";
  while (text.size() < 12000) {
    text.insert(text.end(), phrase, phrase + strlen(phrase));
  }
  text.resize(12000);
  std::vector<uint8_t> noise(3000);
  test_helpers_generate_random(noise.data(), noise.size(), 19);
  expect_roundtrip(text.data(), text.size(), 16);
  expect_roundtrip(text.data(), text.size(), 10);
  expect_roundtrip(noise.data(), noise.size(), 24);
  std::vector<uint8_t> stored = encode_bytes(text.data(), text.size(), 16, 0);
  std::vector<uint8_t> comp = encode_bytes(text.data(), text.size(), 16, 1);
  EXPECT_LT(comp.size(), stored.size() / 2);
}

namespace {

/// A block of noise repeated three times: its only compression is a match at
/// `period`, which is the distance the test wants to control.
std::vector<uint8_t> PeriodicNoise(size_t period, unsigned seed) {
  std::vector<uint8_t> one(period);
  test_helpers_generate_random(one.data(), one.size(), seed);
  std::vector<uint8_t> all;
  for (int i = 0; i < 3; i++) {
    all.insert(all.end(), one.begin(), one.end());
  }
  return all;
}

/// Encode `pre`, flush with `mode`, encode `post`, finish. Returns the whole
/// stream and reports how many bytes were emitted after the flush point.
std::vector<uint8_t> EncodeAcrossFlush(const std::vector<uint8_t> & pre,
    const std::vector<uint8_t> & post, gcomp_flush_t mode, size_t * after) {
  gcomp_encoder_t * enc = nullptr;
  gcomp_options_t * opts = lgwin_options(16, 1);
  EXPECT_EQ(
      gcomp_encoder_create(gcomp_registry_default(), "brotli", opts, &enc),
      GCOMP_OK);
  gcomp_options_destroy(opts);
  std::vector<uint8_t> stream(pre.size() + post.size() + 4096);
  size_t len = 0;
  const std::vector<uint8_t> * phases[2] = {&pre, &post};
  size_t mark = 0;
  for (int phase = 0; phase < 2; phase++) {
    gcomp_buffer_t in = {phases[phase]->data(), phases[phase]->size(), 0};
    while (in.used < in.size) {
      gcomp_buffer_t ob = {stream.data() + len, stream.size() - len, 0};
      EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
      len += ob.used;
    }
    if (phase == 0) {
      for (;;) {
        gcomp_buffer_t ob = {stream.data() + len, stream.size() - len, 0};
        gcomp_status_t s = gcomp_encoder_flush(enc, &ob, mode);
        len += ob.used;
        if (s == GCOMP_OK) {
          break;
        }
        EXPECT_EQ(s, GCOMP_ERR_LIMIT);
        if (s != GCOMP_ERR_LIMIT) {
          break;
        }
      }
      mark = len;
    }
  }
  for (;;) {
    gcomp_buffer_t ob = {stream.data() + len, stream.size() - len, 0};
    gcomp_status_t s = gcomp_encoder_finish(enc, &ob);
    len += ob.used;
    if (s == GCOMP_OK) {
      break;
    }
    EXPECT_EQ(s, GCOMP_ERR_LIMIT);
    if (s != GCOMP_ERR_LIMIT) {
      break;
    }
  }
  gcomp_encoder_destroy(enc);
  stream.resize(len);
  *after = len - mark;
  return stream;
}

} // namespace

/**
 * `GCOMP_FLUSH_FULL` promises that nothing written after the flush refers to
 * anything written before it. A match cannot: level 1 never looks outside the
 * chunk it is compressing. The distance ring buffer can, and did - it is the
 * one piece of decoder state that survives a meta-block boundary, and the mode
 * was ignored outright, so a short or implicit distance code after the flush
 * named a distance established before it. A decoder that lost the earlier
 * bytes resolves that code against a different ring buffer and produces the
 * wrong output with no error.
 *
 * The instrument: identical data after the flush, and only the data before it
 * varies. The variants are chosen so the pre-flush last distance is sometimes
 * exactly the distance the post-flush block wants (700), sometimes one away
 * from it (701, which short code 4 reaches), and sometimes nowhere near. Under
 * a full flush every variant has to produce the same number of post-flush
 * bytes. Under a sync flush they must not all agree - that is the control,
 * without which the full-flush assertion would pass on an input that could
 * never have exposed the dependency in the first place.
 */
TEST(Brotli, AFullFlushLeavesNothingToReferBackTo) {
  const std::vector<uint8_t> post = PeriodicNoise(700, 41);
  const size_t pre_periods[] = {700, 701, 123, 4096};

  std::vector<size_t> full_sizes;
  std::vector<size_t> sync_sizes;
  for (size_t i = 0; i < 4; i++) {
    const std::vector<uint8_t> pre =
        PeriodicNoise(pre_periods[i], (unsigned)(60 + i));
    std::vector<uint8_t> whole = pre;
    whole.insert(whole.end(), post.begin(), post.end());

    for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
      size_t after = 0;
      std::vector<uint8_t> stream = EncodeAcrossFlush(pre, post, mode, &after);
      ASSERT_GT(after, 0u);
      (mode == GCOMP_FLUSH_FULL ? full_sizes : sync_sizes).push_back(after);

      std::vector<uint8_t> dec =
          decode_bytes(stream.data(), stream.size(), whole.size() + 8);
      ASSERT_EQ(dec, whole) << "period " << pre_periods[i] << " mode "
                            << (int)mode;
      const BrotliLib & lib = brotli_lib();
      if (lib.ok()) {
        std::vector<uint8_t> out(whole.size() + 8);
        size_t written = out.size();
        ASSERT_EQ(
            lib.decompress(stream.size(), stream.data(), &written, out.data()),
            1)
            << pre_periods[i];
        ASSERT_EQ(written, whole.size()) << pre_periods[i];
        EXPECT_EQ(memcmp(out.data(), whole.data(), whole.size()), 0)
            << pre_periods[i];
      }
    }
  }

  for (size_t i = 1; i < full_sizes.size(); i++) {
    EXPECT_EQ(full_sizes[i], full_sizes[0])
        << "the bytes after a full flush changed with the data before it, so "
           "something after the flush referred back across it (pre-flush "
           "period "
        << pre_periods[i] << ")";
  }

  bool sync_varies = false;
  for (size_t i = 1; i < sync_sizes.size(); i++) {
    sync_varies = sync_varies || sync_sizes[i] != sync_sizes[0];
  }
  EXPECT_TRUE(sync_varies)
      << "a sync flush kept the ring buffer and still produced the same "
         "post-flush size for every pre-flush variant, so this input cannot "
         "tell the two modes apart and the assertion above proves nothing";
}

TEST(Brotli, RoundTrip) {
  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 7);
  expect_roundtrip(data.data(), data.size(), 16);
  expect_roundtrip(data.data(), data.size(), 10);
  expect_roundtrip(data.data(), data.size(), 24);
}

TEST(Brotli, ByteAtATime) {
  std::vector<uint8_t> data(300);
  test_helpers_generate_sequential(data.data(), data.size());
  std::vector<uint8_t> enc = encode_bytes(data.data(), data.size(), 16);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  std::vector<uint8_t> out(data.size() + 8);
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  for (size_t i = 0; i < enc.size(); i++) {
    gcomp_buffer_t ib = {enc.data() + i, 1, 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK) << i;
  }
  ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  gcomp_decoder_destroy(dec);
  ASSERT_EQ(ob.used, data.size());
  EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0);
}

TEST(Brotli, FlushMakesConsumedBytesDecodable) {
  const uint8_t data[] = "flush-me-please";
  const size_t len = sizeof(data) - 1;
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "brotli", nullptr,
                &enc),
      GCOMP_OK);
  std::vector<uint8_t> stream(64);
  gcomp_buffer_t in = {data, len, 0};
  gcomp_buffer_t out = {stream.data(), stream.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &in, &out), GCOMP_OK);
  ASSERT_EQ(in.used, len);
  ASSERT_EQ(gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_SYNC), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_FULL), GCOMP_OK);
  size_t produced = out.used;

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  std::vector<uint8_t> got(len + 4);
  gcomp_buffer_t ib = {stream.data(), produced, 0};
  gcomp_buffer_t ob = {got.data(), got.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK);
  EXPECT_EQ(ob.used, len);
  EXPECT_EQ(memcmp(got.data(), data, len), 0);
  gcomp_decoder_destroy(dec);

  ASSERT_EQ(gcomp_encoder_finish(enc, &out), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_SYNC),
      GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

TEST(Brotli, EmptyFlushWritesNothing) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "brotli", nullptr,
                &enc),
      GCOMP_OK);
  uint8_t buf[8];
  gcomp_buffer_t out = {buf, sizeof(buf), 0};
  ASSERT_EQ(gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_SYNC), GCOMP_OK);
  EXPECT_EQ(out.used, 0u);
  ASSERT_EQ(gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_FULL), GCOMP_OK);
  EXPECT_EQ(out.used, 0u);
  ASSERT_EQ(gcomp_encoder_finish(enc, &out), GCOMP_OK);
  ASSERT_EQ(out.used, 1u);
  EXPECT_EQ(buf[0], 0x06);
  gcomp_encoder_destroy(enc);
}

TEST(Brotli, RejectsWindowOutsideTheSpec) {
  for (int lgwin : {9, 25}) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "brotli.lgwin", lgwin), GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(gcomp_registry_default(), "brotli", opts,
                  &enc),
        GCOMP_ERR_INVALID_ARG)
        << lgwin;
    EXPECT_EQ(enc, nullptr);
    gcomp_options_destroy(opts);
  }
}

TEST(Brotli, CorruptWindow) {
  const uint8_t bad = 0x11;
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  uint8_t outb[8];
  gcomp_buffer_t in = {&bad, 1, 0};
  gcomp_buffer_t out = {outb, sizeof(outb), 0};
  gcomp_status_t st = gcomp_decoder_update(dec, &in, &out);
  if (st == GCOMP_OK) {
    st = gcomp_decoder_finish(dec, &out);
  }
  EXPECT_EQ(st, GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

TEST(Brotli, TruncatedStreamIsCorrupt) {
  const uint8_t partial[] = {0x0c};
  uint8_t outb[8];
  size_t written = 0;
  EXPECT_EQ(gcomp_decode_buffer(nullptr, "brotli", nullptr, partial,
                sizeof(partial), outb, sizeof(outb), &written),
      GCOMP_ERR_CORRUPT);
}

TEST(Brotli, EmptyInputIsCorrupt) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  uint8_t outb[4];
  gcomp_buffer_t out = {outb, sizeof(outb), 0};
  EXPECT_EQ(gcomp_decoder_finish(dec, &out), GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

TEST(Brotli, TrailingBytesStayUnconsumed) {
  const uint8_t inb[] = {0x06, 0xff};
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  uint8_t outb[4];
  gcomp_buffer_t in = {inb, sizeof(inb), 0};
  gcomp_buffer_t out = {outb, sizeof(outb), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in, &out), GCOMP_OK);
  EXPECT_EQ(in.used, 1u);
  EXPECT_EQ(out.used, 0u);
  EXPECT_EQ(gcomp_decoder_finish(dec, &out), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST(Brotli, PeekReportsTheWindow) {
  gcomp_stream_info_t info;
  size_t needed = 0;
  memset(&info, 0x5a, sizeof(info));
  EXPECT_EQ(gcomp_peek(nullptr, "brotli", nullptr, nullptr, 0, &info, &needed),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(needed, 1u);

  const uint8_t byte = 0x06;
  memset(&info, 0, sizeof(info));
  ASSERT_EQ(gcomp_peek(nullptr, "brotli", nullptr, &byte, 1, &info, nullptr),
      GCOMP_OK);
  EXPECT_EQ(info.header_size, 1u);
  EXPECT_EQ(info.window_size, (1u << 16) - 16u);
  EXPECT_EQ(info.has_content_size, 0);
  EXPECT_EQ(info.has_checksum, 0);

  const uint8_t bad = 0x11;
  EXPECT_EQ(gcomp_peek(nullptr, "brotli", nullptr, &bad, 1, &info, nullptr),
      GCOMP_ERR_CORRUPT);
}

TEST(Brotli, LibbrotliIsActuallyAvailable) {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }
  }
  ASSERT_TRUE(brotli_lib().ok())
      << "libbrotlienc.so.1 and libbrotlidec.so.1 could not be loaded. The "
         "pinned copies are in the oracle image; `make check-oracle` runs "
         "this binary there. Set GCOMP_SKIP_ORACLE_TESTS=1 to skip this on a "
         "machine that has no libbrotli.";
}

namespace {

/// One input, and the level-1 construct it is here to put in front of the
/// reference decoder.
struct OracleCase {
  const char * what;
  std::vector<uint8_t> data;
};

/// Literal frequencies that fall off like a Fibonacci sequence, so the
/// optimal Huffman tree is as deep as an alphabet of this size can make it.
std::vector<uint8_t> SkewedAlphabet(size_t n) {
  std::vector<uint8_t> data;
  data.reserve(n);
  uint64_t f1 = 1;
  uint64_t f2 = 1;
  for (int sym = 0; sym < 24 && data.size() < n; sym++) {
    const uint64_t count = f1;
    f1 = f2;
    f2 = count + f2;
    for (uint64_t i = 0; i < count && data.size() < n; i++) {
      data.push_back((uint8_t)(sym + 1));
    }
  }
  while (data.size() < n) {
    data.push_back(25);
  }
  // Shuffled deterministically: the frequencies are what matter, and a sorted
  // buffer would collapse into one long match and encode no literals at all.
  std::vector<uint8_t> noise(data.size() * 2);
  test_helpers_generate_random(noise.data(), noise.size(), 71);
  for (size_t i = 0; i < data.size(); i++) {
    const size_t j =
        (size_t)(noise[2 * i] | (noise[2 * i + 1] << 8)) % data.size();
    std::swap(data[i], data[j]);
  }
  return data;
}

std::vector<OracleCase> OracleCases() {
  std::vector<OracleCase> cases;

  // One literal symbol, so every prefix code in the block is the single-symbol
  // form that carries no lengths at all.
  cases.push_back({"one byte repeated", std::vector<uint8_t>(8000, 'a')});

  // One long match, two literal symbols: the shape the sweep used to be.
  std::vector<uint8_t> repeat(4000);
  test_helpers_generate_pattern(repeat.data(), repeat.size(),
      reinterpret_cast<const uint8_t *>("brotli"), 6);
  cases.push_back({"a repeated six-byte pattern", repeat});

  // A wide literal alphabet, so the literal code is a complex prefix code
  // with real depth rather than a handful of symbols.
  std::vector<uint8_t> text;
  const char * phrase =
      "The quick brown fox jumps over the lazy dog, and then (0x2A!) "
      "writes 12,345 bytes of mixed-case prose with punctuation. ";
  while (text.size() < 40000) {
    text.insert(text.end(), phrase, phrase + strlen(phrase));
  }
  text.resize(40000);
  cases.push_back({"prose with a wide literal alphabet", text});

  // Every byte value, so the literal alphabet is the whole 256 symbols.
  std::vector<uint8_t> all(2048);
  for (size_t i = 0; i < 256; i++) {
    all[i] = (uint8_t)i;
  }
  for (size_t i = 256; i < all.size(); i++) {
    all[i] = all[i - 256];
  }
  cases.push_back({"all 256 literal values", all});

  // Deep enough to exceed the fifteen-bit limit a Brotli prefix code has, so
  // the length-limited fallback is what writes the code.
  cases.push_back({"a Fibonacci-skewed alphabet", SkewedAlphabet(200000)});

  // Two, three and four distinct literals are the three simple prefix codes,
  // and the symbols are deliberately not in increasing byte order, which is
  // the part a canonical code has to get right.
  static const char * sets[] = {"\xff\x01", "\xff\x01\x80", "\xff\x01\x80\x40"};
  for (int s = 0; s < 3; s++) {
    const size_t distinct = (size_t)s + 2;
    std::vector<uint8_t> few(40000);
    for (size_t i = 0; i < few.size(); i++) {
      few[i] = (uint8_t)sets[s][(i * 7 + i / 13) % distinct];
    }
    cases.push_back(
        {distinct == 2 ? "two distinct literals, descending"
                       : (distinct == 3 ? "three distinct literals, descending"
                                        : "four distinct literals, descending"),
            few});
  }

  // A block that compresses, then bytes that cannot match: the last command
  // is insert-only, whose copy length and distance the format says to ignore
  // once the meta-block length is satisfied. Getting that wrong writes a
  // distance the decoder does not read, and the stream desyncs.
  std::vector<uint8_t> tail(60005);
  for (size_t i = 0; i < 60000; i++) {
    tail[i] = (uint8_t)"abcdefgh"[i % 8];
  }
  for (size_t i = 0; i < 5; i++) {
    tail[60000 + i] = (uint8_t)(0x90 + i * 7);
  }
  cases.push_back({"a compressible block with an insert-only tail", tail});

  // Incompressible, so the chunk is stored rather than Huffman-coded.
  std::vector<uint8_t> noise(50000);
  test_helpers_generate_random(noise.data(), noise.size(), 29);
  cases.push_back({"noise, which falls back to a stored block", noise});

  // Past one chunk, so the stream is several meta-blocks and the distance
  // ring buffer has to survive the boundaries.
  std::vector<uint8_t> many(600000);
  for (size_t i = 0; i < many.size(); i++) {
    many[i] = (uint8_t)("lorem ipsum dolor sit amet "[i % 27]);
  }
  cases.push_back({"more than one chunk", many});

  // A match further back than the distance alphabet's widths used to reach,
  // which is only offered to the matcher at lgwin 18 and above.
  std::vector<uint8_t> far(250000);
  for (size_t i = 0; i < far.size(); i++) {
    far[i] = (uint8_t)("lorem ipsum dolor sit amet consectetur "[i % 39]);
  }
  std::vector<uint8_t> block(8192);
  test_helpers_generate_random(block.data(), block.size(), 23);
  memcpy(far.data(), block.data(), block.size());
  memcpy(far.data() + 232000, block.data(), block.size());
  cases.push_back({"a match 232000 bytes back", far});

  return cases;
}

} // namespace

/**
 * libbrotli's bytes, read by us, at every quality it offers and on input it
 * has reason to split into more than one block type.
 *
 * `ReadsLibbrotliStreams` swept qualities 0, 5 and 11 over four small inputs
 * and passed throughout, while the decoder could not read *any* stream with
 * two or more block types: RFC 7932 has each such category carry the count of
 * its first block, and the decoder never read those bits, so every field after
 * them came from the wrong bit position. What hid it was the inputs - a 43-byte
 * sentence, a thousand zeros, two hundred random bytes and eight thousand
 * sequential ones give libbrotli no reason to split anything.
 *
 * Noise followed by markup does. Qualities 4 through 9 produced streams this
 * refused with "bad code-length code", after reading a block-type count of 162
 * for the insert-and-copy category. The sweep is per quality because that is
 * the axis the defect lived on: 0 to 3 and 10 to 11 were fine on this input
 * and would have gone on being fine.
 */
TEST(Brotli, EveryLibbrotliQualityIsReadIncludingTheBlockSplittingOnes) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  const char * markup =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<title>index</title></head><body><div class=\"content\">"
      "<a href=\"https://example.com/index.html\">link</a></div>"
      "<script src=\"/static/app.js\"></script></body></html>";
  std::vector<uint8_t> data(24000);
  test_helpers_generate_random(data.data(), 12000, 5);
  for (size_t i = 12000; i < data.size(); i++) {
    data[i] = (uint8_t)markup[(i - 12000) % strlen(markup)];
  }

  for (int quality = 0; quality <= 11; quality++) {
    for (int lgwin : {16, 22}) {
      std::vector<uint8_t> enc =
          lib_compress(data.data(), data.size(), quality, lgwin);
      ASSERT_FALSE(enc.empty()) << "q" << quality << " lgwin " << lgwin;
      std::vector<uint8_t> dec =
          decode_bytes(enc.data(), enc.size(), data.size() + 64);
      ASSERT_EQ(dec.size(), data.size()) << "q" << quality << " lgwin " << lgwin;
      EXPECT_EQ(memcmp(dec.data(), data.data(), data.size()), 0)
          << "q" << quality << " lgwin " << lgwin;
    }
  }
}

/**
 * Our bytes, read by libbrotli.
 *
 * This is the only test that says our encoder writes RFC 7932 rather than
 * something only our own decoder happens to agree with, so it has to carry
 * every construct level 1 can emit. It used to hand over two inputs - 8000
 * copies of one byte and a repeated six-byte pattern - which between them
 * reached one long match and at most six literal symbols. The complex prefix
 * code, the fifteen-bit length limit, the three simple codes' symbol order and
 * the insert-only final command were all checked against our own decoder only,
 * which is no check at all if the two agree about the same mistake.
 */
TEST(Brotli, LibbrotliReadsOurStreams) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  for (const OracleCase & c : OracleCases()) {
    for (int lgwin : {16, 22}) {
      std::vector<uint8_t> enc =
          encode_bytes(c.data.data(), c.data.size(), lgwin);
      ASSERT_FALSE(enc.empty()) << c.what << " lgwin " << lgwin;
      std::vector<uint8_t> out(c.data.size() + 64);
      size_t written = out.size();
      ASSERT_EQ(lib.decompress(enc.size(), enc.data(), &written, out.data()), 1)
          << c.what << " lgwin " << lgwin;
      ASSERT_EQ(written, c.data.size()) << c.what << " lgwin " << lgwin;
      EXPECT_EQ(memcmp(out.data(), c.data.data(), c.data.size()), 0)
          << c.what << " lgwin " << lgwin;
    }
  }
}

/**
 * Every one of the 121 word transformations, against libbrotli's own.
 *
 * The decoder's transforms were reached only by whatever words the handful of
 * quality-11 streams in `ReadsLibbrotliStreams` happened to use, which left
 * most of the prefix, suffix, ferment and omit arms untaken - `brotli_dict.c`
 * was the least covered file in the library. libbrotli exports the transform
 * it applies, so the whole table can be asked directly rather than hoped for.
 * Both sides are given the same word, so what is compared is the transform and
 * not the dictionary, which the round trips already agree about.
 */
TEST(Brotli, EveryWordTransformMatchesLibbrotli) {
  void * common = dlopen("libbrotlicommon.so.1", RTLD_NOW);
  using GetTransforms = const void * (*)();
  using TransformWord = int (*)(uint8_t *, const uint8_t *, int, const void *,
      int);
  GetTransforms get = nullptr;
  TransformWord apply = nullptr;
  if (common) {
    get = reinterpret_cast<GetTransforms>(
        dlsym(common, "BrotliGetTransforms"));
    apply = reinterpret_cast<TransformWord>(
        dlsym(common, "BrotliTransformDictionaryWord"));
  }
  if (!get || !apply) {
    GTEST_SKIP() << "libbrotlicommon.so.1 does not export its transforms";
  }
  const void * transforms = get();
  const uint8_t * dict = brotli_dict_data();
  const uint32_t * offsets = brotli_dict_offset();
  const uint8_t * ndbits = brotli_dict_ndbits();

  size_t compared = 0;
  for (int len = 4; len <= 24; len++) {
    const uint32_t words = 1u << ndbits[len];
    // First, last, and one in the middle of each length class.
    const uint32_t picks[3] = {0, words / 2, words - 1};
    for (uint32_t pick : picks) {
      const uint8_t * word = dict + offsets[len] + pick * (uint32_t)len;
      for (int t = 0; t <= 120; t++) {
        uint8_t ours[128];
        uint8_t theirs[128];
        int ours_len = 0;
        memset(ours, 0, sizeof(ours));
        memset(theirs, 0, sizeof(theirs));
        const uint64_t word_id = (uint64_t)t * words + pick;
        ASSERT_EQ(brotli_dict_word(len, word_id, ours, (int)sizeof(ours),
                      &ours_len),
            0)
            << "len " << len << " word " << pick << " transform " << t;
        // Zero is a real answer, not a failure: an omit transform that cuts
        // more characters than a four-byte word has leaves nothing, and with
        // no prefix or suffix the transformed word is empty. libbrotli
        // reports 0 for those, and so does this.
        const int theirs_len = apply(theirs, word, len, transforms, t);
        ASSERT_GE(theirs_len, 0) << "len " << len << " transform " << t;
        EXPECT_EQ(ours_len, theirs_len)
            << "len " << len << " word " << pick << " transform " << t;
        if (ours_len == theirs_len && ours_len > 0) {
          EXPECT_EQ(memcmp(ours, theirs, (size_t)ours_len), 0)
              << "len " << len << " word " << pick << " transform " << t
              << ": '" << std::string((const char *)ours, (size_t)ours_len)
              << "' against '"
              << std::string((const char *)theirs, (size_t)theirs_len) << "'";
        }
        compared++;
      }
    }
  }
  EXPECT_EQ(compared, 21u * 3u * 121u);
  dlclose(common);
}

TEST(Brotli, ReadsLibbrotliStreams) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  const char * fox = "The quick brown fox jumps over the lazy dog";
  std::vector<uint8_t> text(reinterpret_cast<const uint8_t *>(fox),
      reinterpret_cast<const uint8_t *>(fox) + strlen(fox));
  std::vector<uint8_t> zeros(1000, 0);
  std::vector<uint8_t> noise(200);
  test_helpers_generate_random(noise.data(), noise.size(), 11);
  std::vector<uint8_t> mixed(8000);
  test_helpers_generate_sequential(mixed.data(), mixed.size());

  const std::vector<uint8_t> * inputs[] = {&text, &zeros, &noise, &mixed};
  for (int quality : {0, 5, 11}) {
    for (int lgwin : {16, 22}) {
      for (const std::vector<uint8_t> * input : inputs) {
        std::vector<uint8_t> enc =
            lib_compress(input->data(), input->size(), quality, lgwin);
        ASSERT_FALSE(enc.empty()) << quality << " " << lgwin;
        std::vector<uint8_t> dec =
            decode_bytes(enc.data(), enc.size(), input->size() + 64);
        ASSERT_EQ(dec.size(), input->size()) << quality << " " << lgwin;
        EXPECT_EQ(memcmp(dec.data(), input->data(), input->size()), 0)
            << quality << " " << lgwin;
      }
    }
  }
}

TEST(Brotli, ByteAtATimeLibbrotli) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  const char * fox = "The quick brown fox jumps over the lazy dog";
  std::vector<uint8_t> text(reinterpret_cast<const uint8_t *>(fox),
      reinterpret_cast<const uint8_t *>(fox) + strlen(fox));
  std::vector<uint8_t> enc = lib_compress(text.data(), text.size(), 11, 16);
  ASSERT_FALSE(enc.empty());

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(gcomp_registry_default(), "brotli", nullptr,
                &dec),
      GCOMP_OK);
  std::vector<uint8_t> out(text.size() + 8);
  size_t filled = 0;
  for (size_t i = 0; i < enc.size(); i++) {
    gcomp_buffer_t ib = {enc.data() + i, 1, 0};
    gcomp_buffer_t ob = {out.data() + filled, out.size() - filled, 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK) << i;
    filled += ob.used;
  }
  gcomp_buffer_t tail = {out.data() + filled, out.size() - filled, 0};
  ASSERT_EQ(gcomp_decoder_finish(dec, &tail), GCOMP_OK);
  filled += tail.used;
  gcomp_decoder_destroy(dec);
  ASSERT_EQ(filled, text.size());
  EXPECT_EQ(memcmp(out.data(), text.data(), text.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
