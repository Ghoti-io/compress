/**
 * @file fuzz_brotli_encoder.c
 *
 * AFL++ fuzz harness for the Brotli encoder.
 *
 * Reads arbitrary plaintext from stdin and encodes it, streaming and in one
 * shot, at a window size taken from the input. An error from the encoder is
 * not a finding. A crash is.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/brotli.h"
#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/registry.h"
#include "ghoti.io/compress/stream.h"

static gcomp_registry_t * registry;

#define MAX_INPUT_SIZE (256 * 1024)
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + 64 * 1024)

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
  return (int64_t)(10 + (n % 15));
}

/* A second byte, so the level varies independently of the window. The harness
 * used to set neither and so drove whichever level was the default, which
 * meant level 0's stored layout lost its fuzzing the moment level 1 became the
 * default rather than gaining any. An input too short to carry the byte gets
 * the default, so the shortest cases still exercise what a caller sees. */
static int64_t level_from(const uint8_t * input, size_t input_size) {
  return input_size < 2 ? 1 : (int64_t)(input[1] & 1u);
}

static gcomp_options_t * encoder_options(int64_t lgwin, int64_t level) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (opts) {
    gcomp_options_set_int64(opts, "brotli.lgwin", lgwin);
    gcomp_options_set_int64(opts, "brotli.level", level);
  }
  return opts;
}

static void fuzz_encoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output, int64_t lgwin, int64_t level) {
  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_options_t * opts = encoder_options(lgwin, level);
  gcomp_encode_buffer(registry, "brotli", opts, input, input_size, output,
      output_size, &output_size);
  if (opts) {
    gcomp_options_destroy(opts);
  }
}

static void fuzz_encoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output, int64_t lgwin, int64_t level) {
  gcomp_encoder_t * encoder = NULL;
  gcomp_options_t * opts = encoder_options(lgwin, level);
  gcomp_status_t status =
      gcomp_encoder_create(registry, "brotli", opts, &encoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t in_offset = 0;
  size_t out_offset = 0;
  size_t chunk_size = 1;
  int flushed = 0;

  while (in_offset < input_size && out_offset < OUTPUT_BUFFER_SIZE) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - in_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + in_offset), .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = output + out_offset,
        .size = OUTPUT_BUFFER_SIZE - out_offset,
        .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    in_offset += in_buf.used;
    out_offset += out_buf.used;
    if (status != GCOMP_OK || (in_buf.used == 0 && out_buf.used == 0)) {
      gcomp_encoder_destroy(encoder);
      return;
    }

    if (!flushed && in_offset * 2 >= input_size &&
        out_offset < OUTPUT_BUFFER_SIZE) {
      gcomp_buffer_t flush_buf = {.data = output + out_offset,
          .size = OUTPUT_BUFFER_SIZE - out_offset,
          .used = 0};
      status = gcomp_encoder_flush(encoder, &flush_buf, GCOMP_FLUSH_SYNC);
      out_offset += flush_buf.used;
      flushed = 1;
      if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
        gcomp_encoder_destroy(encoder);
        return;
      }
    }
  }

  if (out_offset < OUTPUT_BUFFER_SIZE) {
    gcomp_buffer_t finish_buf = {.data = output + out_offset,
        .size = OUTPUT_BUFFER_SIZE - out_offset,
        .used = 0};
    gcomp_encoder_finish(encoder, &finish_buf);
  }
  gcomp_encoder_destroy(encoder);
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

  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  int64_t lgwin = window_from(input, input_size);
  int64_t level = level_from(input, input_size);
  fuzz_encoder_streaming(input, input_size, output, lgwin, level);
  fuzz_encoder_buffer(input, input_size, output, lgwin, level);

  free(output);
  free(input);
  return 0;
}
