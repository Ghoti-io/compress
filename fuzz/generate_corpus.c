/**
 * @file generate_corpus.c
 *
 * Generate seed corpus files for AFL++ fuzzing from golden test vectors.
 *
 * This program creates seed files in the fuzz/corpus/ directories that
 * give AFL++ a head start by providing valid and interesting inputs.
 *
 * Build:
 *   gcc -O2 -o generate_corpus fuzz/generate_corpus.c -I include/
 *
 * Run:
 *   ./generate_corpus
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/cutil/file.h>

// Include golden vectors (redefine nullptr for C compatibility)
#ifdef __cplusplus
// C++ mode - nullptr is fine
#else
// C mode - define nullptr
#define nullptr ((void *)0)
#endif

// Golden vectors from test data
// Vector 1: Empty input (stored block)
static const uint8_t v1_compressed[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};

// Vector 2: Single byte 'A' (fixed Huffman)
static const uint8_t v2_compressed[] = {0x73, 0x04, 0x00};

// Vector 3: "Hello" (stored block)
static const uint8_t v3_compressed[] = {
    0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

// Vector 4: "Hello, world!" (fixed Huffman)
static const uint8_t v4_compressed[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
    0x51, 0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};

// Vector 5: Repeated "ABC" pattern
static const uint8_t v5_compressed[] = {
    0x73, 0x74, 0x72, 0x76, 0x44, 0x42, 0x00};

// Vector 6: 100 zero bytes
static const uint8_t v6_compressed[] = {0x63, 0x60, 0xA0, 0x3D, 0x00, 0x00};

// Vector 9: Pangram
static const uint8_t v9_compressed[] = {0x0B, 0xC9, 0x48, 0x55, 0x28, 0x2C,
    0xCD, 0x4C, 0xCE, 0x56, 0x48, 0x2A, 0xCA, 0x2F, 0xCF, 0x53, 0x48, 0xCB,
    0xAF, 0x50, 0xC8, 0x2A, 0xCD, 0x2D, 0x28, 0x56, 0xC8, 0x2F, 0x4B, 0x2D,
    0x52, 0x28, 0x01, 0x4A, 0xE7, 0x24, 0x56, 0x55, 0x2A, 0xA4, 0xE4, 0xA7,
    0x03, 0x00};

// Some plaintext inputs for encoder/roundtrip testing
static const char * plaintext_samples[] = {
    "",                                               // Empty
    "A",                                              // Single char
    "Hello",                                          // Short word
    "Hello, world!",                                  // Classic
    "ABCABCABCABCABC",                                // Repeated pattern
    "The quick brown fox jumps over the lazy dog",    // Pangram
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", // Run of same char
    "abcdefghijklmnopqrstuvwxyz",                     // Alphabet
    "0123456789",                                     // Digits
};

typedef struct {
  const char * name;
  const uint8_t * data;
  size_t size;
} corpus_entry_t;

static int mkdir_p(const char * path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return 0; // Already exists
  }
  return mkdir(path, 0755);
}

static int write_file(const char * path, const uint8_t * data, size_t size) {
  // Through cutil, which writes to a temporary file beside the destination
  // and renames it into place.  A corpus is regenerated rather than kept, so
  // GCU_FILE_SYNC_NONE is the right end of file.h's trade: the rename is
  // still atomic, so a re-run interrupted halfway cannot leave a fuzzer
  // reading a half-written seed.
  //
  // GCU_FILE_PERMS_DEFAULT because a seed is read by people and committed:
  // it should look like every other file in the tree, not like the temporary
  // it was renamed from.  The owner-only default this parameter replaced is
  // still visible in a few of fuzz/regression's working-tree modes.
  GCU_File_Result r = gcu_file_write_atomic(
      path, data, size, GCU_FILE_SYNC_NONE, GCU_FILE_PERMS_DEFAULT, NULL);
  if (r != GCU_FILE_OK) {
    fprintf(stderr, "Error: cannot write %s: %s\n", path,
        gcu_file_result_string(r));
    return -1;
  }
  printf("  Created: %s (%zu bytes)\n", path, size);
  return 0;
}

int main(void) {
  char path[256];

  printf("Generating seed corpus for AFL++ fuzzing...\n\n");

  // Create directories
  printf("Creating directories...\n");
  mkdir_p("fuzz");
  mkdir_p("fuzz/corpus");
  mkdir_p("fuzz/corpus/decoder");
  mkdir_p("fuzz/corpus/encoder");
  mkdir_p("fuzz/corpus/roundtrip");
  mkdir_p("fuzz/corpus/rle_decoder");
  mkdir_p("fuzz/corpus/rle_encoder");
  mkdir_p("fuzz/corpus/rle_roundtrip");
  mkdir_p("fuzz/corpus/lzw_decoder");
  mkdir_p("fuzz/corpus/lzw_encoder");
  mkdir_p("fuzz/corpus/lzw_roundtrip");
  printf("\n");

  // Decoder corpus: valid compressed data
  printf("Generating decoder corpus (valid deflate streams)...\n");

  corpus_entry_t decoder_seeds[] = {
      {"empty_stored", v1_compressed, sizeof(v1_compressed)},
      {"single_byte", v2_compressed, sizeof(v2_compressed)},
      {"hello_stored", v3_compressed, sizeof(v3_compressed)},
      {"hello_world_fixed", v4_compressed, sizeof(v4_compressed)},
      {"repeated_abc", v5_compressed, sizeof(v5_compressed)},
      {"zeros_100", v6_compressed, sizeof(v6_compressed)},
      {"pangram", v9_compressed, sizeof(v9_compressed)},
  };

  for (size_t i = 0; i < sizeof(decoder_seeds) / sizeof(decoder_seeds[0]);
       i++) {
    snprintf(path, sizeof(path), "fuzz/corpus/decoder/%s.bin",
        decoder_seeds[i].name);
    write_file(path, decoder_seeds[i].data, decoder_seeds[i].size);
  }

  // Add some malformed inputs for decoder (edge cases)
  printf("\nGenerating decoder corpus (malformed/edge cases)...\n");

  // Truncated stored block header
  uint8_t trunc1[] = {0x01, 0x05}; // Missing LEN/NLEN
  snprintf(path, sizeof(path), "fuzz/corpus/decoder/truncated_stored.bin");
  write_file(path, trunc1, sizeof(trunc1));

  // Invalid block type (3)
  uint8_t invalid_btype[] = {0x07}; // BFINAL=1, BTYPE=3 (invalid)
  snprintf(path, sizeof(path), "fuzz/corpus/decoder/invalid_btype.bin");
  write_file(path, invalid_btype, sizeof(invalid_btype));

  // Stored block with NLEN mismatch
  uint8_t nlen_mismatch[] = {
      0x01, 0x05, 0x00, 0x00, 0x00}; // LEN=5, NLEN=0 (wrong)
  snprintf(path, sizeof(path), "fuzz/corpus/decoder/nlen_mismatch.bin");
  write_file(path, nlen_mismatch, sizeof(nlen_mismatch));

  // Single byte (minimal valid?)
  uint8_t single[] = {0x03}; // BFINAL=1, BTYPE=1 (fixed), incomplete
  snprintf(path, sizeof(path), "fuzz/corpus/decoder/single_byte.bin");
  write_file(path, single, sizeof(single));

  // Random-looking bytes
  uint8_t random_like[] = {
      0x78, 0x9C, 0x4B, 0xCB, 0xCF, 0x07, 0x00, 0x02, 0x82, 0x01, 0x45};
  snprintf(path, sizeof(path), "fuzz/corpus/decoder/random_bytes.bin");
  write_file(path, random_like, sizeof(random_like));

  // Encoder/roundtrip corpus: plaintext data
  printf("\nGenerating encoder/roundtrip corpus (plaintext inputs)...\n");

  for (size_t i = 0;
       i < sizeof(plaintext_samples) / sizeof(plaintext_samples[0]); i++) {
    const char * text = plaintext_samples[i];
    size_t len = strlen(text);

    snprintf(path, sizeof(path), "fuzz/corpus/encoder/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);

    snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);

    snprintf(path, sizeof(path), "fuzz/corpus/rle_encoder/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);
    snprintf(path, sizeof(path), "fuzz/corpus/rle_roundtrip/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);

    snprintf(path, sizeof(path), "fuzz/corpus/lzw_encoder/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);
    snprintf(path, sizeof(path), "fuzz/corpus/lzw_roundtrip/sample_%zu.bin", i);
    write_file(path, (const uint8_t *)text, len);
  }

  // Generate some binary patterns
  printf("\nGenerating binary pattern inputs...\n");

  // All zeros
  uint8_t zeros[256] = {0};
  snprintf(path, sizeof(path), "fuzz/corpus/encoder/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));
  snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_encoder/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_roundtrip/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_encoder/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_roundtrip/zeros_256.bin");
  write_file(path, zeros, sizeof(zeros));

  // All 0xFF
  uint8_t ones[256];
  memset(ones, 0xFF, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/encoder/ones_256.bin");
  write_file(path, ones, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/ones_256.bin");
  write_file(path, ones, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_encoder/ones_256.bin");
  write_file(path, ones, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_roundtrip/ones_256.bin");
  write_file(path, ones, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_encoder/ones_256.bin");
  write_file(path, ones, sizeof(ones));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_roundtrip/ones_256.bin");
  write_file(path, ones, sizeof(ones));

  // Sequential bytes 0x00-0xFF
  uint8_t sequential[256];
  for (int i = 0; i < 256; i++) {
    sequential[i] = (uint8_t)i;
  }
  snprintf(path, sizeof(path), "fuzz/corpus/encoder/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));
  snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_encoder/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_roundtrip/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_encoder/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_roundtrip/sequential_256.bin");
  write_file(path, sequential, sizeof(sequential));

  // Alternating pattern
  uint8_t alternating[256];
  for (int i = 0; i < 256; i++) {
    alternating[i] = (i % 2) ? 0xFF : 0x00;
  }
  snprintf(path, sizeof(path), "fuzz/corpus/encoder/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));
  snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_encoder/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));
  snprintf(path, sizeof(path), "fuzz/corpus/rle_roundtrip/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_encoder/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_roundtrip/alternating_256.bin");
  write_file(path, alternating, sizeof(alternating));

  // Generate edge case sizes for encoder/roundtrip
  printf("\nGenerating edge case size inputs...\n");

  // Power-of-2 boundaries and off-by-one sizes
  size_t edge_sizes[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64,
      65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047,
      2048, 2049, 4095, 4096, 4097};

  for (size_t i = 0; i < sizeof(edge_sizes) / sizeof(edge_sizes[0]); i++) {
    size_t sz = edge_sizes[i];
    if (sz > 4097)
      continue; // Keep files reasonable

    uint8_t * buf = (uint8_t *)malloc(sz > 0 ? sz : 1);
    if (!buf)
      continue;

    // Fill with pattern
    for (size_t j = 0; j < sz; j++) {
      buf[j] = (uint8_t)(j & 0xFF);
    }

    snprintf(path, sizeof(path), "fuzz/corpus/encoder/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);
    snprintf(path, sizeof(path), "fuzz/corpus/roundtrip/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);
    snprintf(
        path, sizeof(path), "fuzz/corpus/rle_encoder/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);
    snprintf(
        path, sizeof(path), "fuzz/corpus/rle_roundtrip/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);
    snprintf(
        path, sizeof(path), "fuzz/corpus/lzw_encoder/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);
    snprintf(
        path, sizeof(path), "fuzz/corpus/lzw_roundtrip/edge_size_%zu.bin", sz);
    write_file(path, buf, sz);

    free(buf);
  }

  // Decoder corpora for RLE and LZW: a few valid and malformed edge cases.
  printf("\nGenerating RLE decoder corpus (PackBits/TGA examples)...\n");
  // PackBits literal "ABC": control=2 (3 literal bytes).
  uint8_t rle_packbits_abc[] = {0x02, 'A', 'B', 'C'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/packbits_abc.bin");
  write_file(path, rle_packbits_abc, sizeof(rle_packbits_abc));
  // PackBits repeat 'Z' x3: control=253 (256-253=3), byte='Z'.
  uint8_t rle_packbits_z3[] = {0xFD, 'Z'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/packbits_z3.bin");
  write_file(path, rle_packbits_z3, sizeof(rle_packbits_z3));
  // PackBits no-op (128) then literal "A".
  uint8_t rle_packbits_nop_a[] = {0x80, 0x00, 'A'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/packbits_nop_a.bin");
  write_file(path, rle_packbits_nop_a, sizeof(rle_packbits_nop_a));
  // TGA raw "ABC": header=2 (count=3), then 3 bytes.
  uint8_t rle_tga_abc[] = {0x02, 'A', 'B', 'C'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/tga_abc.bin");
  write_file(path, rle_tga_abc, sizeof(rle_tga_abc));
  // TGA run 'Z' x3: header=0x82 (run, count=3), then 'Z'.
  uint8_t rle_tga_z3[] = {0x82, 'Z'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/tga_z3.bin");
  write_file(path, rle_tga_z3, sizeof(rle_tga_z3));
  // Truncated examples (should error, not crash).
  uint8_t rle_trunc1[] = {0x02, 'A'};
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/trunc_literal.bin");
  write_file(path, rle_trunc1, sizeof(rle_trunc1));
  uint8_t rle_trunc2[] = {0xFD}; // repeat token missing byte
  snprintf(path, sizeof(path), "fuzz/corpus/rle_decoder/trunc_repeat.bin");
  write_file(path, rle_trunc2, sizeof(rle_trunc2));

  printf("\nGenerating LZW decoder corpus (GIF/TIFF minimal streams)...\n");
  // Minimal GIF-profile (LSB) stream: CLEAR(256), EOI(257) with 9-bit codes.
  uint8_t lzw_gif_clear_eoi[] = {0x00, 0x03, 0x02};
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_decoder/gif_clear_eoi.bin");
  write_file(path, lzw_gif_clear_eoi, sizeof(lzw_gif_clear_eoi));
  // Minimal TIFF-profile (MSB) stream: CLEAR(256), EOI(257) with 9-bit codes.
  uint8_t lzw_tiff_clear_eoi[] = {0x80, 0x40, 0x40};
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_decoder/tiff_clear_eoi.bin");
  write_file(path, lzw_tiff_clear_eoi, sizeof(lzw_tiff_clear_eoi));
  // Truncated bitstreams.
  uint8_t lzw_trunc1[] = {0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_decoder/trunc_1.bin");
  write_file(path, lzw_trunc1, sizeof(lzw_trunc1));
  uint8_t lzw_trunc2[] = {0x80, 0x40};
  snprintf(path, sizeof(path), "fuzz/corpus/lzw_decoder/trunc_2.bin");
  write_file(path, lzw_trunc2, sizeof(lzw_trunc2));

  // Generate zlib (RFC 1950) corpus
  //
  // The header is two bytes, so a fuzzer stumbles onto a well-formed one by
  // chance about once in 8,000 inputs.  These seeds put it past that
  // immediately, and cover each way the header can be wrong: the wrong
  // compression method, a window larger than the format allows, a FCHECK that
  // does not check, and FDICT, which is the one part of RFC 1950 the decoder
  // refuses rather than attempts.
  printf("\nGenerating zlib corpus directory...\n");
  mkdir_p("fuzz/corpus/zlib_decoder");
  mkdir_p("fuzz/corpus/zlib_roundtrip");

  // Minimal valid zlib stream: CMF=0x78 CINFO=7 CM=8, FLG=0x01 (FCHECK fits),
  // an empty stored deflate block, and Adler-32 of nothing, which is 1.
  uint8_t zlib_empty[] = {0x78, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/empty.zz");
  write_file(path, zlib_empty, sizeof(zlib_empty));

  // The three header bytes zlib itself emits for its three common levels.
  uint8_t zlib_level1[] = {0x78, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/level1.zz");
  write_file(path, zlib_level1, sizeof(zlib_level1));
  uint8_t zlib_level6[] = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/level6.zz");
  write_file(path, zlib_level6, sizeof(zlib_level6));
  uint8_t zlib_level9[] = {0x78, 0xda, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/level9.zz");
  write_file(path, zlib_level9, sizeof(zlib_level9));

  // A small window: CINFO=0 is a 256-byte window, the smallest the format
  // names.  CMF=0x08, FLG=0x1d makes 0x081d a multiple of 31.
  uint8_t zlib_small_window[] = {0x08, 0x1d, 0x03, 0x00, 0x00, 0x00, 0x00,
      0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/small_window.zz");
  write_file(path, zlib_small_window, sizeof(zlib_small_window));

  // FDICT set: 0x78bb is a multiple of 31 and 0xbb has bit 5 set.  Four bytes
  // of dictionary id follow.
  uint8_t zlib_fdict[] = {0x78, 0xbb, 0xde, 0xad, 0xbe, 0xef, 0x03, 0x00, 0x00,
      0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/fdict.zz");
  write_file(path, zlib_fdict, sizeof(zlib_fdict));

  // Each way the header can be malformed.
  uint8_t zlib_bad_cm[] = {0x79, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/bad_cm.zz");
  write_file(path, zlib_bad_cm, sizeof(zlib_bad_cm));
  uint8_t zlib_bad_cinfo[] = {0x8c, 0x62, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/bad_cinfo.zz");
  write_file(path, zlib_bad_cinfo, sizeof(zlib_bad_cinfo));
  uint8_t zlib_bad_fcheck[] = {0x78, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/bad_fcheck.zz");
  write_file(path, zlib_bad_fcheck, sizeof(zlib_bad_fcheck));

  // A stream whose Adler-32 does not match its data.
  uint8_t zlib_bad_adler[] = {0x78, 0x01, 0x03, 0x00, 0xde, 0xad, 0xbe, 0xef};
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/bad_adler.zz");
  write_file(path, zlib_bad_adler, sizeof(zlib_bad_adler));

  // Cut short at each stage.
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/trunc_header.zz");
  write_file(path, zlib_empty, 1);
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/trunc_body.zz");
  write_file(path, zlib_empty, 3);
  snprintf(path, sizeof(path), "fuzz/corpus/zlib_decoder/trunc_trailer.zz");
  write_file(path, zlib_empty, 6);

  // Round-trip seeds are plain input; the first byte picks the settings.
  {
    uint8_t seed_text[] = "the quick brown fox jumps over the lazy dog";
    snprintf(path, sizeof(path), "fuzz/corpus/zlib_roundtrip/text.bin");
    write_file(path, seed_text, sizeof(seed_text) - 1);
    uint8_t seed_zeros[256];
    memset(seed_zeros, 0, sizeof(seed_zeros));
    snprintf(path, sizeof(path), "fuzz/corpus/zlib_roundtrip/zeros.bin");
    write_file(path, seed_zeros, sizeof(seed_zeros));
    uint8_t seed_one[] = {0x42};
    snprintf(path, sizeof(path), "fuzz/corpus/zlib_roundtrip/one_byte.bin");
    write_file(path, seed_one, sizeof(seed_one));
  }

  // Generate gzip corpus with various header combinations
  printf("\nGenerating gzip corpus directory...\n");
  mkdir_p("fuzz/corpus/gzip_decoder");
  mkdir_p("fuzz/corpus/gzip_encoder");
  mkdir_p("fuzz/corpus/gzip_roundtrip");

  // Minimal valid gzip (empty content)
  // ID1=1f, ID2=8b, CM=08, FLG=00, MTIME=0, XFL=0, OS=ff,
  // deflate empty stored block, CRC32=0, ISIZE=0
  uint8_t gzip_empty[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xff, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/empty.gz");
  write_file(path, gzip_empty, sizeof(gzip_empty));

  // Gzip with FNAME flag set (0x08)
  uint8_t gzip_fname[] = {0x1f, 0x8b, 0x08, 0x08,      // magic + CM + FLG=FNAME
      0x00, 0x00, 0x00, 0x00,                          // MTIME
      0x00, 0xff,                                      // XFL, OS
      't', 'e', 's', 't', '.', 't', 'x', 't', 0x00,    // FNAME
      0x01, 0x00, 0x00, 0xff, 0xff,                    // deflate empty
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // CRC+ISIZE
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/with_fname.gz");
  write_file(path, gzip_fname, sizeof(gzip_fname));

  // Gzip with FCOMMENT flag set (0x10)
  uint8_t gzip_fcomment[] = {0x1f, 0x8b, 0x08, 0x10, // FLG=FCOMMENT
      0x00, 0x00, 0x00, 0x00, 0x00, 0xff,            // MTIME, XFL, OS
      'H', 'e', 'l', 'l', 'o', 0x00,                 // FCOMMENT
      0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/with_fcomment.gz");
  write_file(path, gzip_fcomment, sizeof(gzip_fcomment));

  // Gzip with FEXTRA flag set (0x04)
  uint8_t gzip_fextra[] = {0x1f, 0x8b, 0x08, 0x04, // FLG=FEXTRA
      0x00, 0x00, 0x00, 0x00, 0x00, 0xff,          // MTIME, XFL, OS
      0x04, 0x00,                                  // XLEN=4
      'E', 'X', 0x02, 0x00, // Extra data (SI1, SI2, LEN=2, data)
      0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/with_fextra.gz");
  write_file(path, gzip_fextra, sizeof(gzip_fextra));

  // Gzip with FHCRC flag set (0x02) - need to compute actual CRC16
  uint8_t gzip_fhcrc[] = {0x1f, 0x8b, 0x08, 0x02, // FLG=FHCRC
      0x00, 0x00, 0x00, 0x00, 0x00, 0xff,         // MTIME, XFL, OS
      0x17, 0xc9, // FHCRC (CRC16 of header bytes 0-9)
      0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/with_fhcrc.gz");
  write_file(path, gzip_fhcrc, sizeof(gzip_fhcrc));

  // Gzip with FTEXT flag set (0x01)
  uint8_t gzip_ftext[] = {0x1f, 0x8b, 0x08, 0x01, // FLG=FTEXT
      0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/with_ftext.gz");
  write_file(path, gzip_ftext, sizeof(gzip_ftext));

  // Gzip with all flags (FTEXT|FHCRC|FEXTRA|FNAME|FCOMMENT = 0x1F)
  uint8_t gzip_all_flags[] = {0x1f, 0x8b, 0x08, 0x1f, // All flags
      0x00, 0x00, 0x00, 0x00, 0x00, 0xff,             // MTIME, XFL, OS
      0x04, 0x00, 'E', 'X', 0x02, 0x00,               // FEXTRA
      'f', 'i', 'l', 'e', 0x00,                       // FNAME
      'c', 'o', 'm', 'm', 'e', 'n', 't', 0x00,        // FCOMMENT
      0x87, 0x56,                                     // FHCRC (pre-computed)
      0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00};
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/all_flags.gz");
  write_file(path, gzip_all_flags, sizeof(gzip_all_flags));

  // Truncated gzip streams for decoder testing
  printf("\nGenerating truncated gzip streams...\n");

  // Truncated in magic
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/trunc_magic.gz");
  write_file(path, gzip_empty, 1);

  // Truncated in header
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/trunc_header.gz");
  write_file(path, gzip_empty, 5);

  // Truncated in body
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/trunc_body.gz");
  write_file(path, gzip_empty, 12);

  // Truncated in trailer
  snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/trunc_trailer.gz");
  write_file(path, gzip_empty, 18);

  // Long filename (near limit boundary)
  printf("\nGenerating long header field tests...\n");
  size_t long_name_len = 1000; // Near boundary
  uint8_t * long_name_buf =
      (uint8_t *)malloc(10 + long_name_len + 1 + 5 + 8 + 10);
  if (long_name_buf) {
    size_t pos = 0;
    long_name_buf[pos++] = 0x1f;
    long_name_buf[pos++] = 0x8b;
    long_name_buf[pos++] = 0x08;
    long_name_buf[pos++] = 0x08; // FNAME
    for (int i = 0; i < 6; i++)
      long_name_buf[pos++] = 0x00;
    for (size_t i = 0; i < long_name_len; i++)
      long_name_buf[pos++] = 'A' + (i % 26);
    long_name_buf[pos++] = 0x00; // null terminator
    // deflate empty
    long_name_buf[pos++] = 0x01;
    long_name_buf[pos++] = 0x00;
    long_name_buf[pos++] = 0x00;
    long_name_buf[pos++] = 0xff;
    long_name_buf[pos++] = 0xff;
    // CRC32 + ISIZE
    for (int i = 0; i < 8; i++)
      long_name_buf[pos++] = 0x00;

    snprintf(path, sizeof(path), "fuzz/corpus/gzip_decoder/long_fname.gz");
    write_file(path, long_name_buf, pos);
    free(long_name_buf);
  }

  // Every method, seeded by the library rather than by hand.
  //
  // The vectors above are hand-written deflate and gzip frames, and they are
  // worth keeping - they are edge cases somebody chose. But they only ever
  // covered deflate, gzip, RLE and LZW, so the LZ4 and zstd campaigns had no
  // seeds at all and gzip's roundtrip had an empty directory. A campaign that
  // starts from one minimal frame is a campaign starting from nothing.
  //
  // So the encoders produce the decoder seeds. A seed corpus made by the
  // implementation under test is the normal arrangement - AFL mutates it, and
  // fuzz/regression holds the inputs that actually found something - and it
  // means a method added later is seeded by adding it to this list.
  printf("\nGenerating per-method corpora from the library...\n");
  {
    // The directory prefix each method's campaign targets name. deflate's
    // are the unprefixed ones, for historical reasons.
    static const struct {
      const char * method;
      const char * prefix;
    } kMethods[] = {
        {"deflate", ""},
        {"gzip", "gzip_"},
        {"zlib", "zlib_"},
        {"lz4", "lz4_"},
        {"lzw", "lzw_"},
        {"rle", "rle_"},
        {"zstd", "zstd_"},
        {"brotli", "brotli_"},
    };

    gcomp_registry_t * registry = gcomp_registry_default();
    if (!registry) {
      fprintf(stderr, "Error: no default registry; cannot seed\n");
      return 1;
    }

    for (size_t m = 0; m < sizeof(kMethods) / sizeof(kMethods[0]); m++) {
      const char * method = kMethods[m].method;
      const char * prefix = kMethods[m].prefix;

      // Deliberately short: "fuzz/corpus/" plus a prefix and a name is well
      // under this, and a size the compiler can see fits is what keeps
      // -Wformat-truncation quiet when these are pasted into path[].
      char dec_dir[64];
      char enc_dir[64];
      char rt_dir[64];
      snprintf(dec_dir, sizeof(dec_dir), "fuzz/corpus/%sdecoder", prefix);
      snprintf(enc_dir, sizeof(enc_dir), "fuzz/corpus/%sencoder", prefix);
      snprintf(rt_dir, sizeof(rt_dir), "fuzz/corpus/%sroundtrip", prefix);
      mkdir_p(dec_dir);
      mkdir_p(enc_dir);
      mkdir_p(rt_dir);

      for (size_t i = 0;
           i < sizeof(plaintext_samples) / sizeof(plaintext_samples[0]); i++) {
        const uint8_t * text = (const uint8_t *)plaintext_samples[i];
        size_t len = strlen(plaintext_samples[i]);

        // The raw sample feeds the encoder and roundtrip harnesses.
        snprintf(path, sizeof(path), "%s/sample_%zu.bin", enc_dir, i);
        write_file(path, text, len);
        snprintf(path, sizeof(path), "%s/sample_%zu.bin", rt_dir, i);
        write_file(path, text, len);

        // Compressed, it feeds the decoder harness. RLE can expand, so the
        // buffer is generous rather than exact.
        uint8_t encoded[8192];
        size_t produced = 0;
        gcomp_status_t status = gcomp_encode_buffer(registry, method, NULL,
            text, len, encoded, sizeof(encoded), &produced);
        if (status != GCOMP_OK) {
          fprintf(stderr, "Error: %s could not encode sample %zu (%d)\n",
              method, i, (int)status);
          return 1;
        }
        snprintf(path, sizeof(path), "%s/sample_%zu.bin", dec_dir, i);
        write_file(path, encoded, produced);
      }

      // One larger, more structured input per method: something with matches
      // worth finding, so the decoder seeds are not all a single block of
      // literals.
      {
        uint8_t prose[4096];
        for (size_t i = 0; i < sizeof(prose); i++) {
          prose[i] = (uint8_t)('a' + (i % 23) + ((i / 97) % 3));
        }
        snprintf(path, sizeof(path), "%s/prose_4096.bin", enc_dir);
        write_file(path, prose, sizeof(prose));
        snprintf(path, sizeof(path), "%s/prose_4096.bin", rt_dir);
        write_file(path, prose, sizeof(prose));

        uint8_t encoded[16384];
        size_t produced = 0;
        gcomp_status_t status = gcomp_encode_buffer(registry, method, NULL,
            prose, sizeof(prose), encoded, sizeof(encoded), &produced);
        if (status != GCOMP_OK) {
          fprintf(stderr, "Error: %s could not encode prose (%d)\n", method,
              (int)status);
          return 1;
        }
        snprintf(path, sizeof(path), "%s/prose_4096.bin", dec_dir);
        write_file(path, encoded, produced);
      }
    }
  }

  printf("\n");
  printf("Seed corpus generation complete!\n");
  printf("\n");
  printf("Corpus locations: fuzz/corpus/<method>_{decoder,encoder,roundtrip}\n");
  printf("(deflate's are the unprefixed decoder/, encoder/, roundtrip/.)\n");
  printf("\n");
  printf("Named in full:\n");
  printf("  Decoder:      fuzz/corpus/decoder/\n");
  printf("  Encoder:      fuzz/corpus/encoder/\n");
  printf("  Roundtrip:    fuzz/corpus/roundtrip/\n");
  printf("  RLE Decoder:  fuzz/corpus/rle_decoder/\n");
  printf("  RLE Encoder:  fuzz/corpus/rle_encoder/\n");
  printf("  RLE Roundtrip:fuzz/corpus/rle_roundtrip/\n");
  printf("  LZW Decoder:  fuzz/corpus/lzw_decoder/\n");
  printf("  LZW Encoder:  fuzz/corpus/lzw_encoder/\n");
  printf("  LZW Roundtrip:fuzz/corpus/lzw_roundtrip/\n");
  printf("  Gzip Decoder: fuzz/corpus/gzip_decoder/\n");
  printf("  Gzip Encoder: fuzz/corpus/gzip_encoder/\n");
  printf("\n");

  return 0;
}
