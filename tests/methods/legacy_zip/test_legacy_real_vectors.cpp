/**
 * @file test_legacy_real_vectors.cpp
 *
 * Real members of the old ZIP methods, written by other programs, kept in other
 * projects' test data and fetched by tools/legacy_zip/fetch.sh (pinned by commit
 * and SHA-256 in tools/legacy_zip/MANIFEST; not committed here). Every member of
 * shrink, reduce and implode in them has to unpack to exactly the size and CRC-32
 * its own ZIP header records, which needs no second reader. They found a bug our
 * own tests did not: reduce's follower index takes at least one bit even for a
 * set of one.
 *
 * Without them the test reports it and says how to fetch them; it fails instead
 * of skipping when GHOTI_CORPUS_REQUIRED=1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../legacy_zip_helpers.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

struct Member {
  std::string name;
  unsigned flags = 0, method = 0;
  uint32_t crc = 0;
  uint64_t csize = 0, usize = 0;
  Bytes raw;
};

uint32_t le(const Bytes & b, size_t at, int n) {
  uint32_t v = 0;
  for (int i = n - 1; i >= 0; i--) {
    v = (v << 8) | b[at + (size_t)i];
  }
  return v;
}

/// The members of a ZIP file, from its central directory.
std::vector<Member> zip_members(const Bytes & d) {
  std::vector<Member> out;
  size_t eocd = std::string::npos;
  for (size_t i = d.size() >= 22 ? d.size() - 22 : 0;; i--) {
    if (le(d, i, 4) == 0x06054b50u) {
      eocd = i;
      break;
    }
    if (i == 0) {
      break;
    }
  }
  if (eocd == std::string::npos) {
    return out;
  }
  size_t p = le(d, eocd + 16, 4);
  for (unsigned n = le(d, eocd + 10, 2); n > 0 && p + 46 <= d.size(); n--) {
    Member m;
    m.flags = le(d, p + 8, 2);
    m.method = le(d, p + 10, 2);
    m.crc = le(d, p + 16, 4);
    m.csize = le(d, p + 20, 4);
    m.usize = le(d, p + 24, 4);
    const size_t nl = le(d, p + 28, 2), el = le(d, p + 30, 2), cl = le(d, p + 32, 2);
    const size_t lo = le(d, p + 42, 4);
    m.name.assign((const char *)&d[p + 46], nl);
    const size_t start = lo + 30 + le(d, lo + 26, 2) + le(d, lo + 28, 2);
    if (start + m.csize <= d.size()) {
      m.raw.assign(d.begin() + (long)start, d.begin() + (long)(start + m.csize));
      out.push_back(m);
    }
    p += 46 + nl + el + cl;
  }
  return out;
}

uint32_t crc32_of(const Bytes & b) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t x : b) {
    crc ^= x;
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

/// The fetched archives, or none (and the test says so).
std::vector<std::filesystem::path> fetched() {
  std::vector<std::filesystem::path> v;
  const std::filesystem::path dir = "third_party/legacy_zip";
  std::error_code ec;
  for (const auto & e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".zip") {
      v.push_back(e.path());
    }
  }
  std::sort(v.begin(), v.end());
  return v;
}

Bytes slurp(const std::filesystem::path & p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

TEST(LegacyRealVectors, EveryMemberUnpacksToWhatItsHeaderSays) {
  const auto files = fetched();
  if (files.empty()) {
    const char * req = std::getenv("GHOTI_CORPUS_REQUIRED");
    const std::string how =
        "the real ZIP members are not fetched: run tools/legacy_zip/fetch.sh";
    if (req && req[0] == '1') {
      FAIL() << how;
    }
    GTEST_SKIP() << how;
  }
  size_t checked = 0;
  for (const auto & path : files) {
    for (const Member & m : zip_members(slurp(path))) {
      if (m.method != 1 && !(m.method >= 2 && m.method <= 6)) {
        continue;
      }
      const char * method =
          m.method == 1 ? "shrink" : m.method == 6 ? "implode" : "reduce";
      Bytes out;
      const gcomp_status_t s = decode_with(method, m.raw, out, [&](gcomp_options_t * o) {
        if (m.method >= 2 && m.method <= 5) {
          gcomp_options_set_int64(o, "reduce.factor", (int64_t)m.method - 1);
          gcomp_options_set_uint64(o, "reduce.uncompressed_size", m.usize);
        }
        if (m.method == 6) {
          gcomp_options_set_uint64(o, "implode.dictionary_size", (m.flags & 2u) ? 8192 : 4096);
          gcomp_options_set_bool(o, "implode.literal_tree", (m.flags & 4u) != 0);
          gcomp_options_set_uint64(o, "implode.uncompressed_size", m.usize);
        }
      });
      const std::string what = path.filename().string() + " " + m.name;
      ASSERT_EQ(s, GCOMP_OK) << what;
      ASSERT_EQ(out.size(), m.usize) << what;
      EXPECT_EQ(crc32_of(out), m.crc) << what;
      checked++;
    }
  }
  EXPECT_GE(checked, 20u) << "far fewer old-method members than the set holds";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
