/**
 * @file lzma_oracle.h
 *
 * liblzma, loaded at run time, as the reference the LZMA tests compare
 * against. Nothing here links it: the oracle is whatever `liblzma.so.5` the
 * machine, or the pinned image, provides, and a machine without one gets a
 * failing sentinel test rather than a silent skip.
 *
 * The structures are the head of the ones in `<lzma.h>`, laid out by hand,
 * because the point of loading the library is that no development package is
 * needed. They are padded well past the real size so that the library can
 * write its whole structure into ours.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_TESTS_LZMA_ORACLE_H
#define GCOMP_TESTS_LZMA_ORACLE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace lzmaref {

constexpr uint64_t kFilterLzma1 = 0x4000000000000001ULL;
constexpr uint64_t kFilterLzma2 = 0x21ULL;
constexpr uint64_t kVliUnknown = UINT64_MAX;

struct Options {
  uint32_t dict_size;
  const uint8_t * preset_dict;
  uint32_t preset_dict_size;
  uint32_t lc, lp, pb;
  int mode;
  uint32_t nice_len;
  int mf;
  uint32_t depth;
  uint8_t reserved[192];
};

struct Filter {
  uint64_t id;
  void * options;
};

struct Stream {
  const uint8_t * next_in;
  size_t avail_in;
  uint64_t total_in;
  uint8_t * next_out;
  size_t avail_out;
  uint64_t total_out;
  const void * allocator;
  void * internal;
  void * r_ptr[4];
  uint64_t r_int64[2];
  size_t r_size[2];
  int r_enum[2];
};

struct Lib {
  unsigned char (*preset)(Options *, uint32_t) = nullptr;
  int (*raw_encode)(const Filter *, const void *, const uint8_t *, size_t,
      uint8_t *, size_t *, size_t) = nullptr;
  int (*raw_decode)(const Filter *, const void *, const uint8_t *, size_t *,
      size_t, uint8_t *, size_t *, size_t) = nullptr;
  int (*alone_encoder)(Stream *, const Options *) = nullptr;
  int (*alone_decoder)(Stream *, uint64_t) = nullptr;
  int (*code)(Stream *, int) = nullptr;
  void (*end)(Stream *) = nullptr;
  bool ok() const {
    return preset && raw_encode && raw_decode && alone_encoder &&
        alone_decoder && code && end;
  }
};

inline const Lib & lib() {
  static Lib l;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    void * h = dlopen("liblzma.so.5", RTLD_NOW);
    if (h) {
      l.preset = reinterpret_cast<decltype(l.preset)>(
          dlsym(h, "lzma_lzma_preset"));
      l.raw_encode = reinterpret_cast<decltype(l.raw_encode)>(
          dlsym(h, "lzma_raw_buffer_encode"));
      l.raw_decode = reinterpret_cast<decltype(l.raw_decode)>(
          dlsym(h, "lzma_raw_buffer_decode"));
      l.alone_encoder = reinterpret_cast<decltype(l.alone_encoder)>(
          dlsym(h, "lzma_alone_encoder"));
      l.alone_decoder = reinterpret_cast<decltype(l.alone_decoder)>(
          dlsym(h, "lzma_alone_decoder"));
      l.code = reinterpret_cast<decltype(l.code)>(dlsym(h, "lzma_code"));
      l.end = reinterpret_cast<decltype(l.end)>(dlsym(h, "lzma_end"));
    }
  }
  return l;
}

/// Options for a preset, with lc, lp and pb overridden when they are >= 0.
inline Options options(uint32_t preset, int lc = -1, int lp = -1, int pb = -1,
    int64_t dict = -1) {
  Options o;
  std::memset(&o, 0, sizeof(o));
  lib().preset(&o, preset);
  if (lc >= 0) {
    o.lc = (uint32_t)lc;
  }
  if (lp >= 0) {
    o.lp = (uint32_t)lp;
  }
  if (pb >= 0) {
    o.pb = (uint32_t)pb;
  }
  if (dict >= 0) {
    o.dict_size = (uint32_t)dict;
  }
  return o;
}

/// `.lzma` bytes from liblzma: header, stream, end marker.
inline std::vector<uint8_t> alone_encode(
    const std::vector<uint8_t> & in, const Options & o) {
  Stream s;
  std::memset(&s, 0, sizeof(s));
  std::vector<uint8_t> out(in.size() + in.size() / 2 + 4096);
  if (lib().alone_encoder(&s, &o) != 0) {
    return {};
  }
  s.next_in = in.data();
  s.avail_in = in.size();
  s.next_out = out.data();
  s.avail_out = out.size();
  int r = lib().code(&s, 3 /* LZMA_FINISH */);
  lib().end(&s);
  if (r != 1 /* LZMA_STREAM_END */) {
    return {};
  }
  out.resize(out.size() - s.avail_out);
  return out;
}

/// Raw LZMA1 or LZMA2, as 7z and zip method 14 hold them.
inline std::vector<uint8_t> raw_encode(
    uint64_t id, const std::vector<uint8_t> & in, Options o) {
  Filter f[2] = {{id, &o}, {kVliUnknown, nullptr}};
  std::vector<uint8_t> out(in.size() + in.size() / 2 + 4096);
  size_t pos = 0;
  if (lib().raw_encode(f, nullptr, in.data(), in.size(), out.data(), &pos,
          out.size()) != 0) {
    return {};
  }
  out.resize(pos);
  return out;
}

/// What liblzma makes of a raw stream; false if it refuses it.
inline bool raw_decode(uint64_t id, const std::vector<uint8_t> & in,
    Options o, std::vector<uint8_t> & out, size_t out_cap) {
  Filter f[2] = {{id, &o}, {kVliUnknown, nullptr}};
  size_t ipos = 0, opos = 0;
  out.assign(out_cap, 0);
  int r = lib().raw_decode(
      f, nullptr, in.data(), &ipos, in.size(), out.data(), &opos, out.size());
  out.resize(opos);
  return r == 0 && ipos == in.size();
}

} // namespace lzmaref

#endif
