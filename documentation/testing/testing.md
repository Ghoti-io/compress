# Running tests

This document describes how to run tests for the compress library, including unit tests, memory checking with valgrind, and sanitizer builds.

## Quick Reference

| Command | Description |
|---------|-------------|
| `make test` | Run all tests (release build) |
| `make test-debug` | Run all tests (debug build) |
| `make test-valgrind` | Run all tests under valgrind |
| `make test-valgrind-debug` | Run all tests under valgrind (debug) |
| `make test-asan` | Run tests with AddressSanitizer + UBSan |
| `make test-ubsan` | Alias for `test-asan` |
| `make test-tsan` | Run tests with ThreadSanitizer |
| `make test-tsan-threads` | ThreadSanitizer, threaded suites only |

## Unit Tests

The test suite uses Google Test (gtest) and covers:
- Core infrastructure (options, registry, stream API)
- Deflate encoder and decoder
- Malformed input handling
- Stress and stability tests
- Cross-tool validation (oracle tests)

### Running Tests

```bash
# Run all tests (release build)
make test

# Run all tests (debug build with assertions)
make test-debug

# Run a specific test file
./build/linux/release/apps/tests/test_deflate_decoder --gtest_brief=1

# Run tests matching a pattern
./build/linux/release/apps/tests/test_deflate_decoder --gtest_filter="*StoredBlock*"
```

### Test Output

Tests use brief output mode by default. For verbose output:

```bash
./build/linux/release/apps/tests/test_deflate_decoder
```

## Valgrind Memory Checking

Valgrind detects memory errors that may not cause immediate crashes:
- Memory leaks (allocated memory never freed)
- Use-after-free
- Invalid reads/writes
- Uninitialized memory usage

### Running Valgrind Tests

```bash
# Run all tests under valgrind (Linux only)
make test-valgrind

# Run with debug build for better stack traces
make test-valgrind-debug
```

### What Valgrind Checks

The test target uses these valgrind flags:
- `--leak-check=full` - Detailed memory leak reporting
- `--show-leak-kinds=all` - Report all types of leaks
- `--track-origins=yes` - Track where uninitialized values came from
- `--error-exitcode=1` - Fail the build if errors are found

### Understanding Valgrind Output

**Clean output:**
```
==12345== HEAP SUMMARY:
==12345==     in use at exit: 0 bytes in 0 blocks
==12345==   total heap usage: 1,234 allocs, 1,234 frees, 56,789 bytes allocated
==12345== All heap blocks were freed -- no leaks are possible
```

**Memory leak example:**
```
==12345== 64 bytes in 1 blocks are definitely lost in loss record 1 of 1
==12345==    at 0x4C2BBAF: malloc (vg_replace_malloc.c:299)
==12345==    by 0x401234: my_function (myfile.c:42)
==12345==    by 0x401345: main (main.c:10)
```

**Invalid read example:**
```
==12345== Invalid read of size 4
==12345==    at 0x401234: my_function (myfile.c:45)
==12345==  Address 0x5204044 is 0 bytes after a block of size 4 alloc'd
```

### Running Valgrind Manually

For debugging a specific test or binary:

```bash
# Basic memory check
valgrind ./build/linux/release/apps/tests/test_deflate_decoder

# With full leak checking
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/linux/release/apps/tests/test_deflate_decoder

# With origin tracking (slower but more informative)
valgrind --leak-check=full --track-origins=yes \
    ./build/linux/release/apps/tests/test_deflate_decoder

# Check a specific test
valgrind --leak-check=full \
    ./build/linux/release/apps/tests/test_deflate_decoder \
    --gtest_filter="*RoundTrip*"
```

### Valgrind Tips

1. **Use debug builds** - Debug builds have symbols and no optimization, making stack traces more useful:
   ```bash
   make test-valgrind-debug
   ```

2. **Focus on one test** - When debugging a leak, isolate it:
   ```bash
   valgrind --leak-check=full ./build/linux/debug/apps/tests/test_options
   ```

3. **Suppressions** - If you encounter false positives from system libraries, you can create a suppressions file. Currently, no suppressions are needed.

4. **Platform note** - Valgrind is only available on Linux. On Windows, use the sanitizer builds or Windows debugging tools.

## Sanitizer Builds

Address Sanitizer (ASan) and Undefined Behavior Sanitizer (UBSan) catch errors at runtime with lower overhead than valgrind.

### Running Sanitizer Tests

```bash
# Run tests with ASan + UBSan
make test-asan

# Show help about sanitizers
make sanitizer-help
```

### What Sanitizers Detect

**AddressSanitizer (ASan):**
- Buffer overflows (heap, stack, global)
- Use-after-free
- Use-after-return
- Double-free
- Memory leaks (at exit)

**UndefinedBehaviorSanitizer (UBSan):**
- Signed integer overflow
- Null pointer dereference
- Misaligned pointer access
- Division by zero
- Out-of-bounds array access
- Invalid enum values

**ThreadSanitizer (TSan):**
- Data races between threads
- Lock-order inversions, which are potential deadlocks
- A thread-unsafe object used from two threads at once

### ThreadSanitizer

TSan builds into its own tree (`build/<platform>-tsan`) because it cannot be
combined with ASan: both replace the allocator, and asking for one after the
other is rejected by the compiler.

```bash
# Every suite
make test-tsan

# Just the threaded ones, which is what a change to job scheduling
# gets run against while it is being written
make test-tsan-threads
```

It answers a question none of the other tools ask. Valgrind's memcheck does
not look for races. ASan reports a use-after-free only once the timing that
produces it has actually occurred, so a race the machine happens to win on
every run leaves no trace at all. TSan reports the race itself, from a single
interleaving, whether or not that interleaving corrupted anything.

Four paths in this library are threaded: parallel encode and parallel decode,
for both LZ4 and Zstandard. `test-tsan-threads` covers those five suites;
`test-tsan` covers everything, because shared state does not have to live in
a file whose name says "parallel".

A run is slower than a release run by roughly an order of magnitude, and
`halt_on_error=1` stops at the first race rather than repeating it from every
test that follows.

#### Do not preload the TSan runtime

The ASan target runs its tests with `LD_PRELOAD` naming `libasan.so`, because
the ASan runtime has to initialise before anything it intercepts and an
`LD_PRELOAD` inherited from the environment would otherwise come first. The
TSan target deliberately does the opposite: it *empties* `LD_PRELOAD`.

Linking with `-fsanitize=thread` already puts `libtsan.so.2` first in the
executable's own `NEEDED` list, so a preload buys nothing. It also breaks the
suite. With `libtsan` preloaded, the shell that `system()` spawns inherits the
preload and dies, and `system()` returns 11 whatever it was asked to run - so
every oracle in this project reports its reference as missing. `python3` backs
the zlib, gzip and deflate oracles; the `zstd` CLI backs the Zstandard and
seekable ones. The first full TSan run failed 135 tests for this reason and not
one of them was a race.

ASan does not have this problem: `system()` works normally with `libasan`
preloaded, and the ASan oracle suites do run their comparisons.

The thing that made this visible is worth keeping in mind when writing an
oracle test. These suites treat an absent reference as a **failure**, not a
skip. Had they skipped, the run would have been green while nothing was ever
compared against a reference implementation.

### Understanding Sanitizer Output

**ASan buffer overflow example:**
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000014
READ of size 1 at 0x602000000014 thread T0
    #0 0x401234 in my_function myfile.c:42
    #1 0x401345 in main main.c:10
```

**UBSan signed overflow example:**
```
myfile.c:42:15: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
```

### Sanitizers vs Valgrind

| Aspect | Valgrind | ASan/UBSan |
|--------|----------|------------|
| Speed | 10-50x slower | 2x slower |
| Memory overhead | High (~2x) | Moderate |
| Compilation | Not required | Requires recompilation |
| Leak detection | Excellent | Good (ASan) |
| Undefined behavior | Limited | Excellent (UBSan) |
| Platform | Linux only | Linux, macOS, Windows |

**Recommendation:** Use ASan/UBSan during development for fast iteration, and valgrind for thorough final verification.

## Oracle Tests

Oracle tests compare our implementation against external reference implementations to verify correctness. These tests cross-validate that our encoder produces output that external decoders can read, and vice versa.

### Running Oracle Tests

```bash
# Oracle tests run automatically with make test
make test

# Skip oracle tests (if dependencies unavailable)
GCOMP_SKIP_ORACLE_TESTS=1 make test

# Verbose oracle test output
GCOMP_ORACLE_VERBOSE=1 make test
```

### Requirements

**For gzip/deflate oracle tests:**
- Python 3 with zlib module (standard library)

**For LZ4 oracle tests (optional):**
- Python 3 with lz4 module: `pip3 install lz4`
- LZ4 CLI tools: `sudo apt install lz4`

Tests skip gracefully if dependencies are unavailable.

### What Oracle Tests Verify

| Method | External Tools | Tests |
|--------|----------------|-------|
| gzip | Python zlib | Compress with us, decompress with Python; compress with Python, decompress with us |
| lz4 | Python lz4.frame, lz4/unlz4 CLI | Bidirectional cross-validation, checksum verification, concatenated frames |

### LZ4 Oracle Tests

The LZ4 oracle tests perform extensive cross-validation:

```bash
# Run just the LZ4 oracle tests
./build/linux/release/apps/testLz4_oracle
```

**Tests include:**
- Golden vectors (verified against Python lz4 v4.4.5)
- Our encoder → Python decoder (various data patterns and sizes)
- Python encoder → Our decoder (various options and checksums)
- CLI tools validation (lz4/unlz4 if installed)
- Concatenated frame handling
- Content and block checksum verification

## Stress Tests

Stress tests verify stability under heavy load.

### Running Stress Tests

```bash
# Run with default settings
make test

# Configure stress test iterations
GCOMP_STRESS_ITERATIONS=1000 make test

# Configure large file size (bytes)
GCOMP_STRESS_LARGE_SIZE=10485760 make test  # 10 MB
```

### What Stress Tests Cover

- Rapid create/destroy cycles (memory leaks)
- Large file compression (1+ MB)
- Many small file compressions
- All compression levels
- Multiple simultaneous encoders/decoders
- Streaming with tiny buffers

## Fuzz Testing

See [fuzzing.md](fuzzing.md) for comprehensive fuzz testing documentation.

## Test Organization

```
tests/
├── core/                         # Core infrastructure tests
│   ├── test_allocator.cpp        # Memory allocator tests
│   ├── test_buffer_wrappers.cpp  # Buffer convenience API tests
│   ├── test_callback_api.cpp     # Callback streaming API tests
│   ├── test_crc32.cpp            # CRC32 utility tests
│   ├── test_errors.cpp           # Error handling tests
│   ├── test_limits.cpp           # Safety limits tests
│   ├── test_options.cpp          # Options API tests
│   ├── test_registry.cpp         # Registry tests
│   ├── test_schema.cpp           # Option schema introspection tests
│   ├── test_state_machine.cpp    # State machine robustness tests
│   ├── test_stream.cpp           # Stream infrastructure tests
│   └── test_xxhash32.cpp         # xxHash32 utility tests
├── integration/                  # Integration tests
│   ├── test_autoreg.cpp          # Auto-registration tests
│   ├── test_expansion_ratio.cpp  # Decompression bomb protection tests
│   ├── test_oracle.cpp           # General cross-tool validation
│   ├── test_passthru.cpp         # Pass-thru method tests
│   └── test_stress.cpp           # Stress and stability tests
├── methods/
│   ├── brotli/                   # Brotli method tests
│   │   ├── test_brotli.cpp       # Format, levels, windows, libbrotli oracle
│   │   ├── test_brotli_encoder.cpp # Arguments, reset, back-pressure, skewed alphabets
│   │   ├── test_brotli_register.cpp # peek across every window; table bounds
│   │   └── test_brotli_robustness.cpp # Hand-built streams, corruption sweep
│   ├── deflate/                  # Deflate method tests
│   │   ├── test_deflate_bitio.cpp
│   │   ├── test_deflate_decoder.cpp
│   │   ├── test_deflate_encoder.cpp
│   │   ├── test_deflate_huffman.cpp
│   │   ├── test_deflate_malformed.cpp
│   │   └── data/golden_vectors.h
│   ├── gzip/                     # Gzip method tests
│   │   ├── test_gzip_*.cpp       # (various gzip tests)
│   │   └── data/golden_vectors.h
│   ├── lzw/                      # LZW method tests
│   │   ├── test_lzw_core.cpp
│   │   ├── test_lzw_decoder.cpp
│   │   ├── test_lzw_encoder.cpp
│   │   ├── test_lzw_register.cpp
│   │   └── test_lzw_roundtrip.cpp
│   ├── lz4/                      # LZ4 method tests
│   │   ├── test_lz4_concat.cpp   # Concatenated frame tests
│   │   ├── test_lz4_corruption.cpp # Corruption handling tests
│   │   ├── test_lz4_decoder.cpp  # Decoder tests
│   │   ├── test_lz4_encoder.cpp  # Encoder tests
│   │   ├── test_lz4_format.cpp   # Frame format tests
│   │   ├── test_lz4_limits.cpp   # Limit enforcement tests
│   │   ├── test_lz4_options.cpp  # Option handling tests
│   │   ├── test_lz4_oracle.cpp   # Cross-tool validation (Python lz4, CLI)
│   │   ├── test_lz4_parallel.cpp # Parallel compression tests
│   │   ├── test_lz4_register.cpp # Registration tests
│   │   ├── test_lz4_reset.cpp    # Encoder/decoder reset tests
│   │   ├── test_lz4_robustness.cpp # Robustness tests
│   │   ├── test_lz4_roundtrip.cpp # Encode/decode roundtrip tests
│   │   ├── test_lz4_streaming.cpp # Streaming API tests
│   │   └── data/golden_vectors.h # Verified test vectors (Python lz4 v4.4.5)
│   ├── rle/                      # RLE method tests
│   │   ├── test_rle_core.cpp
│   │   ├── test_rle_decoder.cpp
│   │   ├── test_rle_encoder.cpp
│   │   ├── test_rle_register.cpp
│   │   └── test_rle_roundtrip.cpp
│   └── zstd/                      # Zstd method tests
│       ├── test_zstd_allocator.cpp
│       ├── test_zstd_concat.cpp
│       ├── test_zstd_corruption.cpp
│       ├── test_zstd_decoder.cpp
│       ├── test_zstd_encoder.cpp
│       ├── test_zstd_entropy.cpp
│       ├── test_zstd_format.cpp
│       ├── test_zstd_limits.cpp
│       ├── test_zstd_options.cpp
│       ├── test_zstd_oracle.cpp
│       ├── test_zstd_parallel.cpp
│       ├── test_zstd_register.cpp
│       ├── test_zstd_reset.cpp
│       ├── test_zstd_robustness.cpp
│       ├── test_zstd_roundtrip.cpp
│       ├── test_zstd_streaming.cpp
│       └── (data/ as needed)
└── common/
    ├── test_helpers.h            # Test utility functions
    ├── test_helpers.cpp          # Test utility implementations
    ├── failing_allocator.h       # Allocator that fails on demand
    ├── passthru_method.h         # A method that copies, for the core tests
    └── temp_file.h               # Temporary files, through cutil
```

### Temporary files

Anything that shells out to an oracle needs a file to hand it, and
`tests/common/temp_file.h` is where that comes from.  Use it rather than
writing another one:

```cpp
#include <temp_file.h>

gcomp_test::TempFile in("gcomp_gzip_oracle", ".gz");
ASSERT_TRUE(in.valid());
ASSERT_TRUE(in.write(compressed));

std::string cmd = "gunzip -c \"" + in.path() + "\"";
// ... run it ...
// No unlink: the file removes itself when `in` goes out of scope, on every
// path out of the test including a failed assertion.
```

The suffix is not decoration - `gzip` and `zstd` both dispatch on the
extension - so it is part of the name rather than something added afterwards.

| you want | call |
| --- | --- |
| a scratch file that cleans itself up | `TempFile(prefix, suffix)` |
| a second name beside it, e.g. the oracle's output | `t.derived(".out")` |
| a name whose lifetime is not a scope | `uniqueTempPath(prefix, suffix)` |
| read a whole file | `readWholeFile(path)`, or the `(path, &out)` form when a missing file must be told apart from an empty one |
| write a whole file | `writeWholeFile(path, data)` |
| a path a Python one-liner can embed | `toPosixPath(path)` |

All of it is cutil underneath (`ghoti.io/cutil/file.h` and `path.h`), which
matters for two reasons.  The name and the file are created in one step that
fails if the name is taken, so nothing can put a symbolic link there in
between; and the directory comes from `gcu_path_temp_dir()`, which honours
`TMPDIR`.  The twelve private copies this replaced did neither reliably, and nine of
them carried a `#ifdef _WIN32` half that no build has ever compiled.

## Continuous Integration

For CI environments, run the full test suite:

```bash
# Build and test
make all
make test
make test-valgrind
make test-asan

# Quick smoke test (faster)
make test
```

### Exit Codes

All test commands return:
- `0` - All tests passed
- `1` - One or more tests failed

This allows CI systems to detect failures automatically.
