# xz

xz for the Ghoti.io Compress library. The method name is `"xz"`, with an encoder and a decoder.

The format is the one `xz` and liblzma read and write. A file is one or more streams. A stream is a 12-byte header (the magic `FD 37 7A 58 5A 00`, two bytes of flags that name the integrity check, and their CRC-32), blocks, an index of the blocks, and a 12-byte footer. A block is a header naming a chain of filters that ends in LZMA2, the chain's output, zero padding to a multiple of four bytes, and a check of the uncompressed bytes. The index lists each block's size, so that a reader can find one without reading the ones before it.

`gcomp_detect()` recognises the six magic bytes. `gcomp_peek()` reads the stream header and reports whether the stream carries a check.

## Registration

xz is auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_xz_register()` from `<ghoti.io/compress/xz.h>`. It needs `"lzma2"`, `"delta"` and `"bcj"` registered in the same registry, because each block is built from them; creating an encoder or decoder without them fails with a message that says so.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `xz.preset` | int64 | 6 | Encoder effort and dictionary, 0..9, as xz's presets. |
| `xz.dict_size` | uint64 | 0 | Dictionary in bytes; 0 takes the preset's. The stream names the next size up that xz can spell (2^n or 3 * 2^(n-1)). |
| `xz.lc`, `xz.lp`, `xz.pb` | int64 | 3, 0, 2 | Literal context bits, literal position bits and position bits, each 0..4; `lc + lp` at most 4, which is LZMA2's limit. |
| `xz.check` | string | `crc64` | The integrity check of each block: `none`, `crc32`, `crc64` or `sha256`. |
| `xz.block_size` | uint64 | 0 | Uncompressed bytes per block; 0 puts the whole stream in one. Smaller blocks cost ratio and let a reader decode one without the rest. |
| `xz.filters` | string | empty | Filters applied before LZMA2, comma separated, in the order they are applied when encoding: `delta`, `delta:N`, or `x86`, `powerpc`, `ia64`, `arm`, `armthumb`, `sparc`, `arm64`, each with an optional `:OFFSET`. At most three. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory, for the decoder's windows and the list of blocks, and the encoder's list. |
| `limits.max_expansion_ratio` | uint64 | 8192 | Maximum output/input ratio, which is LZMA2's own ceiling. |
| `limits.max_window_bytes` | uint64 | 1.5 GiB | The largest dictionary a block may name. |

Unknown keys, an unknown check or filter name, and a number out of range are refused when the encoder or decoder is created.

## What the decoder does

**Everything the container says twice is compared.** The stream header's CRC; the block header's CRC; the sizes a block header may declare against what was read; the check against the decoded bytes; the index against the blocks that were actually read; the index's CRC; the footer's CRC; its backward size against the index that was read; its stream flags against the header's. Zero padding must be zero. A reader that checks one copy and trusts the other can be steered by a file with two different answers.

**Streams may follow one another**, with zero padding in multiples of four bytes between them, and are read as one, as `xz -d` does. A stream's check may differ from the next one's. Anything else after a stream, including padding that is not a multiple of four, is an error.

**The window is the one the header names.** The block's LZMA2 stage is given the block's dictionary as its window, so a match that reaches farther back than the header said is corrupt, as in liblzma. A header that names a dictionary over `limits.max_window_bytes` is refused with `GCOMP_ERR_LIMIT`, before anything is allocated for it.

**Unsupported things are refused, not skipped.** The format reserves twelve more integrity checks than the four implemented, and a stream that names one is `GCOMP_ERR_UNSUPPORTED` rather than decoded without its check: a caller who asked for a stream to be verified cannot be told it was not. The same goes for an unknown filter, a flag bit the format has not defined and the RISC-V filter (see filters.md).

**Truncation is an error at `finish()`**, whatever the point: inside a header, a block, the index, or between the last block and the index.

## What the encoder does

**Blocks.** Input goes into the open block's chain until the block is full (`xz.block_size`) or the stream ends. A block is closed by finishing the chain, which leaves its compressed size known; then the padding and the check go out, and the block's two sizes are kept for the index. The block header does not carry the sizes, which the format allows: the encoder cannot go back to write them, and the index is where they are. An empty input writes a stream with no blocks, 32 bytes, as xz does.

**Blocks are independent.** Each starts a fresh LZMA2 stream with a fresh dictionary and fresh filter state, so a block's output depends only on its bytes. The stream depends on the bytes and the options, and not on how `update()` cut the input or how large the output buffers were.

**The chain.** A block's filters and LZMA2 are the methods of those names (`delta`, `bcj`, `lzma2`), run one into the next through a small buffer between each pair. With no filter the block is LZMA2 alone and nothing is copied extra.

**Flush ends the block.** A block is the unit a reader can decode on its own, and a branch converter may be holding bytes back that a sync flush could not give up. So `gcomp_encoder_flush()`, in either mode, closes the open block and writes it out; what follows starts a new one. The stream is not ended.

**Bound.** The input, 6 bytes for each 16 KiB, and about 150 bytes for each block, plus 44 for the two ends and the index. A real stream is a few bytes over the input at worst.

## Tests

`tests/methods/xz/test_xz.cpp` checks both directions against liblzma, which it loads at run time, and is in `ORACLE_TEST_NAMES`; `Xz.LiblzmaIsActuallyAvailable` fails rather than skips when liblzma is missing. Streams are read from liblzma at every check and several presets, in several blocks (the threaded encoder cuts them), through filter chains, concatenated and padded. Streams written here are read by liblzma, whose decoder verifies the checks and the index.

The refusals are checked the same way: a valid stream with three blocks, a filter and a SHA-256 check is changed one byte at a time, three ways, and the two decoders must agree on every change about whether the result is still a stream; every proper prefix must be refused by both. A differential on valid input alone cannot see the error paths, which are most of a container's code.

`fuzz/fuzz_xz_decoder.c`, `fuzz_xz_encoder.c` and `fuzz_xz_roundtrip.c` fuzz the container; the encoder and round trip take their settings (preset, check, filters, block size) from the first two bytes of the input.
