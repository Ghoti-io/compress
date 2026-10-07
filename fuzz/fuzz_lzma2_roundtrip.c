/**
 * @file fuzz_lzma2_roundtrip.c
 *
 * AFL++ fuzz harness for a lzma2 encode-then-decode round trip.
 *
 * Compresses arbitrary input and decompresses it. A mismatch, or a decode
 * that rejects a stream this encoder just wrote, aborts.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/lzma.h"
#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/registry.h"
#include "ghoti.io/compress/stream.h"

static gcomp_registry_t * registry;

#define MAX_INPUT_SIZE (64 * 1024)
#define COMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 64 * 1024)
#define DECOMPRESS_BUFFER_SIZE (MAX_INPUT_SIZE + 64)

static uint8_t * read_stdin(size_t * size_out) {
  size_t capacity = 4096;
  size_t size = 0;
  uint8_t * buffer = malloc(capacity);
  if (!buffer) {
    return NULL;
  }

  int c;
  while ((c = getchar()) != EOF) {
    if (size >= MAX_INPUT_SIZE) {
      break;
    }
    if (size >= capacity) {
      capacity *= 2;
      if (capacity > MAX_INPUT_SIZE) {
        capacity = MAX_INPUT_SIZE;
      }
      uint8_t * new_buffer = realloc(buffer, capacity);
      if (!new_buffer) {
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
    }
    buffer[size++] = (uint8_t)c;
  }

  *size_out = size;
  return buffer;
}

static int64_t window_from(const uint8_t * input, size_t input_size) {
  unsigned n = input_size == 0 ? 0 : input[0];
  return (int64_t)(n % 3); /* the preset: 0, 1 or 2 */
}

/* A second byte, so the level varies independently of the window. The harness
 * used to set neither and so drove whichever level was the default, which
 * meant level 0's stored layout lost its fuzzing the moment level 1 became the
 * default rather than gaining any. An input too short to carry the byte gets
 * the default, so the shortest cases still exercise what a caller sees. */
static int64_t level_from(const uint8_t * input, size_t input_size) {
  /* The dictionary as a power of two, 4 KiB to 512 KiB: small enough that the
   * window slides inside a fuzzer-sized input, which is where the match
   * finder's bookkeeping lives. */
  return input_size < 2 ? 16 : (int64_t)(12 + (input[1] % 8));
}

static gcomp_options_t * encoder_options(int64_t lgwin, int64_t level) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_int64(opts, "lzma2.preset", lgwin);
    gcomp_options_set_uint64(opts, "lzma2.dict_size", 1ull << level);
  }
  return opts;
}

static gcomp_options_t * decoder_options(void) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (!opts) {
    return NULL;
  }
  gcomp_options_set_uint64(
      opts, "limits.max_output_bytes", (uint64_t)DECOMPRESS_BUFFER_SIZE);
  gcomp_options_set_uint64(opts, "limits.max_window_bytes", 1ull << 22);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);
  return opts;
}

static void fail_mismatch(void) {
  abort();
}

static void test_roundtrip_buffer(const uint8_t * original, size_t original_size,
    uint8_t * compress_buf, uint8_t * decompress_buf, int64_t lgwin,
    int64_t level) {
  size_t compressed_size = COMPRESS_BUFFER_SIZE;
  size_t decompressed_size = DECOMPRESS_BUFFER_SIZE;
  gcomp_options_t * enc_opts = encoder_options(lgwin, level);
  gcomp_status_t status = gcomp_encode_buffer(registry, "lzma2", enc_opts, original,
      original_size, compress_buf, compressed_size, &compressed_size);
  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }
  if (status != GCOMP_OK) {
    fail_mismatch();
  }

  gcomp_options_t * dec_opts = decoder_options();
  status = gcomp_decode_buffer(registry, "lzma2", dec_opts, compress_buf,
      compressed_size, decompress_buf, decompressed_size, &decompressed_size);
  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }
  if (status != GCOMP_OK || decompressed_size != original_size) {
    fail_mismatch();
  }
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
    fail_mismatch();
  }
}

static void test_roundtrip_streaming(const uint8_t * original,
    size_t original_size, uint8_t * compress_buf, uint8_t * decompress_buf,
    int64_t lgwin, int64_t level) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_decoder_t * decoder = NULL;
  gcomp_options_t * enc_opts = encoder_options(lgwin, level);
  gcomp_status_t status =
      gcomp_encoder_create(registry, "lzma2", enc_opts, &encoder);
  if (enc_opts) {
    gcomp_options_destroy(enc_opts);
  }
  if (status != GCOMP_OK) {
    fail_mismatch();
  }

  size_t in_offset = 0;
  size_t out_offset = 0;
  size_t chunk_size = 1;

  while (in_offset < original_size) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = original_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }
    gcomp_buffer_t in_buf = {
        .data = (void *)(original + in_offset), .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = compress_buf + out_offset,
        .size = COMPRESS_BUFFER_SIZE - out_offset,
        .used = 0};
    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK || in_buf.used == 0) {
      gcomp_encoder_destroy(encoder);
      fail_mismatch();
    }
  }

  gcomp_buffer_t finish_buf = {.data = compress_buf + out_offset,
      .size = COMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_encoder_finish(encoder, &finish_buf);
  size_t compressed_size = out_offset + finish_buf.used;
  gcomp_encoder_destroy(encoder);
  if (status != GCOMP_OK) {
    fail_mismatch();
  }

  gcomp_options_t * dec_opts = decoder_options();
  status = gcomp_decoder_create(registry, "lzma2", dec_opts, &decoder);
  if (dec_opts) {
    gcomp_options_destroy(dec_opts);
  }
  if (status != GCOMP_OK) {
    fail_mismatch();
  }

  in_offset = 0;
  out_offset = 0;
  chunk_size = 1;
  while (in_offset < compressed_size) {
    chunk_size = (chunk_size * 11 + 17) % 1024 + 1;
    size_t remaining = compressed_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }
    gcomp_buffer_t in_buf = {.data = (void *)(compress_buf + in_offset),
        .size = chunk_size,
        .used = 0};
    gcomp_buffer_t out_buf = {.data = decompress_buf + out_offset,
        .size = DECOMPRESS_BUFFER_SIZE - out_offset,
        .used = 0};
    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK || (in_buf.used == 0 && out_buf.used == 0)) {
      gcomp_decoder_destroy(decoder);
      fail_mismatch();
    }
  }

  gcomp_buffer_t dec_finish = {.data = decompress_buf + out_offset,
      .size = DECOMPRESS_BUFFER_SIZE - out_offset,
      .used = 0};
  status = gcomp_decoder_finish(decoder, &dec_finish);
  size_t decompressed_size = out_offset + dec_finish.used;
  gcomp_decoder_destroy(decoder);
  if (status != GCOMP_OK || decompressed_size != original_size) {
    fail_mismatch();
  }
  if (original_size > 0 &&
      memcmp(original, decompress_buf, original_size) != 0) {
    fail_mismatch();
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {
  registry = gcomp_registry_default();
  if (!registry) {
    return 0;
  }

  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  uint8_t * compress_buf = malloc(COMPRESS_BUFFER_SIZE);
  uint8_t * decompress_buf = malloc(DECOMPRESS_BUFFER_SIZE);
  if (!compress_buf || !decompress_buf) {
    free(input);
    free(compress_buf);
    free(decompress_buf);
    return 0;
  }

  int64_t lgwin = window_from(input, input_size);
  int64_t level = level_from(input, input_size);
  test_roundtrip_buffer(
      input, input_size, compress_buf, decompress_buf, lgwin, level);
  test_roundtrip_streaming(
      input, input_size, compress_buf, decompress_buf, lgwin, level);

  free(decompress_buf);
  free(compress_buf);
  free(input);
  return 0;
}
