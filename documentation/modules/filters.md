# Filters: delta and bcj

Two methods that do not compress. They rewrite data so that whatever compresses it next does better, and they can be undone exactly. The names are `"delta"` and `"bcj"`, each with an encoder (the forward transform) and a decoder (the inverse).

Both are the same size in and out and carry no header, so a decoder has to be told what the encoder did: the filter and its options belong to the format of whatever holds the stream. That is how xz and 7z carry them, and the ids, the options and the rewriting here are xz's, so a stream written with these filters is read by xz and the other way round.

## What they are for

**delta** replaces each byte with its difference from the byte `delta.distance` before it. A signal that changes slowly becomes mostly small numbers; interleaved samples become runs when the distance is the sample size in bytes; an image becomes its vertical edges when the distance is the row width.

**bcj** (branch, call, jump) rewrites the relative target of a branch or call instruction as an absolute address. A function called from a hundred places is a hundred different relative offsets and one absolute address, so after the rewrite the hundred calls are the same four bytes and a match finder sees them. It is for executable code, and it does no harm to anything else: the converters only touch bytes that have the shape of a branch.

## Registration

Both are auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_delta_register()` and `gcomp_method_bcj_register()` from `<ghoti.io/compress/filter.h>`.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `delta.distance` | int64 | 1 | How far back the byte to subtract is, 1..256. |
| `bcj.arch` | string | `x86` | The instruction set: `x86`, `powerpc`, `ia64`, `arm`, `armthumb`, `sparc` or `arm64`. The decoder must be given the same. |
| `bcj.start_offset` | uint64 | 0 | The address of the first byte as the code will see it, up to 2^32 - 1. Zero unless the data is a piece cut from the middle of a program. |

Unknown keys, an unknown architecture and a distance outside 1..256 are refused when the encoder or decoder is created.

## Behaviour

**A converter holds bytes back.** An x86 call is five bytes and the last four are its displacement, so a buffer that ends in the middle of one cannot be settled yet. The converter returns the bytes it could settle and keeps the rest until more arrives, up to 15 of them for IA-64's 16-byte bundles. At `finish()` the bytes still held are let through unchanged, which is what xz does, and what makes the output the same size as the input. Where the bytes are cut, between `update()` calls or in the size of the output buffer, does not change the output; the tests cut at every size from one byte up.

**Only delta flushes.** Delta settles every byte as it arrives, so `gcomp_encoder_flush()` has nothing to hold back and emits everything consumed. A branch converter cannot flush without giving up the bytes it is holding, which would change what the next bytes become, so `bcj` reports `GCOMP_ERR_UNSUPPORTED`, as xz does.

**Not every architecture is here.** RISC-V (xz filter id 0x0B, added in xz 5.6) is not implemented. Its converter pairs an `auipc` with the instruction after it and has several special cases for which the encoding must agree with xz to the bit; it will be added when there is something to check it against beyond liblzma itself.

**The bound** is the length: nothing grows. `gcomp_encode_bound()` answers with the input size.

**There is no detection and no peek.** The output has no header and the input is anything. `gcomp_detect()` never answers `"delta"` or `"bcj"`.

## Tests

`tests/methods/filters/test_filters.cpp` checks both methods against liblzma, which it loads at run time. liblzma cannot run a filter on its own, so the data goes through the chain [filter, LZMA2] and comes back through [LZMA2], which leaves what the filter made of it; the inverse is the same with the roles swapped. It is in `ORACLE_TEST_NAMES`, and `Filters.LiblzmaIsActuallyAvailable` fails rather than skips when liblzma is missing, so `make check-oracle` cannot pass without it.

The data decides what the test can see. Random bytes almost never look like a branch, so each architecture has a generator that makes them often, with the fields that decide whether a converter fires set both ways (the displacement's top byte for x86; the opcode and the range of the immediate for the others). A test asserts that the generated data does change under each converter, so a converter that never fired could not agree with liblzma by omission. x86 is also run on this program's own machine code.

`fuzz/fuzz_filters_roundtrip.c` sends arbitrary bytes through each filter in both orders, cut at sizes taken from the data, and aborts unless the bytes come back unchanged.
