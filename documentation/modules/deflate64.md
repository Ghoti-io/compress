# Deflate64

Deflate64 ("enhanced deflate") for the Ghoti.io Compress library. The method name is `"deflate64"`; it encodes and decodes a **raw** stream, as `"deflate"` does, with no header, trailer or checksum: the container supplies those. It is ZIP's method 9 and a coder in 7z.

**References:** PKWARE APPNOTE section 4.4.5 (the format), RFC 1951 (the block structure it keeps). 7-Zip is the reference implementation the streams were checked against.

## What differs from deflate

Everything about blocks (stored, fixed Huffman, dynamic Huffman), the code length code and the Huffman codes is RFC 1951's. Three things change:

| | deflate | deflate64 |
|---|---|---|
| window | 32 KiB | 64 KiB |
| length code 285 | length 258, no extra bits | base 3, sixteen extra bits: lengths up to 65538 |
| distance codes 30, 31 | invalid | base 32769 and 49153, fourteen extra bits: distances up to 65536 |

A deflate64 stream that happens to use none of these is a deflate stream, and the reverse holds except that deflate's length code 285 means 258.

## Registration

Auto-registered with the default registry. For a custom registry:

```c
gcomp_method_deflate64_register(custom);
```

## Options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `deflate64.level` | int64 | 6 | 0 to 9. 0 stores; 1 to 3 take the first match found; 4 and up also try the next position and keep the longer. Deeper levels search longer hash chains. |
| `limits.max_output_bytes` | uint64 | 0 (unlimited) | Maximum decompressed output. |
| `limits.max_memory_bytes` | uint64 | 0 (unlimited) | Maximum memory (the decoder holds a 64 KiB window and its tables). |
| `limits.max_expansion_ratio` | uint64 | 29128 | Output over input. The default is the format's own ceiling: 65538 bytes in 18 bits. |

## Behaviour

- **Decoder.** Resumable at any byte: input and output may be given a byte at a time. Bytes after the final block are left unconsumed, so a container's trailer can follow. A stream that stops before its final block is `GCOMP_ERR_CORRUPT` at `finish`.
- **Encoder.** Collects 64 KiB, finds matches with hash chains over a 64 KiB window (up to 65538 long, so a run of zeros is a few matches), and codes the block as whichever of stored, fixed and dynamic is smallest. A sync flush ends the block and sends an empty stored block, so everything so far is on a byte boundary; a full flush also forgets the window.
- **Size.** On text and executables the output is within a few percent of 7-Zip's at its normal level. It is not tuned to match it.
- **Speed.** The decoder works a symbol and a match byte at a time. It is written for being right first.

## Testing

`tests/methods/deflate64/test_deflate64.cpp` builds streams bit by bit for each of the three changes, decodes a stream 7-Zip wrote, round-trips every level over text, noise, zeros, far repeats and mixed data in every buffering, cuts and damages streams, and checks the limits and the bound.
