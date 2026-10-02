# Fuzzing

This document describes how to use fuzz testing to find bugs, crashes, and edge cases in the compress library.

## Overview

Fuzz testing (fuzzing) is a technique that feeds random or semi-random data to a program to find crashes, hangs, memory errors, and other bugs. We use [AFL++](https://github.com/AFLplusplus/AFLplusplus), a powerful coverage-guided fuzzer.

### What Fuzzing Can Find

- **Crashes**: Segmentation faults, null pointer dereferences
- **Memory errors**: Buffer overflows, use-after-free (when combined with ASan)
- **Hangs**: Infinite loops, excessive computation
- **Logic bugs**: Roundtrip mismatches (encode then decode doesn't match original)
- **Edge cases**: Handling of malformed, truncated, or adversarial input

## Prerequisites

### Install AFL++

On Ubuntu/Debian/WSL2:

```bash
sudo apt update
sudo apt install afl++
```

On other systems, see the [AFL++ installation guide](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/INSTALL.md).

### Verify Installation

```bash
afl-gcc --version
afl-fuzz --help
```

## Quick Start

```bash
# 1. Generate seed corpus from test vectors
make fuzz-corpus

# 2. Build fuzz harnesses with AFL instrumentation
make fuzz-build

# 3. Start fuzzing (pick one)
make fuzz-decoder    # Fuzz the decoder
make fuzz-encoder    # Fuzz the encoder
make fuzz-roundtrip  # Fuzz encode+decode roundtrip

# 4. Press Ctrl+C to stop fuzzing
```

Decoder, encoder, and roundtrip fuzz harnesses exist for **deflate**, **gzip**, **LZ4**, **zstd**, **rle**, **lzw**, **zlib**, and **brotli**. See the `fuzz/` directory for the full list of harnesses and `make help` (or the Fuzz Testing section below) for available targets.

## Available Fuzz Targets

### Decoder Fuzzer (`fuzz-decoder`)

Tests the DEFLATE decoder with arbitrary input bytes. This finds:
- Crashes on malformed compressed data
- Hangs on pathological input
- Memory safety issues in parsing

```bash
make fuzz-decoder
```

### Encoder Fuzzer (`fuzz-encoder`)

Tests the DEFLATE encoder with arbitrary plaintext. This finds:
- Crashes during compression
- Issues with different input patterns
- Memory safety in the encoder

```bash
make fuzz-encoder
```

### Roundtrip Fuzzer (`fuzz-roundtrip`)

Compresses input, then decompresses it, and verifies the output matches the input. This is powerful because any mismatch is definitely a bug.

```bash
make fuzz-roundtrip
```

## Gzip Fuzz Targets

The gzip method has dedicated fuzz harnesses that test the wrapper functionality:

### Gzip Decoder Fuzzer (`fuzz-gzip-decoder`)

Tests the gzip decoder with arbitrary input bytes. This finds:
- Header parsing bugs (magic, flags, FNAME, FCOMMENT, FEXTRA, FHCRC)
- Integration issues between gzip wrapper and deflate decoder
- Trailer validation (CRC32, ISIZE mismatch handling)
- Limit enforcement (header field sizes, expansion ratio)
- Concatenated member handling

```bash
make fuzz-gzip-decoder
```

### Gzip Encoder Fuzzer (`fuzz-gzip-encoder`)

Tests the gzip encoder with arbitrary plaintext. This finds:
- Header generation issues
- CRC32 computation bugs
- Integration with inner deflate encoder
- Memory safety in option handling

```bash
make fuzz-gzip-encoder
```

### Gzip Roundtrip Fuzzer (`fuzz-gzip-roundtrip`)

Compresses input as gzip, then decompresses, and verifies the output matches. Tests the complete encode/decode path including:
- Header/trailer roundtrip correctness
- CRC32/ISIZE computation and validation
- Optional fields (name, comment) preservation

```bash
make fuzz-gzip-roundtrip
```

### Gzip vs Deflate Fuzzing

| Aspect | Deflate Harnesses | Gzip Harnesses |
|--------|-------------------|----------------|
| Format | Raw DEFLATE stream | GZIP wrapper (header + deflate + trailer) |
| Decoder input | Compressed bytes | Full gzip file format |
| Validates | Stream integrity | Header, CRC32, ISIZE |
| Limit testing | Basic limits | Header field limits, expansion ratio |
| Concatenation | N/A | Multi-member gzip streams |

**Recommendation**: Run both deflate and gzip harnesses. Deflate harnesses test compression core; gzip harnesses test wrapper and format handling.

## LZ4 Fuzz Targets

The LZ4 method has dedicated fuzz harnesses that test the LZ4 Frame Format implementation:

### LZ4 Decoder Fuzzer (`fuzz-lz4-decoder`)

Tests the LZ4 decoder with arbitrary input bytes. This finds:
- Frame header parsing bugs (magic, FLG, BD, content size, dictionary ID, header checksum)
- Block decompression issues (token parsing, match offsets, literal copies)
- Checksum validation bugs (block and content checksums)
- Limit enforcement (output size, expansion ratio, memory)
- Concatenated frame handling (when `lz4.concat` enabled)

```bash
make fuzz-lz4-decoder
```

### LZ4 Encoder Fuzzer (`fuzz-lz4-encoder`)

Tests the LZ4 encoder with arbitrary plaintext and various options. This finds:
- Block compression bugs (hash table, match finding, literal encoding)
- Frame header generation issues
- Checksum computation bugs
- Memory safety with different block sizes
- Edge cases in option handling

```bash
make fuzz-lz4-encoder
```

### LZ4 Roundtrip Fuzzer (`fuzz-lz4-roundtrip`)

Compresses input as LZ4, then decompresses, and verifies the output matches. This is the most powerful test because any mismatch is definitely a bug.

Tests include:
- Various option combinations (checksums, block sizes, independent vs dependent blocks)
- Streaming patterns (single-shot vs chunked processing)
- Data integrity across the full encode/decode cycle

```bash
make fuzz-lz4-roundtrip
```

### LZ4 Fuzzing Considerations

| Aspect | Notes |
|--------|-------|
| Block format | LZ4 uses a simpler, faster compression format than DEFLATE |
| Match window | 64KB sliding window (vs DEFLATE's 32KB) |
| Checksums | Optional per-block and content xxHash32 checksums |
| Block modes | Independent blocks (parallel-friendly) vs dependent blocks (better ratio) |
| Concatenation | Multiple frames can be concatenated; decoder handles with `lz4.concat` option |

**Recommendation**: The roundtrip fuzzer is most effective for LZ4 because it tests the full encode/decode path. The decoder fuzzer is important for security testing with untrusted input.

## RLE Fuzz Targets

The RLE method has fuzz harnesses that test both profiles:

- PackBits (`rle.format=packbits`)
- TGA (`rle.format=tga`)

The decoder harness runs both profiles per input to maximize coverage.

```bash
make fuzz-rle-decoder
make fuzz-rle-encoder
make fuzz-rle-roundtrip
```

## LZW Fuzz Targets

The LZW method has fuzz harnesses that test both profiles:

- GIF LZW (`lzw.format=gif`, LSB-first bit packing)
- TIFF LZW (`lzw.format=tiff`, MSB-first bit packing)

The decoder harness runs both profiles per input to maximize coverage.

```bash
make fuzz-lzw-decoder
make fuzz-lzw-encoder
make fuzz-lzw-roundtrip
```

## Zstd Fuzz Targets

```bash
make fuzz-zstd-decoder
make fuzz-zstd-encoder
make fuzz-zstd-roundtrip
```

The decoder harness is the interesting one: a zstd frame header carries a
window descriptor, and what a decoder does with a window a frame merely
*claims* is where `limits.max_memory_bytes` earns its keep.

## zlib Fuzz Targets

```bash
make fuzz-zlib-decoder
make fuzz-zlib-encoder
make fuzz-zlib-roundtrip
```

The three harnesses existed before any of these targets did, and could only be
reached through `make fuzz-replay`, which feeds every harness it finds.

The container is thin - CMF/FLG, deflate, a big-endian Adler-32 (RFC 1950) -
so what the decoder harness is really probing is the header's FCHECK and FDICT
bits and the trailer, on top of everything the deflate harness already covers.

## Brotli Fuzz Targets

```bash
make fuzz-brotli-decoder
make fuzz-brotli-encoder
make fuzz-brotli-roundtrip
```

The decoder harness reads arbitrary bytes. `make fuzz-corpus` seeds it with
streams this encoder wrote. The harness caps the window at `1 << 22` and the
expansion ratio at 4096, so a mutated window of 23 or 24 is refused rather
than allocated. The encoder harness takes `brotli.level` from the input's
second byte, so both the LZ77 and Huffman block and the section 11.1 store get
driven; it used to set neither and so drove whichever level was the default,
which meant level 0 lost its fuzzing the moment level 1 became that default.
An input too short to carry the byte gets the default. The
roundtrip harness aborts if a stream this encoder just wrote does not decode
back to the input.

## Understanding AFL++ Output

When you run AFL++, you'll see a status screen:

```
       american fuzzy lop ++4.00c {default} (fuzz_decoder)
┌─ process timing ────────────────────────────────────┬─ overall results ─────┐
│        run time : 0 days, 0 hrs, 5 min, 23 sec      │  cycles done : 42     │
│   last new find : 0 days, 0 hrs, 0 min, 12 sec      │ corpus count : 156    │
│ last uniq crash : none seen yet                     │  saved crashes : 0    │
│  last uniq hang : none seen yet                     │   saved hangs : 0     │
├─ cycle progress ───────────────────┬─ map coverage ─┴───────────────────────┤
│  now processing : 23.1 (14.7%)     │    map density : 2.45% / 3.12%         │
│  runs timed out : 0 (0.00%)        │ count coverage : 4.21 bits/tuple       │
...
```

Key metrics:
- **saved crashes**: Number of unique inputs that caused crashes (bugs!)
- **saved hangs**: Number of inputs that caused timeouts
- **corpus count**: Number of interesting test cases found
- **map coverage**: Code coverage achieved

## Finding and Analyzing Bugs

### Crash Files

When AFL++ finds a crash, it saves the input to:
```
fuzz/findings/<target>/crashes/id:XXXXXX,...
```

To reproduce a crash:
```bash
# Run the harness directly with the crash input
./build/linux/release/apps/fuzz/fuzz_deflate_decoder < fuzz/findings/decoder/crashes/id:000000,...
```

### Analyzing with GDB

```bash
# Build without AFL instrumentation for debugging
gcc -g -O0 -o fuzz_debug fuzz/fuzz_deflate_decoder.c \
    -I include/ build/linux/release/apps/libghoti.io-compress-0.a -lm

# Run under GDB
gdb ./fuzz_debug
(gdb) run < fuzz/findings/decoder/crashes/id:000000,...
```

### Using AddressSanitizer

For better crash analysis, build with ASan:

```bash
AFL_USE_ASAN=1 afl-gcc -O2 -g -fsanitize=address \
    -o fuzz_decoder_asan fuzz/fuzz_deflate_decoder.c \
    -I include/ build/linux/release/apps/libghoti.io-compress-0.a -lm
```

## Advanced Usage

### Running Multiple Fuzzer Instances

AFL++ can run multiple parallel instances for faster fuzzing:

```bash
# Terminal 1 - Main instance
afl-fuzz -M main -i fuzz/corpus/decoder -o fuzz/findings/decoder \
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder

# Terminal 2 - Secondary instance
afl-fuzz -S secondary1 -i fuzz/corpus/decoder -o fuzz/findings/decoder \
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

### Resuming a Fuzzing Session

AFL++ automatically resumes from previous findings:

```bash
# If fuzz/findings/decoder exists with previous results, AFL++ will resume
make fuzz-decoder
```

To start fresh, remove the findings directory:
```bash
rm -rf fuzz/findings/decoder
make fuzz-decoder
```

### Custom Fuzzer Options

Run AFL++ directly for more control:

```bash
afl-fuzz \
    -i fuzz/corpus/decoder \
    -o fuzz/findings/decoder \
    -t 5000 \                    # 5 second timeout
    -m 512 \                     # 512 MB memory limit
    -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

### Minimizing Test Cases

After finding a crash, minimize it to the smallest reproducing input:

```bash
afl-tmin -i fuzz/findings/decoder/crashes/id:000000,... \
         -o minimized_crash.bin \
         -- ./build/linux/release/apps/fuzz/fuzz_deflate_decoder
```

## Seed Corpus

The seed corpus provides starting inputs for the fuzzer. Better seeds lead to better coverage faster.

### Generating Corpus

```bash
make fuzz-corpus
```

This creates, for every method:

- `fuzz/corpus/<method>_decoder/` - streams that method's encoder produced,
  plus hand-written malformed and edge-case frames where they exist
- `fuzz/corpus/<method>_encoder/` - plaintext patterns and edge-case sizes
- `fuzz/corpus/<method>_roundtrip/` - the same plaintext patterns

DEFLATE's three are the unprefixed `decoder/`, `encoder/` and `roundtrip/`.

The decoder seeds are produced by this library's own encoders, which is the
normal arrangement for a seed corpus - AFL mutates them, and the inputs that
have actually found something live in `fuzz/regression/`, which is tracked and
never written to.

`make fuzz-corpus` then **fails if any campaign target names a corpus
directory it did not fill**, and it reads that list out of the Makefile rather
than keeping a second copy beside it. The two had already drifted: the LZ4 and
zstd campaigns named directories nothing created, and `fuzz-gzip-roundtrip`
named one that was created and left empty. Each of those targets has a
fallback that writes a single minimal frame, so the campaigns ran - from one
seed, silently, rather than from the corpus they appeared to be using.

### Adding Custom Seeds

Add your own seed files to the corpus directories:

```bash
# Add a specific test case
cp my_interesting_file.bin fuzz/corpus/decoder/

# Add compressed files from the wild
gunzip -c some_file.gz > temp.deflate  # Note: gzip has header, need raw deflate
```

## Continuous Fuzzing

CI does not run a campaign. It runs `make fuzz-replay`, which feeds every file
in `fuzz/regression/` to every harness and fails if any of them does not exit
cleanly, or if the corpus changed during the replay. That is a regression
check, not a search: it takes seconds, and it is what keeps an input that once
found something found.

A campaign is a search, and it needs a corpus that survives between runs to be
worth starting. For thorough testing, run fuzzers for extended periods:

```bash
# Run overnight
timeout 8h make fuzz-decoder

# Run for a week (use tmux/screen)
timeout 168h make fuzz-roundtrip
```

## Troubleshooting

### "No instrumentation detected"

Make sure you're using `afl-gcc` or `afl-clang-fast`:
```bash
make fuzz-build AFL_CC=afl-gcc
```

### "Hmm, your system is configured to send core dump notifications..."

This is common on Ubuntu/WSL2. You have two options:

**Option 1: Fix core_pattern (recommended for serious fuzzing)**
```bash
echo core | sudo tee /proc/sys/kernel/core_pattern
```
This change persists until reboot.

**Option 2: Skip the check (for quick testing)**
```bash
make fuzz-decoder AFL_ENV='AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1'
```
Note: This may cause AFL to miss some crashes or misreport them as timeouts.

### "The target binary looks like a shell script..."

Ensure the fuzz harness is a compiled binary, not a script.

### Slow Fuzzing

- Use smaller input sizes (the harnesses limit input to prevent this)
- Run multiple parallel instances
- Use `afl-clang-fast` instead of `afl-gcc` (faster instrumentation)

## Directory Structure

```
fuzz/
├── fuzz_deflate_decoder.c   # Deflate decoder fuzz harness
├── fuzz_deflate_encoder.c   # Deflate encoder fuzz harness
├── fuzz_roundtrip.c         # Deflate roundtrip fuzz harness
├── fuzz_gzip_decoder.c      # Gzip decoder fuzz harness
├── fuzz_gzip_encoder.c      # Gzip encoder fuzz harness
├── fuzz_gzip_roundtrip.c    # Gzip roundtrip fuzz harness
├── fuzz_lz4_decoder.c       # LZ4 decoder fuzz harness
├── fuzz_lz4_encoder.c       # LZ4 encoder fuzz harness
├── fuzz_lz4_roundtrip.c     # LZ4 roundtrip fuzz harness
├── fuzz_rle_decoder.c       # RLE decoder fuzz harness
├── fuzz_rle_encoder.c       # RLE encoder fuzz harness
├── fuzz_rle_roundtrip.c     # RLE roundtrip fuzz harness
├── fuzz_lzw_decoder.c       # LZW decoder fuzz harness
├── fuzz_lzw_encoder.c       # LZW encoder fuzz harness
├── fuzz_lzw_roundtrip.c     # LZW roundtrip fuzz harness
├── fuzz_zstd_decoder.c      # Zstd decoder fuzz harness
├── fuzz_zstd_encoder.c      # Zstd encoder fuzz harness
├── fuzz_zstd_roundtrip.c    # Zstd roundtrip fuzz harness
├── fuzz_zlib_decoder.c      # zlib decoder fuzz harness
├── fuzz_zlib_encoder.c      # zlib encoder fuzz harness
├── fuzz_zlib_roundtrip.c    # zlib roundtrip fuzz harness
├── fuzz_brotli_decoder.c    # Brotli decoder fuzz harness
├── fuzz_brotli_encoder.c    # Brotli encoder fuzz harness
├── fuzz_brotli_roundtrip.c  # Brotli roundtrip fuzz harness
├── generate_corpus.c        # Seed corpus generator
├── regression/              # Tracked; replayed by `make fuzz-replay`
├── corpus/                  # Seed inputs (generated)
│   ├── decoder/             # Raw deflate test inputs
│   ├── encoder/             # Plaintext inputs for deflate
│   ├── roundtrip/           # Plaintext for deflate roundtrip
│   ├── gzip_decoder/        # Gzip format test inputs
│   ├── gzip_encoder/        # Plaintext inputs for gzip
│   ├── gzip_roundtrip/      # Plaintext for gzip roundtrip
│   ├── lz4_decoder/         # LZ4 frame format test inputs
│   ├── lz4_encoder/         # Plaintext inputs for LZ4
│   ├── lz4_roundtrip/       # Plaintext for LZ4 roundtrip
│   ├── rle_decoder/         # RLE encoded streams (PackBits/TGA seeds)
│   ├── rle_encoder/         # Plaintext inputs for RLE
│   ├── rle_roundtrip/       # Plaintext inputs for RLE roundtrip
│   ├── lzw_decoder/         # LZW encoded streams (GIF/TIFF seeds)
│   ├── lzw_encoder/         # Plaintext inputs for LZW
│   ├── lzw_roundtrip/       # Plaintext inputs for LZW roundtrip
│   ├── zstd_decoder/        # Zstd frame format test inputs
│   ├── zstd_encoder/        # Plaintext inputs for zstd
│   ├── zstd_roundtrip/      # Plaintext for zstd roundtrip
│   ├── zlib_decoder/        # zlib streams (RFC 1950 container)
│   ├── zlib_encoder/        # Plaintext inputs for zlib
│   ├── zlib_roundtrip/      # Plaintext for zlib roundtrip
│   ├── brotli_decoder/      # Brotli streams this encoder wrote
│   ├── brotli_encoder/      # Plaintext inputs for brotli
│   └── brotli_roundtrip/    # Plaintext for brotli roundtrip
└── findings/                # AFL++ output (generated)
    ├── decoder/
    │   ├── crashes/         # Crash-inducing inputs
    │   ├── hangs/           # Hang-inducing inputs
    │   └── queue/           # Interesting test cases
    ├── encoder/
    ├── roundtrip/
    ├── gzip_decoder/
    ├── gzip_encoder/
    ├── gzip_roundtrip/
    ├── lz4_decoder/
    ├── lz4_encoder/
    ├── lz4_roundtrip/
    ├── rle_decoder/
    ├── rle_encoder/
    ├── rle_roundtrip/
    ├── lzw_decoder/
    ├── lzw_encoder/
    ├── lzw_roundtrip/
    ├── zstd_decoder/
    ├── zstd_encoder/
    ├── zstd_roundtrip/
    ├── zlib_decoder/
    ├── zlib_encoder/
    ├── zlib_roundtrip/
    ├── brotli_decoder/
    ├── brotli_encoder/
    └── brotli_roundtrip/
```

## References

- [AFL++ Documentation](https://github.com/AFLplusplus/AFLplusplus/tree/stable/docs)
- [AFL++ Fuzzing Tutorial](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/fuzzing_in_depth.md)
- [Google Fuzzing Guide](https://github.com/google/fuzzing)

## Replaying the tracked corpus

`fuzz/corpus` and `fuzz/findings` are afl-fuzz's working directories. They are
gitignored, they grow while a campaign runs, and they are not a record of
anything: pointing afl-fuzz at a corpus directory *adds to it*, so a corpus
that lives there cannot also be the thing that is checked in.

`fuzz/regression` is the tracked one. It is small, it is never written to, and
it is where an input that once found something goes so that it stays found.

```bash
make fuzz-replay
```

feeds every file in `fuzz/regression` to every harness and fails if any of them
does not exit cleanly. It mutates nothing and checks afterwards that the corpus
is byte-for-byte what it was, so it is a regression test rather than a search -
it takes seconds, and a change that reintroduces a fixed crash is caught then
rather than by whoever next runs a campaign.

Every file goes to every harness on purpose. Each harness reads arbitrary bytes
from stdin, so a DEFLATE stream is a perfectly good input to the Zstd decoder:
a decoder has to reject what is not its format as safely as it rejects a
corrupt example of it.

The harnesses are built through the same `FUZZ_SAN` path afl-fuzz uses, so a
memory error is an abort and an abort is a failure.

### Adding to it

When a campaign finds something, minimise it and add the minimised input:

```bash
cp fuzz/findings/<target>/default/crashes/<id> fuzz/regression/<what-it-was>.bin
make fuzz-replay        # fails until the bug is fixed
```

The name is for whoever reads the failure, so name it after the defect rather
than after the fuzzer's id.
