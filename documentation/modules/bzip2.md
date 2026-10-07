# bzip2

bzip2 for the Ghoti.io Compress library. The method name is `"bzip2"`, with an encoder and a decoder.

The format is the one `bzip2` and libbz2 read and write. A stream is `BZh` and a level digit 1 to 9 (the block size in hundreds of thousands of bytes), then blocks, then an end marker carrying a CRC of everything. A block is the input after a first run-length pass, sorted with the Burrows-Wheeler transform, move-to-front coded with runs of zeros spelled in a two-symbol code, and Huffman coded with two to six tables switched every 50 symbols. Each block carries a CRC-32 of the bytes it decodes to, taken most-significant bit first (CRC-32/BZIP2, not gzip's). Blocks and the end marker are bit-contiguous and only the stream's last byte is padded.

`gcomp_detect()` recognises the four bytes `BZh1` to `BZh9`. `gcomp_peek()` reads the level, which is the history a decoder keeps (`level * 100000`), and reports that the stream carries a checksum.

## Registration

bzip2 is auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_bzip2_register()` from `<ghoti.io/compress/bzip2.h>`.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `bzip2.level` | int64 | 9 | Encoder block size in hundreds of thousands of bytes, 1..9. The encoder holds about 21 times a block while it sorts one: 19 MB at level 9, 2 MB at level 1. A decoder reads the level from the stream. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory. A block costs four bytes per byte of its level's size, 3.6 MB at level 9, and is checked before it is allocated. |
| `limits.max_expansion_ratio` | uint64 | 2500000 | Maximum output/input ratio. The format's own ceiling is about 2.2 million to one (`bzip2.h` has the arithmetic); a real encoder reaches about 1.3 million on zeros. |

Unknown keys are refused when the encoder or decoder is created.

## What the decoder does

**Concatenated streams are one stream.** After a stream's end marker the decoder reads another if one follows, at any level, as `bzip2 -d` does. Bytes after the last stream that do not begin another are `GCOMP_ERR_CORRUPT`: `bzip2 -d` warns and ignores them, and a library has no way to warn. An empty input is also an error, since a stream with no blocks still has a header.

**A block is read whole before any of it is written.** The Burrows-Wheeler transform is undone from the last column, which is every symbol of the block. So the decoder reads a block's bits as they arrive, holds its symbols in an array of 32-bit words, and writes nothing until the end symbol has been read; then it walks the sorted list and undoes the first run-length pass on the way out, a run of four equal bytes being followed by a count of how many more. A run that does not fit the caller's buffer is parked and finished on the next call.

**The bit half is a state machine.** Every field is read only once the bit buffer holds all of it, and each loop over fields (the symbol map, the selectors, the code lengths, the symbols) keeps its counters in the decoder, so a call that runs out of input stops where it is and the next call resumes. A symbol is decoded only when 20 bits, the longest a code can be, are in hand; every valid stream has at least the 80 bits of its end marker after its last symbol, so that never refuses one. The tests feed streams one byte at a time to reach each of these stopping places.

**Randomised blocks are refused.** A block can carry a bit that says its bytes were randomised before sorting, a defence against a worst case of the original sort which bzip2 0.9.5 removed. No encoder has set it since, and reading one needs a 512-entry table of the original's. It is `GCOMP_ERR_UNSUPPORTED`.

**Stricter than libbz2 in two places.** A block that ends right after four equal bytes, with no count, is corrupt here; libbz2 reads the count from the start of the block. And a table that names a code with no symbol is corrupt, where libbz2 reads whatever was in its table before.

## What the encoder does

**The pipeline, per block.** Input is run-length coded as it arrives (four or more equal bytes become four and a count, with a run cut at 255) into a block of `level * 100000 - 19` bytes, with a CRC over the bytes as they came in. A full block is sorted by the Burrows-Wheeler transform, move-to-front coded with runs of zeros spelled in a bijective two-symbol code, and Huffman coded with two to six tables chosen per 50 symbols. The tables are found as libbz2 finds them: start from a partition of the alphabet by frequency, give each group to the table that codes it cheapest, rebuild the tables from what they were given, and repeat four times. Code lengths are at most 17. The output is within a few bytes of libbz2's at every level on every corpus tested, and sometimes smaller.

**The sort has a guarantee.** The transform sorts rotations by sorting the suffixes of the block written twice (a rotation is a prefix of a suffix of that), with SA-IS, which is linear in the 1.8 million symbols that makes. libbz2 uses a bucketed multikey quicksort and falls back to another algorithm on repetitive input, because the first has no worst-case bound. This has one. It costs two 32-bit arrays of that length, 14 MB at level 9, and is checked against sorting the rotations directly on every string of length up to 13 over two symbols and 8 over three, and on random and periodic strings of every length to 300.

**Flush ends the stream.** A bzip2 block ends in the middle of a byte and the next starts there, so there is no point inside a stream where its bytes stand alone. `gcomp_encoder_flush()` therefore writes the block in hand, the end marker and the stream CRC, and pads to a byte; the next input begins a new stream, and concatenated streams are one stream to `bzip2 -d` and to this decoder. A flush costs 14 bytes of header and footer, a full flush is the same as a sync flush because no block refers to another, and a finish after a flush with nothing new adds nothing. An input of no bytes at all is the 14-byte stream libbz2 writes for it, byte for byte.

**The stream depends on the bytes**, not on how `update()` calls cut them: a block ends when it is full, and nothing else decides where.

**Bound.** The input, 2% of it, and 1024 bytes. libbz2 documents 1% and 600; the margin is doubled so that choosing tables a little differently cannot make a block larger than libbz2's and the bound a lie. Incompressible data is the worst case for the Huffman coder, and runs of exactly four are the worst for the first pass, which adds a count byte to each; both are tested at levels 1 and 9.

## Tests

`tests/methods/bzip2/test_bzip2_decoder.cpp` and `test_bzip2_encoder.cpp` check both directions against libbz2, which they load at run time. Both are in `ORACLE_TEST_NAMES`, and `Libbz2IsActuallyAvailable` fails rather than skips when the library is absent. `test_bzip2_bwt.cpp` checks the sort against the definition. Malformed blocks are built bit by bit, each with one thing wrong, so that each refusal is pinned by the detail it gives rather than by a status that truncation would also produce.
