#ifndef GHOTI_IO_GCOMP_TEST_LEGACY_ZIP_HELPERS_H
#define GHOTI_IO_GCOMP_TEST_LEGACY_ZIP_HELPERS_H

#include "test_helpers.h"
#include <algorithm>
#include <cstdint>
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

using Bytes = std::vector<uint8_t>;

uint32_t g_seed = 1;
[[maybe_unused]] uint32_t next_rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 16;
}

[[maybe_unused]] Bytes noise(size_t n, uint32_t seed) {
  g_seed = seed;
  Bytes b(n);
  for (auto & x : b) {
    x = (uint8_t)next_rand();
  }
  return b;
}

[[maybe_unused]] Bytes words(size_t n) {
  static const char * w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "sleeps. "};
  Bytes out;
  g_seed = 7;
  while (out.size() < n) {
    const char * s = w[next_rand() % 11];
    out.insert(out.end(), s, s + strlen(s));
  }
  out.resize(n);
  return out;
}

/// A bit writer for streams built by hand: bits go in low first.
struct Bits {
  Bytes out;
  uint64_t acc = 0;
  unsigned n = 0;
  void put(uint32_t v, unsigned bits) {
    acc |= (uint64_t)v << n;
    n += bits;
    while (n >= 8) {
      out.push_back((uint8_t)acc);
      acc >>= 8;
      n -= 8;
    }
  }
  Bytes finish() {
    if (n != 0) {
      put(0, 8 - n);
    }
    return out;
  }
};

/// Decode `in`, with options set by `set`; the buffer API.
template <class F>
gcomp_status_t decode_with(const char * method, const Bytes & in, Bytes & out,
    F set, size_t cap = 1u << 24) {
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  set(o);
  size_t n = 0;
  out.assign(cap, 0);
  const gcomp_status_t s = gcomp_decode_buffer(
      nullptr, method, o, in.data(), in.size(), out.data(), cap, &n);
  gcomp_options_destroy(o);
  out.resize(s == GCOMP_OK ? n : 0);
  return s;
}

/// Decode feeding `in_step` bytes at a time into an output of `out_step`.
template <class F>
gcomp_status_t decode_steps(const char * method, const Bytes & in, Bytes & out,
    size_t in_step, size_t out_step, F set) {
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  set(o);
  gcomp_decoder_t * d = nullptr;
  gcomp_status_t s = gcomp_decoder_create(gcomp_registry_default(), method, o, &d);
  gcomp_options_destroy(o);
  if (s != GCOMP_OK) {
    return s;
  }
  Bytes chunk(out_step);
  size_t pos = 0;
  out.clear();
  for (;;) {
    gcomp_buffer_t ib = {in.data() + pos, std::min(in_step, in.size() - pos), 0};
    gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
    s = gcomp_decoder_update(d, &ib, &ob);
    out.insert(out.end(), chunk.data(), chunk.data() + ob.used);
    pos += ib.used;
    if (s != GCOMP_OK) {
      break;
    }
    if (pos == in.size() && ob.used < chunk.size()) {
      break;
    }
  }
  while (s == GCOMP_OK) {
    gcomp_buffer_t ob = {chunk.data(), chunk.size(), 0};
    s = gcomp_decoder_finish(d, &ob);
    out.insert(out.end(), chunk.data(), chunk.data() + ob.used);
    if (s != GCOMP_ERR_LIMIT) {
      break;
    }
  }
  gcomp_decoder_destroy(d);
  return s;
}

template <class F>
Bytes encode_with(const char * method, const Bytes & in, F set) {
  gcomp_options_t * o = nullptr;
  gcomp_options_create(&o);
  set(o);
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, method, o, in.size(), &bound), GCOMP_OK);
  Bytes out(bound);
  size_t n = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, method, o, in.data(), in.size(),
                out.data(), out.size(), &n),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(n);
  return out;
}

[[maybe_unused]] Bytes from_hex(const char * hex) {
  Bytes b;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    b.push_back((uint8_t)std::stoi(std::string(hex + i, 2), nullptr, 16));
  }
  return b;
}

}  // namespace

#endif
