#ifndef CUSTOM_ALLOCATOR_H
#define CUSTOM_ALLOCATOR_H

#include <stddef.h>

typedef struct {
    size_t pool_size;              /* Total bytes in the mmap pool */
    size_t allocated_bytes;        /* Sum of payload bytes in active allocations */
    size_t free_bytes;             /* Sum of payload bytes in free blocks */
    size_t largest_free_block;     /* Payload bytes of largest single free block */
    size_t block_count;            /* Total physical blocks (free + allocated) */
    size_t free_block_count;       /* Number of free blocks */
    size_t header_size;            /* Size in bytes of one block_header_t */
    size_t header_overhead_bytes;  /* Total overhead: block_count * header_size */
    double fragmentation_metric;   /* External fragmentation: 1.0 - (largest_free / free_bytes) */
} allocator_stats_t;

/* Initializes one mmap-backed pool. Calling this is optional: my_malloc()
 * lazily creates a 16 MiB pool in a thread-safe manner. Returns 0 on success,
 * or -1 on failure (setting errno to EINVAL or ENOMEM). */
int my_allocator_init(size_t pool_size);

/* Destroys the allocator and unmaps the underlying memory pool.
 *
 * NOTE ON CONCURRENCY:
 * Caller must ensure that no other threads are concurrently invoking allocator
 * functions or accessing any memory previously allocated from this pool before
 * calling my_allocator_destroy(). */
void my_allocator_destroy(void);

/* Allocates aligned memory. Sets errno = ENOMEM and returns NULL on pool exhaustion. */
void *my_malloc(size_t size);

/* Frees memory and coalesces adjacent free neighbors.
 * Detects double-free and invalid pointer arguments (sets errno = EINVAL).
 * Detects buffer overflow / canary corruption (sets errno = EFAULT). */
void my_free(void *ptr);

/* Resizes an existing block with in-place expansion optimizations.
 * Sets errno = ENOMEM and returns NULL if resize cannot be fulfilled. */
void *my_realloc(void *ptr, size_t size);

/* Allocates zero-initialized memory with overflow checks.
 * Sets errno = ENOMEM and returns NULL on overflow or pool exhaustion. */
void *my_calloc(size_t count, size_t size);

/* Returns snapshot statistics about the allocator pool. */
allocator_stats_t my_allocator_stats(void);

/* Validates the integrity of the heap metadata, physical chain, canary bytes,
 * and segregated free lists. Returns 0 if consistent, non-zero error code if corrupted. */
int my_heap_check(void);

#endif
