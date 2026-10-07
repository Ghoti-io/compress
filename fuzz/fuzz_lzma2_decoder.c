/**
 * @file fuzz_lzma2_decoder.c
 *
 * AFL++ fuzz harness for the lzma2 decoder.
 *
 * Reads arbitrary bytes from stdin and feeds them to the streaming and
 * one-shot decoders. A crash, an ASan report, or a hang is a finding. A
 * corrupt stream that returns an error is not.
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

#define MAX_INPUT_SIZE (256 * 1024)
#define OUTPUT_BUFFER_SIZE (4 * 1024 * 1024)

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

static gcomp_options_t * decoder_options(void) {
  gcomp_options_t * opts = NULL;
  gcomp_options_create(&opts);
  if (!opts) {
    return NULL;
  }
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", OUTPUT_BUFFER_SIZE);
  gcomp_options_set_uint64(opts, "limits.max_window_bytes", 1ull << 22);
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 32u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 4096);
  return opts;
}

static void fuzz_decoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output) {
  gcomp_decoder_t * decoder = NULL;
  gcomp_options_t * opts = decoder_options();
  gcomp_status_t status =
      gcomp_decoder_create(registry, "lzma2", opts, &decoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;

  while (input_offset < input_size && output_offset < OUTPUT_BUFFER_SIZE) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + input_offset), .size = chunk_size, .used = 0};
    gcomp_buffer_t out_buf = {.data = output + output_offset,
        .size = OUTPUT_BUFFER_SIZE - output_offset,
        .used = 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    input_offset += in_buf.used;
    output_offset += out_buf.used;
    if (status != GCOMP_OK && status != GCOMP_ERR_LIMIT) {
      break;
    }
    if (in_buf.used == 0 && out_buf.used == 0) {
      break;
    }
  }

  if (output_offset < OUTPUT_BUFFER_SIZE) {
    gcomp_buffer_t out_buf = {.data = output + output_offset,
        .size = OUTPUT_BUFFER_SIZE - output_offset,
        .used = 0};
    gcomp_decoder_finish(decoder, &out_buf);
  }
  gcomp_decoder_destroy(decoder);
}

static void fuzz_decoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output) {
  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_options_t * opts = decoder_options();
  gcomp_decode_buffer(registry, "lzma2", opts, input, input_size, output,
      output_size, &output_size);
  if (opts) {
    gcomp_options_destroy(opts);
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

  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  fuzz_decoder_streaming(input, input_size, output);
  fuzz_decoder_buffer(input, input_size, output);

  free(output);
  free(input);
  return 0;
}
