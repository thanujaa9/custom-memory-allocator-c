# Custom Memory Allocator (C11)

A defensive, thread-safe dynamic memory allocator implemented in C11. It manages an `mmap`-backed memory pool instead of relying on the system heap for every allocation. The architecture uses segregated free lists (size-class bins) for small allocations with first-fit fallback for large blocks, adjacent-block coalescing, payload alignment, in-place realloc, canary-based buffer overflow defense, and runtime integrity checking (`my_heap_check()`).

## Features

- **Configurable `mmap` Memory Pool**: Defaults to 16 MiB via thread-safe lazy initialization or custom sizing with `my_allocator_init()`.
- **Segregated Size-Class Bins**: 14 size classes ($\le 32, 64, \dots, 4096$ bytes) allow $O(1)$ allocation and free for small blocks, eliminating the linear search bottleneck.
- **Physical Block Coalescing**: Adjacent free blocks automatically merge in $O(1)$ time upon `my_free()` to minimize external fragmentation.
- **Strict Payload Alignment**: Returned payloads align to `alignof(max_align_t)` (8 or 16 bytes depending on architecture).
- **In-Place `realloc`**: Realloc checks if the current block can satisfy the request, then attempts in-place expansion into adjacent free neighbors before falling back to allocate-copy-free.
- **Safe `calloc`**: Handles multiplication overflow checks and zeroes memory.
- **Thread Safety**: Mutex-synchronized metadata operations (`pthread_mutex_t`), fully verified race-free under ThreadSanitizer (TSan).
- **Defensive Heap Integrity**:
  - `my_heap_check()` validates header magics (`BLOCK_MAGIC_ALLOC` / `BLOCK_MAGIC_FREE`), physical adjacency, double links, and bin list consistency.
  - Double-free and invalid-free detection (sets `errno = EINVAL`).
  - Canary guard bytes trailing each payload detect off-by-one writes and buffer overflows (sets `errno = EFAULT`).
  - Out-of-memory error reporting setting `errno = ENOMEM`.
- **Allocation Statistics**:
  - Pool size, allocated bytes, and free bytes
  - Largest contiguous free block
  - Total block count and free block count
  - Per-block header size and total header overhead (`header_overhead_bytes`)
  - External fragmentation metric ($1.0 - \frac{\text{largest\_free}}{\text{total\_free}}$)

## Architecture & Layout

```text
mmap pool -> [header | payload | canary][header | payload | canary]...
               ^                                   ^
               +-- Intrusive physical doubly linked list (coalescing)
               +-- Segregated bin lists (O(1) allocation/deallocation)
```

Each header tracks:
- Magic number (`0x414C4F43` allocated / `0x46524545` free)
- User-requested size (used for canary validation)
- Usable payload capacity
- Physical neighbor pointers (`next`, `prev`)
- Segregated free list pointers (`next_free`, `prev_free`)
- Free status flag and alignment guard

## Validation & Test Suite

The test suite in [`tests/allocator_tests.c`](tests/allocator_tests.c) validates:
1. Basic allocation, writing, and freeing
2. Block splitting correctness
3. Adjacent-block coalescing and fragmentation reduction
4. Data preservation and in-place growth across `my_realloc`
5. `calloc` zero-fill and integer overflow rejection
6. Out-of-memory handling (`errno = ENOMEM`)
7. Defensive double-free and invalid pointer rejection (`errno = EINVAL`)
8. Canary corruption detection on buffer overflow (`errno = EFAULT` & `my_heap_check()`)
9. 20,000 single-threaded randomized operations with full coalescing verification
10. Multithreaded stress testing across 8 concurrent threads with unique per-thread byte pattern verification

## Benchmarks

The benchmark compares 100,000 mixed-size allocation/replacement operations against the native system `libc` allocator and tests external fragmentation coalescing.

Detailed environment information and median results are documented in [`benchmarks/results.md`](benchmarks/results.md).

## Limitations

While high-performing and defensive, this allocator is designed for educational and single-arena workloads and has the following architectural trade-offs:
1. **Fixed Pool Capacity**: The pool is backed by a single `mmap` region and does not automatically grow by mapping additional disjoint non-contiguous chunks once exhausted.
2. **Global Mutex Contention**: Concurrency is synchronized using a single coarse-grained mutex. While safe and race-free, high-concurrency workloads on high core counts will encounter lock contention compared to multi-arena, thread-caching designs like `jemalloc` or `tcmalloc`.
3. **In-Band Metadata**: Headers are stored inline immediately preceding payloads. While protected by header magic checks, pointer boundary validation, and canary bytes, an arbitrary wild pointer write could still damage header data.
4. **Internal Fragmentation**: Small requests are rounded up to size-class bin boundaries and `max_align_t` alignment, trading off minor internal fragmentation for $O(1)$ allocation speeds.

## Build and Run

### Using Make

```bash
make            # Build test and benchmark binaries
make test       # Run comprehensive test suite
make test-tsan  # Run multithreaded test suite with ThreadSanitizer
make test-asan  # Run test suite with AddressSanitizer and UBSan
make benchmark  # Run benchmark suite
make clean      # Clean build artifacts
```

### Using CMake

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/benchmark
```
