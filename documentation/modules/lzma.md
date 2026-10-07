# LZMA and LZMA2

LZMA for the Ghoti.io Compress library, as two methods: `"lzma"` and `"lzma2"`, each with an encoder and a decoder.

`"lzma"` is the `.lzma` format, called LZMA-alone: a 13-byte header (a properties byte that packs `lc`, `lp` and `pb`, a 4-byte dictionary size, an 8-byte uncompressed size) and then one range-coded stream. A size of all ones means the size is not stated and the stream ends with an end marker. With `lzma.raw` the header is left out and the caller says what it would have said, which is how zip method 14 and 7z carry LZMA.

`"lzma2"` is the chunked framing xz and 7z use. A chunk is stored or LZMA-coded; an LZMA chunk restarts the range coder and may reset the state, the properties, or the dictionary, and a single zero byte ends the stream. It has no header of its own: the dictionary size is a property the container carries, so this decoder takes its window from `limits.max_window_bytes`.

Neither format has a magic number that can be told from data, so `gcomp_detect()` does not claim them. `gcomp_peek()` on `"lzma"` reads the header: the window size, and the uncompressed size when the header states one. The properties byte has to be one the format can spell (it is below 225), and that is all that can be checked.

## Registration

Both are auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_lzma_register()` and `gcomp_method_lzma2_register()` from `<ghoti.io/compress/lzma.h>`.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `lzma.raw` | bool | false | No header. The next five say what it would have. |
| `lzma.preset` | int64 | 6 | Encoder effort and dictionary, 0..9, as xz's presets: 256 KiB at 0, 8 MiB at 6, 64 MiB at 9. |
| `lzma.lc` | int64 | 3 | Literal context bits, 0..8. A decoder reads it from the header unless the stream is raw. |
| `lzma.lp` | int64 | 0 | Literal position bits, 0..4. As `lc`. |
| `lzma.pb` | int64 | 2 | Position bits, 0..4. As `lc`. |
| `lzma.dict_size` | uint64 | 8 MiB | Dictionary in bytes, 4096..4294967295. A raw decoder needs it; an encoder uses the preset's unless it is set. |
| `lzma.uncompressed_size` | uint64 | largest value | The bytes the stream holds, so that it needs no end marker. A decoder stops there; an encoder writes it in the header and refuses any other length. The default means unstated. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory, window and literal coders together. |
| `limits.max_expansion_ratio` | uint64 | 8192 | Maximum output/input ratio. The format cannot exceed about 7020: the cheapest symbol is a 273-byte match at the last distance, fourteen decisions of at least 0.022 bits each. |
| `limits.max_window_bytes` | uint64 | 1.5 GiB | The largest window a stream may claim, which is xz's largest. |

`lzma2` takes `lzma2.preset`, `lzma2.lc` (0..4), `lzma2.lp`, `lzma2.pb` and `lzma2.dict_size`, which mean what they do above and are read by the encoder, and the `limits.*` keys. Its `lc + lp` may not exceed 4. Unknown keys are refused when the encoder or decoder is created.

## What the decoder does

**The window grows with the output.** A header can claim a 4 GiB dictionary; the decoder starts with 64 KiB and doubles up to the claim as output arrives, so a stream that has produced a kilobyte holds a kilobyte. The claim is still a promise the stream keeps: a match that reaches further back than the header's dictionary size is `GCOMP_ERR_CORRUPT`, even into bytes still held. `limits.max_window_bytes` refuses a claim at the header, before any output, and `limits.max_memory_bytes` is checked as the window grows.

**A symbol is parsed once.** A symbol's input can end half way through a decision, and the range coder moves its probabilities as it reads. One function parses a symbol and takes a flag that makes it write nothing, so the same code that decodes a symbol also answers whether the input holds one. It runs that way only at the edge of a call, when under 20 bytes are in hand (the most one symbol can need); a symbol that is not all there waits in a 20-byte carry. The tests feed every stream one byte at a time, which reaches that arm for each field of each symbol.

**A declared size ends the stream.** When the header names a size, the decoder stops there. A range coder that is not at zero at that point means an end marker must follow, and one is accepted; anything else is `GCOMP_ERR_CORRUPT`. A stream that ends before its declared size, or a match that runs past it, is corrupt.

**LZMA2 follows liblzma's rules.** The first chunk must reset the dictionary; an LZMA chunk before any properties is refused; properties with `lc + lp` over 4 are refused; control bytes 3 to 0x7F are refused; an LZMA chunk must end exactly where its header says, with the range coder at zero; and an end marker inside a chunk is refused. A dictionary reset in the middle of a stream is read, which liblzma's encoder does not write and the format allows.

Distance and length are checked against the bytes decoded since the last dictionary reset, so a match cannot reach back across one.

## Tests

`tests/methods/lzma/test_lzma_decoder.cpp` and `test_lzma_encoder.cpp` check both directions against liblzma, which they load at run time. Both are in `ORACLE_TEST_NAMES`, and `LiblzmaIsActuallyAvailable` fails rather than skips when the library is absent. The encoder's streams are read by this library's decoder and by liblzma; the shapes only this library can write (an lc of 8, an lp of 4) are read by the decoder alone.

## What the encoder does

**One symbol coder, two framings.** `lzma` codes one range-coded stream: the 13-byte header, the symbols, an end marker unless `lzma.uncompressed_size` was given, and the coder's five flush bytes. `lzma2` codes the same symbols a chunk at a time, restarting the range coder for each. A chunk is at most 2 MiB of input and 65536 bytes of output; when coding a chunk does not make it smaller, it is written stored instead, the model goes back to where it was, and the next coded chunk resets the state (control byte `0xA0`), because the decoder never saw the symbols that were tried. A stream whose first chunk is stored then needs its properties on the first coded one (`0xC0`).

**The window and the batch.** Input waits in a window of one dictionary plus room for the next batch, and is coded when a batch of 128 KiB and one maximum match is there. The window never holds more than that batch of uncoded input, so it slides after a megabyte or so of progress and moves a fraction of the bytes coded. The match finder keeps running 32-bit positions, so a slide moves bytes and changes one base address. The cost of this: bytes sit in the encoder until a batch is complete, a `finish()`, or, for `lzma2`, a `flush`. The benefit: the stream depends only on the bytes, and not on how `update()` calls cut them.

**The parser is fast mode, not optimal.** It takes the longest match from hash chains on two, three and four bytes, prefers a repeat of one of the last four distances when it is nearly as long, and looks one byte ahead before committing. It does not price symbols. Against liblzma, the output is within 1% at preset 1 and 3% to 5% at preset 0 on text and mixed data; at preset 6, where liblzma parses optimally, text is 30% larger. That gap is the next piece of work (`notes/compress/LZMA-XZ-BZIP2.md`).

**Memory.** The encoder holds the window (2 to 3 times the dictionary), a position table of four bytes per dictionary byte rounded up to a power of two, and 16 MiB of hash heads at most. At the default preset that is about 52 MiB; at preset 9, 400 MiB.

**Flush.** `lzma2` flushes: a sync flush ends the chunk, so every byte fed so far decodes, and a full flush also resets the dictionary, so what follows decodes without what came before. `lzma` cannot flush: its stream is one range coder from first byte to last, so there is no point at which its bytes stand alone, and `gcomp_encoder_flush()` returns `GCOMP_ERR_UNSUPPORTED`.

**Bound.** `lzma2` has one: the input, six bytes per 16 KiB, and the end byte. `lzma` has none. It cannot store, so its worst case is the cost of the least probable symbols, about ten bytes out for each byte in, which no caller means to allocate; `gcomp_encode_bound()` and `gcomp_encode_alloc()` return `GCOMP_ERR_UNSUPPORTED` for it, and `gcomp_encode_buffer()` takes the caller's size.
