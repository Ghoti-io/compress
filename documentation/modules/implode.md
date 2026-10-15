# Implode

Imploding for the Ghoti.io Compress library. The method name is `"implode"`; it encodes and decodes a **raw** stream with no header, trailer or checksum. It is ZIP method 6.

**References:** PKWARE APPNOTE section 5.3.

## The format

A sliding dictionary of 4 or 8 KiB, whose output is coded with Shannon-Fano trees stored at the start: literals (256 codes, optional), match lengths (64) and the top six bits of the distance (64). Each tree is stored as runs of equal code lengths; the codes are built from the lengths as the APPNOTE gives (5.3.8). Then, a bit at a time: 1 and a literal (a tree code, or eight bits without the tree), or 0 and the low 6 or 7 bits of the distance, the tree code for its top six bits, and the length (tree code plus the minimum match, 3 with a literal tree and 2 without; the last code, 63, is followed by eight bits more). Before the start of the output the dictionary is zeros. There is no end marker.

## Options

| Key | Type | Default | Description |
|---|---|---|---|
| `implode.dictionary_size` | uint64 | 8192 | 4096 or 8192: bit 1 of a ZIP member's general purpose flags. |
| `implode.literal_tree` | bool | true | Bit 2 of the flags. |
| `implode.uncompressed_size` | uint64 | 0 | Where the decoder stops, as the container records it. With 0 the decoder runs to the end of the input; the padding bits cannot start a symbol, so this works for the streams this encoder writes. |
| `limits.max_*` | | | Output, memory, expansion ratio (default 152). |

## Behaviour

- **Encoder.** Buffers all the input, finds matches greedily with hash chains, builds the three trees as optimal length-limited Huffman codes (every symbol has one, since the format stores a length for each) and writes them. It does not support `gcomp_encoder_flush`. Random data grows by about 12% (a flag bit and a code for each byte); a container would store it.
- **Readers.** Info-ZIP unzip 6.00 and 7-Zip 25.01 both read what the encoder writes, at both dictionaries and with and without the literal tree, on text, executables, zeros, noise and mixed data.
- **Real files.** Real members written by other programs are fetched by `tools/legacy_zip/fetch.sh` from other projects' test data, pinned by commit and SHA-256 and not committed; each unpacks to the CRC-32 its header records.
