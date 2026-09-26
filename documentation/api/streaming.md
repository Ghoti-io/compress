# Streaming

The Ghoti.io Compress library provides a streaming API for incremental compression and decompression. This allows processing data in chunks without loading entire files into memory.

## Overview

The streaming API consists of:

- **Encoders** (`gcomp_encoder_t`) - compress data incrementally
- **Decoders** (`gcomp_decoder_t`) - decompress data incrementally
- **Buffers** (`gcomp_buffer_t`) - track input/output progress

## Buffer Structure

```c
typedef struct {
    const void *data;   // Pointer to data
    size_t size;        // Size of data in bytes
    size_t used;        // Number of bytes consumed/produced
} gcomp_buffer_t;
```

The `used` field is updated by `update()` calls to track how many bytes were consumed (input) or produced (output).

## Bit-level methods and chunking

Some methods pack data internally at the **bit** level (not byte-aligned on the wire). Examples include:

- **Deflate** (`"deflate"`) — LSB-first bitstream per RFC 1951
- **LZW** (`"lzw"`) — profile-driven LSB (GIF) or MSB (TIFF) bitstream

Practical implications:

- Your `gcomp_buffer_t` objects are always **byte buffers**, and `used` is
  always **byte counts**.
- Encoders/decoders for bit-level methods may retain **partial-byte state**
  internally between calls; this is normal.
- For reliable streaming, call `*_finish()` to complete the stream (flush any
  pending bits / emit end markers) before treating output as complete.

## Encoder Lifecycle

### Creation

```c
gcomp_encoder_t *encoder = NULL;
gcomp_status_t status = gcomp_encoder_create(
    registry,           // Registry (or NULL for default)
    "deflate",          // Method name
    options,            // Options (or NULL for defaults)
    &encoder            // Output pointer
);
```

### Processing

```c
gcomp_buffer_t input = { data, data_len, 0 };
gcomp_buffer_t output = { out_buf, out_capacity, 0 };

// Call update() one or more times
status = gcomp_encoder_update(encoder, &input, &output);
// input.used indicates bytes consumed
// output.used indicates bytes produced
```

### Finishing

```c
gcomp_buffer_t final_output = { out_buf + offset, remaining_capacity, 0 };
status = gcomp_encoder_finish(encoder, &final_output);
// Writes final block, flushes to byte boundary
```

### Flushing

`gcomp_encoder_finish()` ends the stream. When you need the *peer* to see what
you have written so far but the stream is not over — a protocol message, a log
record, a chunk on a socket — use `gcomp_encoder_flush()`:

```c
for (;;) {
    gcomp_buffer_t out = { buf, sizeof(buf), 0 };
    gcomp_status_t s = gcomp_encoder_flush(encoder, &out, GCOMP_FLUSH_SYNC);
    write_out(buf, out.used);
    if (s == GCOMP_OK) break;
    if (s != GCOMP_ERR_LIMIT) return s;   // real error
}
// keep calling update(); end with finish() as usual
```

**The promise:** once flush returns `GCOMP_OK`, a decoder given the bytes
produced so far will produce every byte the encoder has consumed so far.
Without it an encoder may hold data indefinitely, waiting for a full block.

The same `GCOMP_ERR_LIMIT` contract as `finish()` applies: drain and call again
with the same mode until it returns `GCOMP_OK`. Stopping early leaves the peer
without data the encoder has already taken.

#### Modes

| Mode | Keeps compression history? | Use when |
|------|---------------------------|----------|
| `GCOMP_FLUSH_SYNC` | Yes | You want the peer to see the data now. This is the common case, and corresponds to zlib's `Z_SYNC_FLUSH`. |
| `GCOMP_FLUSH_FULL` | No | You want a decoder to be able to resume here after damage earlier in the stream. Costs ratio. Corresponds to zlib's `Z_FULL_FLUSH`. |

#### Cost

A flush ends a block early and pads to a byte boundary, so flushing often
compresses worse than not flushing. Flush at message boundaries, not per
write. Measured on 300 KB of mixed text, run and noise:

| flush every | deflate | zstd | lz4 | lzw | rle |
|---|---|---|---|---|---|
| 60 KB | 113,576 | 116,154 | 140,354 | 155,172 | 203,339 |
| 1 KB  | 117,989 | 120,895 | 150,306 | 160,563 | 203,382 |

On 4 KB of the same data, flushing after *every byte* produces 28,002 bytes
with deflate — seven times the input. The API will let you do it; do not.

#### What each method does

| Method | Sync flush | Full flush |
|---|---|---|
| **deflate**, **gzip**, **zlib** | Ends the block, writes the empty stored block (`00 00 FF FF`) that byte-aligns the output — zlib's `Z_SYNC_FLUSH` tail | Also empties the match history, so nothing afterwards reaches back |
| **lz4** | Closes the block being filled; an ordinary LZ4 block, so the frame is unchanged | Identical with independent blocks (the default), which carry no history anyway. With `lz4.independent_blocks=false` it also drops the 64 KB window |
| **zstd** | Emits the block being filled; the frame stays open | **Ends the frame and starts another.** The decoder reads every frame by default (RFC 8878 §3.1), so the result reads back without any option |
| **lzw** | Emits the pending code followed by a `CLEAR`, which is the only way to push a partial code out where a decoder can read it | Identical — the `CLEAR` already resets the dictionary |
| **rle** | Emits the pending run and literal packet | Identical — RLE keeps no history |

Two consequences worth knowing before you rely on them:

- **zstd's full flush ends the frame**, at every `threads.count`. Repeat
  offsets are frame-level state that the decoder tracks in step with the
  encoder, so an encoder cannot reset them mid-frame; the unit of recovery in
  zstd is the frame. libzstd draws the same line — it offers `ZSTD_e_flush` and
  `ZSTD_e_end` and nothing in between. Such a stream decodes fully by default.
  Before 2026-09-26 the parallel encoder did none of this and `FULL` was
  silently a `SYNC`; see [zstd](../modules/zstd.md#parallel-mode).
- **LZW's flush is not byte-aligned.** Codes are 9–12 bits and the partial byte
  stays in the encoder to be continued. The data promise still holds; the
  byte-boundary one does not. If you need aligned boundaries, use a framed
  format.

#### Minimum output space

Every method but RLE can hand a flush out a byte at a time. A PackBits literal
packet is a length byte plus up to 128 bytes written as a unit, so RLE needs
**129 bytes** of free output space to flush; given less it returns
`GCOMP_ERR_LIMIT` with an error detail saying so, rather than looping.

#### Notes

- Flushing with nothing buffered writes nothing (beyond any stream header not
  yet emitted) and returns `GCOMP_OK`. It is not an error.
- Flushing after `finish()` has completed the stream is an error.
- Every method in this library supports flush. A method that did not would
  return `GCOMP_ERR_UNSUPPORTED`, and would do so before writing anything.

### Reset (Optional)

For compressing multiple independent streams with the same options, reuse the encoder instead of destroying and recreating:

```c
// First compression...
gcomp_encoder_finish(encoder, &output);

// Reset for next stream (more efficient than destroy/create)
status = gcomp_encoder_reset(encoder);
if (status == GCOMP_ERR_UNSUPPORTED) {
    // Method doesn't support reset - must destroy and recreate
}

// Second compression with same options...
```

### Cleanup

```c
gcomp_encoder_destroy(encoder);
```

## Decoder Lifecycle

Decoders follow the same pattern:

```c
gcomp_decoder_t *decoder = NULL;
gcomp_decoder_create(registry, "deflate", options, &decoder);

gcomp_buffer_t input = { compressed, compressed_len, 0 };
gcomp_buffer_t output = { out_buf, out_capacity, 0 };

while (/* more data */) {
    status = gcomp_decoder_update(decoder, &input, &output);
    if (status != GCOMP_OK) break;
    // Process output.used bytes, advance buffers
}

status = gcomp_decoder_finish(decoder, &output);

// Optional: reset for another stream
status = gcomp_decoder_reset(decoder);

gcomp_decoder_destroy(decoder);
```

## Error Handling

### Status Codes

All streaming functions return `gcomp_status_t`:

| Code | Meaning |
|------|---------|
| `GCOMP_OK` | Operation succeeded |
| `GCOMP_ERR_INVALID_ARG` | NULL pointer or invalid argument |
| `GCOMP_ERR_MEMORY` | Memory allocation failed |
| `GCOMP_ERR_LIMIT` | Resource limit exceeded (e.g., max output bytes) |
| `GCOMP_ERR_CORRUPT` | Corrupted or invalid input data |
| `GCOMP_ERR_UNSUPPORTED` | Method not found or operation not supported |
| `GCOMP_ERR_INTERNAL` | Internal library error (bug) |
| `GCOMP_ERR_IO` | I/O error (callback API only) |

### Error Detail API

When an error occurs, encoders and decoders store detailed error information that can be queried for debugging:

```c
// Get the last error status
gcomp_status_t gcomp_encoder_get_error(const gcomp_encoder_t *encoder);
gcomp_status_t gcomp_decoder_get_error(const gcomp_decoder_t *decoder);

// Get detailed error message (human-readable)
const char *gcomp_encoder_get_error_detail(const gcomp_encoder_t *encoder);
const char *gcomp_decoder_get_error_detail(const gcomp_decoder_t *decoder);
```

**Example: Checking error details**

```c
gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
if (status != GCOMP_OK) {
    fprintf(stderr, "Decode failed: %s\n", gcomp_status_to_string(status));
    fprintf(stderr, "Details: %s\n", gcomp_decoder_get_error_detail(decoder));
}
```

**Example output:**

```
Decode failed: GCOMP_ERR_CORRUPT
Details: corrupt deflate stream at stage 'huffman_data' (output: 1024 bytes)
```

The error detail string includes:
- The decode stage where the error occurred
- Bytes processed so far (for debugging truncated/corrupt streams)
- Specific error context when available

**Notes:**
- The returned string pointer is valid until the encoder/decoder is destroyed or another operation is performed
- Returns empty string `""` if no error has occurred or if the pointer is NULL
- Error details are set by method implementations (e.g., deflate decoder)

## Callback-Based Streaming

For scenarios where you want the library to drive I/O, use the callback API:

```c
// Callback function types
typedef gcomp_status_t (*gcomp_read_cb)(
    void *ctx,          // User context
    uint8_t *dst,       // Buffer to read into
    size_t cap,         // Buffer capacity
    size_t *out_n       // Output: bytes actually read (0 = EOF)
);

typedef gcomp_status_t (*gcomp_write_cb)(
    void *ctx,          // User context
    const uint8_t *src, // Data to write
    size_t n,           // Number of bytes
    size_t *out_n       // Output: bytes actually written
);
```

**Convenience functions:**

```c
// Encode using callbacks
gcomp_status_t gcomp_encode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb, void *read_ctx,
    gcomp_write_cb write_cb, void *write_ctx
);

// Decode using callbacks
gcomp_status_t gcomp_decode_stream_cb(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    gcomp_read_cb read_cb, void *read_ctx,
    gcomp_write_cb write_cb, void *write_ctx
);
```

### Callback Semantics

#### Read Callback

The read callback is called when the library needs more input data.

**Contract:**
- `ctx`: Your user-provided context pointer (e.g., file handle, socket, buffer position)
- `dst`: Buffer to read data into (library-owned, valid for the duration of the call)
- `cap`: Maximum bytes to read (always > 0)
- `out_n`: Must be set to the number of bytes actually read

**Return values:**
- `GCOMP_OK`: Read succeeded. Check `*out_n` for bytes read.
- `GCOMP_ERR_IO`: I/O error occurred (e.g., file read error, network error)
- Other error codes: Treated as fatal errors

**EOF handling:**
- Return `GCOMP_OK` with `*out_n = 0` to indicate end-of-file
- Once EOF is signaled, the callback will not be called again

**Partial reads:**
- Returning fewer bytes than `cap` is always valid (the library will call again if needed)
- This allows for non-blocking I/O or reading from slow sources

#### Write Callback

The write callback is called when the library has output data to write.

**Contract:**
- `ctx`: Your user-provided context pointer
- `src`: Data to write (library-owned, valid for the duration of the call)
- `n`: Number of bytes to write (always > 0)
- `out_n`: Must be set to the number of bytes actually written

**Return values:**
- `GCOMP_OK`: Write succeeded. Check `*out_n` for bytes written.
- `GCOMP_ERR_IO`: I/O error occurred
- Other error codes: Treated as fatal errors

**Partial writes:**
- Returning fewer bytes than `n` is valid (the library retries with remaining data)
- Returning `*out_n = 0` with `GCOMP_OK` is treated as `GCOMP_ERR_IO` (no progress)

**Backpressure:**
- The library automatically handles partial writes by retrying
- All output is eventually written before returning to the caller

### Error Propagation

Errors from callbacks are propagated directly to the caller:

```c
gcomp_status_t my_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    // Return GCOMP_ERR_IO on failure
    if (read_failed) {
        return GCOMP_ERR_IO;
    }
    // ...
}

// This error propagates to the caller
gcomp_status_t status = gcomp_encode_stream_cb(..., my_read, ...);
// status == GCOMP_ERR_IO if my_read returned GCOMP_ERR_IO
```

**Cleanup on error:**
- The library always cleans up internal resources (encoder/decoder, buffers) on error
- You are responsible for cleaning up your callback context

### Example: File-Based Compression

```c
typedef struct {
    FILE *file;
} file_ctx_t;

gcomp_status_t read_file(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    file_ctx_t *fc = (file_ctx_t *)ctx;
    *out_n = fread(dst, 1, cap, fc->file);
    return ferror(fc->file) ? GCOMP_ERR_IO : GCOMP_OK;
}

gcomp_status_t write_file(void *ctx, const uint8_t *src, size_t n, size_t *out_n) {
    file_ctx_t *fc = (file_ctx_t *)ctx;
    *out_n = fwrite(src, 1, n, fc->file);
    return (*out_n < n && ferror(fc->file)) ? GCOMP_ERR_IO : GCOMP_OK;
}

// Usage
file_ctx_t in_ctx = { fopen("input.txt", "rb") };
file_ctx_t out_ctx = { fopen("output.deflate", "wb") };

gcomp_status_t status = gcomp_encode_stream_cb(NULL, "deflate", NULL,
    read_file, &in_ctx, write_file, &out_ctx);

fclose(in_ctx.file);
fclose(out_ctx.file);

if (status != GCOMP_OK) {
    fprintf(stderr, "Compression failed: %s\n", gcomp_status_to_string(status));
}
```

### Example: Memory Buffer with Partial Reads

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    size_t chunk_size;  // Simulate slow reads
} chunked_reader_t;

gcomp_status_t read_chunked(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    chunked_reader_t *r = (chunked_reader_t *)ctx;
    
    if (r->pos >= r->size) {
        *out_n = 0;  // EOF
        return GCOMP_OK;
    }
    
    // Read at most chunk_size bytes at a time
    size_t remaining = r->size - r->pos;
    size_t to_read = (remaining < r->chunk_size) ? remaining : r->chunk_size;
    if (to_read > cap) to_read = cap;
    
    memcpy(dst, r->data + r->pos, to_read);
    r->pos += to_read;
    *out_n = to_read;
    return GCOMP_OK;
}
```

### Internal Buffer Size

The callback API uses internal 64 KiB buffers for reading and writing. This means:
- Your read callback will be asked to read up to 64 KiB at a time
- Your write callback will receive up to 64 KiB at a time

This is an implementation detail and may change in future versions.

## Buffer Convenience Functions

For simple cases where you have all data in memory:

```c
// Encode entire buffer
gcomp_status_t gcomp_encode_buffer(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    const void *input_data, size_t input_size,
    void *output_data, size_t output_capacity,
    size_t *output_size_out
);

// Decode entire buffer
gcomp_status_t gcomp_decode_buffer(
    gcomp_registry_t *registry,
    const char *method_name,
    gcomp_options_t *options,
    const void *input_data, size_t input_size,
    void *output_data, size_t output_capacity,
    size_t *output_size_out
);
```

**Example:**

```c
uint8_t compressed[4096];
size_t compressed_size;

gcomp_status_t status = gcomp_encode_buffer(
    NULL, "deflate", NULL,
    input_data, input_size,
    compressed, sizeof(compressed),
    &compressed_size
);

if (status == GCOMP_ERR_LIMIT) {
    // Output buffer too small
}
```

## Thread Safety

- **Encoders/decoders are NOT thread-safe**: Each encoder/decoder should only be used by one thread at a time.
- **Registries**: The default registry is thread-safe for lookups after initialization. Custom registries should not be modified while in use.
- **Options**: Once frozen with `gcomp_options_freeze()`, options objects are safe to share across threads.

## Resource Limits

The library provides several safety limits to protect against resource exhaustion from malicious or malformed inputs:

| Option | Default | Description |
|--------|---------|-------------|
| `limits.max_output_bytes` | 512 MiB | Maximum decompressed output size |
| `limits.max_memory_bytes` | 256 MiB | Maximum working memory for decoder |
| `limits.max_expansion_ratio` | the format's own ceiling | Maximum output/input byte ratio; see [Limits](limits.md) |

### Expansion ratio protection

The `limits.max_expansion_ratio` option protects against "decompression bombs" - maliciously crafted archives that decompress to massive outputs from tiny inputs (e.g., a 42 KB file that expands to 4.5 PB).

The ratio is calculated as `output_bytes / input_bytes`. With the default limit of 1000:
- 1 KB input → max 1 MB output
- 1 MB input → max 1 GB output

**Example: Setting limits for untrusted input**

```c
gcomp_options_t *opts = NULL;
gcomp_options_create(&opts);

// Strict limits for untrusted data
gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);  // 10 MB
gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);            // 100x max

gcomp_decoder_t *dec = NULL;
gcomp_decoder_create(registry, "deflate", opts, &dec);
// ... use decoder ...
gcomp_decoder_destroy(dec);
gcomp_options_destroy(opts);
```

When a limit is exceeded, the decoder returns `GCOMP_ERR_LIMIT`. Call `gcomp_decoder_get_error_detail()` to get diagnostic information about which limit was hit.

## Best Practices

1. **Check all return values** - Every API call can fail
2. **Use error details for debugging** - Call `gcomp_*_get_error_detail()` after failures
3. **Pre-allocate output buffers** - For encoding, output is typically slightly larger than input; for decoding, you need to know or estimate the decompressed size
4. **Set limits for untrusted input** - Use `limits.max_output_bytes` and `limits.max_expansion_ratio` to prevent decompression bombs
5. **Reuse encoders/decoders** - Call `gcomp_*_reset()` between streams instead of destroy/create for better performance
6. **Destroy after use** - Always call `gcomp_*_destroy()` to free resources

---

## Advanced Streaming Patterns

### Complex Streaming: Multiple Blocks and Interrupts

The streaming API is designed to handle interrupts at any point. When `update()` returns `GCOMP_OK`, it may have consumed only part of the input or produced only part of the output. The caller must track progress and resume:

```c
// Process data that may arrive in unpredictable chunks (e.g., network)
gcomp_status_t process_with_interrupts(gcomp_decoder_t *decoder,
    const uint8_t *compressed, size_t compressed_size,
    uint8_t *output, size_t output_capacity, size_t *total_out) {
    
    size_t input_pos = 0;
    size_t output_pos = 0;
    gcomp_status_t status;
    
    while (input_pos < compressed_size || output_pos < output_capacity) {
        // Set up buffers for this iteration
        gcomp_buffer_t in_buf = {
            .data = compressed + input_pos,
            .size = compressed_size - input_pos,
            .used = 0
        };
        gcomp_buffer_t out_buf = {
            .data = output + output_pos,
            .size = output_capacity - output_pos,
            .used = 0
        };
        
        status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
        if (status != GCOMP_OK) {
            return status;  // Error or limit exceeded
        }
        
        input_pos += in_buf.used;
        output_pos += out_buf.used;
        
        // Check for completion: all input consumed and no output produced
        if (in_buf.used == 0 && out_buf.used == 0) {
            break;  // Decoder has no more buffered data
        }
    }
    
    *total_out = output_pos;
    return GCOMP_OK;
}
```

**Key points for interrupted streams:**

- Always check both `input.used` and `output.used` after each call
- The decoder may buffer data internally; drain it by calling `update()` with empty input
- Deflate blocks can span multiple `update()` calls transparently
- The encoder may hold data until enough accumulates for efficient compression

### Backpressure Handling

Backpressure occurs when the output consumer (e.g., network, disk) is slower than compression/decompression. The streaming API handles this naturally:

```c
// Compress with backpressure - writer may accept fewer bytes than offered
gcomp_status_t compress_with_backpressure(gcomp_encoder_t *encoder,
    const uint8_t *input, size_t input_size,
    gcomp_write_cb slow_writer, void *writer_ctx) {
    
    size_t input_pos = 0;
    uint8_t buffer[4096];  // Small buffer for backpressure scenarios
    gcomp_status_t status;
    
    while (input_pos < input_size) {
        gcomp_buffer_t in_buf = {
            .data = input + input_pos,
            .size = input_size - input_pos,
            .used = 0
        };
        gcomp_buffer_t out_buf = {
            .data = buffer,
            .size = sizeof(buffer),
            .used = 0
        };
        
        status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
        if (status != GCOMP_OK) return status;
        
        input_pos += in_buf.used;
        
        // Write compressed data, handling partial writes
        size_t write_pos = 0;
        while (write_pos < out_buf.used) {
            size_t written = 0;
            status = slow_writer(writer_ctx, buffer + write_pos,
                                 out_buf.used - write_pos, &written);
            if (status != GCOMP_OK) return status;
            if (written == 0) return GCOMP_ERR_IO;  // No progress
            write_pos += written;
        }
    }
    
    // Finish and flush remaining data
    gcomp_buffer_t out_buf = { .data = buffer, .size = sizeof(buffer), .used = 0 };
    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) return status;
    
    // Write final bytes
    size_t write_pos = 0;
    while (write_pos < out_buf.used) {
        size_t written = 0;
        status = slow_writer(writer_ctx, buffer + write_pos,
                             out_buf.used - write_pos, &written);
        if (status != GCOMP_OK) return status;
        if (written == 0) return GCOMP_ERR_IO;
        write_pos += written;
    }
    
    return GCOMP_OK;
}
```

### Performance Considerations for Chunk Sizes

Chunk size significantly affects both throughput and memory usage:

| Chunk Size | Throughput | Memory | Latency | Use Case |
|------------|------------|--------|---------|----------|
| 4 KiB | Lower | Minimal | Low | Memory-constrained, interactive |
| 16-64 KiB | Good | Low | Low-Medium | General purpose |
| 256 KiB - 1 MiB | Optimal | Medium | Medium | Bulk file processing |
| > 1 MiB | Marginal gain | High | High | Large file archival |

**Recommendations:**

- **Default:** Use 64 KiB chunks for a good balance
- **Memory-constrained:** Use 4-16 KiB chunks
- **Maximum throughput:** Use 256 KiB - 1 MiB chunks
- **Network streaming:** Match chunk size to typical packet/message size
- **Interactive:** Use small chunks (4-16 KiB) for responsive progress

```c
// Benchmark different chunk sizes
#define CHUNK_SMALL   (4 * 1024)      // 4 KiB - memory constrained
#define CHUNK_DEFAULT (64 * 1024)     // 64 KiB - general purpose
#define CHUNK_LARGE   (256 * 1024)    // 256 KiB - throughput optimized
```

### Example: Streaming to/from Network Sockets

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

typedef struct {
    int fd;
} socket_ctx_t;

// Non-blocking socket read callback
gcomp_status_t socket_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    socket_ctx_t *sock = (socket_ctx_t *)ctx;
    
    ssize_t n = recv(sock->fd, dst, cap, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *out_n = 0;  // No data available, but not an error
            return GCOMP_OK;
        }
        return GCOMP_ERR_IO;
    }
    if (n == 0) {
        *out_n = 0;  // EOF - connection closed
        return GCOMP_OK;
    }
    
    *out_n = (size_t)n;
    return GCOMP_OK;
}

// Non-blocking socket write callback
gcomp_status_t socket_write(void *ctx, const uint8_t *src, size_t n, size_t *out_n) {
    socket_ctx_t *sock = (socket_ctx_t *)ctx;
    
    ssize_t written = send(sock->fd, src, n, MSG_DONTWAIT);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *out_n = 0;  // Would block, try again later
            return GCOMP_OK;
        }
        return GCOMP_ERR_IO;
    }
    
    *out_n = (size_t)written;
    return GCOMP_OK;
}

// Decompress from one socket to another
gcomp_status_t decompress_socket_to_socket(int in_fd, int out_fd) {
    socket_ctx_t in_ctx = { .fd = in_fd };
    socket_ctx_t out_ctx = { .fd = out_fd };
    
    gcomp_options_t *opts = NULL;
    gcomp_options_create(&opts);
    
    // Set strict limits for network data
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100 * 1024 * 1024);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);
    
    gcomp_status_t status = gcomp_decode_stream_cb(
        NULL, "deflate", opts,
        socket_read, &in_ctx,
        socket_write, &out_ctx
    );
    
    gcomp_options_destroy(opts);
    return status;
}
```

### Example: Streaming with Progress Reporting

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    void (*progress_cb)(size_t current, size_t total, void *user_data);
    void *user_data;
    size_t last_report_pos;
    size_t report_interval;  // Report every N bytes
} progress_reader_t;

gcomp_status_t progress_read(void *ctx, uint8_t *dst, size_t cap, size_t *out_n) {
    progress_reader_t *r = (progress_reader_t *)ctx;
    
    if (r->pos >= r->size) {
        *out_n = 0;  // EOF
        return GCOMP_OK;
    }
    
    size_t remaining = r->size - r->pos;
    size_t to_read = (remaining < cap) ? remaining : cap;
    
    memcpy(dst, r->data + r->pos, to_read);
    r->pos += to_read;
    *out_n = to_read;
    
    // Report progress at intervals
    if (r->progress_cb && (r->pos - r->last_report_pos) >= r->report_interval) {
        r->progress_cb(r->pos, r->size, r->user_data);
        r->last_report_pos = r->pos;
    }
    
    return GCOMP_OK;
}

// Usage example
void my_progress(size_t current, size_t total, void *user_data) {
    int percent = (int)((current * 100) / total);
    printf("\rCompressing: %d%% (%zu / %zu bytes)", percent, current, total);
    fflush(stdout);
}

gcomp_status_t compress_with_progress(const uint8_t *input, size_t input_size,
    uint8_t *output, size_t output_capacity, size_t *output_size) {
    
    progress_reader_t reader = {
        .data = input,
        .size = input_size,
        .pos = 0,
        .progress_cb = my_progress,
        .user_data = NULL,
        .last_report_pos = 0,
        .report_interval = 1024 * 1024  // Report every 1 MiB
    };
    
    typedef struct {
        uint8_t *data;
        size_t capacity;
        size_t pos;
    } buffer_writer_t;
    
    buffer_writer_t writer = {
        .data = output,
        .capacity = output_capacity,
        .pos = 0
    };
    
    // Write callback that accumulates into buffer
    gcomp_status_t buffer_write(void *ctx, const uint8_t *src, size_t n,
                                 size_t *out_n) {
        buffer_writer_t *w = (buffer_writer_t *)ctx;
        size_t remaining = w->capacity - w->pos;
        size_t to_write = (n < remaining) ? n : remaining;
        if (to_write == 0) {
            *out_n = 0;
            return GCOMP_OK;  // Buffer full
        }
        memcpy(w->data + w->pos, src, to_write);
        w->pos += to_write;
        *out_n = to_write;
        return GCOMP_OK;
    }
    
    gcomp_status_t status = gcomp_encode_stream_cb(
        NULL, "deflate", NULL,
        progress_read, &reader,
        buffer_write, &writer
    );
    
    printf("\n");  // Newline after progress
    *output_size = writer.pos;
    return status;
}
```

### Example: Cancellable Streaming

For long-running operations, you may want to support cancellation:

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    volatile int *cancel_flag;  // Set by another thread to request cancellation
} cancellable_reader_t;

gcomp_status_t cancellable_read(void *ctx, uint8_t *dst, size_t cap,
                                 size_t *out_n) {
    cancellable_reader_t *r = (cancellable_reader_t *)ctx;
    
    // Check cancellation flag
    if (r->cancel_flag && *r->cancel_flag) {
        return GCOMP_ERR_IO;  // Use IO error to signal cancellation
    }
    
    if (r->pos >= r->size) {
        *out_n = 0;
        return GCOMP_OK;
    }
    
    size_t remaining = r->size - r->pos;
    size_t to_read = (remaining < cap) ? remaining : cap;
    
    memcpy(dst, r->data + r->pos, to_read);
    r->pos += to_read;
    *out_n = to_read;
    
    return GCOMP_OK;
}
```

### Multiple Independent Streams with Reset

For applications that process many independent streams (e.g., HTTP compression, database storage), reuse encoders/decoders with `reset()`:

```c
// Process multiple independent streams efficiently
gcomp_status_t process_many_streams(stream_t *streams, size_t count) {
    gcomp_encoder_t *encoder = NULL;
    gcomp_status_t status = gcomp_encoder_create(NULL, "deflate", NULL, &encoder);
    if (status != GCOMP_OK) return status;
    
    for (size_t i = 0; i < count; i++) {
        // Compress stream[i]
        status = compress_one_stream(encoder, &streams[i]);
        if (status != GCOMP_OK) {
            gcomp_encoder_destroy(encoder);
            return status;
        }
        
        // Reset for next stream (faster than destroy/create)
        status = gcomp_encoder_reset(encoder);
        if (status == GCOMP_ERR_UNSUPPORTED) {
            // Method doesn't support reset; must recreate
            gcomp_encoder_destroy(encoder);
            status = gcomp_encoder_create(NULL, "deflate", NULL, &encoder);
            if (status != GCOMP_OK) return status;
        } else if (status != GCOMP_OK) {
            gcomp_encoder_destroy(encoder);
            return status;
        }
    }
    
    gcomp_encoder_destroy(encoder);
    return GCOMP_OK;
}
```
