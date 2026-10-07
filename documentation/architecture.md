# Architecture

This document provides a high-level overview of the Ghoti.io Compress library architecture for contributors and maintainers.

## Component Overview

The library is organized into the following major components:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           User Application                              │
├─────────────────────────────────────────────────────────────────────────┤
│                          Public API Layer                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │   compress.h │  │   stream.h   │  │  registry.h  │  │  options.h  │  │
│  │  (Buffers)   │  │  (Streaming) │  │  (Methods)   │  │  (Config)   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────────────────────────┤
│                           Core Layer                                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │  allocator.c │  │  registry.c  │  │   stream.c   │  │  options.c  │  │
│  │  limits.c    │  │   errors.c   │  │  stream_cb.c │  │  buffer.c   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────────────────────────┤
│                         Method Layer                                    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                        deflate/                                 │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │ deflate_encode.c │  │ deflate_decode.c │  │   huffman.c   │  │    │
│  │  │   bitwriter.c    │  │   bitreader.c    │  │  (RFC 1951)   │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                          gzip/                                  │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │  gzip_encoder.c  │  │  gzip_decoder.c  │  │ gzip_format.c │  │    │
│  │  │                  │  │                  │  │  (RFC 1952)   │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                           lz4/                                  │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │  lz4_encoder.c   │  │  lz4_decoder.c   │  │  lz4_block.c  │  │    │
│  │  │  lz4_parallel.c  │  │                  │  │  lz4_frame.c  │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                           zstd/                                 │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │ zstd_encoder.c   │  │ zstd_decoder.c   │  │ zstd_huf.c    │  │    │
│  │  │ zstd_parallel.c  │  │                  │  │ zstd_fse.c    │  │    │
│  │  │                  │  │                  │  │ zstd_*.c      │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                            rle/                                 │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │  rle_encoder.c   │  │  rle_decoder.c   │  │ rle_profile.c │  │    │
│  │  │  rle_register.c  │  │  rle_core.c      │  │ rle_internal  │  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │                            lzw/                                 │    │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │  lzw_encoder.c   │  │  lzw_decoder.c   │  │ lzw_core.c    │  │    │
│  │  │  lzw_hash.c      │  │  lzw_bitio.c     │  │ lzw_register.c│  │    │
│  │  └──────────────────┘  └──────────────────┘  └───────────────┘  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | Header | Purpose |
|-----------|--------|---------|
| **Registry** | `registry.h` | Method lookup and storage |
| **Stream** | `stream.h` | Encoder/decoder lifecycle and streaming API |
| **Options** | `options.h` | Configuration key/value system |
| **Allocator** | `allocator.h` | Memory allocation abstraction |
| **Limits** | `limits.h` | Safety limits and memory tracking |
| **Errors** | `errors.h` | Status codes and error handling |
| **Method** | `method.h` | Method vtable interface |

### Public API

Public headers and main entry points (all under `include/ghoti.io/compress/`):

| Header | Purpose |
|--------|---------|
| `compress.h` | Buffer-to-buffer encode/decode convenience API; version |
| `stream.h` | Streaming encoder/decoder lifecycle, update/finish, callbacks |
| `registry.h` | Method registry create/destroy, lookup, registration |
| `options.h` | Configuration key/value system, schema |
| `limits.h` | Safety limits, memory tracking, expansion ratio |
| `errors.h` | Status codes and string conversion |
| `allocator.h` | Memory allocation abstraction (malloc/free/calloc) |
| `deflate.h` | Deflate-specific API (options, helpers) |
| `gzip.h` | Gzip-specific API (options, helpers) |
| `lz4.h` | LZ4-specific API (options, helpers) |
| `lzw.h` | LZW-specific API (registration, format options) |
| `zstd.h` | Zstd-specific API (options, helpers) |
| `rle.h` | RLE-specific API (registration, format options) |
| `crc32.h` | CRC-32 computation (gzip) |
| `xxhash32.h` | XXH32 hash (LZ4 content checksum) |
| `xxhash64.h` | XXH64 hash (zstd checksum) |
| `method.h` | Method vtable interface (for custom methods) |
| `macros.h` | Cross-compiler macros (e.g. GCOMP_API) |
| `namespace.h` | Renames every symbol to its `ghotiio_compress_<branch>_` form |
| `libver.h` | Version macros, generated at build time into `libver_gen.h` |

Parallel encoding needs no header of its own here:  it is built on cutil's
`GCU_Pool` and `GCU_Sequencer`, driven by the private `src/core/parallel_block.c`.

Every header listed here is installed, `namespace.h` and `libver.h` included -
`make install` copies the whole of `include/ghoti.io/` plus the generated
headers, so anything in the tree above is part of what a consumer receives.

### Internal-Only Headers

The following are used only inside the library and are **not** part of the public API. Do not include them from application code; they may change or be removed without notice.

- **Core internal:** `endian.h`, `alloc_internal.h`, `registry_internal.h`, `stream_internal.h`, and other `*_internal.h` under `src/core/`. Overflow-checked size arithmetic comes from cutil (`<ghoti.io/cutil/safemath.h>`); it used to be a local `safe_math.h` carrying the same functions.
- **Method internal:** Each method has an `*_internal.h` (e.g. `deflate_internal.h`, `gzip_internal.h`, `lz4_internal.h`, `zstd_internal.h`, `rle_internal.h`, `lzw_internal.h`, `brotli_internal.h`, `lzma_internal.h`) for shared state and helpers within that method only. Some methods also use internal-only modules (e.g. `lzw_hash.h` / `lzw_hash.c`).

### Method Layer

Each compression method (deflate, gzip, zlib, LZ4, LZW, RLE, zstd, brotli, lzma, lzma2) implements the `gcomp_method_t` interface:

```c
struct gcomp_method_s {
    uint32_t abi_version;                    // Forward compatibility
    size_t size;                             // Structure size
    const char *name;                        // Method name (e.g., "deflate")
    gcomp_capabilities_t capabilities;       // ENCODE, DECODE, or both
    
    // Encoder/decoder factory functions
    gcomp_status_t (*create_encoder)(...);
    gcomp_status_t (*create_decoder)(...);
    void (*destroy_encoder)(...);
    void (*destroy_decoder)(...);
    
    // Option schema (for introspection)
    const gcomp_method_schema_t *(*get_schema)(void);
};
```

## Data Flow

### Encoding Flow

```
                        ┌─────────────────┐
                        │   Application   │
                        └────────┬────────┘
                                 │ Input data
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_create()                          │
│  1. Look up method in registry                                         │
│  2. Allocate encoder structure                                         │
│  3. Call method->create_encoder() to initialize state                  │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_update()                          │
│  ┌─────────────┐    ┌─────────────────────────┐    ┌────────────────┐  │
│  │ Input buffer│ ─► │ Method-specific encoder │ ─► │ Output buffer  │  │
│  │  (raw data) │    │  (LZ77, Huffman, etc.)  │    │ (compressed)   │  │
│  └─────────────┘    └─────────────────────────┘    └────────────────┘  │
│  • input.used = bytes consumed from input                              │
│  • output.used = bytes produced to output                              │
│  • May be called multiple times for streaming                          │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_encoder_finish()                          │
│  • Flushes any buffered data                                           │
│  • Writes final block marker                                           │
│  • Flushes to byte boundary                                            │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                       gcomp_encoder_destroy()                          │
│  1. Call method->destroy_encoder() to clean up state                   │
│  2. Free encoder structure                                             │
└────────────────────────────────────────────────────────────────────────┘
```

### Decoding Flow

```
                        ┌─────────────────┐
                        │   Application   │
                        └────────┬────────┘
                                 │ Compressed data
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_create()                          │
│  1. Look up method in registry                                         │
│  2. Allocate decoder structure                                         │
│  3. Call method->create_decoder() to initialize state                  │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_update()                          │
│  ┌────────────────┐    ┌────────────────────┐    ┌─────────────────┐   │
│  │  Input buffer  │ ─► │ Method-specific    │ ─► │  Output buffer  │   │
│  │  (compressed)  │    │ decoder (Huffman,  │    │   (raw data)    │   │
│  │                │    │ LZ77 window, etc.) │    │                 │   │
│  └────────────────┘    └────────────────────┘    └─────────────────┘   │
│  • Checks limits: max_output_bytes, max_expansion_ratio                │
│  • Returns GCOMP_ERR_LIMIT if exceeded                                 │
│  • Returns GCOMP_ERR_CORRUPT on invalid data                           │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        gcomp_decoder_finish()                          │
│  • Validates stream is complete (final block seen)                     │
│  • Drains any remaining output                                         │
│  • Returns GCOMP_ERR_CORRUPT if stream is truncated                    │
└────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                       gcomp_decoder_destroy()                          │
│  1. Call method->destroy_decoder() to clean up state                   │
│  2. Free decoder structure                                             │
└────────────────────────────────────────────────────────────────────────┘
```

## Memory Ownership Model

### Ownership Rules

| Object | Owner | Lifetime |
|--------|-------|----------|
| `gcomp_registry_t` | Caller (or library for default) | Until `destroy()` or program exit |
| `gcomp_encoder_t` | Caller | Until `destroy()` |
| `gcomp_decoder_t` | Caller | Until `destroy()` |
| `gcomp_options_t` | Caller | Until `destroy()` (can outlive encoder/decoder) |
| Input buffer data | Caller | Must remain valid during `update()` call |
| Output buffer data | Caller | Must remain valid during `update()` call |
| Method state | Method (via method_state pointer) | Managed by method's create/destroy |

### Allocator Hierarchy

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      Registry Allocator                                 │
│  • Set via gcomp_registry_create(allocator, ...)                        │
│  • Default registry uses gcomp_allocator_default() (stdlib malloc)      │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      Encoder/Decoder Allocator                          │
│  • Inherits from registry that created it                               │
│  • All internal allocations use gcomp_malloc/calloc/free helpers        │
│  • Method implementations must use st->allocator (not raw malloc)       │
└─────────────────────────────────────────────────────────────────────────┘
```

### Memory Tracking

Methods track memory usage via `gcomp_memory_tracker_t`:

```c
// In decoder state initialization
gcomp_memory_track_alloc(&state->mem_tracker, sizeof(decode_state));
gcomp_memory_track_alloc(&state->mem_tracker, window_size);

// Check against limits
if (gcomp_limits_check_memory(state->mem_tracker.current_bytes, max_memory)
    != GCOMP_OK) {
    return GCOMP_ERR_LIMIT;
}

// On cleanup
gcomp_memory_track_free(&state->mem_tracker, window_size);
```

## Error Propagation Patterns

### Status Code Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Method Implementation                               │
│                                                                         │
│  // Set error with context                                              │
│  return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,             │
│      "invalid block type %d at stage '%s'", block_type, stage_name);    │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     Core Stream Layer                                   │
│                                                                         │
│  // Pass through to caller                                              │
│  return encoder->update_fn(encoder, input, output);                     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     Application                                         │
│                                                                         │
│  gcomp_status_t status = gcomp_decoder_update(dec, &in, &out);          │
│  if (status != GCOMP_OK) {                                              │
│      printf("Error: %s\n", gcomp_status_to_string(status));             │
│      printf("Detail: %s\n", gcomp_decoder_get_error_detail(dec));       │
│  }                                                                      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Error Categories

| Status Code | Recoverable? | Typical Cause |
|-------------|--------------|---------------|
| `GCOMP_OK` | N/A | Success |
| `GCOMP_ERR_INVALID_ARG` | Yes | NULL pointer, invalid parameter |
| `GCOMP_ERR_MEMORY` | Maybe | Allocation failed |
| `GCOMP_ERR_LIMIT` | Yes | Resource limit exceeded |
| `GCOMP_ERR_CORRUPT` | No | Invalid compressed data |
| `GCOMP_ERR_UNSUPPORTED` | No | Method not found or feature unavailable |
| `GCOMP_ERR_INTERNAL` | No | Bug in library |
| `GCOMP_ERR_IO` | Maybe | I/O callback error |

### Error Detail Storage

Encoders and decoders store error details in a fixed-size buffer (256 bytes) to avoid allocation during error paths:

```c
struct gcomp_encoder_s {
    // ... other fields ...
    gcomp_status_t last_error;           // Error code
    char error_detail[256];              // Human-readable context
};
```

## Extension Points for New Methods

### Adding a New Compression Method

1. **Create method files** in `src/methods/<method_name>/`:

```
src/methods/mymethod/
├── mymethod_decode.c      # Decoder implementation
├── mymethod_encode.c      # Encoder implementation
├── mymethod_internal.h    # Shared internal definitions
└── mymethod_register.c    # Registration and vtable
```

2. **Define the method vtable** in `mymethod_register.c`:

```c
static const gcomp_method_t g_mymethod = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "mymethod",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = mymethod_create_encoder,
    .create_decoder = mymethod_create_decoder,
    .destroy_encoder = mymethod_destroy_encoder,
    .destroy_decoder = mymethod_destroy_decoder,
    .get_schema = mymethod_get_schema,
};
```

3. **Implement the encoder**:

```c
// Include internal header for stream structures
#include "core/stream_internal.h"

gcomp_status_t mymethod_create_encoder(gcomp_registry_t *registry,
    gcomp_options_t *options, gcomp_encoder_t **encoder_out) {
    
    gcomp_encoder_t *enc = *encoder_out;
    const gcomp_allocator_t *alloc = gcomp_registry_get_allocator(registry);
    
    // Allocate method-specific state
    mymethod_encode_state_t *state = gcomp_calloc(alloc, 1, sizeof(*state));
    if (!state) return GCOMP_ERR_MEMORY;
    
    state->allocator = alloc;
    // ... initialize state ...
    
    // Set up function pointers
    enc->method_state = state;
    enc->update_fn = mymethod_encode_update;
    enc->finish_fn = mymethod_encode_finish;
    enc->reset_fn = mymethod_encode_reset;  // Optional
    
    return GCOMP_OK;
}

static gcomp_status_t mymethod_encode_update(gcomp_encoder_t *encoder,
    gcomp_buffer_t *input, gcomp_buffer_t *output) {
    
    mymethod_encode_state_t *state = encoder->method_state;
    // ... compress input to output ...
    return GCOMP_OK;
}
```

4. **Add auto-registration** (optional):

```c
#include "autoreg/autoreg_platform.h"

gcomp_status_t gcomp_method_mymethod_register(gcomp_registry_t *registry) {
    if (!registry) return GCOMP_ERR_INVALID_ARG;
    return gcomp_registry_register(registry, &g_mymethod);
}

GCOMP_AUTOREG_METHOD(mymethod, gcomp_method_mymethod_register)
```

5. **Define option schema** for configuration:

```c
static const gcomp_option_schema_t g_mymethod_options[] = {
    {
        .key = "mymethod.level",
        .type = GCOMP_OPT_INT64,
        .has_default = 1,
        .default_value.i64 = 6,
        .has_min = 1,
        .has_max = 1,
        .min_int = 1,
        .max_int = 9,
        .help = "Compression level (1=fast, 9=best)"
    },
    {
        .key = "mymethod.mode",
        .type = GCOMP_OPT_STRING,
        .has_default = 1,
        .default_value.str = "fast",
        .help = "Mode: fast or small",
        // A string option that takes a fixed set must say so. Validation
        // then refuses anything else where the caller set it, naming the
        // key, instead of leaving the method to fail later with a message
        // about something further downstream - and a caller can discover
        // the set instead of parsing `help`.
        //
        // Leave it NULL only when the option really is free-form, the way
        // `gzip.name` is.
        .allowed = (const char * const[]){"fast", "small", NULL}
    },
    // ... more options ...
};

static const gcomp_method_schema_t g_mymethod_schema = {
    .options = g_mymethod_options,
    .num_options = sizeof(g_mymethod_options) / sizeof(g_mymethod_options[0]),
    .unknown_key_policy = GCOMP_UNKNOWN_KEY_IGNORE,
};
```

### Implementing a Wrapper Method

Wrapper methods (like gzip) wrap another method (like deflate) and add headers/trailers. See [Wrapper Methods](wrapper-methods.md) for detailed documentation.

Key pattern:

```c
gcomp_status_t gzip_create_encoder(gcomp_registry_t *registry,
    gcomp_options_t *options, gcomp_encoder_t **encoder_out) {
    
    // Look up wrapped method
    const gcomp_method_t *deflate = gcomp_registry_find(registry, "deflate");
    if (!deflate) return GCOMP_ERR_UNSUPPORTED;
    
    // Create inner encoder
    gcomp_encoder_t *inner = NULL;
    gcomp_status_t status = deflate->create_encoder(registry, options, &inner);
    if (status != GCOMP_OK) return status;
    
    // Store inner encoder in wrapper state
    state->inner_encoder = inner;
    // ... set up gzip header/trailer handling ...
}
```

## Threading Model

| Component | Thread Safety |
|-----------|--------------|
| Default registry | Safe for concurrent reads after initialization |
| Custom registries | Not thread-safe; use one registry per thread |
| Encoders/decoders | Not thread-safe; use one instance per thread |
| Options (frozen) | Safe to share across threads |
| Options (mutable) | Not thread-safe |
| Allocator callbacks | Must be thread-safe if registry is shared |

## File Organization

```
compress/
├── include/ghoti.io/compress/    # Public headers
│   ├── compress.h                # Main header (convenience functions)
│   ├── stream.h                  # Streaming API
│   ├── registry.h                # Method registry
│   ├── method.h                  # Method vtable interface
│   ├── options.h                 # Configuration
│   ├── errors.h                  # Status codes
│   ├── allocator.h               # Memory allocation
│   ├── limits.h                  # Safety limits
│   ├── brotli.h                  # Brotli-specific API
│   ├── lzma.h                    # LZMA and LZMA2 registration
│   ├── deflate.h                 # Deflate-specific API
│   ├── gzip.h                    # Gzip-specific API
│   ├── lz4.h                     # LZ4-specific API
│   ├── lzw.h                     # LZW-specific API
│   ├── zstd.h                    # Zstd-specific API
│   ├── rle.h                     # RLE-specific API
│   ├── crc32.h                   # CRC-32 (gzip)
│   ├── xxhash32.h                # XXH32 (LZ4)
│   ├── xxhash64.h                # XXH64 (zstd)
│   ├── macros.h                  # Cross-compiler utilities
│   ├── namespace.h               # GHOTIIO_COMPRESS symbol prefixing
│   └── libver.h                  # Generated version macros
│
├── src/
│   ├── compress.c                # Convenience functions
│   ├── core/                     # Core implementation
│   │   ├── stream.c              # Encoder/decoder management
│   │   ├── stream_cb.c           # Callback-based streaming
│   │   ├── registry.c            # Method registry
│   │   ├── method.c              # Vtable helpers and schema validation
│   │   ├── options.c             # Options system
│   │   ├── allocator.c           # Default allocator
│   │   ├── limits.c              # Limit checking
│   │   ├── errors.c              # Error utilities
│   │   ├── buffer.c              # Buffer convenience functions
│   │   ├── block_job.h           # The job struct the parallel encoders embed
│   │   ├── parallel_block.c      # Shared block-parallel driver (LZ4, zstd),
│   │   │                         #   over cutil's GCU_Pool and GCU_Sequencer
│   │   ├── huffman_lengths.c     # Length-limited code lengths by
│   │   │                         #   boundary package-merge (deflate, zstd)
│   │   ├── wrapper_options.c     # Option cloning for wrappers (gzip, zlib)
│   │   ├── crc32.c               # CRC-32 (gzip)
│   │   ├── adler32.c             # Adler-32 (zlib, and so PNG)
│   │   ├── xxhash32.c            # XXH32 (LZ4)
│   │   ├── xxhash64.c            # XXH64 (zstd)
│   │   ├── bitcost.h             # Fixed-point log2 and symbol pricing
│   │   │                         #   shared by both optimal parses
│   │   ├── stepdown.h            # Why an encoder fell back, for tests
│   │   ├── endian.h              # Unaligned little-endian load/store
│   │   └── *_internal.h          # Internal headers
│   │
│   ├── autoreg/                  # Auto-registration support
│   │   └── autoreg_platform.h    # Platform-specific constructors
│   │
│   └── methods/                  # Compression method implementations
│       ├── brotli/
│       │   ├── brotli_decode.c   # RFC 7932 decoder
│       │   ├── brotli_bw.h       # LSB-first bit writer, shared by both halves
│       │   ├── brotli_encode.c   # Stream, level 0 store, level 1 dispatch
│       │   ├── brotli_lz.c       # Level 1 LZ77 and Huffman meta-blocks
│       │   ├── brotli_prefix.c   # Prefix codes
│       │   ├── brotli_dict.c     # Static dictionary transforms
│       │   ├── brotli_tables.c   # Dictionary, transforms, context LUTs
│       │   └── brotli_register.c # Vtable and registration
│       ├── lzma/
│       │   ├── lzma_decode.c     # Both decoders: one parse, a carry, a growing window
│       │   ├── lzma_encode.c     # Range coder, match finder, fast parser, LZMA2 chunks
│       │   ├── lzma_common.c     # Initial probabilities, the properties byte
│       │   └── lzma_register.c   # Vtables and registration
│       ├── deflate/
│       │   ├── deflate_encode.c  # Encoder: match finder, parses, blocks
│       │   ├── deflate_decode.c  # Decoder
│       │   ├── deflate_register.c# Vtable and registration
│       │   ├── huffman.c         # Huffman table building
│       │   ├── bitreader.c       # Bit-level input
│       │   └── bitwriter.c       # Bit-level output
│       ├── gzip/
│       │   ├── gzip_encoder.c    # Gzip wrapper encoder (RFC 1952)
│       │   ├── gzip_decoder.c    # Gzip wrapper decoder
│       │   ├── gzip_format.c     # Header/trailer parsing
│       │   └── gzip_register.c   # Vtable and registration
│       ├── lz4/
│       │   ├── lz4_encoder.c     # LZ4 frame encoder
│       │   ├── lz4_decoder.c     # LZ4 frame decoder
│       │   ├── lz4_block.c       # Block compression/decompression
│       │   ├── lz4_frame.c       # Frame header/trailer
│       │   ├── lz4_parallel.c    # Parallel encoding support
│       │   └── lz4_register.c    # Vtable and registration
│       ├── lzw/
│       │   ├── lzw_encoder.c     # LZW encoder (profile-driven)
│       │   ├── lzw_decoder.c     # LZW decoder (profile-driven)
│       │   ├── lzw_core.c        # Code table and string extension
│       │   ├── lzw_hash.c        # Hash lookup; the default, and the fast one
│       │   ├── lzw_bitio.c       # Variable-width code packing, both orders
│       │   ├── lzw_profile.c     # GIF / TIFF / compress(1) grammars
│       │   └── lzw_register.c    # Vtable and registration
│       ├── rle/
│       │   ├── rle_encoder.c     # RLE encoder (profile-driven)
│       │   ├── rle_decoder.c     # RLE decoder (profile-driven)
│       │   ├── rle_core.c        # Literal/repeat emission, bounds
│       │   ├── rle_profile.c     # PackBits and TGA token grammars
│       │   ├── rle_internal.h    # Shared state and phases
│       │   └── rle_register.c    # Vtable and registration
│       ├── zlib/
│       │   ├── zlib_encoder.c    # RFC 1950 wrapper encoder
│       │   ├── zlib_decoder.c    # RFC 1950 wrapper decoder
│       │   ├── zlib_format.c     # CMF/FLG build and validation
│       │   └── zlib_register.c   # Vtable and registration
│       └── zstd/
│           ├── zstd_encoder.c    # Zstd frame encoder
│           ├── zstd_decoder.c    # Zstd frame decoder
│           ├── zstd_frame.c      # Frame header and trailer
│           ├── zstd_block.c      # Block framing and type choice
│           ├── zstd_matchfinder.c# Hash chain and binary tree search
│           ├── zstd_optimal.c    # Shortest-path parse (levels 16+)
│           ├── zstd_literals.c   # Literals section, all four types
│           ├── zstd_huf.c        # Huffman for literals
│           ├── zstd_fse.c        # FSE tables, both directions
│           ├── zstd_sequences.c  # Sequence decode
│           ├── zstd_sequences_encode.c # Sequence encode and table choice
│           ├── zstd_dict.c       # Dictionary parsing and priming
│           ├── zstd_parallel.c   # Parallel encoding support
│           └── zstd_register.c   # Vtable and registration
│
├── tests/                        # Unit tests (Google Test)
├── examples/                     # Example programs
├── bench/                        # Benchmarks
├── fuzz/                         # Fuzz testing harnesses
└── documentation/                # Detailed documentation
```

## Parallel Block Compression

LZ4 and Zstd parallel encoders share a generic **parallel block** helper in `src/core/parallel_block.c` (internal API in `parallel_block.h`). The helper encapsulates:

- **Inline vs threaded mode:** When `num_threads <= 1`, jobs run in the caller thread; otherwise cutil's `GCU_Pool` runs them and cutil's `GCU_Sequencer` orders them.
- **Ordered results:** Jobs may complete out of order; the helper returns results in submission order via `GCU_Sequencer` (threaded) or an inline FIFO.
- **Submit / get_result:** Methods submit an opaque job (whose first member is `gcomp_block_job_t`) and a process callback; they retrieve the next completed job in order.

Method-specific logic (per-job allocation, block or frame compression, checksums) remains in `lz4_parallel.c` and `zstd_parallel.c`; the helper only manages context lifecycle, threading, and ordering.

## Wrapper Methods

`gzip` and `zlib` are the same shape: a header, an inner deflate stream, and a
checksum trailer over the *uncompressed* data. Neither compresses anything
itself; each owns a `gcomp_encoder_t` for "deflate" and frames what it
produces.

| | gzip (RFC 1952) | zlib (RFC 1950) |
|---|---|---|
| Header | 10 bytes plus optional name, comment, extra | 2 bytes |
| Checksum | CRC-32, little-endian | Adler-32, **big-endian** |
| Overhead | 18+ bytes | 6 bytes |
| Concatenation | Supported (`gzip.concat`) | Not a thing the format does |

Both decide what of the caller's options the inner deflate should see through
`gcomp_clone_options_for_method()` in `src/core/wrapper_options.c`. The inner
method's own schema is the authority: deflate's policy is
`GCOMP_UNKNOWN_KEY_ERROR`, so passing it a `gzip.name` or a `zlib.dictionary`
would fail the call, and a hand-written list of what to pass would be a second
copy of deflate's schema kept up to date by hope.

Neither container records the length of the deflate stream, so both find its
end the same way: by asking the inner decoder, whose `finish()` reports
`GCOMP_OK` exactly when it has seen the final block and which leaves the
trailer bytes unconsumed.

The two methods divide work differently, because their formats do:

| | LZ4 | Zstd |
|---|---|---|
| Work unit | one block (`lz4.block_size`) | one job (`zstd.job_size`) |
| Output | one frame, independent blocks | one frame, a run of blocks per job |
| Same bytes as 1 thread? | **Yes** | No |
| Requires | `lz4.independent_blocks` (default) | — |

LZ4 can promise byte-identical output because the Block Independence flag
already guarantees a block is compressed knowing nothing about its
predecessors, so a worker receives exactly the window the serial encoder
would have built.

Zstd cannot, but not for the reason this paragraph used to give. It said "a
zstd frame carries its own window and cannot be split that way, so its parallel
mode emits a frame per job instead" — the conclusion was wrong: zstd's parallel
mode emits **one** frame, and the jobs are its blocks. What differs from serial
output is the *history* each job starts from. A job is seeded with a window's
worth of the preceding stream rather than with everything before it, so it makes
different (slightly worse) choices, and the offsets it names cannot be predicted
in advance because the jobs are compressed at the same time — which is why the
repeat-offset history in `zstd_parallel.c` starts at zero rather than at RFC
8878's 1, 4, 8.

One frame is also what lets a job match back into the job before it, and so
what lets `zstd.long` work with `threads.count > 1`.

Both encoders hold back at most one job's output when the caller's buffer
fills mid-result, and neither may collect another result while one is staged
— doing so overwrites the staged bytes and silently drops them from the
stream. That was a live defect in both, fixed in `lz4_encoder.c` and
`zstd_encoder.c`; see `MoreBlocksThanSlotsDoesNotStall` and
`ASmallOutputBufferDoesNotLoseAJobsOutput`.

## Bit I/O and Format Differences

Deflate and zstd use separate bit I/O code by design; the duplication is intentional.

- **Deflate** uses LSB-first (least significant bit first) bit I/O in `src/methods/deflate/bitreader.c` and `bitwriter.c`, per RFC 1951.
- **Buffer arguments** are checked once, in `gcomp_encoder_update()`, `gcomp_encoder_finish()`, `gcomp_encoder_flush()`, `gcomp_decoder_update()` and `gcomp_decoder_finish()`, rather than by each method. A `gcomp_buffer_t` with `used` past its own `size`, or with no `data` and bytes still to go, is refused with `GCOMP_ERR_INVALID_ARG` and a detail naming the field. The reason it belongs there: every method computes the bytes available as `size - used`, which on `size_t` underflows to nearly `SIZE_MAX` rather than to zero, so an unchecked inconsistent buffer is a read or write off the end of the caller's memory - measured as a heap-buffer-overflow in brotli's encoder and a wild read in RLE's before this check existed. One method had made these checks for itself and the other seven had not.
- **Brotli** uses LSB-first bit I/O in `src/methods/brotli/brotli_bits.h` (reader) and `brotli_bw.h` (writer), per RFC 7932. They are its own; they do not share deflate's. Both halves of the encoder write bits and both use the one writer - the stream header and stored meta-blocks in `brotli_encode.c`, the compressed meta-block in `brotli_lz.c` - because when each kept a copy the two had already drifted over whether a put wider than 24 bits is refused.
- **Zstd** uses MSB-first (most significant bit first) bit I/O in its own modules (`zstd_fse.c`, `zstd_huf.c`, `zstd_sequences.c`, `zstd_sequences_encode.c`, etc.) per the Zstandard specification.
- **LZW** uses LSB-first (GIF) or MSB-first (TIFF) bit I/O in `src/methods/lzw/lzw_bitio.c`. The choice is profile-driven and affects on-the-wire representation.

Implementations are intentionally separate due to bit order and format-specific alignment; do not unify them.

## Related Documentation

- [Streaming API](api/streaming.md) - Detailed streaming usage patterns
- [Brotli Module](modules/brotli.md) - Brotli window, levels, and what the encoder writes
- [Deflate Module](modules/deflate.md) - Deflate-specific options and usage
- [Gzip Module](modules/gzip.md) - Gzip-specific options and usage
- [LZ4 Module](modules/lz4.md) - LZ4-specific options and usage
- [LZW Module](modules/lzw.md) - LZW (GIF/TIFF) options and usage
- [RLE Module](modules/rle.md) - RLE (PackBits/TGA) options and usage
- [Auto-Registration](auto-registration.md) - How methods are automatically registered
- [Wrapper Methods](wrapper-methods.md) - Implementing wrapper methods (gzip, etc.)
- [Testing](testing/testing.md) - Testing infrastructure
- [Fuzzing](testing/fuzzing.md) - Fuzz testing documentation
