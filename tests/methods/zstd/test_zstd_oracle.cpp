/**
 * @file test_zstd_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the zstd method.
 *
 * This file used to carry a second set of tests driven through the Python
 * `zstandard` module.  Sixteen of the twenty tests here needed it, and where
 * it is not installed -- which is usual -- they skipped, leaving a summary
 * line that read PASSED over four tests that between them never exercised
 * Huffman-coded literals, FSE-coded sequence tables, or treeless blocks.  All
 * three were broken.
 *
 * They were removed rather than fixed: the zstd CLI covers the same ground,
 * is far more commonly present, and is what the tests below use.  Short
 * inputs were the only thing the Python tests reached that nothing else did,
 * and that moved into *_EveryShortLength.
 *
 * These tests compare our zstd implementation against external tools
 * (the system zstd CLI) to verify correctness.
 * Tests are skipped gracefully when external tools are not available.
 *
 * Environment variables:
 *   GCOMP_SKIP_ORACLE_TESTS - Set to "1" to skip all oracle tests
 *   GCOMP_ORACLE_VERBOSE - Set to "1" for verbose output
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ghoti.io/compress/compress.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <temp_file.h>
#include <vector>

// Check if oracle tests should be skipped
static bool shouldSkipOracleTests() {
  const char * env = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return env && std::string(env) == "1";
}

static bool isVerbose() {
  const char * env = std::getenv("GCOMP_ORACLE_VERBOSE");
  return env && std::string(env) == "1";
}



// Check if zstd CLI is available
static bool hasZstdCli() {
#ifdef _WIN32
  int result = system("zstd --version >NUL 2>&1");
#else
  int result = system("zstd --version >/dev/null 2>&1");
#endif
  return result == 0;
}

class ZstdOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (shouldSkipOracleTests()) {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }

    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    has_zstd_cli_ = hasZstdCli();

    if (isVerbose()) {
      std::cout << "zstd CLI available: " << (has_zstd_cli_ ? "yes" : "no")
                << std::endl;
    }
  }


  // Helper to run a command and capture stdout
  std::vector<uint8_t> runCommandGetOutput(const std::string & cmd) {
#ifdef _WIN32
    FILE * pipe = popen(cmd.c_str(), "rb");
#else
    FILE * pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
      return {};
    }
    std::vector<uint8_t> result;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
      result.insert(result.end(), buffer, buffer + n);
    }
    pclose(pipe);
    return result;
  }

  // Run a command (same environment as popen). Returns true if exit code is 0.
  bool runCommand(const std::string & cmd) {
    FILE * pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      return false;
    }
    char buffer[4096];
    while (fread(buffer, 1, sizeof(buffer), pipe) > 0) {
      (void)0;
    }
    int st = pclose(pipe);
#ifdef _WIN32
    return (st == 0);
#else
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
#endif
  }


  // Uses stream_reader for frames without content size in header

  // Use zstd CLI to compress data
  std::vector<uint8_t> zstdCliCompress(const std::vector<uint8_t> & data) {
    if (!has_zstd_cli_) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_zstd_oracle", ".bin");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd -c \"" << tmp.path() << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Use zstd CLI to decompress data
  std::vector<uint8_t> zstdCliDecompress(const std::vector<uint8_t> & data) {
    if (!has_zstd_cli_ || data.empty()) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_zstd_oracle", ".zst");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd -dc \"" << tmp.path() << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Create a raw content dictionary (>= 8 bytes). Same bytes used by our
  // encoder/decoder and written to a temp file for the zstd CLI.
  // Returns the bytes and the file, which removes itself when the caller
  // drops it.
  using DictFile = std::shared_ptr<gcomp_test::TempFile>;

  std::pair<std::vector<uint8_t>, DictFile> createDictionaryFromSamples() {
    const size_t raw_dict_size = 8 * 1024;
    std::vector<uint8_t> dict_content(raw_dict_size);
    test_helpers_generate_random(
        dict_content.data(), dict_content.size(), 54321u);
    auto dict = std::make_shared<gcomp_test::TempFile>(
        "gcomp_zstd_oracle", ".dict");
    if (!dict->valid() || !dict->write(dict_content)) {
      return {{}, nullptr};
    }
    return {dict_content, dict};
  }

  // Create a formatted dictionary using zstd --train so that external encoders
  // (the zstd CLI) writes Dictionary_ID and our decoder can match.
  // Returns the bytes and the file; both the samples and the dictionary remove
  // themselves, so the three unlink loops this used to need are gone.
  // Requires zstd CLI. Uses --maxdict and -B so small samples suffice.
  std::pair<std::vector<uint8_t>, DictFile> createFormattedDictionary() {
    if (!has_zstd_cli_) {
      return {{}, nullptr};
    }
    const size_t sample_size = 4096;
    const int num_samples = 6;
    std::vector<std::unique_ptr<gcomp_test::TempFile>> samples;
    samples.reserve(static_cast<size_t>(num_samples));
    for (int i = 0; i < num_samples; i++) {
      std::vector<uint8_t> sample(sample_size + i * 128);
      test_helpers_generate_random(
          sample.data(), sample.size(), static_cast<uint32_t>(12345 + i * 111));
      auto f = std::make_unique<gcomp_test::TempFile>(
          "gcomp_zstd_oracle", ".sample");
      if (!f->valid() || !f->write(sample)) {
        return {{}, nullptr};
      }
      samples.push_back(std::move(f));
    }
    auto dict = std::make_shared<gcomp_test::TempFile>(
        "gcomp_zstd_oracle", ".zstd");
    if (!dict->valid()) {
      return {{}, nullptr};
    }
    std::stringstream cmd;
    cmd << "zstd --train --maxdict=512 -B2048";
    for (const auto & f : samples) {
      cmd << " \"" << f->path() << "\"";
    }
    cmd << " -o \"" << dict->path() << "\"";
#ifdef _WIN32
    cmd << " >NUL 2>&1";
#else
    cmd << " 2>/dev/null";
#endif
    if (!runCommand(cmd.str())) {
      return {{}, nullptr};
    }
    std::vector<uint8_t> dict_bytes = gcomp_test::readWholeFile(dict->path());
    if (dict_bytes.size() < 8) {
      return {{}, nullptr};
    }
    return {dict_bytes, dict};
  }

  // Compress with our library (optional dictionary)
  std::vector<uint8_t> gcompCompressWithDict(const std::vector<uint8_t> & data,
      const std::vector<uint8_t> * dict_data = nullptr) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (dict_data && !dict_data->empty()) {
      if (gcomp_options_set_bytes(opts, "zstd.dictionary", dict_data->data(),
              dict_data->size()) != GCOMP_OK) {
        gcomp_options_destroy(opts);
        return {};
      }
    }
    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;
    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            compressed.data(), compressed.size(), &comp_size);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    compressed.resize(comp_size);
    return compressed;
  }

  // Decompress with our library (optional dictionary)
  std::vector<uint8_t> gcompDecompressWithDict(
      const std::vector<uint8_t> & data,
      const std::vector<uint8_t> * dict_data = nullptr,
      size_t expected_size = 0) {
    if (data.empty()) {
      return {};
    }
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0) !=
        GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }
    if (dict_data && !dict_data->empty()) {
      if (gcomp_options_set_bytes(opts, "zstd.dictionary", dict_data->data(),
              dict_data->size()) != GCOMP_OK) {
        gcomp_options_destroy(opts);
        return {};
      }
    }
    size_t decomp_capacity =
        expected_size > 0 ? expected_size + 1024 : data.size() * 100 + 1024;
    std::vector<uint8_t> decompressed(decomp_capacity);
    size_t decomp_size = 0;
    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            decompressed.data(), decompressed.size(), &decomp_size);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    decompressed.resize(decomp_size);
    return decompressed;
  }

  // Use zstd CLI to compress with dictionary
  // Same as zstdCliCompress(), but with explicit flags so a test can ask the
  // reference encoder for a specific level or frame shape.
  std::vector<uint8_t> zstdCliCompressWith(
      const std::vector<uint8_t> & data, const std::string & flags) {
    if (!has_zstd_cli_) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_zstd_oracle", ".bin");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd " << flags << " -c \"" << tmp.path() << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  std::vector<uint8_t> zstdCliCompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_zstd_cli_ || dict_path.empty()) {
      return {};
    }
    gcomp_test::TempFile datafile("gcomp_zstd_oracle", ".bin");
    if (!datafile.valid() || !datafile.write(data)) {
      return {};
    }
    std::stringstream cmd;
    cmd << "zstd -D \"" << dict_path << "\" -c \"" << datafile.path()
        << "\"";
    // No unlink: ~TempFile does it.
    return runCommandGetOutput(cmd.str());
  }

  // Use zstd CLI to decompress with dictionary
  std::vector<uint8_t> zstdCliDecompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_zstd_cli_ || data.empty() || dict_path.empty()) {
      return {};
    }
    gcomp_test::TempFile tmp("gcomp_zstd_oracle", ".zst");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }
    std::stringstream cmd;
    cmd << "zstd -D \"" << dict_path << "\" -dc \"" << tmp.path() << "\"";
    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }



  // Options-flexible variants.
  //
  // The helpers above fix the options they set.  The tests added for
  // concatenation, window_log, threads and streaming each need a different
  // encoder or decoder configuration, so they pass a setter instead.
  using OptSetter = std::function<bool(gcomp_options_t *)>;

  // Why the last streaming helper returned nothing.  Without this an internal
  // bail-out and a genuine decode failure are the same empty vector.
  std::string stream_why_;

  std::vector<uint8_t> gcompCompressOpts(
      const std::vector<uint8_t> & data, const OptSetter & set = nullptr) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (set && !set(opts)) {
      gcomp_options_destroy(opts);
      return {};
    }
    size_t cap = (data.size() * 12 / 10) + 4096;
    std::vector<uint8_t> compressed(std::max(cap, size_t(4096)));
    size_t used = 0;
    gcomp_status_t status = gcomp_encode_buffer(registry_, "zstd", opts,
        data.data(), data.size(), compressed.data(), compressed.size(), &used);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    compressed.resize(used);
    return compressed;
  }

  // Decoder limits are deliberately generous here: these tests are about
  // format agreement with the reference tool, not about our limit policy,
  // which has its own tests in test_zstd_limits.cpp.
  std::vector<uint8_t> gcompDecompressOpts(const std::vector<uint8_t> & data,
      size_t expected_size, const OptSetter & set = nullptr) {
    if (data.empty()) {
      return {};
    }
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    bool ok =
        gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0) ==
            GCOMP_OK &&
        gcomp_options_set_uint64(opts, "limits.max_window_bytes",
            (uint64_t)512 * 1024 * 1024) == GCOMP_OK;
    if (ok && set) {
      ok = set(opts);
    }
    if (!ok) {
      gcomp_options_destroy(opts);
      return {};
    }
    size_t cap = expected_size > 0 ? expected_size + 4096
                                   : data.size() * 100 + 4096;
    std::vector<uint8_t> out(cap);
    size_t used = 0;
    gcomp_status_t status = gcomp_decode_buffer(registry_, "zstd", opts,
        data.data(), data.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  // Streaming encode: input handed over in in_chunk-sized pieces and output
  // collected through an out_chunk-sized window.  Either may be smaller than
  // any internal block, which is the point -- the encoder has to carry its
  // state across a call boundary that falls wherever the caller put it.
  std::vector<uint8_t> gcompCompressStreaming(const std::vector<uint8_t> & data,
      size_t in_chunk, size_t out_chunk, const OptSetter & set = nullptr) {
    stream_why_.clear();
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (set && !set(opts)) {
      gcomp_options_destroy(opts);
      return {};
    }
    gcomp_encoder_t * enc = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "zstd", opts, &enc);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    std::vector<uint8_t> obuf(out_chunk);
    size_t offset = 0;
    while (offset < data.size()) {
      size_t n = std::min(in_chunk, data.size() - offset);
      gcomp_buffer_t in = {const_cast<uint8_t *>(data.data() + offset), n, 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {obuf.data(), obuf.size(), 0};
        size_t before = in.used;
        gcomp_status_t ust = gcomp_encoder_update(enc, &in, &ob);
        if (ust != GCOMP_OK) {
          stream_why_ = std::string("encoder_update -> ") +
              gcomp_status_to_string(ust);
          gcomp_encoder_destroy(enc);
          return {};
        }
        result.insert(result.end(), obuf.data(), obuf.data() + ob.used);
        if (in.used == before && ob.used == 0) {
          stream_why_ = "encoder made no progress";
          gcomp_encoder_destroy(enc);
          return {};
        }
      }
      offset += in.used;
    }

    for (;;) {
      gcomp_buffer_t ob = {obuf.data(), obuf.size(), 0};
      gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
      result.insert(result.end(), obuf.data(), obuf.data() + ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      if (st != GCOMP_ERR_LIMIT || ob.used == 0) {
        stream_why_ = std::string("encoder_finish -> ") +
            gcomp_status_to_string(st);
        gcomp_encoder_destroy(enc);
        return {};
      }
    }
    gcomp_encoder_destroy(enc);
    return result;
  }

  std::vector<uint8_t> gcompDecompressStreaming(
      const std::vector<uint8_t> & data, size_t expected_size, size_t in_chunk,
      size_t out_chunk, const OptSetter & set = nullptr) {
    stream_why_.clear();
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    bool ok =
        gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0) ==
            GCOMP_OK &&
        gcomp_options_set_uint64(opts, "limits.max_window_bytes",
            (uint64_t)512 * 1024 * 1024) == GCOMP_OK;
    if (ok && set) {
      ok = set(opts);
    }
    if (!ok) {
      gcomp_options_destroy(opts);
      return {};
    }
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zstd", opts, &dec);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    std::vector<uint8_t> obuf(out_chunk);
    size_t offset = 0;
    while (offset < data.size()) {
      size_t n = std::min(in_chunk, data.size() - offset);
      gcomp_buffer_t in = {const_cast<uint8_t *>(data.data() + offset), n, 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {obuf.data(), obuf.size(), 0};
        size_t before = in.used;
        gcomp_status_t ust = gcomp_decoder_update(dec, &in, &ob);
        if (ust != GCOMP_OK) {
          stream_why_ = std::string("decoder_update -> ") +
              gcomp_status_to_string(ust);
          gcomp_decoder_destroy(dec);
          return {};
        }
        result.insert(result.end(), obuf.data(), obuf.data() + ob.used);
        if (in.used == before && ob.used == 0) {
          stream_why_ = "decoder made no progress";
          gcomp_decoder_destroy(dec);
          return {};
        }
      }
      offset += in.used;
    }

    for (;;) {
      gcomp_buffer_t ob = {obuf.data(), obuf.size(), 0};
      gcomp_status_t st = gcomp_decoder_finish(dec, &ob);
      result.insert(result.end(), obuf.data(), obuf.data() + ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      if (st != GCOMP_ERR_LIMIT || ob.used == 0) {
        stream_why_ = std::string("decoder_finish -> ") +
            gcomp_status_to_string(st);
        gcomp_decoder_destroy(dec);
        return {};
      }
    }
    gcomp_decoder_destroy(dec);
    (void)expected_size;
    return result;
  }

  // A skippable frame (RFC 8878 section 3.1.2): magic 0x184D2A5?, a four byte
  // little-endian length, then that many bytes a decoder must ignore.
  static std::vector<uint8_t> makeSkippableFrame(size_t payload) {
    std::vector<uint8_t> f = {0x50, 0x2a, 0x4d, 0x18};
    f.push_back((uint8_t)(payload & 0xFF));
    f.push_back((uint8_t)((payload >> 8) & 0xFF));
    f.push_back((uint8_t)((payload >> 16) & 0xFF));
    f.push_back((uint8_t)((payload >> 24) & 0xFF));
    for (size_t i = 0; i < payload; i++) {
      f.push_back((uint8_t)(i * 31 + 7));
    }
    return f;
  }

  static std::vector<uint8_t> concatFrames(
      const std::vector<std::vector<uint8_t>> & parts) {
    std::vector<uint8_t> out;
    for (const auto & p : parts) {
      out.insert(out.end(), p.begin(), p.end());
    }
    return out;
  }

  // Compress with our library
  std::vector<uint8_t> gcompCompress(
      const std::vector<uint8_t> & data, bool content_checksum = false) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (content_checksum) {
      gcomp_options_set_bool(opts, "zstd.checksum", 1);
    }

    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            compressed.data(), compressed.size(), &comp_size);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      return {};
    }
    compressed.resize(comp_size);
    return compressed;
  }

  // Decompress with our library
  std::vector<uint8_t> gcompDecompress(
      const std::vector<uint8_t> & data, size_t expected_size = 0) {
    if (data.empty()) {
      return {};
    }

    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    // Disable expansion ratio limit for oracle tests
    if (gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0) !=
        GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }

    size_t decomp_capacity =
        expected_size > 0 ? expected_size + 1024 : data.size() * 100 + 1024;
    std::vector<uint8_t> decompressed(decomp_capacity);
    size_t decomp_size = 0;

    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            decompressed.data(), decompressed.size(), &decomp_size);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      return {};
    }
    decompressed.resize(decomp_size);
    return decompressed;
  }

  // Generate test data
  std::vector<uint8_t> generateTextData(size_t size) {
    std::vector<uint8_t> data(size);
    const char * words[] = {"hello", "world", "test", "data", "compression",
        "zstd", "frame", "oracle"};
    size_t pos = 0;
    size_t word_idx = 0;
    while (pos < size) {
      const char * word = words[word_idx % 8];
      size_t word_len = strlen(word);
      for (size_t i = 0; i < word_len && pos < size; i++) {
        data[pos++] = word[i];
      }
      if (pos < size) {
        data[pos++] = ' ';
      }
      word_idx++;
    }
    return data;
  }

  std::vector<uint8_t> generateRandomData(size_t size, unsigned int seed = 42) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  std::vector<uint8_t> generateRepeatedPattern(size_t size) {
    std::vector<uint8_t> data(size);
    const uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    for (size_t i = 0; i < size; i++) {
      data[i] = pattern[i % sizeof(pattern)];
    }
    return data;
  }

  gcomp_registry_t * registry_ = nullptr;
  bool has_zstd_cli_ = false;
};

//
// Tests: zstd CLI interop
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_TextData) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = zstdCliDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_TextData) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = zstdCliCompress(original);
  ASSERT_FALSE(compressed.empty()) << "zstd CLI compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Empty and edge cases
//

//
// Tests: Various sizes
//

//
// Tests: Large data
//

//
// Tests: Dictionary compression (oracle)
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_WithDictionary) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  auto [dict_bytes, dict] = createDictionaryFromSamples();
  if (dict_bytes.empty() || !dict) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompressWithDict(original, &dict_bytes);
  ASSERT_FALSE(compressed.empty()) << "Our compression with dict failed";

  std::vector<uint8_t> decompressed =
      zstdCliDecompressWithDict(compressed, dict->path());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_WithDictionary) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  auto [dict_bytes, dict] = createDictionaryFromSamples();
  if (dict_bytes.empty() || !dict) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      zstdCliCompressWithDict(original, dict->path());
  ASSERT_FALSE(compressed.empty()) << "zstd CLI compression with dict failed";

  std::vector<uint8_t> decompressed =
      gcompDecompressWithDict(compressed, &dict_bytes, original.size());
  ASSERT_FALSE(decompressed.empty()) << "Our decoder failed to decode";
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

}

//
// Tests: paths that only a real zstd stream reaches
//
// The oracle tests above compress generateTextData(), an eight-word list
// repeated to fill the buffer.  That is compressible enough that the
// reference encoder spends almost all of it on sequences and leaves the
// literals STORED -- so the single test that fed real zstd output to our
// decoder never exercised Huffman-coded literals, the FSE-compressed
// Huffman tree description, treeless blocks, or FSE-coded sequence tables.
// Every one of those was broken, and the suite was green.
//
// The tests below assert first that the stream actually contains the
// construct under test, and only then that we decode it.  Without that
// assertion a test like this silently stops testing anything the day the
// reference encoder changes its mind about a heuristic.
//

// What a frame actually contains, by walking its block headers.
struct ZstdFrameShape {
  int blocks = 0;
  int raw_blocks = 0;
  int rle_blocks = 0;
  int compressed_blocks = 0;
  int literals_raw = 0;
  int literals_rle = 0;
  int literals_huffman = 0;  // Compressed_Literals_Block (new tree)
  int literals_treeless = 0; // Treeless_Literals_Block (reuses previous tree)
  bool parsed = false;
};

// Walk a zstd frame far enough to classify each block and, for compressed
// blocks, the literals section type.  RFC 8878 sections 3.1.1.1 (frame
// header), 3.1.1.2 (block header) and 3.1.1.3.1.1 (literals header).
static ZstdFrameShape inspectZstdFrame(const std::vector<uint8_t> & f) {
  ZstdFrameShape shape;
  size_t p = 0;

  if (f.size() < 6) {
    return shape;
  }
  if (!(f[0] == 0x28 && f[1] == 0xB5 && f[2] == 0x2F && f[3] == 0xFD)) {
    return shape;
  }
  p = 4;

  uint8_t fhd = f[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single_segment = ((fhd >> 5) & 1) != 0;
  unsigned did_flag = fhd & 3;

  if (!single_segment) {
    p += 1; // Window_Descriptor
  }
  static const unsigned kDidSize[4] = {0, 1, 2, 4};
  p += kDidSize[did_flag];

  unsigned fcs_size;
  switch (fcs_flag) {
  case 0: fcs_size = single_segment ? 1u : 0u; break;
  case 1: fcs_size = 2; break;
  case 2: fcs_size = 4; break;
  default: fcs_size = 8; break;
  }
  p += fcs_size;

  for (;;) {
    if (p + 3 > f.size()) {
      return shape;
    }
    uint32_t h = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16);
    p += 3;

    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;

    shape.blocks++;
    switch (type) {
    case 0: shape.raw_blocks++; break;
    case 1: shape.rle_blocks++; break;
    case 2: shape.compressed_blocks++; break;
    default: return shape; // Reserved: not a frame we can classify
    }

    if (type == 2) {
      if (p >= f.size()) {
        return shape;
      }
      switch (f[p] & 3) {
      case 0: shape.literals_raw++; break;
      case 1: shape.literals_rle++; break;
      case 2: shape.literals_huffman++; break;
      default: shape.literals_treeless++; break;
      }
    }

    p += (type == 1) ? 1u : size;
    if (p > f.size()) {
      return shape;
    }
    if (last) {
      break;
    }
  }

  shape.parsed = true;
  return shape;
}

// Prose-shaped bytes: a skewed letter distribution with few long repeats, so
// the reference encoder Huffman-codes the literals instead of storing them.
static std::vector<uint8_t> generateProseLikeData(size_t size, unsigned seed) {
  static const char * kWords[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "several", "unusual", "souvenirs", "from", "distant", "places",
      "nobody", "remembers", "clearly", "anymore", "which", "is", "perhaps",
      "the", "point", "of", "travelling", "at", "all"};
  const size_t kNumWords = sizeof(kWords) / sizeof(kWords[0]);

  std::mt19937 gen(seed);
  std::vector<uint8_t> data;
  data.reserve(size + 16);

  while (data.size() < size) {
    const char * w = kWords[gen() % kNumWords];
    for (const char * c = w; *c; c++) {
      data.push_back((uint8_t)*c);
    }
    unsigned r = gen() % 16;
    data.push_back(r == 0 ? (uint8_t)'\n' : (r < 3 ? (uint8_t)',' : (uint8_t)' '));
  }

  data.resize(size);
  return data;
}

// A skewed alphabet with no repeated substrings worth matching: the entire
// gain has to come from entropy-coding the literals.
static std::vector<uint8_t> generateSkewedLiterals(size_t size, unsigned seed) {
  static const int kWeights[16] = {
      100, 50, 25, 12, 6, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  std::mt19937 gen(seed);
  std::discrete_distribution<int> dist(kWeights, kWeights + 16);

  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = (uint8_t)dist(gen);
  }
  return data;
}

TEST_F(ZstdOracleTest, OracleIsActuallyAvailable) {
  // A skipped oracle test and an absent one look identical in the summary
  // line.  If no oracle at all can run, say so as a failure rather than
  // reporting a green suite that checked nothing against a reference.
  ASSERT_TRUE(has_zstd_cli_)
      << "The zstd CLI was not found, so nothing in this file compares our "
         "output against a reference implementation. Install it, or set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to say the gap is intentional.";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_HuffmanCodedLiterals) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(64 * 1024, 20260917u);
  std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
  ASSERT_GT(shape.literals_huffman, 0)
      << "This input no longer produces Huffman-coded literals, so the test "
         "is not exercising the path it was written for";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_TreelessLiterals) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Over a long enough input the encoder keeps the Huffman tree it built for
  // an earlier block and emits Treeless_Literals_Blocks.  Which shapes do
  // that is a heuristic, so sweep a few and require that at least one did.
  static const char * kFlags[] = {"-1", "-3", "-1 -B16384", "-6", "-9"};
  static const size_t kSizes[] = {256 * 1024, 512 * 1024, 1024 * 1024};

  int treeless_seen = 0;

  for (const char * flags : kFlags) {
    for (size_t n : kSizes) {
      std::vector<uint8_t> original = generateProseLikeData(n, 7u);
      std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
      ASSERT_FALSE(compressed.empty()) << flags << " n=" << n;

      ZstdFrameShape shape = inspectZstdFrame(compressed);
      ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
      treeless_seen += shape.literals_treeless;

      std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
      ASSERT_EQ(out.size(), original.size()) << flags << " n=" << n;
      ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
          << flags << " n=" << n;
    }
  }

  ASSERT_GT(treeless_seen, 0)
      << "None of these inputs produced a treeless literals block, so the "
         "test is not exercising the path it was written for";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_FourStreamLiteralsBelowJumpThreshold) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Small inputs still get four Huffman streams, and the Jump_Table is
  // present at any size (RFC 8878 4.2.2).  Sweep the sizes where the
  // reference encoder switches between one and four streams.
  for (size_t n = 128; n <= 4096; n += 97) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n);
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << "n=" << n;
    ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "n=" << n;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_EveryLevel) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 31337u);

  static const char * kFlags[] = {"-1", "-3", "-6", "-9", "-12", "-19",
      "--ultra -22", "--long=27 -9", "-5 -B32768", "-3 --no-check"};

  for (const char * flags : kFlags) {
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
    ASSERT_FALSE(compressed.empty()) << "flags: " << flags;

    std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << "flags: " << flags;
    ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "flags: " << flags;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_LiteralsOnlyBlocks) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateSkewedLiterals(256 * 1024, 99u);
  std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
  ASSERT_GT(shape.literals_huffman + shape.literals_treeless, 0)
      << "This input no longer produces entropy-coded literals";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, OurEncoder_EmitsEntropyCodedLiterals) {
  // Not an interoperability test: it pins the encoder to actually using its
  // Huffman coder.  Every literals section came out stored for as long as
  // zstd_huf_build_enc_table returned zero-length codes, and nothing noticed
  // because the output was still valid, just large.
  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 5u);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk our own frame";
  ASSERT_GT(shape.compressed_blocks, 0)
      << "Every block was stored rather than compressed";
  ASSERT_GT(shape.literals_huffman, 0)
      << "No literals section was Huffman-coded";
  ASSERT_LT(compressed.size(), original.size() / 2)
      << "Prose-shaped input should compress to well under half its size";
}

TEST_F(ZstdOracleTest, OurEncoder_CompressesBlocksWithoutMatches) {
  // A block with no usable matches still has entropy to remove: RFC 8878
  // 3.1.1.3 allows a Compressed_Block whose Sequences_Section is empty.
  std::vector<uint8_t> original = generateSkewedLiterals(256 * 1024, 4242u);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty());
  ASSERT_LT(compressed.size(), original.size() * 3 / 4)
      << "Skewed literals were stored rather than entropy-coded";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_AcrossShapes) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  struct Case {
    const char * name;
    std::vector<uint8_t> data;
  };

  std::vector<Case> cases;
  cases.push_back({"prose", generateProseLikeData(128 * 1024, 1u)});
  cases.push_back({"skewed literals", generateSkewedLiterals(128 * 1024, 2u)});
  cases.push_back({"repetitive", generateTextData(128 * 1024)});
  cases.push_back({"incompressible", generateRandomData(64 * 1024, 3u)});
  cases.push_back({"single run", std::vector<uint8_t>(64 * 1024, 'A')});

  for (const Case & c : cases) {
    std::vector<uint8_t> compressed = gcompCompress(c.data);
    ASSERT_FALSE(compressed.empty()) << c.name;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), c.data.size())
        << c.name << ": the reference CLI could not read our frame";
    ASSERT_EQ(memcmp(out.data(), c.data.data(), c.data.size()), 0) << c.name;
  }
}


TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_EveryShortLength) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Empty, single bytes, and the sizes around the frame header's content-size
  // field widths.  Short inputs were the only ground the Python-module tests
  // covered that nothing else did, so it moved here when those were removed.
  std::vector<size_t> lengths;
  for (size_t n = 0; n <= 40; n++) {
    lengths.push_back(n);
  }
  for (size_t n : {size_t(254), size_t(255), size_t(256), size_t(257),
           size_t(65534), size_t(65535), size_t(65536), size_t(65537)}) {
    lengths.push_back(n);
  }

  for (size_t n : lengths) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n + 1u);
    ASSERT_EQ(original.size(), n);

    std::vector<uint8_t> compressed = gcompCompress(original);
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), n)
        << "n=" << n << ": the zstd CLI could not read our frame";
    if (n) {
      ASSERT_EQ(memcmp(out.data(), original.data(), n), 0) << "n=" << n;
    }
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_EveryShortLength) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  for (size_t n = 0; n <= 40; n++) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n + 7u);
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = gcompDecompress(compressed, n);
    ASSERT_EQ(out.size(), n) << "n=" << n;
    if (n) {
      ASSERT_EQ(memcmp(out.data(), original.data(), n), 0) << "n=" << n;
    }
  }
}

//
// Gap 1: concatenated frames
//
// RFC 8878 section 3: a zstd file is a SEQUENCE of frames, and a decoder is
// expected to run through all of them.  The oracle tests above only ever
// handed the decoder one.  The zstd CLI concatenates by simple byte
// concatenation, so the reference behaviour is easy to state and easy to
// check in both directions.
//

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_ConcatenatedFrames) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<std::vector<uint8_t>> originals = {
      generateProseLikeData(1000, 11u),
      generateProseLikeData(64 * 1024, 12u),
      generateProseLikeData(1, 13u),
      generateProseLikeData(9000, 14u),
  };

  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint8_t> expected;
  for (const auto & o : originals) {
    std::vector<uint8_t> f = zstdCliCompressWith(o, "-3");
    ASSERT_FALSE(f.empty());
    frames.push_back(f);
    expected.insert(expected.end(), o.begin(), o.end());
  }

  std::vector<uint8_t> joined = concatFrames(frames);
  std::vector<uint8_t> out =
      gcompDecompressOpts(joined, expected.size(), [](gcomp_options_t * o) {
        return gcomp_options_set_bool(o, "zstd.concat", 1) == GCOMP_OK;
      });

  ASSERT_EQ(out.size(), expected.size())
      << "four reference frames concatenated";
  EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0);
}

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_ConcatenatedFrames) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<std::vector<uint8_t>> originals = {
      generateProseLikeData(777, 21u),
      generateProseLikeData(32 * 1024, 22u),
      generateProseLikeData(5, 23u),
  };

  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint8_t> expected;
  for (const auto & o : originals) {
    std::vector<uint8_t> f = gcompCompress(o);
    ASSERT_FALSE(f.empty());
    frames.push_back(f);
    expected.insert(expected.end(), o.begin(), o.end());
  }

  std::vector<uint8_t> out = zstdCliDecompress(concatFrames(frames));
  ASSERT_EQ(out.size(), expected.size())
      << "the zstd CLI could not read our frames concatenated";
  EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0);
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_MixedAndSkippableFrames) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> a = generateProseLikeData(4096, 31u);
  std::vector<uint8_t> b = generateProseLikeData(20000, 32u);

  std::vector<uint8_t> ref_a = zstdCliCompressWith(a, "-5");
  std::vector<uint8_t> our_b = gcompCompress(b);
  ASSERT_FALSE(ref_a.empty());
  ASSERT_FALSE(our_b.empty());

  // A skippable frame before, between and after the real ones.  A decoder
  // that mistakes one for a real frame, or that stops at it, fails here.
  std::vector<uint8_t> joined = concatFrames({makeSkippableFrame(0),
      ref_a, makeSkippableFrame(37), our_b, makeSkippableFrame(1024)});

  std::vector<uint8_t> expected = a;
  expected.insert(expected.end(), b.begin(), b.end());

  std::vector<uint8_t> out =
      gcompDecompressOpts(joined, expected.size(), [](gcomp_options_t * o) {
        return gcomp_options_set_bool(o, "zstd.concat", 1) == GCOMP_OK;
      });
  ASSERT_EQ(out.size(), expected.size())
      << "reference frame + our frame, with skippable frames interleaved";
  EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0);

  // The reference tool must read the same byte sequence the same way.
  std::vector<uint8_t> ref_out = zstdCliDecompress(joined);
  ASSERT_EQ(ref_out.size(), expected.size())
      << "the zstd CLI disagrees about this sequence";
  EXPECT_EQ(memcmp(ref_out.data(), expected.data(), expected.size()), 0);
}

//
// Gap 2: window_log
//
// RFC 8878 section 3.1.1.1.2.  The window size decides how far back a match
// may reach, so it is the one frame header field that changes what the
// decoder must keep.  A decoder that sizes its window from something other
// than the header is correct only while the two agree, and nothing above
// ever made them disagree: every earlier test used whatever window the level
// picked.
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_EveryWindowLog) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // 256 KB of input against a 1 KB window forces matches to be dropped and
  // rediscovered constantly; against a 16 MB window the whole input is
  // reachable.  Both must round-trip through the reference tool.
  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 41u);

  for (uint64_t wlog = 10; wlog <= 24; wlog++) {
    std::vector<uint8_t> compressed =
        gcompCompressOpts(original, [wlog](gcomp_options_t * o) {
          return gcomp_options_set_uint64(o, "zstd.window_log", wlog) ==
              GCOMP_OK;
        });
    ASSERT_FALSE(compressed.empty()) << "window_log=" << wlog;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), original.size())
        << "window_log=" << wlog << ": the zstd CLI could not read our frame";
    EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "window_log=" << wlog;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_EveryWindowLog) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 42u);

  for (int wlog = 10; wlog <= 24; wlog++) {
    std::stringstream flags;
    flags << "-3 --zstd=wlog=" << wlog;
    std::vector<uint8_t> compressed =
        zstdCliCompressWith(original, flags.str());
    ASSERT_FALSE(compressed.empty()) << "wlog=" << wlog;

    std::vector<uint8_t> out = gcompDecompressOpts(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << "wlog=" << wlog;
    EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "wlog=" << wlog;
  }
}

//
// Gap 3: threads and job_size
//
// The parallel encoder is a second code path to the same format: it splits
// the input into jobs and compresses them independently.  Nothing above ran
// it, so every oracle result so far described the single-threaded encoder
// only.  The output has to be readable by the reference tool and has to carry
// the same bytes, whatever the split.
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_ParallelMatchesSingleThreaded) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Comfortably more than the 64 KB minimum job size, so the work really is
  // split rather than quietly falling back to one job.
  std::vector<uint8_t> original = generateProseLikeData(768 * 1024, 51u);

  std::vector<uint8_t> single = gcompCompress(original);
  ASSERT_FALSE(single.empty());

  struct Case {
    uint64_t threads;
    uint64_t job_size;
  };
  const std::vector<Case> cases = {
      {2, 0},              // auto job size
      {4, 0},
      {2, 64 * 1024},      // the minimum
      {4, 128 * 1024},
      {8, 256 * 1024},
  };

  for (const auto & c : cases) {
    std::vector<uint8_t> compressed =
        gcompCompressOpts(original, [&c](gcomp_options_t * o) {
          return gcomp_options_set_uint64(o, "threads.count", c.threads) ==
              GCOMP_OK &&
              gcomp_options_set_uint64(o, "zstd.job_size", c.job_size) ==
              GCOMP_OK;
        });
    ASSERT_FALSE(compressed.empty())
        << "threads=" << c.threads << " job_size=" << c.job_size;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), original.size())
        << "threads=" << c.threads << " job_size=" << c.job_size
        << ": the zstd CLI could not read our frame";
    EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "threads=" << c.threads << " job_size=" << c.job_size;

    // And our own decoder must agree with the reference about it.
    //
    // zstd.concat is set here but is not what makes this pass: the parallel
    // encoder emits ONE frame, so there is nothing to concatenate. The reason
    // given here used to be "the parallel encoder emits one frame per job",
    // which is the stale belief corrected in zstd_encoder.c's file header --
    // and, until 2026-09-26, still trusted by the flush path, where it cost
    // GCOMP_FLUSH_FULL its whole meaning above one thread. Left set because it
    // is harmless and because a FULL flush would make it load-bearing.
    std::vector<uint8_t> ours = gcompDecompressOpts(
        compressed, original.size(), [](gcomp_options_t * o) {
          return gcomp_options_set_bool(o, "zstd.concat", 1) == GCOMP_OK;
        });
    ASSERT_EQ(ours.size(), original.size())
        << "threads=" << c.threads << " job_size=" << c.job_size;
    EXPECT_EQ(memcmp(ours.data(), original.data(), original.size()), 0)
        << "threads=" << c.threads << " job_size=" << c.job_size;
  }
}

/**
 * The reference reads what a flush produced, at every thread count and in both
 * modes.
 *
 * Nothing in this file asked that before: `grep -c flush` over it was zero, so
 * no outside implementation had ever read a stream that a
 * gcomp_encoder_flush() helped build.  That is the gap worth closing on its
 * own, and GCOMP_FLUSH_FULL in parallel mode makes it urgent -- it emits a
 * shape the parallel encoder never emitted before, several frames where there
 * used to be one, and our own decoder agreeing with our own encoder about a
 * new shape is not evidence.
 *
 * The zstd CLI decodes concatenated frames without being asked, so the same
 * invocation checks both modes.
 */
TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_FlushedStreamsAtEveryThreadCount) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Past the 64 KB minimum job size several times over, so a flush lands with
  // real work both behind and ahead of it.
  std::vector<uint8_t> original = generateProseLikeData(512 * 1024, 53u);

  for (uint64_t threads : {(uint64_t)1, (uint64_t)2, (uint64_t)4}) {
    for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
      const char * mode_name =
          (mode == GCOMP_FLUSH_SYNC) ? "sync" : "full";

      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(opts, "threads.count", threads), GCOMP_OK);
      gcomp_encoder_t * enc = nullptr;
      ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
      gcomp_options_destroy(opts);

      std::vector<uint8_t> compressed;
      uint8_t window[16384];
      const size_t piece = 100 * 1024;
      size_t consumed = 0;
      while (consumed < original.size()) {
        const size_t take = std::min(piece, original.size() - consumed);
        gcomp_buffer_t in = {
            const_cast<uint8_t *>(original.data() + consumed), take, 0};
        while (in.used < in.size) {
          gcomp_buffer_t out = {window, sizeof(window), 0};
          ASSERT_EQ(gcomp_encoder_update(enc, &in, &out), GCOMP_OK)
              << "threads=" << threads << " mode=" << mode_name;
          compressed.insert(compressed.end(), window, window + out.used);
        }
        consumed += take;

        // A flush after every piece, including the last one before finish().
        for (;;) {
          gcomp_buffer_t out = {window, sizeof(window), 0};
          gcomp_status_t st = gcomp_encoder_flush(enc, &out, mode);
          compressed.insert(compressed.end(), window, window + out.used);
          if (st == GCOMP_OK) {
            break;
          }
          ASSERT_EQ(st, GCOMP_ERR_LIMIT)
              << "threads=" << threads << " mode=" << mode_name;
        }
      }
      for (;;) {
        gcomp_buffer_t out = {window, sizeof(window), 0};
        gcomp_status_t st = gcomp_encoder_finish(enc, &out);
        compressed.insert(compressed.end(), window, window + out.used);
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT)
            << "threads=" << threads << " mode=" << mode_name;
      }
      gcomp_encoder_destroy(enc);
      ASSERT_FALSE(compressed.empty())
          << "threads=" << threads << " mode=" << mode_name;

      std::vector<uint8_t> out = zstdCliDecompress(compressed);
      ASSERT_EQ(out.size(), original.size())
          << "threads=" << threads << " mode=" << mode_name
          << ": the zstd CLI could not read what our flush produced";
      EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
          << "threads=" << threads << " mode=" << mode_name;
    }
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_ReferenceMultiThreadedOutput) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(768 * 1024, 52u);

  for (const char * flags : {"-3 -T4", "-3 -T2 --zstd=chainLog=15", "-5 -T0"}) {
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
    ASSERT_FALSE(compressed.empty()) << flags;

    std::vector<uint8_t> out = gcompDecompressOpts(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << flags;
    EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << flags;
  }
}

//
// Gap 4: streaming across awkward chunk boundaries
//
// The buffer API hands the whole input over at once, which is the one case
// where no piece of decoder state has to survive a call boundary.  Every
// oracle test above used it.  A caller reading from a socket does not have
// that luxury, and a state machine that is wrong about where it stopped is
// wrong only when the boundary lands inside a structure -- a frame header, a
// block header, a Huffman table, a bitstream.
//
// The chunk sizes below are chosen to be coprime with every block and header
// size in the format, so the boundaries walk through those structures rather
// than landing in the same relative place each time.
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_StreamingChunkBoundaries) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(140 * 1024, 61u);

  struct Case {
    size_t in_chunk;
    size_t out_chunk;
  };
  const std::vector<Case> cases = {
      {1, 1},          // one byte in, one byte out
      {1, 64 * 1024},
      {7, 13},
      {13, 7},
      {1023, 17},
      {4099, 4099},
      {65537, 251},
      {140 * 1024, 1}, // all input at once, output one byte at a time
  };

  for (const auto & c : cases) {
    std::vector<uint8_t> compressed =
        gcompCompressStreaming(original, c.in_chunk, c.out_chunk);
    ASSERT_FALSE(compressed.empty())
        << "in_chunk=" << c.in_chunk << " out_chunk=" << c.out_chunk;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), original.size())
        << "in_chunk=" << c.in_chunk << " out_chunk=" << c.out_chunk
        << ": the zstd CLI could not read what streaming produced";
    EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "in_chunk=" << c.in_chunk << " out_chunk=" << c.out_chunk;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_StreamingChunkBoundaries) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(140 * 1024, 62u);

  // Checksummed and not: the frame trailer is the structure most likely to
  // straddle a boundary, being the last four bytes of the stream.
  for (const char * flags : {"-3", "-3 --check", "-9 --check", "-1"}) {
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
    ASSERT_FALSE(compressed.empty()) << flags;

    const std::vector<std::pair<size_t, size_t>> cases = {
        {1, 1}, {1, 64 * 1024}, {7, 13}, {13, 7}, {1023, 17}, {4099, 4099},
        {compressed.size(), 1}};

    for (const auto & c : cases) {
      std::vector<uint8_t> out = gcompDecompressStreaming(
          compressed, original.size(), c.first, c.second);
      ASSERT_EQ(out.size(), original.size())
          << flags << " in_chunk=" << c.first << " out_chunk=" << c.second
          << " why=" << stream_why_;
      EXPECT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
          << flags << " in_chunk=" << c.first << " out_chunk=" << c.second;
    }
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_StreamingConcatenatedFrames) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // The two gaps together: a frame boundary is the one place a decoder has to
  // tear down and rebuild its state, and doing that halfway through a caller's
  // buffer is where the two failure modes meet.
  std::vector<uint8_t> a = generateProseLikeData(9000, 71u);
  std::vector<uint8_t> b = generateProseLikeData(33000, 72u);
  std::vector<uint8_t> fa = zstdCliCompressWith(a, "-3 --check");
  std::vector<uint8_t> fb = zstdCliCompressWith(b, "-7");
  ASSERT_FALSE(fa.empty());
  ASSERT_FALSE(fb.empty());

  std::vector<uint8_t> joined =
      concatFrames({fa, makeSkippableFrame(11), fb});
  std::vector<uint8_t> expected = a;
  expected.insert(expected.end(), b.begin(), b.end());

  for (size_t in_chunk : {(size_t)1, (size_t)3, (size_t)97, (size_t)4096}) {
    std::vector<uint8_t> out = gcompDecompressStreaming(joined,
        expected.size(), in_chunk, 1024, [](gcomp_options_t * o) {
          return gcomp_options_set_bool(o, "zstd.concat", 1) == GCOMP_OK;
        });
    ASSERT_EQ(out.size(), expected.size())
        << "in_chunk=" << in_chunk << " why=" << stream_why_;
    EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0)
        << "in_chunk=" << in_chunk;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
