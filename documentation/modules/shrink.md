# Shrink

Shrinking for the Ghoti.io Compress library. The method name is `"shrink"`; it encodes and decodes a **raw** stream with no header, trailer or checksum: the container supplies those. It is ZIP method 1, the first method PKZIP had.

**References:** PKWARE APPNOTE section 5.1.

## The format

Lempel-Ziv-Welch with codes of 9 to 13 bits, read least significant bit first. Codes 0 to 255 are bytes; every code after the first makes an entry of the code before it and the first byte of its own string, at the lowest free code from 257. Code 256 is a control, followed by a code that says which:

| after 256 | meaning |
|---|---|
| 1 | the codes after it are one bit longer (the compressor, not the table, decides when) |
| 2 | free every entry that no other entry builds on; the freed codes are made again, lowest first |

There is no end marker: the stream ends where the input does, and the fewer than eight bits after the last code are padding.

## Options

Only the limits: `limits.max_output_bytes`, `limits.max_memory_bytes` and `limits.max_expansion_ratio` (default 7282, the format's own ceiling: 8192 bytes in nine bits).

## Behaviour

- **Decoder.** Resumable at any byte. A stream that stops in the middle of a code, or between a 256 and its control, is `GCOMP_ERR_CORRUPT` at `finish`.
- **Encoder.** A dictionary of strings with a hash lookup. Within 64 codes of the end of the table it sends 256, 2 at a moment when the code it has just sent is a byte or has a child, since the entry made next is built on that code and a leaf would be freed from under it. It does not support `gcomp_encoder_flush`: the codes run on across bytes, so there is no place to stop.
- **Readers.** 7-Zip 25.01 reads everything this encoder writes. Info-ZIP unzip 6.00 reads it except after many partial clears, where it differs from the APPNOTE and from 7-Zip in a corner (found at about two megabytes of text); The Unarchiver 1.10.8 stops on larger streams. Real shrink members written by other programs, one of them filling the table and clearing it, all unpack here to the CRC-32 their headers record.
- **Real files.** Real members written by other programs (a table that fills and is cleared among them) are fetched by `tools/legacy_zip/fetch.sh` from other projects' test data, pinned by commit and SHA-256 and not committed; each unpacks to the CRC-32 its header records.
