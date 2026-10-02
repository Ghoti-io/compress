# Brotli

Brotli (RFC 7932) for the Ghoti.io Compress library. The method name is `"brotli"`.

The decoder reads the format: prefix codes, block switches, context maps, distances, and the static dictionary with its 121 transforms. The encoder writes the trivial compressor from section 11.1. It stores the input in uncompressed meta-blocks of at most 65536 bytes, aligned by an empty metadata block, and ends the stream with an empty last meta-block. That output is a valid brotli stream. It is not a small one. There is no `brotli.level` until a setting changes the bytes.

The stream has no magic number, so `gcomp_detect()` does not claim it. `gcomp_peek()` reads the window size from the first byte.

## Registration

Brotli is auto-registered with the default registry when the library loads:

```c
#include <ghoti.io/compress/compress.h>

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "brotli", NULL, &enc);
```

For a registry of your own, call `gcomp_method_brotli_register()`.

## Options

| Key | Type | Default | Who reads it | Description |
| --- | --- | --- | --- | --- |
| `brotli.lgwin` | int64 | 16 | encoder | Window bits written into the stream, 10..24. The decoder takes the window from the stream. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | decoder | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | decoder | Maximum working memory. The window is checked against this before it is allocated. |
| `limits.max_expansion_ratio` | uint64 | 1290556 | decoder | Maximum output/input ratio. A last compressed meta-block can carry 16777216 bytes in 13 bytes of input, and 1290556 is the smallest integer ceiling that still accepts that stream. |
| `limits.max_window_bytes` | uint64 | 16777216 | decoder | Maximum window. The default accepts every window RFC 7932 defines. 0 means unlimited. |

Unknown keys are refused when the encoder or decoder is created.

## What the encoder writes

For the default 16-bit window an empty input is the single byte `0x06`: window, `ISLAST`, `ISLASTEMPTY`. A non-empty input opens with `0x0C` (window, then an empty metadata meta-block, which lands the rest of the stream on a byte boundary), then one or more uncompressed meta-blocks, then `0x03`.

`gcomp_encoder_flush()` ends the current uncompressed meta-block and does not write the terminator, so a decoder can produce every byte consumed so far without `finish()`.
