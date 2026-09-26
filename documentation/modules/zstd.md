# Zstd

Zstandard compression and decompression for the Ghoti.io Compress library. The method name is `"zstd"` and supports stream encode and decode. This implementation follows the Zstandard compression format specification, providing high compression ratios with excellent decompression speed.

**Reference:** [Zstandard compression format specification (RFC 8878)](https://www.rfc-editor.org/rfc/rfc8878.html)

## Registration

Zstd is **auto-registered** with the default registry when the library loads. No explicit initialization is required:

```c
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/zstd.h>

// zstd is already available - just use it
gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(gcomp_registry_default(), "zstd", NULL, &enc);
```

For custom registries or when auto-registration is disabled, register explicitly:

```c
gcomp_registry_t *custom = NULL;
gcomp_registry_create(NULL, &custom);
gcomp_method_zstd_register(custom);  // Now zstd is available
```

See [Auto-Registration](../auto-registration.md) for details on disabling auto-registration.

## Architecture

The Zstd method is a **standalone** compression method (not a wrapper). It implements the complete Zstandard frame format, which provides:

- Frame header with configuration flags and optional content size
- Multiple data blocks (raw, RLE, or compressed)
- Block-level entropy coding using FSE (Finite State Entropy) and Huffman
- Sequence-based LZ compression with repeat offset optimization
- Optional content checksum (xxHash64, low 32 bits) in trailer

```
┌────────────────────────────────────────────────────────────────────┐
│                      Zstd Frame Structure                          │
│  ┌─────────────┐  ┌─────────────────────────┐  ┌─────────────────┐ │
│  │   Header    │  │        Data Blocks      │  │    Trailer      │ │
│  │  (6-18 B)   │  │                         │  │   (0-4 B)       │ │
│  └─────────────┘  └─────────────────────────┘  └─────────────────┘ │
│        │                    │                         │            │
│        v                    v                         v            │
│   Magic (4B)           Block 1:                 Content checksum   │
│   Frame Header          - Header (3B)            (if enabled)      │
│   Descriptor (1B)       - Data (var)                               │
│   [Window Desc]        Block 2...                                  │
│   [Dict ID]            Last Block (last=1)                         │
│   [Content Size]                                                   │
└────────────────────────────────────────────────────────────────────┘
```

### Block types

| Type | Value | Description |
|------|-------|-------------|
| Raw | 0 | Uncompressed data, copied directly |
| RLE | 1 | Run-length encoded: single byte repeated |
| Compressed | 2 | Entropy-coded literals + LZ sequences |
| Reserved | 3 | Invalid, returns `GCOMP_ERR_CORRUPT` |

### Compressed block structure

Compressed blocks contain two sections:

1. **Literals Section**: The literal bytes (non-matched data)
   - Can be raw, RLE, or Huffman-compressed
   - A Huffman table can be carried inline or reused from the previous block;
     this encoder always writes its own, and reads either

2. **Sequences Section**: LZ match instructions
   - Each sequence: (literal_length, match_offset, match_length)
   - Encoded using FSE (Finite State Entropy) with predefined or custom tables
   - Supports repeat offsets for improved compression

```
┌─────────────────────────────────────────────────────────────┐
│                    Compressed Block                         │
│  ┌──────────────────────┐  ┌──────────────────────────────┐ │
│  │   Literals Section   │  │      Sequences Section       │ │
│  │  (header + data)     │  │  (header + FSE bitstream)    │ │
│  └──────────────────────┘  └──────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Options

### Zstd-specific options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `zstd.level` | int64 | 3 | Compression level (1-22). Higher = better compression, slower |
| `zstd.checksum` | bool | false | Enable content checksum (xxHash64, low 32 bits) |
| `zstd.window_log` | uint64 | 0 (auto) | Window log size (10-31). 0 = auto from level |
| `zstd.dictionary` | bytes | (none) | Optional dictionary (raw content or formatted per RFC 8878 §5) |
| `zstd.dictionary_id` | uint64 | (none) | Dictionary ID to write (encoder) or validate (decoder); used when dictionary provided. The format carries at most 32 bits |
| `zstd.content_size` | uint64 | (none) | Content size to write in header (optional) |
| `zstd.seekable_frame_size` | uint64 | 1 MiB | Decompressed bytes per frame when writing a seekable file with `gcomp_seekable_write_buffer()`. See [Seekable](../api/seekable.md). |
| `zstd.seekable_checksum` | bool | true | Record each frame's checksum in the seek table. |
| `zstd.concat` | bool | true | Decoder: decode every frame in the input (RFC 8878 §3.1). Set false to stop after the first frame |
| `zstd.job_size` | uint64 | 0 (auto) | Encoder: job size for parallel compression (64KB–16MB, 0=auto) |
| `zstd.long` | bool | false | Find matches across the whole window, not just the nearest 8 MB. See [Long-distance matching](#long-distance-matching) |
| `zstd.ldm_min_match` | uint64 | 64 | Bytes a long-distance match must have (16–4096) |
| `zstd.ldm_hash_log` | uint64 | 0 (auto) | Log2 of the long-distance table (6–30). 0 = sized from the window |
| `zstd.ldm_hash_rate_log` | uint64 | 6 | Index one position in 2^this for long-distance matching (0–12) |

### Threading options (encoder only)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `threads.count` | uint64 | 1 | Number of worker threads (0 or 1 = single-threaded). On decode it applies only to a stream of several frames that each declare a content size — blocks inside one frame share a window and cannot be split. See [Threading](../threading.md). |

When `threads.count > 1`, the encoder uses parallel compression where input is split into independent jobs. See [Parallel Compression](#parallel-compression) below.

### Core limit options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `limits.max_output_bytes` | uint64 | 512 MiB | Maximum decompressed output (decoder) |
| `limits.max_window_bytes` | uint64 | 128 MiB | Maximum window a frame may declare |
| `limits.max_memory_bytes` | uint64 | 256 MiB | Maximum working memory |
| `limits.max_expansion_ratio` | uint64 | 32768 | Maximum output/input ratio (decoder). The default is the format's own ceiling: RFC 8878 §3.1.1.2.2's four-byte RLE_Block carries at most 128 KB, so no valid stream can exceed it. |

Limits are enforced by the infrastructure; the decoder returns `GCOMP_ERR_LIMIT` when exceeded.

### Compression level guidelines

RFC 8878 says nothing about levels: it describes the frame, not how to choose
what goes in it. A level here is a promise about how much work the encoder will
spend, and it sets three things - what the match finder searches, how the parse
picks from what it finds, and how large a window it declares.

| Levels | Search | Parse | Window | Use case |
|--------|--------|-------|-------:|----------|
| 1 | hash chain, 4 candidates | greedy | 128 KB | real-time, logging |
| 2-3 | hash chain, 8-16 | deferred | 128 KB | general purpose (**default: 3**) |
| 4-6 | hash chain, 24-64 | deferred | 512 KB | |
| 7-8 | hash chain, 80-112 | deferred | 2 MB | the fast end of the chain |
| 9-10 | **binary tree**, 24-32 | deferred | 2 MB | where the tree takes over |
| 11-15 | binary tree, 36-52 | **shortest path** | 8 MB | archival; where the parse takes over |
| 16-19 | binary tree, 56-72 | shortest path | 8 MB at 16, 32 MB from 17 | maximum compression |
| 20-22 | binary tree, 80-128 | shortest path, **twice** | 32 MB | everything it has |

**The candidate counts either side of level 9 are not comparable.** A hash-chain
step crosses off one position and learns nothing about the next, so searching
harder means walking further and the counts run to the hundreds. A binary-tree
step halves what is left, and a descent takes about six steps - so 14 candidates
at level 9 is *more* search than 112 at level 8, not less.

The two bands that matter most are 8 to 9, where the search changes, and 10 to
11, where the parse does. The second is the larger: on a 19 MB corpus level 11
is 8.0% smaller than level 10, which is more than levels 11 through 22 together
used to buy. A level above 11 buys tenths of a percent; the step onto 11 buys
the rest.

**Deferred** parsing holds a match back to see whether the next position starts a
longer one. **Shortest path** does not defer at all: it prices every candidate
match and every literal in 256ths of a bit, from statistics primed off the
predefined FSE distributions of RFC 8878 section 3.1.1.3.2.2 and the block's own
byte histogram, and walks the cheapest path through the block. Because a zstd
sequence carries a literal-length code, a path's cost is not exactly the sum of
its edges, so this is a very good approximation rather than a proof of optimality;
`src/methods/zstd/zstd_optimal.c` says where the approximation lies.

Levels 20 to 22 parse each sweep twice: once priced by what came before it,
then again priced by what the first pass found in the sweep itself. It is
worth 0.06% at level 20 and 0.10% at level 22 over the 19 MB corpus, and
costs about a quarter of the encode rate - which is the trade those levels
exist to make. It also costs memory: the match finder cannot be asked the
same question twice, because searching a position is what inserts it into the
tree, so the first pass keeps every candidate it was given for the second to
read back. That is 8 MB at level 22.

Levels 9 and 10 pay for the tree in memory: two slots per position where the
chain had one, 16 MiB against 8.5 over the 2 MB window they declare.

### What it costs and buys

Measured 2026-09-18 over 19,181,880 bytes of source, manuals, XML, CSV, binaries
and images, against **libzstd 1.5.7** at the same level, in one run of
`bench/bench_ratio` so that both sides saw the same machine. Negative means our
output is smaller. Speed is MiB of input per second; sizes are exact, speeds
good to about five percent.

| Level | Ours | libzstd | Delta | Encode | libzstd encode |
|-------|-----:|--------:|------:|-------:|---------------:|
| 1 | 5,029,655 | 5,223,057 | **-3.70%** | 108.6 MiB/s | 531.9 MiB/s |
| 3 | 4,693,228 | 4,831,204 | **-2.86%** | 55.5 MiB/s | 401.7 MiB/s |
| 9 | 4,334,525 | 4,341,823 | **-0.17%** | 11.2 MiB/s | 85.7 MiB/s |
| 16 | 3,907,753 | 3,959,522 | **-1.31%** | 5.4 MiB/s | 7.7 MiB/s |
| 19 | 3,881,087 | 3,836,814 | +1.15% | 5.1 MiB/s | 4.3 MiB/s |

A corpus total is an average. Per file these span a wider range than the totals
suggest - level 1 runs from -9.4% on prose to +7.2% on a skewed-alphabet file -
so measure on your own data before choosing a level on ratio alone.

### Window size

The window size controls how far back the compressor can reference previous
data. Larger windows compress better and cost more memory.

| Window Log | Window Size | Where it is used |
|------------|-------------|------------------|
| 17 | 128 KB | levels 1-3 |
| 19 | 512 KB | levels 4-6 |
| 21 | 2 MB | levels 7-10 |
| 23 | 8 MB | levels 11-16 |
| 25 | 32 MB | levels 17-22 |
| 31 | 2 GB | maximum the format allows; reachable only by setting `zstd.window_log` |

When `zstd.window_log=0` (the default) the window comes from the level, as
above. Setting it explicitly overrides the level, and the decoder enforces
`limits.max_window_bytes` against whatever the frame declares.

## Long-distance matching

The ordinary match finder reaches about 8 MB back. A file holding two copies
of the same megabyte thirty megabytes apart therefore compresses as though the
second copy were new, however large `zstd.window_log` is: the window says how
far a sequence *may* point, and the finder's tables say how far it actually
looks.

`zstd.long=true` adds a second, much coarser index that covers the whole
declared window. It hashes 64 bytes at a time (`zstd.ldm_min_match`) and
records one position in every 64 (`zstd.ldm_hash_rate_log`), so the cost of
covering a gigabyte stays proportional to the input rather than to the window.
It looks only for long matches, which is what makes a distant offset worth its
bits.

```c
gcomp_options_set_bool(opts, "zstd.long", true);
gcomp_options_set_uint64(opts, "zstd.window_log", 27);  // 128 MB
```

Nothing about it reaches the format. RFC 8878 constrains an offset only by the
window the frame header declares (§3.1.1.1.2), so the output is an ordinary
Zstandard stream that any decoder reads — this was measured both ways against
the reference `zstd` before the feature was written.

On 32 MB holding two copies of the same 4 MB block 24 MB apart:

| | bytes |
|---|---|
| input | 33,554,432 |
| `zstd.level=19`, no long | 33,555,209 |
| reference `zstd -19` | 33,555,214 |
| `zstd.level=19`, `zstd.long` | **29,361,427** |
| reference `zstd -19 --long=27` | 29,361,171 |

It is worth turning on only for data that actually repeats itself at long
range — backups, VM images, packfiles, concatenated logs. On data that does
not, it costs a table and a sweep and finds nothing.

### With threads

`zstd.long` works with `threads.count > 1`, and the ratio is within 1% of the
single-threaded encoder's on the same input.

This was refused until 2026-09-24, on the stated grounds that "a parallel job
compresses its block against its own window, so a match reaching tens of
megabytes back is not available to it". That was wrong about this encoder. A job
is seeded with a **whole window** of the preceding stream — the overlap is sized
from `zstd.window_log` and is deliberately not capped at the job size — and a
long match may not reach past the declared window in any case (RFC 8878
§3.1.1.1.2). A job's reach is therefore the single-threaded encoder's reach, and
the reference's design of scanning once in the calling thread answers a problem
this encoder does not have.

`zstd.job_size` does not change the reach, only where the block seams fall.

## Content checksum

When `zstd.checksum=true`, the encoder computes an xxHash64 checksum over all uncompressed content. The low 32 bits are written to the frame trailer.

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bool(opts, "zstd.checksum", 1);

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
// ... encode data ...
// Checksum is automatically computed and written to trailer
```

The decoder validates the checksum if present. A mismatch returns `GCOMP_ERR_CORRUPT`.

## Content size

The encoder can write the uncompressed content size in the frame header:

```c
// Encoder: write content size in header
gcomp_options_set_uint64(opts, "zstd.content_size", 1000);

// Decoder will validate: decompressed size must equal 1000 bytes
```

This enables:
- Progress reporting during decompression
- Pre-allocation of output buffers
- Validation that all data was received

## Dictionary compression

When compressing or decompressing many small files with similar structure (e.g. log lines, JSON records), a **dictionary** improves ratio by providing shared initial context. The encoder uses the dictionary as initial window history; the decoder preloads the same content so matches can be resolved.

### Options

| Option | Encoder | Decoder |
|--------|---------|---------|
| `zstd.dictionary` | bytes: raw or formatted dictionary (RFC 8878 §5) | Same: must match encoder’s dictionary |
| `zstd.dictionary_id` | uint32: ID written in frame header (optional) | Validated against frame header when present |

### Encoder usage

```c
// Build or load a dictionary (e.g. from samples)
uint8_t *dict_bytes = ...;
size_t dict_len = ...;

gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bytes(opts, "zstd.dictionary", dict_bytes, dict_len);
gcomp_options_set_uint32(opts, "zstd.dictionary_id", 1);  // optional

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
// ... encode data; frame header will include dictionary ID when set ...
```

### Decoder usage

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_bytes(opts, "zstd.dictionary", dict_bytes, dict_len);

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", opts, &dec);
// ... decode; window is preloaded with dictionary content ...
```

If the frame header specifies a dictionary ID and the decoder was created without a dictionary (or with a different ID for formatted dictionaries), the decoder returns `GCOMP_ERR_UNSUPPORTED` or `GCOMP_ERR_CORRUPT` with a message indicating the required dictionary ID. Raw dictionaries (no magic) are accepted for any frame dictionary ID to support external encoders that use content-derived IDs.

### Dictionaries with threads

A dictionary works at every `threads.count`. Each job is handed the dictionary's
content as the history in front of it — the same mechanism that gives a job the
tail of the job before it — so every job can match into it, and the dictionary
survives a `GCOMP_FLUSH_FULL` because the frame that follows declares the same
Dictionary_ID.

**Before 2026-09-26 it did not.** `zstd.dictionary` with `threads.count > 1` was
accepted, returned `GCOMP_OK`, and was silently ignored: on 1.5 MB whose content
the dictionary held, one thread produced 163 bytes and four threads 65,705, the
same as with no dictionary at all. The frame header still declared the
Dictionary_ID, so the output *hard-required* a dictionary it had never used — a
decoder without it fails, and a caller had to keep one for no benefit. If you have
archives written by a parallel encoder before that date, they are decodable and
correct, just larger than they should be and tied to a dictionary they do not need.

What a formatted dictionary's entropy section contributes is unchanged by thread
count, because the encoder does not use it in either mode: the Huffman and FSE
tables are read only by the decoder, and the three repeat offsets change no output
(measured — see `RepeatOffsetsDoNotChangeTheOutput`, which is the tripwire for
that stopping being true).

### Dictionary format

- **Raw**: Any bytes, no magic. `dict_id` is 0; encoder can still set `zstd.dictionary_id` in the header for identification.
- **Formatted** (RFC 8878 §5): Magic 0xEC30A437, 4-byte dictionary ID, entropy tables (Huffman, FSE), repeat offsets, then content. Encoder and decoder can use the entropy tables and repeat offsets from the dictionary for the first block.

## Parallel compression

The Zstd encoder supports parallel compression for improved throughput on multi-core systems. When enabled, input data is split into independent jobs that are compressed concurrently.

### Enabling parallel compression

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);
gcomp_options_set_uint64(opts, "threads.count", 4);  // Use 4 worker threads

gcomp_encoder_t *enc = NULL;
gcomp_encoder_create(registry, "zstd", opts, &enc);
```

### How it works

1. Input data is accumulated until `zstd.job_size` bytes are buffered (512 KB when the option is left at 0)
2. Each full job is submitted to a thread pool for compression
3. Each job compresses its slice into a run of **blocks** — not a frame
4. Those blocks are collected in submission order and written to the output
5. The encoder writes one frame header and one final block, so **the output is a
   single Zstd frame**

```
┌───────────────────────────────────────────────────────────────────────┐
│                    Parallel Compression Flow                          │
│                                                                       │
│  Input Stream:    ────────────────────────────────────────────────►   │
│                   │  job_size  │  job_size  │  job_size  │ partial │  │
│                                                                       │
│  Worker Threads:  ┌────────┐   ┌────────┐   ┌────────┐                │
│                   │ Thread │   │ Thread │   │ Thread │   ...          │
│                   │   1    │   │   2    │   │   3    │                │
│                   └────────┘   └────────┘   └────────┘                │
│                        ↓           ↓           ↓                      │
│  Output:          [blocks 1]  [blocks 2]  [blocks 3]  [blocks 4]      │
│                   ◄───────────────────────────────────────────────    │
│                  one frame: header, all blocks in order, final block  │
└───────────────────────────────────────────────────────────────────────┘
```

### Job size configuration

| `zstd.job_size` | Behavior |
|-----------------|----------|
| 0 (default) | 512 KB, whatever the level is |
| 64 KB - 16 MB | Used as given |
| anything else | `gcomp_encoder_create()` returns `GCOMP_ERR_INVALID_ARG`, with an error detail naming the bounds |

Out-of-range values are **rejected, not clamped**: `zstd.job_size = 1024`
fails rather than quietly becoming 64 KB, so a caller who asked for something
the encoder will not do hears about it.

The default does not vary with the level or the window, which matters at the
top of the ladder: level 19 declares a 32 MB window and still takes 512 KB
jobs, so each job sees far less history than a single-threaded encode of the
same input would.

Larger job sizes provide better compression (more context for the match finder) but reduce parallelism. Smaller job sizes enable more parallelism but may reduce compression ratio slightly.

### Output format

Parallel compression produces **one Zstd frame**, whose blocks were compressed
on several threads. Nothing about the output says it was made in parallel.

- Standard tools (`zstd`, `unzstd`) decompress it directly, and no decoder
  option is needed
- `zstd.concat` is irrelevant to it: there is only one frame

> **This section said the opposite until 2026-09-24** — "concatenated Zstd
> frames. Each frame is complete and valid" — and it mattered, because a reader
> who believes that might split the output at a frame boundary and decode the
> pieces. There are no such boundaries to split at. Measured on 8 MB at
> `threads.count` 4: one frame magic in the output, a plain decode with no
> concat option returns all 8,388,608 bytes, and `zstd -l` reports
> `Frames 1  Skips 0`.
>
> One frame is also what makes a job able to match back into the job before it,
> and so what makes `zstd.long` work with threads.

```c
// Decoding parallel-compressed output: exactly like any other zstd stream.
gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", NULL, &dec);
```

### Single-threaded fallback

When `threads.count` is 0 or 1, the encoder operates in single-threaded mode:

- No threading overhead
- Standard streaming compression behavior
- The output is a single frame, as it is in parallel mode — what differs is the
  ratio (each job starts from an overlap rather than from the whole history),
  not the shape

### Memory usage

Parallel compression uses additional memory:
- Per-job input buffer (`job_size` bytes each)
- Per-job output buffer (up to `job_size + overhead` bytes)
- Match finder state per in-flight job

The encoder respects `limits.max_memory_bytes` by limiting the number of concurrent in-flight jobs.

### Error handling

If any worker thread encounters an error:
- The error is propagated to the main encoder
- Subsequent `update()` or `finish()` calls return the error
- All in-flight jobs are cleaned up

## Flushing

`gcomp_encoder_flush()` emits the block being filled without ending the frame.
See [Streaming API](../api/streaming.md#flushing) for the general contract.

A zstd frame is a run of blocks, so a flushed block is an ordinary one and
`GCOMP_FLUSH_SYNC` costs only the block boundary it forces early.

### Why a full flush ends the frame

`GCOMP_FLUSH_FULL` is different: **it ends the current frame and starts
another**. The decoder reads every frame in its input by default, so the
result reads back with no option set.

The repeat offsets are frame-level state that the decoder tracks in step with
the encoder (RFC 8878 §3.1.1.3.2.1.1). An encoder that quietly reset them
mid-frame would be describing distances the decoder computes differently — and
that is not hypothetical, it is what the first version of this did, caught six
flushes in by a decoder producing the wrong bytes. Nor is there a cheaper fix:
keeping the repeat offsets and clearing only the match finder still leaves a
sequence free to name a repeat code whose value came from before the flush,
which is exactly what a full flush promises will not happen.

So the unit of recovery in zstd is the frame. libzstd draws the same line: it
offers `ZSTD_e_flush` and `ZSTD_e_end`, and no mid-frame equivalent of zlib's
`Z_FULL_FLUSH`.

Each frame after the first declares no `Frame_Content_Size`: the value the
caller gave describes the whole content, which no single frame holds any more.
The content checksum, when enabled, covers each frame's own content.

### Parallel mode

With `threads.count > 1` a flush submits the job in hand so its bytes go out,
and the next input starts a new job. A flush waits for every block still out
with the workers, which is the point — nothing the caller handed over is left in
flight.

**Both modes mean exactly what they mean single-threaded.** `GCOMP_FLUSH_SYNC`
moves a block seam and leaves the frame open; `GCOMP_FLUSH_FULL` ends the frame
and starts another, for the reasons above, which apply unchanged when the blocks
were produced by workers. A flush is in fact the one place in the parallel path
where a frame *can* be closed, because it has already waited.

Until 2026-09-26 neither was true, and the two defects were opposite:

- `GCOMP_FLUSH_FULL` did nothing a `GCOMP_FLUSH_SYNC` does not. The branch
  returned before the code that ends the frame and never looked at the mode at
  all, under a comment asserting that each job was its own frame with its own
  window — the same belief corrected under [Output format](#output-format),
  which had been fixed there and not here. So the recovery point the mode
  promises was never created.
- `GCOMP_FLUSH_SYNC` dropped history it is supposed to keep. The flush
  allocated the next job without seeding it from the previous one's tail, which
  `update()` does for every job it allocates. Measured on 100 KB written twice
  with a flush between: one thread cost 50% of 2x, four threads cost 100%.

Both are fixed, and the pair is why this section is worth reading twice if you
are relying on either mode. Neither defect was visible in the output — both
streams decoded correctly, and the only instrument that can see either is the
ratio.

## Concatenated frames

Multiple Zstd frames can be concatenated into a single stream. RFC 8878 §3.1: "The decompressed content of multiple concatenated frames is the concatenation of each frame's decompressed content." The decoder does this by default. Set `zstd.concat=false` to stop after the first frame instead:

```c
// Every frame in the input is decoded by default -- no option required.
gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", NULL, &dec);

// Now the decoder will process all concatenated frames,
// writing their decompressed output contiguously.
```

**Important behaviors:**
- Each frame's checksum is validated independently
- Output is continuous across frames (no separation markers)
- Limits apply to total output across all frames
- If any frame fails validation, the entire decode fails

## Error handling

### Error codes

| Code | Meaning |
|------|---------|
| `GCOMP_ERR_CORRUPT` | Invalid magic, bad checksums, malformed blocks, invalid sequences |
| `GCOMP_ERR_LIMIT` | Output/window/memory/expansion limits exceeded |
| `GCOMP_ERR_MEMORY` | Failed to allocate encoder/decoder state or buffers |
| `GCOMP_ERR_UNSUPPORTED` | Unsupported feature (e.g., dictionary required) |

### Error details

When decoding fails, call `gcomp_decoder_get_error_detail()` for diagnostic information:

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    printf("Error: %s\n", gcomp_status_to_string(status));
    printf("Detail: %s\n", gcomp_decoder_get_error_detail(decoder));
}
```

**Example error details:**
- `"invalid zstd magic number"` - Not a Zstd frame
- `"reserved bit set in frame header"` - Corrupted header
- `"reserved block type"` - Invalid block type
- `"invalid match offset"` - Sequence references before window start
- `"content checksum mismatch"` - Data corruption detected
- `"output size exceeds limit"` - Output limit reached
- `"expansion ratio exceeds limit"` - Decompression bomb detected

## Streaming usage

### Encoding

```c
gcomp_encoder_t *enc = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Configure options
gcomp_options_set_int64(opts, "zstd.level", 5);
gcomp_options_set_bool(opts, "zstd.checksum", 1);

gcomp_encoder_create(registry, "zstd", opts, &enc);

gcomp_buffer_t in_buf = { input_data, input_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Feed input (can be called multiple times)
gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
if (s != GCOMP_OK) { /* handle error */ }

// Finalize (writes last block and optional checksum)
gcomp_buffer_t finish_buf = { output_array + out_buf.used,
                              output_capacity - out_buf.used, 0 };
s = gcomp_encoder_finish(enc, &finish_buf);

size_t total_compressed = out_buf.used + finish_buf.used;

gcomp_encoder_destroy(enc);
gcomp_options_destroy(opts);
```

### Decoding

```c
gcomp_decoder_t *dec = NULL;
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Concatenated frames decode by default; set this only to stop after the
// first frame.
// gcomp_options_set_bool(opts, "zstd.concat", 0);

gcomp_decoder_create(registry, "zstd", opts, &dec);

gcomp_buffer_t in_buf = { compressed_data, compressed_len, 0 };
gcomp_buffer_t out_buf = { output_array, output_capacity, 0 };

// Process all input
while (in_buf.used < in_buf.size) {
    gcomp_status_t s = gcomp_decoder_update(dec, &in_buf, &out_buf);
    if (s != GCOMP_OK) {
        printf("Error: %s - %s\n", 
               gcomp_status_to_string(s),
               gcomp_decoder_get_error_detail(dec));
        break;
    }
    // Process out_buf.used bytes, reset out_buf for more
}

// Verify stream is complete
gcomp_status_t s = gcomp_decoder_finish(dec, &out_buf);
if (s != GCOMP_OK) { /* stream incomplete or corrupt */ }

gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

## Security considerations

### Decompression bomb protection

The Zstd decoder implements multiple layers of protection against malicious inputs:

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted input
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);  // 100x max
gcomp_options_set_uint64(opts, "limits.max_window_bytes", 4 * 1024 * 1024);  // 4 MB

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "zstd", opts, &dec);
```

### Checksum validation

- **Content checksum**: Validated after all data is decompressed
- **Content size**: Validated at end of frame if present in header

### Memory limits

The decoder tracks memory usage and enforces `limits.max_memory_bytes`. This includes:
- Decoder state structure
- Window buffer (history)
- FSE decoding tables
- Huffman decoding table
- Block buffers

## Entropy coding

Zstd uses two entropy coding methods for efficient data representation:

### FSE (Finite State Entropy)

FSE is an asymmetric numeral system (ANS) variant used for encoding sequences (literal lengths, match lengths, and offsets). It provides near-optimal compression with fast decoding.

**Predefined tables**: For common data patterns, Zstd uses predefined FSE tables for:
- Literal lengths (36 symbols, accuracy log 6)
- Match lengths (53 symbols, accuracy log 6)
- Match offsets (29 symbols, accuracy log 5)

**Custom tables**: For better compression, the encoder can include custom FSE tables in the bitstream.

### Huffman coding

Huffman coding is used for literals (the non-matched bytes in compressed blocks). The encoder automatically selects the best encoding:

| Literals Encoding | When Used |
|-------------------|-----------|
| Raw | Fewer than 32 literal bytes, or when the coded form does not come out smaller |
| Huffman compressed | Anything else - the coded form is written whenever it fits in fewer bytes |

**Encoder behavior:**
1. Count symbol frequencies in the literal bytes
2. Build a Huffman code from those frequencies
3. Write the weights and the bitstream, and measure what they came to
4. Keep that only if `weights + bitstream + 5` is smaller than the literals
   themselves - the 5 being the largest literals header - and fall back to raw
   otherwise. There is no percentage threshold: one byte saved is kept.

**Format requirements (RFC 8878 Section 4.2.1.2):**
- Maximum code length: 11 bits
- **Weight description**: One header byte then weight data.
  - **Header byte ≥ 128**: Direct representation. Number_Of_Symbols = header_byte − 127 (1–128 symbols). Weights follow as 4-bit values packed high nibble first.
  - **Header byte < 128**: FSE-compressed weights. header_byte = compressed size in bytes. Followed by FSE table header and backward FSE bitstream encoding the weight sequence (used when the Huffman table has more than 127 symbols).
- Bitstream written backwards with marker bit for decoder synchronization

**Four-stream mode (RFC 8878 Section 4.2.1.4):**
- When the literals section has **regenerated_size ≥ 1024**, the encoder uses four parallel Huffman streams: a 3×2-byte jump table (little-endian) followed by the concatenated streams. The decoder supports both single-stream and four-stream formats.

**Implementation notes:**
> The 32-byte minimum and the "must come out smaller" test are encoder heuristics in this implementation, not format requirements. The Zstd specification allows encoders complete freedom to choose raw, RLE, or Huffman for any literals section. Other Zstd encoders (e.g., the reference `libzstd`) may use different decision criteria.

**Decoding:** the decoder takes this from the literals header's `Size_Format`
field, not from the size. Formats 1, 2 and 3 are four streams and carry the
jump table; format 0 is one stream. That distinction matters: the reference
encoder picks four streams well below 1024 regenerated bytes - 263 is enough -
and a decoder that keys on the size instead reads the jump table as literal
bits and produces garbage from a perfectly valid frame. This one did, until
`zstd_literals.c` was corrected to follow the field.

## Interoperability

This implementation is fully compatible with:
- Standard `zstd` / `unzstd` command-line tools
- Python's `zstandard` module
- libzstd library
- Any RFC 8878-compliant implementation

Files created by this library can be decompressed by standard tools, and files created by standard tools can be decompressed by this library.

Dictionaries are used on both sides - see [Dictionary compression](#dictionary-compression)
above. The pieces of the format this library **decodes but never writes** are:

- `Treeless_Compressed` literals (type 3), which reuse the previous block's
  Huffman table. Every compressed literals section this encoder writes carries
  its own table.
- FSE `Repeat_Mode` for sequences, which reuses the previous block's
  distribution tables. The encoder chooses per block between the predefined
  tables, RLE, and a table fitted to that block.

Both cost bytes rather than correctness: a stream is valid without them, and
anything that writes them is read correctly here.

## Comparison with other methods

| Feature | Zstd | LZ4 | Gzip |
|---------|------|-----|------|
| Compression ratio | High | Low | Medium |
| Compression speed | Fast | Fastest | Slow |
| Decompression speed | Very Fast | Fastest | Medium |
| Memory usage | Moderate | Higher | Lower |
| Streaming | Yes | Yes | Yes |
| Content checksum | xxHash64 | xxHash32 | CRC32 |
| Compression levels | 1-22 | N/A | 1-9 |

**When to use Zstd:**
- Need high compression ratio with fast decompression
- Variable compression/speed tradeoff is desired
- Modern format with good tooling support is preferred
- Content integrity verification is needed

## See also

- [Streaming API](../api/streaming.md) - General streaming usage patterns
- [Limits](../api/limits.md) - Safety limit configuration
- [Errors](../api/errors.md) - Error handling patterns
- [Auto-Registration](../auto-registration.md) - How methods are registered
