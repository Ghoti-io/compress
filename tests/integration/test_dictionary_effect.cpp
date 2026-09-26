/**
 * @file test_dictionary_effect.cpp
 *
 * A dictionary that is accepted but not used looks exactly like one that
 * works: the stream round-trips either way, the dictionary ID is written, and
 * every existing test passes. The only thing that tells the two apart is
 * whether the output actually gets smaller.
 *
 * So this measures. The case is the strongest one there is - the payload is
 * the dictionary, byte for byte - where a working dictionary turns the whole
 * input into one long match and a few bytes of framing. The reference
 * implementation puts 4096 such bytes in 21 bytes; anything in the hundreds
 * means the dictionary content is not reaching the match finder.
 *
 * ## What this found, and what was done about it
 *
 * The mechanism was sound for content whose four-byte prefixes are mostly
 * distinct - a dictionary worked at every level there, taking 4096 bytes to
 * 18 - and useless at the low levels on data with very few distinct prefixes.
 * With a twelve-word vocabulary, 4091 bytes measured 739 at level 1 and 655 at
 * level 3, the default, against 17 from level 6 up.
 *
 * That was the chain running out of attempts rather than the dictionary
 * failing to load. The chain is walked newest-first for at most `search_depth`
 * candidates - four at level 1, sixteen at level 3 - and where thousands of
 * positions share a hash, every one of those is recent and none is in the
 * dictionary.
 *
 * The dictionary now has a search budget of its own and a way into its part of
 * the chain that does not run through the recent part first
 * (`zstd_mf_note_dictionary`, and ZSTD_MF_DICT_MIN_DEPTH in
 * zstd_matchfinder.c). On that same twelve-word input every level from 1 to 19
 * now reaches 17 bytes.
 *
 * Worth recording, because it was measured rather than assumed: libzstd is
 * erratic on this shape. On the identical dictionary and payload it produces
 * 810 bytes at level 1, 810 at level 2, 21 at level 3, 661 at level 4 - worse
 * than level 3 - 345 at level 5, and 23 at level 6. An earlier version of this
 * comment said libzstd handled these levels properly and we did not; that was
 * wrong, and the measurement is above.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Content whose four-byte prefixes are almost all distinct.
std::vector<uint8_t> distinct_prefixes(size_t len, uint32_t seed) {
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

/// Encode with the given dictionary, and check it reads back.
size_t encode_with_dict(const char * method, const std::vector<uint8_t> & input,
    const std::vector<uint8_t> * dict, int64_t level) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  if (std::strcmp(method, "zstd") == 0) {
    EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
  }
  if (dict) {
    EXPECT_EQ(gcomp_options_set_bytes(o,
                  std::string(method).append(".dictionary").c_str(),
                  dict->data(), dict->size()),
        GCOMP_OK);
  }

  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, method, o, input.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, method, o, input.data(), input.size(),
                out.data(), out.size(), &written),
      GCOMP_OK);

  // A small stream is worth nothing if it does not decode back, and a
  // dictionary is the easiest way to produce one that does not: the decoder
  // has to load the same bytes as history before the first block.
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  std::vector<uint8_t> back(input.size() + 1024);
  size_t produced = 0;
  EXPECT_EQ(gcomp_decode_buffer(nullptr, method, o, out.data(), written,
                back.data(), back.size(), &produced),
      GCOMP_OK)
      << method << " level " << level << ": dictionary stream did not decode";
  EXPECT_EQ(produced, input.size()) << method << " level " << level;
  if (produced == input.size() && produced > 0) {
    EXPECT_EQ(std::memcmp(back.data(), input.data(), produced), 0)
        << method << " level " << level;
  }

  gcomp_options_destroy(o);
  return written;
}

/**
 * @brief As encode_with_dict, with a thread count instead of a level.
 *
 * Deliberately a second function rather than another parameter on the first:
 * every caller of that one is a level sweep, and adding a thread argument to it
 * would have let the combination look covered while no test passed anything but
 * the default.
 */
size_t encode_with_dict_threads(const std::vector<uint8_t> & input,
    const std::vector<uint8_t> * dict, uint64_t threads) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "threads.count", threads), GCOMP_OK);
  if (dict) {
    EXPECT_EQ(gcomp_options_set_bytes(
                  o, "zstd.dictionary", dict->data(), dict->size()),
        GCOMP_OK);
  }

  size_t bound = 0;
  EXPECT_EQ(
      gcomp_encode_bound(nullptr, "zstd", o, input.size(), &bound), GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, input.data(), input.size(),
                out.data(), out.size(), &written),
      GCOMP_OK)
      << "threads=" << threads;

  EXPECT_EQ(gcomp_options_set_bool(o, "zstd.concat", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  std::vector<uint8_t> back(input.size() + 65536);
  size_t produced = 0;
  EXPECT_EQ(gcomp_decode_buffer(nullptr, "zstd", o, out.data(), written,
                back.data(), back.size(), &produced),
      GCOMP_OK)
      << "threads=" << threads << ": dictionary stream did not decode";
  EXPECT_EQ(produced, input.size()) << "threads=" << threads;
  if (produced == input.size() && produced > 0) {
    EXPECT_EQ(std::memcmp(back.data(), input.data(), produced), 0)
        << "threads=" << threads;
  }

  gcomp_options_destroy(o);
  return written;
}

/**
 * @brief The dictionary reaches the match finder, at every level.
 *
 * Payload identical to the dictionary: one long match, and almost nothing
 * else. The threshold is deliberately loose - a fortieth of the input - because
 * the point is to tell "the dictionary is being searched" from "it is not",
 * and those differ by a factor of two hundred, not by a few percent.
 */
TEST(DictionaryEffect, ZstdUsesTheDictionaryAtEveryLevel) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 12345);
  const std::vector<uint8_t> payload = dict;

  for (int64_t level : {1, 3, 6, 9, 12, 19}) {
    const size_t without = encode_with_dict("zstd", payload, nullptr, level);
    const size_t with = encode_with_dict("zstd", payload, &dict, level);

    EXPECT_LT(with, payload.size() / 40)
        << "zstd level " << level << ": " << with
        << " bytes for a payload identical to its dictionary; the dictionary "
           "is not reaching the match finder";
    EXPECT_LT(with * 10, without)
        << "zstd level " << level << ": " << with << " with a dictionary "
        << "against " << without << " without";
  }
}

/**
 * @brief The same, with threads, which is where it was not true at all.
 *
 * `zstd.dictionary` with `threads.count > 1` was accepted, reported GCOMP_OK,
 * and did nothing. `zstd_parallel.c` had no mention of a dictionary anywhere and
 * nothing refused the combination, so the option was silently inert above one
 * thread - and the frame header still declared the Dictionary_ID, which made the
 * output hard-require a dictionary it had never used.
 *
 * Measured before the fix, on 8,800 bytes whose content the dictionary held:
 * one thread went 61 bytes to 17, four threads stayed at 64.
 *
 * Nothing existing could see it. Every dictionary test above goes through
 * `gcomp_encode_buffer()` with no thread option, and every parallel test uses no
 * dictionary, so the combination had no coverage in either direction - and a
 * round trip passes regardless, because a dictionary that is loaded and never
 * consulted produces a perfectly ordinary stream.
 *
 * Enough input to be several jobs is the point of the size: one job would let a
 * fix that only seeded the first job pass.
 */
TEST(DictionaryEffect, ZstdUsesTheDictionaryAtEveryThreadCount) {
  const std::vector<uint8_t> dict = distinct_prefixes(65536, 4242);
  // The dictionary repeated, so every job's content is matchable against it and
  // the input spans several default 512 KB jobs.
  std::vector<uint8_t> payload;
  for (int i = 0; i < 24; i++) {
    payload.insert(payload.end(), dict.begin(), dict.end());
  }

  size_t single_with = 0;
  for (uint64_t threads : {(uint64_t)1, (uint64_t)2, (uint64_t)4}) {
    const size_t without =
        encode_with_dict_threads(payload, nullptr, threads);
    const size_t with = encode_with_dict_threads(payload, &dict, threads);
    if (threads == 1) {
      single_with = with;
    }

    EXPECT_LT(with * 4, without)
        << "threads=" << threads << ": " << with << " bytes with a dictionary "
        << "against " << without << " without - the dictionary is not reaching "
           "the jobs' match finders";

    // And it is worth about what it is worth single-threaded. A parallel job
    // sees the dictionary plus a window of the real stream, so the figures are
    // close but not equal; a factor of two is the bound that separates "the
    // dictionary is being used" from "only the first job got it".
    EXPECT_LT(with, single_with * 2 + 4096)
        << "threads=" << threads << ": " << with
        << " bytes against " << single_with << " on one thread";
  }
}

/**
 * @brief A dictionary survives a full flush in parallel mode.
 *
 * Each frame after the flush declares the same Dictionary_ID, so the dictionary
 * applies to it too - that is what the single-threaded rearm does, and what a
 * full flush drops is the stream before it rather than the history the caller
 * supplied. Without this the second frame would claim a dictionary it had
 * stopped using, which is the same false dependency in a new place.
 */
TEST(DictionaryEffect, ZstdKeepsTheDictionaryAcrossAParallelFullFlush) {
  const std::vector<uint8_t> dict = distinct_prefixes(65536, 7777);
  std::vector<uint8_t> half;
  for (int i = 0; i < 12; i++) {
    half.insert(half.end(), dict.begin(), dict.end());
  }

  auto run = [&](uint64_t threads, bool with_dict) {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_uint64(o, "threads.count", threads), GCOMP_OK);
    if (with_dict) {
      EXPECT_EQ(gcomp_options_set_bytes(
                    o, "zstd.dictionary", dict.data(), dict.size()),
          GCOMP_OK);
    }
    gcomp_registry_t * reg = gcomp_registry_default();
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(reg, "zstd", o, &enc), GCOMP_OK);
    if (!enc) {
      gcomp_options_destroy(o);
      return (size_t)0;
    }

    std::vector<uint8_t> out;
    std::vector<uint8_t> win(65536);
    auto push = [&](const uint8_t * d, size_t n) {
      gcomp_buffer_t in = {const_cast<uint8_t *>(d), n, 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {win.data(), win.size(), 0};
        EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
        out.insert(out.end(), win.begin(), win.begin() + ob.used);
      }
    };
    push(half.data(), half.size());
    for (;;) {
      gcomp_buffer_t ob = {win.data(), win.size(), 0};
      const gcomp_status_t st =
          gcomp_encoder_flush(enc, &ob, GCOMP_FLUSH_FULL);
      out.insert(out.end(), win.begin(), win.begin() + ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      EXPECT_EQ(st, GCOMP_ERR_LIMIT);
    }
    // The second half after the flush is what the dictionary has to still be
    // serving: the stream before it has been dropped, so the dictionary is the
    // only history left.
    push(half.data(), half.size());
    for (;;) {
      gcomp_buffer_t ob = {win.data(), win.size(), 0};
      const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
      out.insert(out.end(), win.begin(), win.begin() + ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      EXPECT_EQ(st, GCOMP_ERR_LIMIT);
    }
    gcomp_encoder_destroy(enc);

    // It must still read back, with the dictionary the header names.
    gcomp_options_set_bool(o, "zstd.concat", 1);
    gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0);
    std::vector<uint8_t> back(half.size() * 2 + 65536);
    size_t produced = 0;
    EXPECT_EQ(gcomp_decode_buffer(nullptr, "zstd", o, out.data(), out.size(),
                  back.data(), back.size(), &produced),
        GCOMP_OK)
        << "threads=" << threads;
    EXPECT_EQ(produced, half.size() * 2) << "threads=" << threads;
    gcomp_options_destroy(o);
    return out.size();
  };

  for (uint64_t threads : {(uint64_t)1, (uint64_t)4}) {
    const size_t without = run(threads, false);
    const size_t with = run(threads, true);
    EXPECT_LT(with * 4, without)
        << "threads=" << threads << ": " << with
        << " bytes with a dictionary across a full flush against " << without
        << " without - the flush dropped the dictionary along with the stream";
  }
}

/// The same for LZ4, which indexes its dictionary for every independent block.
TEST(DictionaryEffect, Lz4UsesTheDictionary) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 999);
  const std::vector<uint8_t> payload = dict;

  const size_t without = encode_with_dict("lz4", payload, nullptr, 0);
  const size_t with = encode_with_dict("lz4", payload, &dict, 0);

  EXPECT_LT(with, payload.size() / 10)
      << "lz4: " << with << " bytes for a payload identical to its dictionary";
  EXPECT_LT(with * 4, without) << "lz4: " << with << " against " << without;
}

/**
 * @brief A dictionary that is only part of the payload still pays.
 *
 * The realistic shape: many small records drawn from a shared vocabulary,
 * where the dictionary holds what they have in common and each record is
 * mostly not in it.
 */
TEST(DictionaryEffect, HelpsASmallRecord) {
  const std::vector<uint8_t> dict = distinct_prefixes(8192, 4242);
  // A record built out of pieces of the dictionary, in a different order.
  std::vector<uint8_t> record;
  for (int i = 0; i < 8; i++) {
    size_t at = (size_t)(i * 887) % (dict.size() - 64);
    record.insert(record.end(), dict.begin() + at, dict.begin() + at + 64);
  }

  for (int64_t level : {1, 3, 9}) {
    const size_t without = encode_with_dict("zstd", record, nullptr, level);
    const size_t with = encode_with_dict("zstd", record, &dict, level);
    EXPECT_LT(with, without)
        << "zstd level " << level << ": a dictionary holding every byte of the "
        << "record did not make it smaller (" << with << " against " << without
        << ")";
  }
}

/**
 * @brief The decoder needs the same dictionary, and says so when it has none.
 *
 * A stream encoded against a dictionary and decoded without one must not
 * quietly produce different bytes.
 */
TEST(DictionaryEffect, DecodingWithoutTheDictionaryFails) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 31337);
  const std::vector<uint8_t> payload = dict;

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                o, "zstd.dictionary", dict.data(), dict.size()),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.dictionary_id", 0xABCDu),
      GCOMP_OK);

  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", o, payload.data(),
                payload.size(), &enc, &enc_len),
      GCOMP_OK);
  gcomp_options_destroy(o);

  // peek says which dictionary is wanted, which is how a reader fails usefully.
  gcomp_stream_info_t info;
  ASSERT_EQ(
      gcomp_peek(nullptr, "zstd", nullptr, enc, enc_len, &info, nullptr),
      GCOMP_OK);
  EXPECT_TRUE(info.has_dictionary);
  EXPECT_EQ(info.dictionary_id, 0xABCDu);

  void * dec = nullptr;
  size_t dec_len = 0;
  gcomp_status_t s =
      gcomp_decode_alloc(nullptr, "zstd", nullptr, enc, enc_len, &dec, &dec_len);
  EXPECT_NE(s, GCOMP_OK)
      << "decoded a dictionary stream without the dictionary, producing "
      << dec_len << " bytes";
  gcomp_buffer_free(nullptr, dec);
  gcomp_buffer_free(nullptr, enc);
}

} // namespace

/// Text drawn from a tiny vocabulary: thousands of positions per hash.
static std::vector<uint8_t> colliding_prefixes(size_t cap, unsigned seed) {
  static const char * const words[12] = {"alpha", "bravo", "charlie", "delta",
      "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima"};
  std::vector<uint8_t> v;
  unsigned s = seed;
  while (v.size() < cap) {
    s = s * 1103515245u + 12345u;
    const char * w = words[(s >> 16) % 12];
    const size_t l = std::strlen(w);
    if (v.size() + l + 1 > cap) {
      break;
    }
    v.insert(v.end(), w, w + l);
    v.push_back(' ');
  }
  return v;
}

/**
 * @brief The dictionary is reached even where every position collides.
 *
 * This is the case the header describes. Before the dictionary had a budget of
 * its own it failed at levels 1 to 4 - 739 bytes at level 1 where 17 was
 * available - because the level's whole search_depth went on recent positions
 * that share a hash with the one being matched, and the dictionary sits behind
 * all of them.
 *
 * The threshold is a fortieth of the input, as elsewhere in this file: the
 * question is whether the dictionary is being searched at all, and the answer
 * differs by a factor of forty, not by a few percent.
 */
TEST(DictionaryEffect, ZstdReachesTheDictionaryWhenEveryPrefixCollides) {
  const std::vector<uint8_t> dict = colliding_prefixes(4096, 7);
  const std::vector<uint8_t> payload = dict;
  ASSERT_GT(dict.size(), 4000u);

  for (int64_t level : {1, 2, 3, 4, 5, 6, 9, 19}) {
    const size_t without = encode_with_dict("zstd", payload, nullptr, level);
    const size_t with = encode_with_dict("zstd", payload, &dict, level);

    EXPECT_LT(with, payload.size() / 40)
        << "zstd level " << level << ": " << with
        << " bytes for a payload identical to its dictionary; the level's own "
           "search budget is being spent before the dictionary is reached";
    EXPECT_LT(with * 10, without)
        << "zstd level " << level << ": " << with << " with a dictionary "
        << "against " << without << " without";
  }
}

/**
 * @brief A dictionary that slides out of the window stays decodable.
 *
 * The shortcut into the dictionary's part of the chain names window positions,
 * and the window moves. If those entries were left behind after a slide they
 * would name bytes that are no longer there, and the encoder would emit an
 * offset reaching past the window it declared - which RFC 8878 section
 * 3.1.1.1.2 does not allow and no decoder can honour.
 *
 * So: a window far smaller than the payload, a dictionary larger than some of
 * those windows, and input fed in chunks of a size that is not a factor of
 * anything, so block and chunk edges fall in different places.
 */
TEST(DictionaryEffect, ADictionaryThatSlidesOutOfTheWindowStaysCorrect) {
  const size_t dict_len = 24000;
  const size_t body_len = 512u * 1024u;
  std::vector<uint8_t> dict(dict_len);
  uint32_t s = 4242u;
  for (size_t i = 0; i < dict_len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    dict[i] = (uint8_t)(s >> 24);
  }
  // Quotes the dictionary at the start, then drifts away from it, so the
  // encoder has reason to reach back early and none later.
  std::vector<uint8_t> body(body_len);
  for (size_t i = 0; i < body_len; i++) {
    if (i < dict_len) {
      body[i] = dict[i];
    }
    else {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      body[i] = (uint8_t)((s >> 24) ^ (i & 0x3F));
    }
  }

  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  for (int64_t level : {1, 3, 6, 9}) {
    for (uint64_t window_log : {10u, 14u, 18u}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.window_log", window_log),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bytes(
                    o, "zstd.dictionary", dict.data(), dict.size()),
          GCOMP_OK);

      gcomp_encoder_t * enc = nullptr;
      ASSERT_EQ(gcomp_encoder_create(reg, "zstd", o, &enc), GCOMP_OK)
          << "level " << level << " window_log " << window_log;

      std::vector<uint8_t> out(body_len + body_len / 4 + 65536);
      gcomp_buffer_t ob = {out.data(), out.size(), 0};
      size_t fed = 0;
      const size_t chunk = 7919; // prime: chunk edges land everywhere
      while (fed < body_len) {
        const size_t take =
            (body_len - fed < chunk) ? (body_len - fed) : chunk;
        gcomp_buffer_t ib = {body.data() + fed, take, 0};
        while (ib.used < ib.size) {
          ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK)
              << "level " << level << " window_log " << window_log;
        }
        fed += take;
      }
      ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
      gcomp_encoder_destroy(enc);

      // Back through our own decoder with the same dictionary.  An offset
      // reaching past the declared window fails here.
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
          GCOMP_OK);
      std::vector<uint8_t> back(body_len + 1024);
      size_t produced = 0;
      ASSERT_EQ(gcomp_decode_buffer(reg, "zstd", o, out.data(), ob.used,
                    back.data(), back.size(), &produced),
          GCOMP_OK)
          << "level " << level << " window_log " << window_log;
      ASSERT_EQ(produced, body_len)
          << "level " << level << " window_log " << window_log;
      ASSERT_EQ(std::memcmp(back.data(), body.data(), body_len), 0)
          << "level " << level << " window_log " << window_log;
      gcomp_options_destroy(o);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
