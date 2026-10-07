# bzip2

bzip2 for the Ghoti.io Compress library. The method name is `"bzip2"`. It is a decoder so far; the encoder is the next change.

The format is the one `bzip2` and libbz2 read and write. A stream is `BZh` and a level digit 1 to 9 (the block size in hundreds of thousands of bytes), then blocks, then an end marker carrying a CRC of everything. A block is the input after a first run-length pass, sorted with the Burrows-Wheeler transform, move-to-front coded with runs of zeros spelled in a two-symbol code, and Huffman coded with two to six tables switched every 50 symbols. Each block carries a CRC-32 of the bytes it decodes to, taken most-significant bit first (CRC-32/BZIP2, not gzip's). Blocks and the end marker are bit-contiguous and only the stream's last byte is padded.

`gcomp_detect()` recognises the four bytes `BZh1` to `BZh9`. `gcomp_peek()` reads the level, which is the history a decoder keeps (`level * 100000`), and reports that the stream carries a checksum.

## Registration

bzip2 is auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_bzip2_register()` from `<ghoti.io/compress/bzip2.h>`.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory. A block costs four bytes per byte of its level's size, 3.6 MB at level 9, and is checked before it is allocated. |
| `limits.max_expansion_ratio` | uint64 | 2500000 | Maximum output/input ratio. The format's own ceiling is about 2.2 million to one (`bzip2.h` has the arithmetic); a real encoder reaches about 1.3 million on zeros. |

Unknown keys are refused when the decoder is created.

## What the decoder does

**Concatenated streams are one stream.** After a stream's end marker the decoder reads another if one follows, at any level, as `bzip2 -d` does. Bytes after the last stream that do not begin another are `GCOMP_ERR_CORRUPT`: `bzip2 -d` warns and ignores them, and a library has no way to warn. An empty input is also an error, since a stream with no blocks still has a header.

**A block is read whole before any of it is written.** The Burrows-Wheeler transform is undone from the last column, which is every symbol of the block. So the decoder reads a block's bits as they arrive, holds its symbols in an array of 32-bit words, and writes nothing until the end symbol has been read; then it walks the sorted list and undoes the first run-length pass on the way out, a run of four equal bytes being followed by a count of how many more. A run that does not fit the caller's buffer is parked and finished on the next call.

**The bit half is a state machine.** Every field is read only once the bit buffer holds all of it, and each loop over fields (the symbol map, the selectors, the code lengths, the symbols) keeps its counters in the decoder, so a call that runs out of input stops where it is and the next call resumes. A symbol is decoded only when 20 bits, the longest a code can be, are in hand; every valid stream has at least the 80 bits of its end marker after its last symbol, so that never refuses one. The tests feed streams one byte at a time to reach each of these stopping places.

**Randomised blocks are refused.** A block can carry a bit that says its bytes were randomised before sorting, a defence against a worst case of the original sort which bzip2 0.9.5 removed. No encoder has set it since, and reading one needs a 512-entry table of the original's. It is `GCOMP_ERR_UNSUPPORTED`.

**Stricter than libbz2 in two places.** A block that ends right after four equal bytes, with no count, is corrupt here; libbz2 reads the count from the start of the block. And a table that names a code with no symbol is corrupt, where libbz2 reads whatever was in its table before.

## Tests

`tests/methods/bzip2/test_bzip2_decoder.cpp` checks the decoder against libbz2, which it loads at run time. It is in `ORACLE_TEST_NAMES`, and `Libbz2IsActuallyAvailable` fails rather than skips when the library is absent. Malformed blocks are built bit by bit, each with one thing wrong, so that each refusal is pinned by the detail it gives rather than by a status that truncation would also produce.
