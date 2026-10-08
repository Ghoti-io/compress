/**
 * @file test_alloc_failure.cpp
 *
 * Every allocation failure, in every method, on every path.
 *
 * ## What this tests
 *
 * A successful run of an operation makes some number of allocations, N.  This
 * counts N, then runs the same operation N more times, failing allocation 1,
 * then 2, and so on.  Each run must do one of two things (CONVENTIONS.md
 * section 5):
 *
 * - finish, with output that round-trips; or
 * - return ::GCOMP_ERR_MEMORY.
 *
 * Nothing else is allowed.  ::GCOMP_ERR_INTERNAL for an allocation failure is
 * the wrong code.  ::GCOMP_OK with output that does not decode back to the
 * input is silent data loss, which is the defect this is really hunting - it
 * is what an unchecked allocation in a growth path produces.
 *
 * And whatever the run returned, every block must be freed once the registry,
 * options, encoder and decoder are destroyed.
 *
 * ## Why the streaming scenarios use a tiny output buffer
 *
 * A growth path only runs when a buffer has to be enlarged.  With a large
 * output buffer, an encoder hands everything over as it goes and never has to
 * stage anything.  Sixteen bytes of output forces it to keep what it cannot
 * deliver, which is where the reallocations are - and
 * `tools/coverage.sh`'s list of growth lines no test reaches is the list this
 * is written to empty.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "failing_allocator.h"

#include <ghoti.io/compress/brotli.h>
#include <ghoti.io/compress/bzip2.h>
#include <ghoti.io/compress/filter.h>
#include <ghoti.io/compress/lzma.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <ghoti.io/compress/xz.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

//
// Input shapes
//
// Each is chosen to drive a different part of the encoders: runs exercise the
// match finders and RLE, text exercises the entropy coders, random defeats
// them and forces the stored/raw fallbacks, and the mixed one makes an encoder
// change its mind partway through a stream.
//

std::vector<uint8_t> make_runs(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    v[i] = (uint8_t)((i / 97) & 0xFF);
  }
  return v;
}

std::vector<uint8_t> make_text(size_t len) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "compress", "allocation", "failure", "window"};
  std::string s;
  uint32_t state = 12345u;
  while (s.size() < len) {
    state = state * 1103515245u + 12345u;
    s += words[(state >> 16) % 12];
    s += ((state >> 8) & 7) ? " " : "\n";
  }
  s.resize(len);
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> make_random(size_t len) {
  std::vector<uint8_t> v(len);
  uint32_t state = 987654321u;
  for (size_t i = 0; i < len; i++) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    v[i] = (uint8_t)(state & 0xFF);
  }
  return v;
}

std::vector<uint8_t> make_mixed(size_t len) {
  std::vector<uint8_t> v = make_text(len / 2);
  std::vector<uint8_t> r = make_random(len - v.size());
  v.insert(v.end(), r.begin(), r.end());
  return v;
}

//
// Scenarios
//

/// How the data is pushed through the library.
enum class Mode {
  /// gcomp_encode_buffer() / gcomp_decode_buffer().
  Buffer,
  /// gcomp_encoder_update()/finish() with a 16-byte output buffer.
  StreamingTiny,
  /// As above, plus a gcomp_encoder_flush() partway through.
  StreamingFlush,
  /// Encode a stream, reset, encode a second one through the same encoder.
  ResetReuse,
};

using OptionSetter = std::function<void(gcomp_options_t *)>;

struct Scenario {
  std::string name;
  const char * method;
  OptionSetter set_options;
  std::vector<uint8_t> input;
  Mode mode;
  /// Run every k in 1..N rather than a sample of them.
  bool exhaustive;
  /**
   * @brief Output buffer for the streaming modes.
   *
   * Sixteen bytes by default, to force every staging and growth path.  RLE is
   * the exception: a PackBits literal packet is a length byte and up to 128
   * bytes written as a unit, so its encoder documents a minimum of
   * RLE_MIN_OUTPUT_SPACE (129) bytes of free output and reports
   * GCOMP_ERR_LIMIT below that rather than making no progress.  Asking it for
   * less is testing against a contract the library does not offer.
   */
  size_t out_chunk = 16;
};

/// What a run did, in enough detail to say where it stopped.
struct RunResult {
  gcomp_status_t status = GCOMP_OK;
  bool completed = false; ///< Reached the end with matching bytes.
  std::string stage;      ///< Where it stopped, when it did not complete.
};

/// Register every built-in method.  Deflate first: gzip and zlib wrap it.
gcomp_status_t register_all(gcomp_registry_t * reg) {
  gcomp_status_t s = gcomp_method_deflate_register(reg);
  if (s != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_zlib_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_gzip_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_lz4_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_lzw_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_rle_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_brotli_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_lzma_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_lzma2_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_bzip2_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_delta_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_xz_register(reg)) != GCOMP_OK) {
    return s;
  }
  if ((s = gcomp_method_bcj_register(reg)) != GCOMP_OK) {
    return s;
  }
  return gcomp_method_zstd_register(reg);
}

/**
 * @brief Encode and decode @p s.input, and report what happened.
 *
 * Everything created here is destroyed here, whatever goes wrong, so that the
 * caller's live-block count means what it says.
 */
RunResult run_scenario(const Scenario & s, gcomp_allocator_t * alloc) {
  RunResult r;

  gcomp_registry_t * reg = nullptr;
  gcomp_status_t status = gcomp_registry_create(alloc, &reg);
  if (status != GCOMP_OK) {
    r.status = status;
    r.stage = "registry_create";
    return r;
  }

  status = register_all(reg);
  if (status != GCOMP_OK) {
    r.status = status;
    r.stage = "register";
    gcomp_registry_destroy(reg);
    return r;
  }

  gcomp_options_t * opts = nullptr;
  status = gcomp_options_create_with_allocator(alloc, &opts);
  if (status != GCOMP_OK) {
    r.status = status;
    r.stage = "options_create";
    gcomp_registry_destroy(reg);
    return r;
  }
  if (s.set_options) {
    s.set_options(opts);
    // A setter that could not allocate leaves the option unset; that is a
    // different configuration, not a failure, and the run below is still
    // required to be correct.
  }

  std::vector<uint8_t> encoded;
  std::vector<uint8_t> decoded;

  if (s.mode == Mode::Buffer) {
    // An encode bound does not exist yet (see COMPRESS-PLAN.md phase B), so
    // size generously: twice the input plus a kilobyte covers every method's
    // worst case at these sizes.
    encoded.resize(s.input.size() * 2 + 1024);
    size_t written = 0;
    status = gcomp_encode_buffer(reg, s.method, opts, s.input.data(),
        s.input.size(), encoded.data(), encoded.size(), &written);
    if (status != GCOMP_OK) {
      r.status = status;
      r.stage = "encode_buffer";
      goto done;
    }
    encoded.resize(written);

    decoded.resize(s.input.size() + 64);
    size_t produced = 0;
    status = gcomp_decode_buffer(reg, s.method, opts, encoded.data(),
        encoded.size(), decoded.data(), decoded.size(), &produced);
    if (status != GCOMP_OK) {
      r.status = status;
      r.stage = "decode_buffer";
      goto done;
    }
    decoded.resize(produced);
  }
  else {
    // Streaming, with an output buffer far smaller than the data, so that
    // every method has to stage what it cannot hand over yet.
    gcomp_encoder_t * enc = nullptr;
    status = gcomp_encoder_create(reg, s.method, opts, &enc);
    if (status != GCOMP_OK) {
      r.status = status;
      r.stage = "encoder_create";
      goto done;
    }

    {
      std::vector<uint8_t> outvec(s.out_chunk);
      uint8_t * outbuf = outvec.data();
      const size_t outcap = s.out_chunk;
      const size_t kChunk = 1024;
      size_t consumed = 0;
      bool flushed = false;

      while (consumed < s.input.size()) {
        size_t take = s.input.size() - consumed;
        if (take > kChunk) {
          take = kChunk;
        }
        gcomp_buffer_t in = {
            (uint8_t *)s.input.data() + consumed, take, 0};
        while (in.used < in.size) {
          gcomp_buffer_t out = {outbuf, outcap, 0};
          size_t before = in.used;
          status = gcomp_encoder_update(enc, &in, &out);
          if (status != GCOMP_OK) {
            r.status = status;
            r.stage = "encoder_update";
            gcomp_encoder_destroy(enc);
            goto done;
          }
          encoded.insert(encoded.end(), outbuf, outbuf + out.used);
          if (out.used == 0 && in.used == before) {
            // Neither consumed nor produced, with input still to give: the
            // loop would spin here forever.
            r.status = GCOMP_ERR_INTERNAL;
            r.stage = "encoder_update_no_progress";
            gcomp_encoder_destroy(enc);
            goto done;
          }
        }
        consumed += take;

        if (s.mode == Mode::StreamingFlush && !flushed &&
            consumed >= s.input.size() / 2) {
          flushed = true;
          for (;;) {
            gcomp_buffer_t out = {outbuf, outcap, 0};
            status = gcomp_encoder_flush(enc, &out, GCOMP_FLUSH_SYNC);
            encoded.insert(encoded.end(), outbuf, outbuf + out.used);
            if (status == GCOMP_OK) {
              break;
            }
            if (status == GCOMP_ERR_LIMIT) {
              continue; // More staged output than the buffer holds.
            }
            if (status == GCOMP_ERR_UNSUPPORTED) {
              break; // This method does not flush; not a failure.
            }
            r.status = status;
            r.stage = "encoder_flush";
            gcomp_encoder_destroy(enc);
            goto done;
          }
        }
      }

      // Finish: GCOMP_ERR_LIMIT means more output is staged than fits.
      for (;;) {
        gcomp_buffer_t out = {outbuf, outcap, 0};
        status = gcomp_encoder_finish(enc, &out);
        encoded.insert(encoded.end(), outbuf, outbuf + out.used);
        if (status == GCOMP_OK) {
          break;
        }
        if (status == GCOMP_ERR_LIMIT) {
          continue;
        }
        r.status = status;
        r.stage = "encoder_finish";
        gcomp_encoder_destroy(enc);
        goto done;
      }

      if (s.mode == Mode::ResetReuse) {
        status = gcomp_encoder_reset(enc);
        if (status == GCOMP_ERR_UNSUPPORTED) {
          // Not every method resets; nothing more to check.
          gcomp_encoder_destroy(enc);
          goto decode;
        }
        if (status != GCOMP_OK) {
          r.status = status;
          r.stage = "encoder_reset";
          gcomp_encoder_destroy(enc);
          goto done;
        }
        // The second stream replaces the first, so that what is decoded
        // below is the output of a reset encoder rather than of a fresh one.
        encoded.clear();
        gcomp_buffer_t in = {
            (uint8_t *)s.input.data(), s.input.size(), 0};
        while (in.used < in.size) {
          gcomp_buffer_t out = {outbuf, outcap, 0};
          status = gcomp_encoder_update(enc, &in, &out);
          if (status != GCOMP_OK) {
            r.status = status;
            r.stage = "encoder_update_after_reset";
            gcomp_encoder_destroy(enc);
            goto done;
          }
          encoded.insert(encoded.end(), outbuf, outbuf + out.used);
        }
        for (;;) {
          gcomp_buffer_t out = {outbuf, outcap, 0};
          status = gcomp_encoder_finish(enc, &out);
          encoded.insert(encoded.end(), outbuf, outbuf + out.used);
          if (status == GCOMP_OK) {
            break;
          }
          if (status == GCOMP_ERR_LIMIT) {
            continue;
          }
          r.status = status;
          r.stage = "encoder_finish_after_reset";
          gcomp_encoder_destroy(enc);
          goto done;
        }
      }

      gcomp_encoder_destroy(enc);
    }

  decode:
    gcomp_decoder_t * dec = nullptr;
    status = gcomp_decoder_create(reg, s.method, opts, &dec);
    if (status != GCOMP_OK) {
      r.status = status;
      r.stage = "decoder_create";
      goto done;
    }
    {
      std::vector<uint8_t> outvec(s.out_chunk);
      uint8_t * outbuf = outvec.data();
      const size_t outcap = s.out_chunk;
      // The documented loop (stream.h): stop when a call neither consumes
      // input nor produces output, not when the input is used up - the
      // decoder can still hold bits it has read but not turned into bytes.
      gcomp_buffer_t in = {encoded.data(), encoded.size(), 0};
      for (;;) {
        gcomp_buffer_t out = {outbuf, outcap, 0};
        size_t before = in.used;
        status = gcomp_decoder_update(dec, &in, &out);
        if (status != GCOMP_OK) {
          r.status = status;
          r.stage = "decoder_update";
          gcomp_decoder_destroy(dec);
          goto done;
        }
        decoded.insert(decoded.end(), outbuf, outbuf + out.used);
        if (out.used == 0 && in.used == before) {
          break;
        }
      }
      for (;;) {
        gcomp_buffer_t out = {outbuf, outcap, 0};
        status = gcomp_decoder_finish(dec, &out);
        decoded.insert(decoded.end(), outbuf, outbuf + out.used);
        if (status == GCOMP_OK) {
          break;
        }
        if (status == GCOMP_ERR_LIMIT) {
          continue;
        }
        r.status = status;
        r.stage = "decoder_finish";
        gcomp_decoder_destroy(dec);
        goto done;
      }
      gcomp_decoder_destroy(dec);
    }
  }

  // Got all the way through: the bytes must be the bytes.
  r.completed = (decoded.size() == s.input.size() &&
      (s.input.empty() ||
          std::memcmp(decoded.data(), s.input.data(), s.input.size()) == 0));
  if (!r.completed) {
    r.stage = "roundtrip_mismatch (" + std::to_string(decoded.size()) +
        " bytes out of " + std::to_string(s.input.size()) + ")";
  }

done:
  gcomp_options_destroy(opts);
  gcomp_registry_destroy(reg);
  return r;
}

/**
 * @brief Which allocations to fail.
 *
 * Exhaustive where a scenario is the representative for its method, and
 * otherwise the first and last ten plus a spread through the middle - the
 * ends are where setup and teardown are, and the middle is where the growth
 * paths are.  Deterministic, so a failure reproduces.
 */
std::vector<size_t> select_ks(size_t n, bool exhaustive) {
  std::vector<size_t> ks;
  if (n == 0) {
    return ks;
  }
  if (exhaustive || n <= 60) {
    for (size_t k = 1; k <= n; k++) {
      ks.push_back(k);
    }
    return ks;
  }
  for (size_t k = 1; k <= 10; k++) {
    ks.push_back(k);
  }
  const size_t kSamples = 40;
  for (size_t i = 0; i < kSamples; i++) {
    size_t k = 11 + (size_t)((double)(n - 20) * i / kSamples);
    if (k > 10 && k <= n - 10) {
      ks.push_back(k);
    }
  }
  for (size_t k = (n > 10 ? n - 9 : 1); k <= n; k++) {
    ks.push_back(k);
  }
  return ks;
}

/**
 * @brief Whether this run is under Memcheck.
 *
 * The sweep is one whole encode and decode per allocation, which is over a
 * thousand round trips.  Under valgrind that is hours.
 *
 * It also has less to prove there than anywhere else: the leak checking this
 * test does is its own, and finer than Memcheck's, because the failing
 * allocator knows every block it handed out and can say so the instant an
 * operation returns.  What valgrind adds is the uninitialised-read and
 * overrun checking on the error paths, and a sample of allocations reaches
 * those as well as all of them would.
 *
 * The Makefile's valgrind targets set this.
 */
static bool UnderValgrind() {
  const char * vg = std::getenv("GCOMP_UNDER_VALGRIND");
  return vg && vg[0] == '1';
}

/// Run one scenario's whole sweep.
void sweep(const Scenario & s) {
  // Count pass.
  FailingAllocator counter;
  RunResult base = run_scenario(s, counter.allocator());
  ASSERT_EQ(base.status, GCOMP_OK)
      << s.name << ": the unfailing run did not succeed at " << base.stage;
  ASSERT_TRUE(base.completed)
      << s.name << ": the unfailing run did not round-trip: " << base.stage;
  ASSERT_EQ(counter.live_blocks(), 0u)
      << s.name << ": the unfailing run leaked " << counter.live_bytes()
      << " bytes";

  const size_t n = counter.calls();
  ASSERT_GT(n, 0u) << s.name << ": no allocations at all?";

  // A sweep over a handful of allocations proves very little, and the number
  // is not obvious from the outside: set GCOMP_ALLOC_SWEEP_VERBOSE=1 to see
  // what each scenario is actually covering.
  std::vector<size_t> ks = select_ks(n, s.exhaustive && !UnderValgrind());
  if (UnderValgrind() && ks.size() > 8) {
    std::vector<size_t> trimmed;
    for (size_t i = 0; i < 8; i++) {
      trimmed.push_back(ks[i * ks.size() / 8]);
    }
    ks = trimmed;
  }
  if (std::getenv("GCOMP_ALLOC_SWEEP_VERBOSE")) {
    std::fprintf(stderr, "%-24s allocations=%-5zu injected=%zu\n",
        s.name.c_str(), n, ks.size());
  }

  for (size_t k : ks) {
    FailingAllocator fa;
    fa.fail_at(k);
    RunResult r = run_scenario(s, fa.allocator());

    if (fa.injected_failures() == 0) {
      // This run made fewer allocations than the counting one - an earlier
      // failure changed its shape - so allocation k never happened.
      continue;
    }

    EXPECT_TRUE(r.status == GCOMP_OK || r.status == GCOMP_ERR_MEMORY)
        << s.name << ": failing allocation " << k << " of " << n
        << " returned " << gcomp_status_to_string(r.status) << " at "
        << r.stage << "; only GCOMP_OK or GCOMP_ERR_MEMORY are allowed";

    if (r.status == GCOMP_OK) {
      EXPECT_TRUE(r.completed)
          << s.name << ": failing allocation " << k << " of " << n
          << " reported success but " << r.stage;
    }

    EXPECT_EQ(fa.live_blocks(), 0u)
        << s.name << ": failing allocation " << k << " of " << n << " leaked "
        << fa.live_bytes() << " bytes in " << fa.live_blocks() << " blocks";
  }
}

//
// The grid
//

void add_scenarios(std::vector<Scenario> & out) {
  const std::vector<uint8_t> text = make_text(24 * 1024);
  const std::vector<uint8_t> runs = make_runs(24 * 1024);
  const std::vector<uint8_t> rnd = make_random(8 * 1024);
  const std::vector<uint8_t> mixed = make_mixed(24 * 1024);
  const std::vector<uint8_t> tiny = make_text(3);
  const std::vector<uint8_t> empty;

  auto lvl = [](const char * key, int64_t v) {
    return [key, v](gcomp_options_t * o) {
      gcomp_options_set_int64(o, key, v);
    };
  };
  auto u64 = [](const char * key, uint64_t v) {
    return [key, v](gcomp_options_t * o) {
      gcomp_options_set_uint64(o, key, v);
    };
  };
  auto str = [](const char * key, const char * v) {
    return [key, v](gcomp_options_t * o) {
      gcomp_options_set_string(o, key, v);
    };
  };

  // deflate: the three level bands, and both window extremes.
  out.push_back({"deflate/L1/text", "deflate", lvl("deflate.level", 1), text,
      Mode::StreamingTiny, true});
  out.push_back({"deflate/L6/mixed", "deflate", lvl("deflate.level", 6), mixed,
      Mode::StreamingTiny, false});
  out.push_back({"deflate/L9/text", "deflate", lvl("deflate.level", 9), text,
      Mode::StreamingTiny, false});
  out.push_back({"deflate/L6/random", "deflate", lvl("deflate.level", 6), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"deflate/win8", "deflate", u64("deflate.window_bits", 8),
      text, Mode::StreamingTiny, false});
  out.push_back({"deflate/buffer", "deflate", nullptr, text, Mode::Buffer,
      false});
  out.push_back({"deflate/flush", "deflate", nullptr, text,
      Mode::StreamingFlush, false});
  out.push_back({"deflate/reset", "deflate", nullptr, text, Mode::ResetReuse,
      false});
  out.push_back({"deflate/empty", "deflate", nullptr, empty,
      Mode::StreamingTiny, true});

  // zlib and gzip: the wrappers, including the header fields gzip allocates.
  out.push_back({"zlib/text", "zlib", nullptr, text, Mode::StreamingTiny,
      true});
  out.push_back({"zlib/buffer", "zlib", nullptr, mixed, Mode::Buffer, false});
  out.push_back({"gzip/text", "gzip", nullptr, text, Mode::StreamingTiny,
      true});
  out.push_back({"gzip/named", "gzip",
      [](gcomp_options_t * o) {
        gcomp_options_set_string(o, "gzip.name", "allocation-failure.txt");
        gcomp_options_set_string(o, "gzip.comment", "written by a test");
        gcomp_options_set_bool(o, "gzip.header_crc", 1);
      },
      text, Mode::StreamingTiny, false});
  out.push_back({"gzip/buffer", "gzip", nullptr, runs, Mode::Buffer, false});
  out.push_back({"gzip/flush", "gzip", nullptr, text, Mode::StreamingFlush,
      false});

  // lz4: block sizes and both block-linkage modes.
  out.push_back({"lz4/64k-indep", "lz4",
      [](gcomp_options_t * o) {
        gcomp_options_set_uint64(o, "lz4.block_size", 65536);
        gcomp_options_set_bool(o, "lz4.independent_blocks", 1);
      },
      text, Mode::StreamingTiny, true});
  out.push_back({"lz4/64k-linked", "lz4",
      [](gcomp_options_t * o) {
        gcomp_options_set_uint64(o, "lz4.block_size", 65536);
        gcomp_options_set_bool(o, "lz4.independent_blocks", 0);
      },
      text, Mode::StreamingTiny, false});
  const uint64_t mixed_size = (uint64_t)mixed.size();
  out.push_back({"lz4/checksums", "lz4",
      [mixed_size](gcomp_options_t * o) {
        gcomp_options_set_bool(o, "lz4.block_checksum", 1);
        gcomp_options_set_bool(o, "lz4.content_checksum", 1);
        // content_size is the size itself, not a flag: it is written into the
        // frame header and the decoder checks the stream against it.
        gcomp_options_set_uint64(o, "lz4.content_size", mixed_size);
      },
      mixed, Mode::StreamingTiny, false});
  out.push_back({"lz4/buffer", "lz4", nullptr, runs, Mode::Buffer, false});
  out.push_back({"lz4/random", "lz4", nullptr, rnd, Mode::StreamingTiny,
      false});
  out.push_back({"lz4/reset", "lz4", nullptr, text, Mode::ResetReuse, false});

  // zstd: a chain level, a tree level and an optimal level, and both window
  // extremes.  These are the deepest allocation graphs in the library.
  out.push_back({"zstd/L1/text", "zstd", lvl("zstd.level", 1), text,
      Mode::StreamingTiny, true});
  out.push_back({"zstd/L9/mixed", "zstd", lvl("zstd.level", 9), mixed,
      Mode::StreamingTiny, false});
  out.push_back({"zstd/L11/text", "zstd", lvl("zstd.level", 11), text,
      Mode::StreamingTiny, false});
  out.push_back({"zstd/win10", "zstd", u64("zstd.window_log", 10), text,
      Mode::StreamingTiny, false});
  out.push_back({"zstd/win24", "zstd", u64("zstd.window_log", 24), mixed,
      Mode::StreamingTiny, false});
  out.push_back({"zstd/random", "zstd", nullptr, rnd, Mode::StreamingTiny,
      false});
  out.push_back({"zstd/buffer", "zstd", nullptr, runs, Mode::Buffer, false});
  out.push_back({"zstd/flush", "zstd", nullptr, text, Mode::StreamingFlush,
      false});
  out.push_back({"zstd/reset", "zstd", nullptr, text, Mode::ResetReuse,
      false});
  const uint64_t text_size = (uint64_t)text.size();
  out.push_back({"zstd/checksum", "zstd",
      [text_size](gcomp_options_t * o) {
        gcomp_options_set_bool(o, "zstd.checksum", 1);
        gcomp_options_set_uint64(o, "zstd.content_size", text_size);
      },
      text, Mode::StreamingTiny, false});
  out.push_back({"zstd/empty", "zstd", nullptr, empty, Mode::StreamingTiny,
      true});
  out.push_back({"zstd/tiny", "zstd", nullptr, tiny, Mode::StreamingTiny,
      true});

  // lzw: both formats and both encoder lookups.  The pending buffer grows to
  // 1 << max_code_bits, which needs enough distinct codes to reach.
  out.push_back({"lzw/tiff", "lzw", str("lzw.format", "tiff"), text,
      Mode::StreamingTiny, true});
  out.push_back({"lzw/gif", "lzw", str("lzw.format", "gif"), text,
      Mode::StreamingTiny, false});
  out.push_back({"lzw/hash", "lzw",
      [](gcomp_options_t * o) {
        gcomp_options_set_string(o, "lzw.encoder_lookup", "hash");
      },
      mixed, Mode::StreamingTiny, false});
  out.push_back({"lzw/random", "lzw", nullptr, rnd, Mode::StreamingTiny,
      false});
  out.push_back({"lzw/buffer", "lzw", nullptr, text, Mode::Buffer, false});

  // Dictionaries.  These reach allocation paths nothing else does: zstd
  // allocates a dictionary block buffer, and lz4 seeds its decode history from
  // the dictionary and clamps it to the history capacity when it is larger.
  // The dictionaries are static so the bytes outlive the option object, which
  // borrows rather than copies until the encoder is created.
  static const std::vector<uint8_t> small_dict = make_text(4 * 1024);
  static const std::vector<uint8_t> big_dict = make_text(96 * 1024);

  out.push_back({"zstd/dict", "zstd",
      [](gcomp_options_t * o) {
        gcomp_options_set_bytes(
            o, "zstd.dictionary", small_dict.data(), small_dict.size());
      },
      text, Mode::StreamingTiny, false});
  out.push_back({"zstd/dict-buffer", "zstd",
      [](gcomp_options_t * o) {
        gcomp_options_set_bytes(
            o, "zstd.dictionary", small_dict.data(), small_dict.size());
      },
      text, Mode::Buffer, false});
  out.push_back({"lz4/dict", "lz4",
      [](gcomp_options_t * o) {
        gcomp_options_set_bytes(
            o, "lz4.dictionary", small_dict.data(), small_dict.size());
      },
      text, Mode::StreamingTiny, false});
  // Larger than LZ4's 64 KB history window, so the seed has to be clamped to
  // the most recent 64 KB of it.
  out.push_back({"lz4/dict-large", "lz4",
      [](gcomp_options_t * o) {
        gcomp_options_set_bytes(
            o, "lz4.dictionary", big_dict.data(), big_dict.size());
      },
      text, Mode::StreamingTiny, false});

  // rle: both formats, on data that is all runs and on data that is none.
  out.push_back({"rle/packbits/runs", "rle", str("rle.format", "packbits"),
      runs, Mode::StreamingTiny, true, 256});
  out.push_back({"rle/packbits/random", "rle", str("rle.format", "packbits"),
      rnd, Mode::StreamingTiny, false, 256});
  out.push_back({"rle/tga/runs", "rle", str("rle.format", "tga"), runs,
      Mode::StreamingTiny, false, 256});
  out.push_back({"rle/buffer", "rle", nullptr, runs, Mode::Buffer, false});

  // brotli: both levels, because they are two encoders.  Level 0 stores
  // meta-blocks and allocates almost nothing; level 1 builds LZ77 commands, a
  // hash chain and three Huffman codes per block, and every one of those
  // allocations has a failure arm that was never taken - the two scenarios
  // here were both at the default level on compressible text, so the store
  // fallback and the whole prefix-code path went unvisited.
  //
  // A block larger than one chunk, incompressible input (which makes the
  // encoder abandon the compressed form and store), and the smallest window
  // are each a different shape of the same sweep.
  const std::vector<uint8_t> brotli_long = make_mixed(300 * 1024);
  auto brotli_level = [](int64_t level) {
    return [level](gcomp_options_t * o) {
      gcomp_options_set_int64(o, "brotli.level", level);
    };
  };
  auto brotli_level_window = [](int64_t level, int64_t lgwin) {
    return [level, lgwin](gcomp_options_t * o) {
      gcomp_options_set_int64(o, "brotli.level", level);
      gcomp_options_set_int64(o, "brotli.lgwin", lgwin);
    };
  };

  out.push_back(
      {"brotli/text", "brotli", nullptr, text, Mode::StreamingTiny, false});
  out.push_back(
      {"brotli/buffer", "brotli", nullptr, text, Mode::Buffer, false});
  out.push_back({"brotli/L0/text", "brotli", brotli_level(0), text,
      Mode::StreamingTiny, true});
  out.push_back({"brotli/L1/text", "brotli", brotli_level(1), text,
      Mode::StreamingTiny, true});
  out.push_back({"brotli/L1/runs", "brotli", brotli_level(1), runs,
      Mode::StreamingTiny, false});
  out.push_back({"brotli/L1/random", "brotli", brotli_level(1), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"brotli/L1/mixed-long", "brotli", brotli_level(1),
      brotli_long, Mode::StreamingTiny, false});
  out.push_back({"brotli/L1/lgwin10", "brotli", brotli_level_window(1, 10),
      mixed, Mode::StreamingTiny, false});
  out.push_back({"brotli/L1/lgwin24", "brotli", brotli_level_window(1, 24),
      mixed, Mode::StreamingTiny, false});
  out.push_back({"brotli/L1/buffer", "brotli", brotli_level(1), mixed,
      Mode::Buffer, false});
  out.push_back({"brotli/L0/buffer", "brotli", brotli_level(0), rnd,
      Mode::Buffer, false});
  out.push_back({"brotli/L1/tiny", "brotli", brotli_level(1), tiny,
      Mode::StreamingTiny, true});
  out.push_back({"brotli/L1/empty", "brotli", brotli_level(1), empty,
      Mode::StreamingTiny, true});

  // lzma and lzma2 allocate a window, four match-finder tables, the literal
  // coders and a stage, and lzma2 a snapshot of the model as well. A small
  // dictionary keeps each of the hundred or so attempts cheap; the 300 KiB
  // input is past the 128 KiB batch, so the encoder is made to code in the
  // middle of the stream and not only at the end. Random input drives lzma2's
  // stored-chunk path, which is the one that copies the snapshot back.
  auto lzma_opts = [](const char * pre, bool raw) {
    return [pre, raw](gcomp_options_t * o) {
      std::string p(pre);
      gcomp_options_set_int64(o, (p + ".preset").c_str(), 0);
      gcomp_options_set_uint64(o, (p + ".dict_size").c_str(), 65536);
      if (raw) {
        gcomp_options_set_bool(o, "lzma.raw", 1);
      }
    };
  };
  out.push_back({"lzma/text", "lzma", lzma_opts("lzma", false), text,
      Mode::StreamingTiny, true});
  out.push_back({"lzma/runs", "lzma", lzma_opts("lzma", false), runs,
      Mode::StreamingTiny, false});
  out.push_back({"lzma/random", "lzma", lzma_opts("lzma", false), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"lzma/mixed-long", "lzma", lzma_opts("lzma", false),
      brotli_long, Mode::StreamingTiny, false});
  out.push_back({"lzma/raw", "lzma", lzma_opts("lzma", true), mixed,
      Mode::StreamingTiny, false});
  out.push_back({"lzma/buffer", "lzma", lzma_opts("lzma", false), mixed,
      Mode::Buffer, false});
  out.push_back({"lzma/reset", "lzma", lzma_opts("lzma", false), text,
      Mode::ResetReuse, false});
  out.push_back({"lzma/empty", "lzma", lzma_opts("lzma", false), empty,
      Mode::StreamingTiny, true});
  out.push_back({"lzma2/text", "lzma2", lzma_opts("lzma2", false), text,
      Mode::StreamingTiny, true});
  out.push_back({"lzma2/flush", "lzma2", lzma_opts("lzma2", false), text,
      Mode::StreamingFlush, true});
  out.push_back({"lzma2/random", "lzma2", lzma_opts("lzma2", false), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"lzma2/mixed-long", "lzma2", lzma_opts("lzma2", false),
      brotli_long, Mode::StreamingTiny, false});
  out.push_back({"lzma2/buffer", "lzma2", lzma_opts("lzma2", false), mixed,
      Mode::Buffer, false});
  out.push_back({"lzma2/reset", "lzma2", lzma_opts("lzma2", false), text,
      Mode::ResetReuse, false});
  out.push_back({"lzma2/empty", "lzma2", lzma_opts("lzma2", false), empty,
      Mode::StreamingTiny, true});

  // bzip2 allocates a block, its sorted copy, the suffix sorter's two arrays
  // and a stage up front, then a scratch set for each block and the sorter's
  // own per-level arrays during it. Level 1 keeps each attempt small, and the
  // 300 KiB input is three blocks, so the mid-stream emission and the
  // allocations of the second and third block are swept as well as the first.
  auto bz_level = [](int64_t level) {
    return [level](gcomp_options_t * o) {
      gcomp_options_set_int64(o, "bzip2.level", level);
    };
  };
  out.push_back({"bzip2/text", "bzip2", bz_level(1), text,
      Mode::StreamingTiny, true});
  out.push_back({"bzip2/runs", "bzip2", bz_level(1), runs,
      Mode::StreamingTiny, false});
  out.push_back({"bzip2/random", "bzip2", bz_level(1), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"bzip2/mixed-long", "bzip2", bz_level(1), brotli_long,
      Mode::StreamingTiny, false});
  out.push_back({"bzip2/flush", "bzip2", bz_level(1), text,
      Mode::StreamingFlush, true});
  out.push_back({"bzip2/buffer", "bzip2", bz_level(1), mixed, Mode::Buffer,
      false});
  out.push_back({"bzip2/reset", "bzip2", bz_level(1), text, Mode::ResetReuse,
      false});
  out.push_back({"bzip2/empty", "bzip2", bz_level(1), empty,
      Mode::StreamingTiny, true});

  // xz makes a chain of lzma2 and its filters, so it allocates through four
  // methods and the container's own index; a failure in any of them has to come
  // back as an error, whatever block it falls in.
  auto xz_opts = [](const char * check, const char * filters, int64_t block) {
    return [=](gcomp_options_t * o) {
      gcomp_options_set_string(o, "xz.check", check);
      gcomp_options_set_int64(o, "xz.preset", 0);
      gcomp_options_set_uint64(o, "xz.dict_size", 65536);
      if (filters[0]) {
        gcomp_options_set_string(o, "xz.filters", filters);
      }
      if (block) {
        gcomp_options_set_uint64(o, "xz.block_size", (uint64_t)block);
      }
    };
  };
  out.push_back({"xz/text", "xz", xz_opts("crc64", "", 0), text,
      Mode::StreamingTiny, true});
  out.push_back({"xz/random", "xz", xz_opts("sha256", "", 0), rnd,
      Mode::StreamingTiny, false});
  out.push_back({"xz/blocks", "xz", xz_opts("crc32", "", 40000), brotli_long,
      Mode::StreamingTiny, false});
  out.push_back({"xz/filters", "xz", xz_opts("crc64", "x86,delta:2", 50000),
      mixed, Mode::StreamingTiny, false});
  out.push_back({"xz/flush", "xz", xz_opts("none", "", 0), text,
      Mode::StreamingFlush, false});
  out.push_back({"xz/buffer", "xz", xz_opts("crc64", "arm64", 0), mixed,
      Mode::Buffer, false});
  out.push_back({"xz/reset", "xz", xz_opts("crc64", "delta", 0), text,
      Mode::ResetReuse, false});
  out.push_back({"xz/empty", "xz", xz_opts("crc64", "", 0), empty,
      Mode::StreamingTiny, true});

  // delta and bcj allocate their state and nothing else, so one allocation
  // each; the grid is here to see that a failure of that one is reported and
  // that nothing is left behind.
  out.push_back({"delta/text", "delta", nullptr, text, Mode::StreamingTiny,
      true});
  out.push_back({"delta/flush", "delta", nullptr, text, Mode::StreamingFlush,
      false});
  out.push_back({"delta/reset", "delta", nullptr, text, Mode::ResetReuse,
      false});
  out.push_back({"bcj/mixed", "bcj", nullptr, mixed, Mode::StreamingTiny,
      true});
  out.push_back({"bcj/reset", "bcj", nullptr, mixed, Mode::ResetReuse, false});
  out.push_back({"bcj/empty", "bcj", nullptr, empty, Mode::StreamingTiny,
      true});
}

//
// Tests
//
// One gtest per method, so a failure names the method without the whole grid
// having to run first.
//

void sweep_method(const char * method) {
  std::vector<Scenario> all;
  add_scenarios(all);
  size_t ran = 0;
  for (const Scenario & s : all) {
    if (std::strcmp(s.method, method) != 0) {
      continue;
    }
    ran++;
    SCOPED_TRACE(s.name);
    sweep(s);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
  EXPECT_GT(ran, 0u) << "no scenarios for " << method;
}

TEST(AllocFailure, Deflate) {
  sweep_method("deflate");
}
TEST(AllocFailure, Zlib) {
  sweep_method("zlib");
}
TEST(AllocFailure, Gzip) {
  sweep_method("gzip");
}
TEST(AllocFailure, Lz4) {
  sweep_method("lz4");
}
TEST(AllocFailure, Zstd) {
  sweep_method("zstd");
}
TEST(AllocFailure, Lzw) {
  sweep_method("lzw");
}
TEST(AllocFailure, Rle) {
  sweep_method("rle");
}
TEST(AllocFailure, Brotli) {
  sweep_method("brotli");
}
TEST(AllocFailure, Lzma) {
  sweep_method("lzma");
}
TEST(AllocFailure, Lzma2) {
  sweep_method("lzma2");
}
TEST(AllocFailure, Bzip2) {
  sweep_method("bzip2");
}
TEST(AllocFailure, Xz) {
  sweep_method("xz");
}
TEST(AllocFailure, Delta) {
  sweep_method("delta");
}
TEST(AllocFailure, Bcj) {
  sweep_method("bcj");
}

/**
 * @brief Sustained pressure: everything from the kth allocation on fails.
 *
 * Failing one allocation lets a caller that retries succeed on the retry.
 * This is the case where it cannot, which is what a machine actually out of
 * memory looks like.
 */
TEST(AllocFailure, SustainedPressure) {
  std::vector<Scenario> all;
  add_scenarios(all);
  for (const Scenario & s : all) {
    if (!s.exhaustive) {
      continue; // One representative per method is enough here.
    }
    SCOPED_TRACE(s.name);

    FailingAllocator counter;
    RunResult base = run_scenario(s, counter.allocator());
    ASSERT_EQ(base.status, GCOMP_OK) << s.name << " at " << base.stage;
    const size_t n = counter.calls();

    const size_t step = UnderValgrind() ? (n / 8 + 1) : 1;
    for (size_t k = 1; k <= n; k += step) {
      FailingAllocator fa;
      fa.fail_from(k);
      RunResult r = run_scenario(s, fa.allocator());
      EXPECT_TRUE(r.status == GCOMP_OK || r.status == GCOMP_ERR_MEMORY)
          << s.name << ": failing everything from allocation " << k
          << " returned " << gcomp_status_to_string(r.status) << " at "
          << r.stage;
      if (r.status == GCOMP_OK) {
        EXPECT_TRUE(r.completed) << s.name << ": reported success but "
                                 << r.stage;
      }
      EXPECT_EQ(fa.live_blocks(), 0u)
          << s.name << ": failing everything from allocation " << k
          << " leaked " << fa.live_bytes() << " bytes";
      if (::testing::Test::HasFailure()) {
        break; // One report per scenario; the rest would all say the same.
      }
    }
  }
}

/**
 * @brief The registry and options objects on their own.
 *
 * These are the allocations a caller makes before any compression happens, and
 * they have their own failure paths.
 */
TEST(AllocFailure, RegistryAndOptions) {
  FailingAllocator counter;
  {
    gcomp_registry_t * reg = nullptr;
    ASSERT_EQ(gcomp_registry_create(counter.allocator(), &reg), GCOMP_OK);
    ASSERT_EQ(register_all(reg), GCOMP_OK);
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(
        gcomp_options_create_with_allocator(counter.allocator(), &opts),
        GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "zstd.level", 5), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_string(opts, "gzip.name", "x.txt"), GCOMP_OK);
    gcomp_options_destroy(opts);
    gcomp_registry_destroy(reg);
  }
  ASSERT_EQ(counter.live_blocks(), 0u);
  const size_t n = counter.calls();
  ASSERT_GT(n, 0u);

  const size_t step = UnderValgrind() ? (n / 8 + 1) : 1;
  for (size_t k = 1; k <= n; k += step) {
    FailingAllocator fa;
    fa.fail_at(k);

    gcomp_registry_t * reg = nullptr;
    gcomp_status_t s = gcomp_registry_create(fa.allocator(), &reg);
    EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
        << "registry_create at " << k << ": " << gcomp_status_to_string(s);
    if (s == GCOMP_OK) {
      s = register_all(reg);
      EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
          << "register at " << k << ": " << gcomp_status_to_string(s);

      gcomp_options_t * opts = nullptr;
      s = gcomp_options_create_with_allocator(fa.allocator(), &opts);
      EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
          << "options_create at " << k << ": " << gcomp_status_to_string(s);
      if (s == GCOMP_OK) {
        s = gcomp_options_set_int64(opts, "zstd.level", 5);
        EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
            << "set_int64 at " << k << ": " << gcomp_status_to_string(s);
        s = gcomp_options_set_string(opts, "gzip.name", "x.txt");
        EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_MEMORY)
            << "set_string at " << k << ": " << gcomp_status_to_string(s);
        gcomp_options_destroy(opts);
      }
      gcomp_registry_destroy(reg);
    }

    EXPECT_EQ(fa.live_blocks(), 0u)
        << "failing allocation " << k << " leaked " << fa.live_bytes()
        << " bytes";
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
