# Threading

This document describes how compression methods in the Ghoti.io Compress library can implement parallel compression using the threading primitives provided by the sibling `cutil` library.

## Overview

The compress library framework does **not** include its own threading infrastructure. Instead, methods that need parallel compression should use the cross-platform threading APIs provided by `cutil`:

- **`ghoti.io/cutil/thread.h`** - Thread creation, joining, and utilities
- **`ghoti.io/cutil/mutex.h`** - Mutual exclusion locks
- **`ghoti.io/cutil/semaphore.h`** - Counting semaphores for coordination

This design keeps the compress library focused on compression algorithms while leveraging the battle-tested threading primitives in `cutil`.

## Threading Primitives from cutil

### Thread Management (`ghoti.io/cutil/thread.h`)

```c
#include <ghoti.io/cutil/thread.h>

// Thread handle type
GCU_Thread thread;

// Thread function signature
GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION my_worker(GCU_THREAD_FUNC_ARG_T arg) {
    // Worker code here
    return 0;
}

// Create and start a thread
int gcu_thread_create(GCU_Thread *thread, GCU_THREAD_FUNC func, void *arg);

// Wait for a thread to complete
int gcu_thread_join(GCU_Thread thread);

// Detach a thread (let it run independently)
int gcu_thread_detach(GCU_Thread thread);

// Get the number of logical processors (useful for default thread count)
unsigned int gcu_thread_get_num_processors();

// Utility functions
void gcu_thread_sleep(unsigned long milliseconds);
void gcu_thread_yield();
```

### Mutexes (`ghoti.io/cutil/mutex.h`)

```c
#include <ghoti.io/cutil/mutex.h>

GCU_MUTEX_T my_mutex;

// Create a mutex (returns 0 on success)
if (GCU_MUTEX_CREATE(my_mutex) != 0) {
    // Handle error
}

// Lock the mutex (blocking)
GCU_MUTEX_LOCK(my_mutex);

// Critical section...

// Unlock the mutex
GCU_MUTEX_UNLOCK(my_mutex);

// Try to lock without blocking (returns 0 if acquired)
if (GCU_MUTEX_TRYLOCK(my_mutex) == 0) {
    // Got the lock
}

// Destroy the mutex when done
GCU_MUTEX_DESTROY(my_mutex);
```

### Semaphores (`ghoti.io/cutil/semaphore.h`)

```c
#include <ghoti.io/cutil/semaphore.h>

GCU_Semaphore sem;

// Create a semaphore with initial value
int gcu_semaphore_create(GCU_Semaphore *semaphore, int value);

// Wait (decrement, blocking if zero)
int gcu_semaphore_wait(GCU_Semaphore *semaphore);

// Signal (increment)
int gcu_semaphore_signal(GCU_Semaphore *semaphore);

// Try to wait without blocking
int gcu_semaphore_trywait(GCU_Semaphore *semaphore);

// Wait with timeout (milliseconds)
int gcu_semaphore_timedwait(GCU_Semaphore *semaphore, int timeout);

// Get current value
int gcu_semaphore_getvalue(GCU_Semaphore *semaphore, int *value);

// Destroy the semaphore
int gcu_semaphore_destroy(GCU_Semaphore *semaphore);
```

## Option Convention: `threads.count`

Methods that support parallel compression should check for a `threads.count` option:

```c
int64_t thread_count = 1;  // Default: single-threaded
gcomp_options_get_int64(options, "threads.count", &thread_count);

if (thread_count < 1) {
    thread_count = 1;  // Minimum 1 thread
}
```

### Recommended Behavior

| `threads.count` | Behavior |
|-----------------|----------|
| 1 (default)     | Single-threaded operation (no threading overhead) |
| N > 1           | Use N worker threads for parallel compression |
| 0               | Auto-detect: use `gcu_thread_get_num_processors()` |

Methods should document whether they support the `threads.count` option in their schema.

## `threads.count` on decode

The same option, and it means the same thing — use this many threads — but what
it can do is decided by the stream rather than by the caller.

**A unit can be decoded on its own only if it references nothing before it.**
That is a property of how the stream was written:

| Method | Can decode in parallel? |
|--------|-------------------------|
| `lz4` | Yes, when the frame sets `B.Indep` (this library's default). Every block is a job, so a single frame parallelises. |
| `zstd` | Only across frames, and only when every frame declares its content size. Blocks inside one frame share a window (RFC 8878 §3.1.1.1.2) and cannot be split, and our encoder emits one frame — so this benefits streams somebody made multi-frame on purpose: concatenated files, one frame per thread, and the seekable files Phase D will write. |
| `deflate`, `gzip`, `zlib`, `lzw`, `rle` | No. Each is one stream of back-references from beginning to end. |

A method opts in through `gcomp_method_s::decode_parallel` (method ABI 3), which
`gcomp_decode_buffer()` offers the whole input. It is offered only there, not to
the streaming decoder: splitting needs to see where the next unit ends, and a
streaming decoder is given the stream a piece at a time.

### Declining is the normal answer

The hook returns `GCOMP_ERR_UNSUPPORTED` to say "not this stream", and the
caller then decodes it the ordinary way. LZ4 declines a frame with linked
blocks, a frame with one block, a frame compressed against a dictionary, and
anything it could not walk. Zstandard declines a single-frame stream, and one
where any frame omits `Frame_Content_Size` — a job needs somewhere to put its
output before the frames ahead of it have finished, so it has to know how big
that is first.

A method must decline anything it is not certain of: the single-threaded path
is always correct, so declining costs speed and guessing costs a wrong decode.
Nothing is decoded until every frame has been examined, so a stream that
declines cannot leave bytes in the caller's buffer for the fallback to write
over.

A declared size is not a trusted one. It bounds an allocation that the caller's
own limits are checked against first, and the decode that follows enforces
everything it normally would: a frame that lies about its size fails exactly as
it does single-threaded.

### What a parallel decoder must not change

The bytes, and every check the ordinary path makes — block checksums, content
checksums, declared content sizes, `limits.max_output_bytes`. A path that is
faster because it checks less is invisible on valid input, which is why
`tests/methods/lz4/test_lz4_parallel_decode.cpp` corrupts each of them
deliberately and requires the same answer at 1, 2, 3, 4 and 12 threads.

Ordering is the other half. Blocks finish out of order and must be written in
order; getting that wrong on a stream of similar blocks gives output of exactly
the right length that is subtly scrambled, which no length check would catch.
The sequencer in `src/core/parallel_block.h` is what hands results back in
submission order, and it is the same one the encoders use.

## Implementation Pattern

Here's a typical pattern for methods implementing parallel compression:

```c
typedef struct {
    // Shared state
    GCU_MUTEX_T mutex;
    gcomp_status_t error;      // First error encountered
    
    // Job coordination
    GCU_Semaphore jobs_available;
    GCU_Semaphore jobs_complete;
    
    // Job queue (protected by mutex)
    // ...
} parallel_state_t;

static GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION
worker_thread(GCU_THREAD_FUNC_ARG_T arg) {
    parallel_state_t *state = (parallel_state_t *)arg;
    
    while (1) {
        // Wait for a job
        gcu_semaphore_wait(&state->jobs_available);
        
        // Check for shutdown signal
        GCU_MUTEX_LOCK(state->mutex);
        // ... get job from queue or check shutdown flag ...
        GCU_MUTEX_UNLOCK(state->mutex);
        
        // Process job
        gcomp_status_t result = process_job(/* ... */);
        
        // Report errors
        if (result != GCOMP_OK) {
            GCU_MUTEX_LOCK(state->mutex);
            if (state->error == GCOMP_OK) {
                state->error = result;  // Record first error
            }
            GCU_MUTEX_UNLOCK(state->mutex);
        }
        
        // Signal completion
        gcu_semaphore_signal(&state->jobs_complete);
    }
    
    return 0;
}
```

## Memory Bounds with Parallelism

Methods must respect `limits.max_memory_bytes` even when using multiple threads. Strategies:

### 1. Limit In-Flight Jobs

Use a semaphore to bound the number of concurrent jobs:

```c
// At initialization
int max_jobs = max_memory_bytes / per_job_memory;
gcu_semaphore_create(&job_slots, max_jobs);

// Before starting a job
gcu_semaphore_wait(&job_slots);  // Blocks if at limit

// After job completes
gcu_semaphore_signal(&job_slots);
```

### 2. Shared Memory Tracking

If using the framework's `gcomp_memory_tracker_t` across threads, protect it with a mutex:

```c
GCU_MUTEX_LOCK(memory_mutex);
gcomp_memory_track_alloc(tracker, size);
gcomp_status_t status = gcomp_memory_check_limit(tracker, limit);
GCU_MUTEX_UNLOCK(memory_mutex);

if (status != GCOMP_OK) {
    // Over limit - wait for other jobs to free memory
}
```

### 3. Per-Thread Tracking

Alternatively, track memory per-thread and merge periodically:

```c
// Each thread has local tracking
__thread size_t local_memory = 0;

// Periodically merge to shared tracker (with mutex)
```

## Error Propagation

Worker threads cannot directly return errors to the caller. Instead:

1. **Record the first error** in shared state (protected by mutex)
2. **Check for errors** in the main thread after joining workers
3. **Surface errors** via the encoder/decoder error state

```c
// After joining all workers
if (parallel_state.error != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, parallel_state.error,
        "Worker thread failed: %s", gcomp_status_name(parallel_state.error));
    return parallel_state.error;
}
```

## Requirements

### Single-Threaded Mode Must Always Work

Every method that supports `threads.count > 1` **must** also work correctly with `threads.count = 1` (single-threaded). This ensures:

- Methods work on systems where threading may be problematic
- Users can disable parallelism for debugging
- Fallback behavior when `cutil` is not linked

### Thread Safety of Framework Components

The compress library framework is **not** internally thread-safe. Methods are responsible for:

- Protecting any shared access to `gcomp_memory_tracker_t`
- Not calling encoder/decoder APIs from multiple threads simultaneously
- Synchronizing access to shared buffers

### Cleanup on Error

If an error occurs, methods must:

1. Signal all worker threads to stop
2. Join all threads before returning
3. Free all allocated resources

Do not leave orphaned threads running after returning an error.

## Core Threading Infrastructure

The compress library provides core threading infrastructure that methods can use:

### Thread Pool (`GCU_Pool`, from cutil)

Parallel work runs on cutil's thread pool.  This library had its own, in
`src/core/thread_pool.c`, and it was removed:  its `destroy` was documented
as draining but tested its shutdown flag before the queue, so an
unpredictable number of queued blocks were silently discarded.

```c
#include <ghoti.io/cutil/pool.h>

GCU_Pool_Config config = {
    .thread_count = 4,
    .max_queued = 0,
    .name_prefix = "gcomp-blk",
    .allocator = allocator,
};
GCU_Pool * pool = gcu_pool_create(&config);

gcu_pool_enqueue_cb(pool, process_fn, job_ctx, on_complete, user_data);

gcu_pool_destroy(pool);  // drains:  runs what is queued, then stops
```

### Sequencer (`GCU_Sequencer`, from cutil)

Blocks are compressed in parallel and finish in whatever order they finish,
but must be written in file order.  That reordering is cutil's sequencer.
This library had its own, as `gcomp_job_queue_t`, and it was replaced:  the
mechanism is entirely generic, and keeping a private copy meant fixing the
same concurrency defects twice.

```c
#include <ghoti.io/cutil/sequencer.h>

GCU_Sequencer_Config config = {
    .capacity = max_in_flight,
    .allocator = allocator,
};
GCU_Sequencer * sequencer = gcu_sequencer_create(&config);

// Submit; the ticket is what names this item from now on.
uint64_t ticket = 0;
gcu_sequencer_submit(sequencer, &job->base, &ticket);
job->base.sequence_num = ticket;

// Whichever worker finishes it says so, by ticket.
gcu_sequencer_complete(sequencer, job->base.sequence_num, status);

// Collect in submission order, blocking while the oldest is unfinished.
void * payload = NULL;
gcu_sequencer_next(sequencer, &payload, NULL);
```

Both are wired together in `src/core/parallel_block.c`, which is what the
LZ4 and Zstd parallel encoders actually use; neither calls the pool or the
sequencer directly.

## Method-Specific Options

Methods may define additional threading-related options beyond `threads.count`:

| Option | Used By | Description |
|--------|---------|-------------|
| `zstd.job_size` | Zstd | Size of each parallel compression job; defaults to four windows, clamped to 512 KB - 16 MB |
| `lz4.block_size` | LZ4 | Block size, which is also LZ4's parallel job size |

### What decides whether threads help

Worth reading before sizing a workload around `threads.count`, because the answer
depends on the input more than on the core count. Measured on this machine at four
threads against one, with 64 MB of input:

| Input | Speedup |
|---|---|
| Prose-like text | 2.6x - 2.9x |
| Incompressible bytes | ~2.2x |
| One pattern repeated (serial already ~2 GB/s) | ~0.7x |

Two rules come out of that:

- **There has to be work to parallelise.** On input a single thread already
  compresses at gigabytes per second there is nothing to win, and the per-job
  cost shows through. That is not a defect; it is the ceiling.
- **The input has to be worth more than about two jobs.** 256 KB against a
  512 KB job is one job, so threads are pure overhead - 0.34x at four. Compress
  many small objects one thread each instead.

For zstd there is a third, and it is why `zstd.job_size` now follows the window
rather than sitting at a fixed 512 KB: each job re-indexes a window of the
preceding stream, so the wasted work per useful byte is `window_size / job_size`.
See [zstd's job size section](modules/zstd.md#job-size-configuration).
`notes/compress/PERFORMANCE.md` has the sweep.

The speedups above were measured at the default window. **At a large window they
are lower than they were, and everything is faster** — the single-threaded
encoder used to copy its whole window once per block, and removing that gained it
more than it gained the jobs. One pattern repeated, 64 MB, at a 32 MB window: 109
to 400 MB/s on four threads, but 126 to 1,340 on one, so four threads against one
went from 0.87x to 0.30x. On prose at the same window, 77.4 to 116.6 MB/s and
scaling unchanged at about 2.2x. See [what a large window
costs](modules/zstd.md#what-a-large-window-costs).

## Existing Implementations

### Zstd Parallel Compression

The Zstd encoder implements parallel compression through `parallel_block.c`, and so through cutil's pool and sequencer:

- **Location**: `src/methods/zstd/zstd_parallel.c`
- **Options**: `threads.count`, `zstd.job_size`
- **Output format**: One frame, whose blocks were compressed on several
  threads. Not byte-identical to single-threaded output, unlike LZ4 — each job
  starts from an overlap rather than from the whole history, so it makes
  different choices — but the *shape* is the same and no decoder option is
  needed.

  This line said "concatenated independent frames" until 2026-09-24, which
  contradicted the zstd row in the decode table above ("our encoder emits one
  frame") in the same document. The table was right.

See [Zstd Module Documentation](modules/zstd.md#parallel-compression) for details.

### LZ4 Parallel Compression

The LZ4 encoder also supports parallel compression:

- **Location**: `src/methods/lz4/lz4_parallel.c`
- **Options**: `threads.count`, `lz4.block_size`
- **Work unit**: one block, so `lz4.block_size` is also the parallel
  granularity — there is no LZ4 equivalent of `zstd.job_size`
- **Output format**: one ordinary LZ4 frame with independent blocks,
  **byte-for-byte identical to single-threaded output**
- **Requires** `lz4.independent_blocks` (the default). With linked blocks a
  block may reference the one before it, so there is nothing to parallelise;
  the encoder compresses inline and reports it through
  `gcomp_lz4_encoder_worker_count()`.

See [LZ4 Module Documentation](modules/lz4.md#parallel-compression) for
details and measurements.

## Future Considerations

Additional threading features may be added as needed:

- **Atomic memory tracker** - Lock-free memory accounting
- **Parallel decompression** - For formats that support it
