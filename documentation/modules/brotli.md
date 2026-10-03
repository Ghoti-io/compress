# Brotli

Brotli (RFC 7932) for the Ghoti.io Compress library. The method name is `"brotli"`.

The decoder reads the format: prefix codes, block switches, context maps, distances, and the static dictionary with its 121 transforms. It is checked against libbrotli 1.1.0 at every one of its twelve quality levels, on input the reference encoder has reason to split into several block types, because block switching was the one item in that list the decoder claimed and did not do: RFC 7932 has each block-type category with two or more types carry the count of its first block, that count was never read, and so every field after it came from the wrong bit position. Qualities 4 through 9 were refused outright. The encoder's default (`brotli.level` 1) writes an LZ77 meta-block of up to 256KiB: one block type, no distance postfix, no direct distances, and one Huffman code each for literals, commands, and distances. A chunk that does not get smaller than storing it is stored instead. Level 0 always stores, in uncompressed meta-blocks of at most 65536 bytes. Either way an empty metadata block keeps the stream on a byte boundary, and the stream ends with an empty last meta-block. The encoder does not use a context model or the static dictionary yet.

The stream has no magic number, so `gcomp_detect()` does not claim it. `gcomp_peek()` reads the window size from the first byte, at every window `brotli.lgwin` allows. Two of the 256 possible first bytes are refused and no others: RFC 7932 section 9.1 reserves one `WBITS` encoding, and the eighth bit belongs to the next field, so the reserved value appears twice.

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
| `brotli.level` | int64 | 1 | encoder | 0 stores every chunk. 1 writes LZ77 and Huffman when that is smaller. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | decoder | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | decoder | Maximum working memory. The window is checked against this before it is allocated. |
| `limits.max_expansion_ratio` | uint64 | 1290556 | decoder | Maximum output/input ratio. A last compressed meta-block can carry 16777216 bytes in 13 bytes of input, and 1290556 is the smallest integer ceiling that still accepts that stream. |
| `limits.max_window_bytes` | uint64 | 16777216 | decoder | Maximum window. The default accepts every window RFC 7932 defines. 0 means unlimited. |

Unknown keys are refused when the encoder or decoder is created.

## What the encoder writes

For the default 16-bit window an empty input is the single byte `0x06`: window, `ISLAST`, `ISLASTEMPTY`. A non-empty input opens with `0x0C` (window, then an empty metadata meta-block, which lands the rest of the stream on a byte boundary). Level 0 then writes uncompressed meta-blocks and ends with `0x03`. Level 1 writes a compressed meta-block when it is smaller, and an empty metadata block after it so the next block and the `0x03` terminator stay on a byte boundary.

`gcomp_encoder_flush()` ends the current meta-block and does not write the terminator, so a decoder can produce every byte consumed so far without `finish()`.

`gcomp_encoder_reset()` returns the encoder to the state a freshly created one is in, distance ring buffer included, and discards anything held but not yet emitted. The next stream is byte-for-byte what a new encoder writes for the same input.

`GCOMP_FLUSH_FULL` additionally writes the next four distances as absolute codes rather than as references into the distance ring buffer. A match never reaches outside the chunk it is in, so ending the meta-block is enough to keep matches from crossing the flush; the ring buffer is the one piece of decoder state that does cross it, and four absolute distances refill every slot the short codes can read. It costs a few bits, which is what the mode is documented to cost.
