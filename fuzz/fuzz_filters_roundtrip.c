/**
 * @file fuzz_filters_roundtrip.c
 *
 * AFL++ fuzz harness for the `delta` and `bcj` filters.
 *
 * The first bytes choose the filter, its option and where the stream starts;
 * the rest is the data. Each filter is exact in both directions, so the input
 * goes through the forward filter and back and must come out the same, and
 * goes through the inverse and forward and must come out the same too: an
 * arbitrary byte string is as valid an input to either direction as the output
 * of the other, which is what lets a fuzzer reach branch encodings no corpus
 * of real programs contains. The bytes are cut at lengths taken from the data
 * itself, since where a converter must hold bytes back depends on the cut.
 * Any mismatch, or a call that refuses, aborts.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/filter.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/registry.h"
#include "ghoti.io/compress/stream.h"

#define MAX_INPUT_SIZE (64 * 1024)

static const char * const k_arch[] = {
    "x86", "powerpc", "ia64", "arm", "armthumb", "sparc", "arm64"};

static uint8_t * read_stdin(size_t * size_out) {
  uint8_t * buffer = malloc(MAX_INPUT_SIZE);
  size_t size = 0;
  int c;
  if (!buffer) {
    return NULL;
  }
  while (size < MAX_INPUT_SIZE && (c = getchar()) != EOF) {
    buffer[size++] = (uint8_t)c;
  }
  *size_out = size;
  return buffer;
}

static void fail(void) {
  abort();
}

/* One pass of a filter over the data in pieces whose sizes come from the data,
 * into pieces of output whose sizes do too. */
static size_t pass(gcomp_registry_t * registry, const char * method,
    gcomp_options_t * opts, int encode, const uint8_t * in, size_t in_size,
    uint8_t * out, size_t out_cap, unsigned cut) {
  gcomp_encoder_t * enc = NULL;
  gcomp_decoder_t * dec = NULL;
  size_t ipos = 0, opos = 0;
  gcomp_status_t s;
  if (encode) {
    if (gcomp_encoder_create(registry, method, opts, &enc) != GCOMP_OK) {
      fail();
    }
  }
  else if (gcomp_decoder_create(registry, method, opts, &dec) != GCOMP_OK) {
    fail();
  }
  while (ipos < in_size) {
    size_t take = 1 + (cut * 2654435761u >> 7) % 37u;
    gcomp_buffer_t ib, ob;
    cut = cut * 1664525u + 1013904223u;
    if (take > in_size - ipos) {
      take = in_size - ipos;
    }
    ib.data = (void *)(in + ipos);
    ib.size = take;
    ib.used = 0;
    while (ib.used < ib.size) {
      size_t room = 1 + (cut >> 11) % 29u;
      size_t before = ib.used;
      cut = cut * 1664525u + 1013904223u;
      if (room > out_cap - opos) {
        room = out_cap - opos;
      }
      ob.data = out + opos;
      ob.size = room;
      ob.used = 0;
      s = encode ? gcomp_encoder_update(enc, &ib, &ob)
                 : gcomp_decoder_update(dec, &ib, &ob);
      opos += ob.used;
      if (s != GCOMP_OK || (ib.used == before && ob.used == 0)) {
        fail();
      }
    }
    ipos += take;
  }
  for (;;) {
    gcomp_buffer_t ob = {out + opos, out_cap - opos, 0};
    s = encode ? gcomp_encoder_finish(enc, &ob) : gcomp_decoder_finish(dec, &ob);
    opos += ob.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT) {
      fail();
    }
  }
  if (enc) {
    gcomp_encoder_destroy(enc);
  }
  if (dec) {
    gcomp_decoder_destroy(dec);
  }
  return opos;
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {
  gcomp_registry_t * registry = gcomp_registry_default();
  size_t size = 0, n;
  uint8_t * input = NULL;
  uint8_t * mid = NULL;
  uint8_t * back = NULL;
  gcomp_options_t * opts = NULL;
  const char * method;
  unsigned sel, cut;
  const uint8_t * data;
  if (!registry) {
    return 0;
  }
  input = read_stdin(&size);
  if (!input) {
    return 0;
  }
  if (size < 6) {
    free(input);
    return 0;
  }
  sel = input[0];
  cut = (unsigned)input[1] | (unsigned)input[2] << 8;
  data = input + 6;
  n = size - 6;
  mid = malloc(n + 1);
  back = malloc(n + 1);
  gcomp_options_create(&opts);
  if (!mid || !back || !opts) {
    goto done;
  }
  if (sel & 1) {
    method = "delta";
    gcomp_options_set_int64(opts, "delta.distance", (int64_t)(1 + input[3]));
  }
  else {
    uint32_t start = (uint32_t)input[3] << 4 | (uint32_t)input[4] << 12 |
        (uint32_t)input[5] << 20;
    method = "bcj";
    gcomp_options_set_string(opts, "bcj.arch", k_arch[(sel >> 1) % 7]);
    gcomp_options_set_uint64(opts, "bcj.start_offset", start);
  }

  /* forward then back */
  if (pass(registry, method, opts, 1, data, n, mid, n + 1, cut) != n) {
    fail();
  }
  if (pass(registry, method, opts, 0, mid, n, back, n + 1, cut + 1) != n ||
      (n && memcmp(back, data, n) != 0)) {
    fail();
  }
  /* back then forward */
  if (pass(registry, method, opts, 0, data, n, mid, n + 1, cut + 2) != n) {
    fail();
  }
  if (pass(registry, method, opts, 1, mid, n, back, n + 1, cut + 3) != n ||
      (n && memcmp(back, data, n) != 0)) {
    fail();
  }

done:
  if (opts) {
    gcomp_options_destroy(opts);
  }
  free(mid);
  free(back);
  free(input);
  return 0;
}
