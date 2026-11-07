# Benchmark Results

Performance evaluation comparing the custom memory allocator against the native system allocator (`libc` malloc/free) on macOS.

## Environment

- **CPU model**: Apple M2
- **Physical cores / logical CPUs**: 8 cores
- **Operating system**: macOS Darwin 24.6.0 (arm64)
- **Compiler and flags**: Apple clang 17.0.0 (`-std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude -pthread`)
- **Date**: 2026-11-07

## Raw Allocation Throughput (100,000 Operations)

The benchmark executes 100,000 randomized allocation and replacement operations across 1,024 active slots with sizes varying from 16 to 1,024 bytes.

| Allocator Architecture | Median Time (ms) | Operations/s | Relative Speed |
| :--- | ---: | ---: | :--- |
| **Custom Segregated Free Lists (Bins)** | **5.17 ms** | **19,353,590 ops/s** | **1.24x faster than system** |
| Native System `malloc`/`free` | 6.43 ms | 15,544,847 ops/s | Baseline |
| Custom Baseline First-Fit (Original) | 29.41 ms | 3,399,626 ops/s | ~0.22x of system |

### Architecture Optimization Impact
- **Original Linear First-Fit**: Required $O(N)$ traversal through the physical doubly-linked list on each allocation, causing performance to degrade as the active slot count grew.
- **Segregated Free Lists (Bins)**: Segregates free blocks into 14 distinct size-class bins ($\le 32, 64, \dots, 4096$ bytes) plus a large block fallback. Allocations pop matching free blocks in $O(1)$ time, yielding an immediate **~5.7x speedup** over the linear scan baseline and outperforming native malloc on single-arena mixed-size churn.

## External Fragmentation & Coalescing Experiment

The test allocates three adjacent 4,096-byte blocks, frees the first two, inspects fragmentation metrics, and verifies that adjacent-block coalescing successfully satisfies a contiguous 7,000-byte allocation.

- **Adjacent free region after coalescing**: 16,764,648 bytes (merging freed blocks with contiguous pool remainder)
- **Fragmentation metric before coalescing**: ~0.0005 ($1 - \frac{\text{largest\_free}}{\text{total\_free}}$)
- **7,000-byte allocation after coalescing**: Succeeded
- **Fragmentation metric after full pool free**: 0.0000 (100% coalesced back into 1 single contiguous block)
