# LZMA and LZMA2

LZMA for the Ghoti.io Compress library, as two methods: `"lzma"` and `"lzma2"`. Both are decoders so far; the encoders are the next change.

`"lzma"` is the `.lzma` format, called LZMA-alone: a 13-byte header (a properties byte that packs `lc`, `lp` and `pb`, a 4-byte dictionary size, an 8-byte uncompressed size) and then one range-coded stream. A size of all ones means the size is not stated and the stream ends with an end marker. With `lzma.raw` the header is left out and the caller says what it would have said, which is how zip method 14 and 7z carry LZMA.

`"lzma2"` is the chunked framing xz and 7z use. A chunk is stored or LZMA-coded; an LZMA chunk restarts the range coder and may reset the state, the properties, or the dictionary, and a single zero byte ends the stream. It has no header of its own: the dictionary size is a property the container carries, so this decoder takes its window from `limits.max_window_bytes`.

Neither format has a magic number that can be told from data, so `gcomp_detect()` does not claim them. `gcomp_peek()` on `"lzma"` reads the header: the window size, and the uncompressed size when the header states one. The properties byte has to be one the format can spell (it is below 225), and that is all that can be checked.

## Registration

Both are auto-registered with the default registry when the library loads. For a registry of your own, call `gcomp_method_lzma_register()` and `gcomp_method_lzma2_register()` from `<ghoti.io/compress/lzma.h>`.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `lzma.raw` | bool | false | No header. The next five say what it would have. |
| `lzma.lc` | int64 | 3 | Literal context bits, 0..8. Raw only. |
| `lzma.lp` | int64 | 0 | Literal position bits, 0..4. Raw only. |
| `lzma.pb` | int64 | 2 | Position bits, 0..4. Raw only. |
| `lzma.dict_size` | uint64 | 8 MiB | Dictionary in bytes, 4096..4294967295. Raw only. |
| `lzma.uncompressed_size` | uint64 | largest value | Raw decoder: the bytes the stream holds, so that it needs no end marker. The default means the stream ends with a marker. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum working memory, window and literal coders together. |
| `limits.max_expansion_ratio` | uint64 | 8192 | Maximum output/input ratio. The format cannot exceed about 7020: the cheapest symbol is a 273-byte match at the last distance, fourteen decisions of at least 0.022 bits each. |
| `limits.max_window_bytes` | uint64 | 1.5 GiB | The largest window a stream may claim, which is xz's largest. |

`lzma2` takes only the `limits.*` keys. Unknown keys are refused when the decoder is created.

## What the decoder does

**The window grows with the output.** A header can claim a 4 GiB dictionary; the decoder starts with 64 KiB and doubles up to the claim as output arrives, so a stream that has produced a kilobyte holds a kilobyte. The claim is still a promise the stream keeps: a match that reaches further back than the header's dictionary size is `GCOMP_ERR_CORRUPT`, even into bytes still held. `limits.max_window_bytes` refuses a claim at the header, before any output, and `limits.max_memory_bytes` is checked as the window grows.

**A symbol is parsed once.** A symbol's input can end half way through a decision, and the range coder moves its probabilities as it reads. One function parses a symbol and takes a flag that makes it write nothing, so the same code that decodes a symbol also answers whether the input holds one. It runs that way only at the edge of a call, when under 20 bytes are in hand (the most one symbol can need); a symbol that is not all there waits in a 20-byte carry. The tests feed every stream one byte at a time, which reaches that arm for each field of each symbol.

**A declared size ends the stream.** When the header names a size, the decoder stops there. A range coder that is not at zero at that point means an end marker must follow, and one is accepted; anything else is `GCOMP_ERR_CORRUPT`. A stream that ends before its declared size, or a match that runs past it, is corrupt.

**LZMA2 follows liblzma's rules.** The first chunk must reset the dictionary; an LZMA chunk before any properties is refused; properties with `lc + lp` over 4 are refused; control bytes 3 to 0x7F are refused; an LZMA chunk must end exactly where its header says, with the range coder at zero; and an end marker inside a chunk is refused. A dictionary reset in the middle of a stream is read, which liblzma's encoder does not write and the format allows.

Distance and length are checked against the bytes decoded since the last dictionary reset, so a match cannot reach back across one.

## Tests

`tests/methods/lzma/test_lzma_decoder.cpp` checks both decoders against liblzma, which it loads at run time. It is in `ORACLE_TEST_NAMES`, and `LiblzmaIsActuallyAvailable` fails rather than skips when the library is absent.
