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

TEST(Brotli, LibbrotliReadsOurStreams) {
  const BrotliLib & lib = brotli_lib();
  if (!lib.ok()) {
    GTEST_SKIP() << "libbrotli is not installed";
  }
  std::vector<uint8_t> data(4000);
  test_helpers_generate_pattern(data.data(), data.size(),
      reinterpret_cast<const uint8_t *>("brotli"), 6);
  for (int lgwin : {16, 22}) {
    std::vector<uint8_t> enc = encode_bytes(data.data(), data.size(), lgwin);
    std::vector<uint8_t> out(data.size() + 8);
    size_t written = out.size();
    ASSERT_EQ(lib.decompress(enc.size(), enc.data(), &written, out.data()), 1)
        << lgwin;
    ASSERT_EQ(written, data.size());
    EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0);
  }
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
