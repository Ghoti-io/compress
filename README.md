# Ghoti.io Compress

Streaming compression in C. Checksums (CRC-32, Adler-32, xxHash) and a
method registry sit beside the methods.

## Methods

This is what the library implements. Each method is written here against
libc.

- Deflate (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952).
- Brotli (RFC 7932), LZ4, LZW, RLE and zstd.

## Before you call it

- `GCOMP_ERR_LIMIT` means the output buffer is full and the stream is not finished. Call again with more room. `GCOMP_OK` from a finish means the stream is complete.
- Encoded bytes do not depend on how small that buffer was.
- `gcomp_encoder_flush()` hands a peer everything consumed so far without ending the stream.
- `gcomp_registry_default()` already holds the eight methods. A caller can build a registry of its own.

| Method | What it means here |
| --- | --- |
| `"deflate"` | RFC 1951. |
| `"zlib"` | RFC 1950. |
| `"gzip"` | RFC 1952. |
| `"lz4"` | `threads.count` encodes blocks in parallel. That output is byte-identical to the single-threaded stream. |
| `"brotli"` | RFC 7932. Level 1 (the default) writes LZ77 and Huffman; level 0 stores uncompressed meta-blocks. The decoder reads the format. |
| `"bzip2"` | Streams of Burrows-Wheeler blocks, as `bzip2` and libbz2 write them. The encoder sorts with SA-IS and is within a few bytes of libbz2's size; a flush ends the stream. |
| `"lzma"`, `"lzma2"` | LZMA-alone (`.lzma`, or raw with `lzma.raw`) and the LZMA2 chunk framing. The encoder is a fast-mode parser, within a few percent of liblzma at presets 0 to 3. |
| `"lzw"`, `"rle"` | The same registry, the same buffer rule. |
| `"zstd"` | `threads.count` encodes blocks in parallel. `seekable.h` is random access into a seekable frame. |

## Examples

```c
#include <ghoti.io/compress/compress.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char * message = "Hello, Compression!";
  size_t message_len = strlen(message);
  gcomp_registry_t * registry = gcomp_registry_default();

  size_t compressed_capacity = message_len + 256;
  uint8_t * compressed = malloc(compressed_capacity);
  size_t compressed_size = 0;
  gcomp_status_t status = gcomp_encode_buffer(registry, "deflate", NULL,
      message, message_len, compressed, compressed_capacity, &compressed_size);
  if (status != GCOMP_OK) {
    fprintf(stderr, "%s\n", gcomp_status_to_string(status));
    free(compressed);
    return 1;
  }

  uint8_t * plain = malloc(message_len);
  size_t plain_size = 0;
  status = gcomp_decode_buffer(registry, "deflate", NULL,
      compressed, compressed_size, plain, message_len, &plain_size);
  free(compressed);
  free(plain);
  return status == GCOMP_OK ? 0 : 1;
}
```

It prints nothing. A zero exit status is the round trip succeeding.
`examples/gzip_file.c` compresses a file. `examples/chunked_streaming.c`
feeds a stream through a fixed output buffer.

## Compile and link

Once the library is installed, pkg-config carries the include path, the
library, and its dependencies:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-compress-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-compress-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil) must already be installed where
pkg-config can see it. A dependency it cannot find is a hard error naming
the fix. No other library implements a method.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/compress test PREFIX="$PWD/.local"
```

`make test` is the suite. `make help` lists the rest, including
`make test-asan` and `make test-valgrind`.

| Target | What it does |
| --- | --- |
| `make examples` | The programs under `examples/` |
| `make bench` | Throughput micro-benchmarks |
| `make test-tsan` | The concurrency tests under ThreadSanitizer |
| `make fuzz-help` | AFL++ harnesses. Needs `afl++` installed |
| `make check-oracle` | Differentials against pinned reference tools, in a container |
| `make oracle-build` | Build that container |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `gcomp_` / `GCOMP_`, under `<ghoti.io/compress/...>`.
`<ghoti.io/compress/compress.h>` is the umbrella.

- **`registry.h`** — methods by name.
- **`stream.h`** — incremental encode and decode, flush, and finish. `gcomp_encode_buffer()` and `gcomp_decode_buffer()` are the one-shot forms.
- **`options.h`** — a key/value option set: level, window, threads, and the per-method knobs.
- **`limits.h`** — caps on memory and expansion. Defaults are set; override them for untrusted input.
- **`gzip.h`**, **`zlib.h`**, **`deflate.h`**, **`lz4.h`**, **`lzw.h`**, **`rle.h`**, **`zstd.h`** — one method, for a caller who wants it by name rather than through the registry.
- **`seekable.h`** — random access into a zstd seekable frame.
- **`crc32.h`**, **`adler32.h`**, **`xxhash32.h`**, **`xxhash64.h`** — the checksums.
- **`allocator.h`** — `gcomp_allocator_t`, which is cutil's `GCU_Allocator`.

[Methods](#methods) is what is implemented.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

Found through pkg-config, and the installed `.pc` file names it, so a
program that links `ghoti.io-compress-0` links this too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator, the thread pool and the sequencer that parallel compression is built on.

## Documentation

| Page | What it settles |
| --- | --- |
| [documentation/architecture.md](documentation/architecture.md) | How a stream, a method and the registry fit together |
| [documentation/threading.md](documentation/threading.md) | Parallel block compression |
| [documentation/testing/fuzzing.md](documentation/testing/fuzzing.md) | The AFL++ harnesses |
| [documentation/testing/oracles.md](documentation/testing/oracles.md) | The reference tools and how to run them |

`make docs` builds the manual.

## Status

All eight methods encode and decode, including through a bounded output
buffer.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
