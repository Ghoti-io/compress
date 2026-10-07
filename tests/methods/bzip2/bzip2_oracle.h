/**
 * @file bzip2_oracle.h
 *
 * libbz2, loaded at run time, as the reference the bzip2 tests compare
 * against. Nothing here links it: the oracle is whatever `libbz2.so.1.0` the
 * machine, or the pinned image, provides, and a machine without one gets a
 * failing sentinel test rather than a silent skip.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_TESTS_BZIP2_ORACLE_H
#define GCOMP_TESTS_BZIP2_ORACLE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

namespace bzref {

struct Lib {
  int (*compress)(char *, unsigned *, char *, unsigned, int, int, int) = nullptr;
  int (*decompress)(char *, unsigned *, char *, unsigned, int, int) = nullptr;
  bool ok() const {
    return compress && decompress;
  }
};

inline const Lib & lib() {
  static Lib l;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    void * h = dlopen("libbz2.so.1.0", RTLD_NOW);
    if (!h) {
      h = dlopen("libbz2.so.1", RTLD_NOW);
    }
    if (h) {
      l.compress = reinterpret_cast<decltype(l.compress)>(
          dlsym(h, "BZ2_bzBuffToBuffCompress"));
      l.decompress = reinterpret_cast<decltype(l.decompress)>(
          dlsym(h, "BZ2_bzBuffToBuffDecompress"));
    }
  }
  return l;
}

/// One bzip2 stream from libbz2 at `level` (1..9); empty on failure.
inline std::vector<uint8_t> compress(
    const std::vector<uint8_t> & in, int level) {
  unsigned cap = (unsigned)(in.size() + in.size() / 100 + 1024);
  std::vector<uint8_t> out(cap);
  unsigned len = cap;
  /* libbz2 refuses a null source even for no bytes, and an empty vector's
   * data() may be null. */
  static char none[1] = {0};
  char * src = in.empty() ? none : (char *)in.data();
  if (lib().compress((char *)out.data(), &len, src,
          (unsigned)in.size(), level, 0, 0) != 0) {
    return {};
  }
  out.resize(len);
  return out;
}

/// What libbz2 makes of one stream; false if it refuses it. Exactly the one
/// stream: libbz2 reports trailing bytes as an error here.
inline bool decompress(const std::vector<uint8_t> & in,
    std::vector<uint8_t> & out, size_t cap) {
  unsigned len = (unsigned)cap;
  out.assign(cap ? cap : 1, 0);
  static char none[1] = {0};
  char * src = in.empty() ? none : (char *)in.data();
  int r = lib().decompress((char *)out.data(), &len, src, (unsigned)in.size(), 0, 0);
  out.resize(len);
  return r == 0;
}

} // namespace bzref

#endif
