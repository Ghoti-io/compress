# Brotli

Brotli (RFC 7932) for the Ghoti.io Compress library. The method name is `"brotli"`.

The decoder reads the format: prefix codes, block switches, context maps, distances, and the static dictionary with its 121 transforms. It is checked against libbrotli 1.1.0 at every one of its twelve quality levels, on input the reference encoder has reason to split into several block types, because block switching was the one item in that list the decoder claimed and did not do: RFC 7932 has each block-type category with two or more types carry the count of its first block, that count was never read, and so every field after it came from the wrong bit position. Qualities 4 through 9 were refused outright. The decoder is a resumable state machine that can stop for input or room at any bit, and on top of it sits a fast path that decodes whole commands from locals while the input holds a few words past the bit position and the output has room: prefix codes are read through a 10-bit table, bits are refilled eight bytes at a time, and a copy moves in chunks. The fast path never reports an error and never stops for more input; anything unusual (a corrupt code, a dictionary reference, a block switch, a limit, the end of the input or the room) is handed back to the machine at a step it already has. It decodes at 210 to 340 MB/s where the machine alone did 80 (libbrotli: 360 to 460 on the same machine), and `LibbrotliStreamsAreReadWhateverTheChunking` reads libbrotli's streams at every combination of small and large input and output buffers to hold the hand-off to account. The encoder (`brotli.level` 1 to 4) writes an LZ77 meta-block of up to 256KiB: no distance postfix, no direct distances, one block type and one Huffman code each for commands and distances, and for literals whatever the model below chooses. A match may reach back across meta-blocks as far as the window allows, to a limit of 4MiB: the encoder keeps that much history and a hash-chain match finder over it, and slides both when its buffer fills. A full flush drops the history, a sync flush does not. Level 1 takes the first match worth having among 16 candidates, level 2 defers a match when the next position starts a longer one, and levels 3 and 4 search 48 and 128 candidates. A chunk that does not get smaller than storing it is stored instead. Level 0 always stores, in uncompressed meta-blocks of at most 65536 bytes. Either way an empty metadata block keeps the stream on a byte boundary, and the stream ends with an empty last meta-block. **Literal model (levels 2 and up).** A literal can be coded under the context of the two bytes before it. The encoder counts the literals under each of the four context modes, ranks the modes by what each context would cost with a code of its own, merges the best two modes' 64 contexts into at most 16 trees by greedy union of histograms (a saving only, with a tree's header counted), and keeps the cheapest of that and one code for everything if it saves more than the map costs; the context map is written run-length coded with the move-to-front option, whichever is smaller. Level 4 also splits the literals into block types: segments of a few hundred are clustered into up to 8 histograms by rounds of Viterbi with a price for each switch, the types are modelled the same way each, and the cheapest of the three plans is written. The model is off in the first meta-block after a full flush until four absolute distances have been written, because the two bytes before the first literal are then the last two of the data before the flush, which a decoder that lost it does not have. The parse prices a match in bits (a byte is worth about what a literal costs, a distance as many bits as its magnitude, the last four distances a few) and so does not take a far four-byte match, and passes over a match for one that starts a byte later and is worth more than a literal. The encoder does not use the static dictionary yet, or block types for commands and distances.

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
| `brotli.level` | int64 | 1 | encoder | 0 stores every chunk. 1 to 4 write LZ77 and Huffman when that is smaller, searching harder and slower at each step. |
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
