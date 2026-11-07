#include "allocator.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_THREADS 8
#define THREAD_SLOTS 64
#define THREAD_OPS 5000

typedef struct {
    void *ptr;
    size_t size;
    uint32_t seed;
} slot_entry_t;

typedef struct {
    int thread_id;
} thread_arg_t;

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static uint8_t compute_pattern_byte(int thread_id, uint32_t seed, size_t index) {
    return (uint8_t)((thread_id * 101 + seed * 37 + index * 13 + 7) & 0xFF);
}

static void *thread_worker(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    int tid = targ->thread_id;
    slot_entry_t slots[THREAD_SLOTS] = {0};
    uint32_t seed = (uint32_t)(tid * 997 + 12345);

    for (int i = 0; i < THREAD_OPS; ++i) {
        size_t slot_idx = next_random(&seed) % THREAD_SLOTS;

        if (slots[slot_idx].ptr != NULL) {
            /* Verify per-thread fill pattern before free */
            uint8_t *bytes = (uint8_t *)slots[slot_idx].ptr;
            for (size_t b = 0; b < slots[slot_idx].size; ++b) {
                uint8_t expected = compute_pattern_byte(tid, slots[slot_idx].seed, b);
                if (bytes[b] != expected) {
                    fprintf(stderr, "Thread %d: data corruption at slot %zu, byte %zu (got %02x, expected %02x)\n",
                            tid, slot_idx, b, bytes[b], expected);
                    abort();
                }
            }
            my_free(slots[slot_idx].ptr);
            slots[slot_idx].ptr = NULL;
            slots[slot_idx].size = 0;
        } else {
            size_t alloc_size = 16 + (next_random(&seed) % 512);
            uint32_t fill_seed = next_random(&seed);
            void *p = my_malloc(alloc_size);
            if (p != NULL) {
                uint8_t *bytes = (uint8_t *)p;
                for (size_t b = 0; b < alloc_size; ++b) {
                    bytes[b] = compute_pattern_byte(tid, fill_seed, b);
                }
                slots[slot_idx].ptr = p;
                slots[slot_idx].size = alloc_size;
                slots[slot_idx].seed = fill_seed;
            }
        }
    }

    /* Clean up remaining slots and verify patterns */
    for (size_t s = 0; s < THREAD_SLOTS; ++s) {
        if (slots[s].ptr != NULL) {
            uint8_t *bytes = (uint8_t *)slots[s].ptr;
            for (size_t b = 0; b < slots[s].size; ++b) {
                uint8_t expected = compute_pattern_byte(tid, slots[s].seed, b);
                if (bytes[b] != expected) {
                    fprintf(stderr, "Thread %d teardown: data corruption at slot %zu, byte %zu\n", tid, s, b);
                    abort();
                }
            }
            my_free(slots[s].ptr);
            slots[s].ptr = NULL;
        }
    }

    return NULL;
}

static void test_multithreaded_stress(void) {
    printf("Running multithreaded stress test (%d threads, %d ops each)...\n",
           NUM_THREADS, THREAD_OPS);
    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int t = 0; t < NUM_THREADS; ++t) {
        args[t].thread_id = t;
        int rc = pthread_create(&threads[t], NULL, thread_worker, &args[t]);
        assert(rc == 0);
    }

    for (int t = 0; t < NUM_THREADS; ++t) {
        int rc = pthread_join(threads[t], NULL);
        assert(rc == 0);
    }

    assert(my_heap_check() == 0);
    allocator_stats_t stats = my_allocator_stats();
    assert(stats.free_block_count == 1);
    assert(stats.fragmentation_metric == 0.0);
    printf("Multithreaded stress test passed.\n");
}

int main(void) {
    assert(my_allocator_init(16 * 1024 * 1024) == 0);
    assert(my_heap_check() == 0);

    /* 1. Basic allocation and data verification */
    char *message = my_malloc(32);
    assert(message != NULL);
    strcpy(message, "allocator works");
    assert(strcmp(message, "allocator works") == 0);
    assert(my_heap_check() == 0);
    my_free(message);
    assert(my_heap_check() == 0);

    /* 2. Splitting verification */
    void *small = my_malloc(64);
    void *another = my_malloc(64);
    assert(small != NULL && another != NULL);
    assert(my_heap_check() == 0);
    my_free(small);
    my_free(another);
    assert(my_heap_check() == 0);

    /* 3. Coalescing verification */
    void *a = my_malloc(4096);
    void *b = my_malloc(4096);
    void *c = my_malloc(4096);
    assert(a && b && c);
    my_free(a);
    my_free(b);
    assert(my_allocator_stats().largest_free_block >= 8192);
    assert(my_heap_check() == 0);
    my_free(c);
    assert(my_heap_check() == 0);

    /* 4. Realloc in-place and fallback verification */
    char *grow = my_malloc(32);
    strcpy(grow, "preserve this string");
    grow = my_realloc(grow, 4096);
    assert(grow != NULL && strcmp(grow, "preserve this string") == 0);
    assert(my_heap_check() == 0);
    my_free(grow);
    assert(my_heap_check() == 0);

    /* 5. Calloc zero-initialization and overflow check */
    int *numbers = (int *)my_calloc(100, sizeof(int));
    assert(numbers != NULL);
    for (int i = 0; i < 100; ++i) {
        assert(numbers[i] == 0);
    }
    my_free(numbers);
    errno = 0;
    void *overflow_calloc = my_calloc(SIZE_MAX / 2, SIZE_MAX / 2);
    assert(overflow_calloc == NULL);
    assert(errno == ENOMEM);

    /* 6. Pool exhaustion and errno = ENOMEM */
    errno = 0;
    void *huge = my_malloc(32 * 1024 * 1024); /* Exceeds 16 MiB pool */
    assert(huge == NULL);
    assert(errno == ENOMEM);

    /* 7. Double-free and invalid-free defense */
    void *p = my_malloc(128);
    assert(p != NULL);
    my_free(p);
    errno = 0;
    my_free(p); /* Double free */
    assert(errno == EINVAL);

    int stack_var = 123;
    errno = 0;
    my_free(&stack_var); /* Invalid free outside pool */
    assert(errno == EINVAL);

    errno = 0;
    my_free((void *)0x1234); /* Misaligned invalid address */
    assert(errno == EINVAL);

    /* 8. Canary / Buffer overflow detection */
    uint8_t *buffer = (uint8_t *)my_malloc(32);
    assert(buffer != NULL);
    memset(buffer, 'Z', 32);
    assert(my_heap_check() == 0);
    /* Intentionally corrupt the canary byte immediately following payload */
    buffer[32] = 0x00;
    assert(my_heap_check() != 0); /* Caught by heap check! */
    errno = 0;
    my_free(buffer); /* Caught on free! */
    assert(errno == EFAULT);
    /* Restore canary so pool cleanup succeeds cleanly */
    buffer[32] = 0xDE;
    assert(my_heap_check() == 0);
    my_free(buffer);
    assert(my_heap_check() == 0);

    /* 9. Header overhead and fragmentation metric in stats */
    allocator_stats_t stats_before = my_allocator_stats();
    assert(stats_before.header_overhead_bytes == stats_before.block_count * stats_before.header_size);
    assert(stats_before.free_block_count == 1);
    assert(stats_before.fragmentation_metric == 0.0);

    /* 10. Single-threaded randomized stress test (20,000 ops) */
    enum { SLOT_COUNT = 256, OPERATIONS = 20000 };
    void *slots[SLOT_COUNT] = {0};
    uint32_t seed = 12345;
    for (int i = 0; i < OPERATIONS; ++i) {
        size_t index = next_random(&seed) % SLOT_COUNT;
        if (slots[index] != NULL) {
            my_free(slots[index]);
            slots[index] = NULL;
        } else {
            size_t size = 1 + (next_random(&seed) % 512);
            slots[index] = my_malloc(size);
            assert(slots[index] != NULL);
            memset(slots[index], 0xA5, size);
        }
    }
    for (size_t i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i] != NULL) my_free(slots[i]);
    }
    assert(my_heap_check() == 0);
    allocator_stats_t stats_after = my_allocator_stats();
    assert(stats_after.free_block_count == 1);
    assert(stats_after.fragmentation_metric == 0.0);

    /* 11. Multithreaded stress test under concurrent threads */
    test_multithreaded_stress();

    printf("All allocator tests passed successfully.\n");
    printf("Header overhead per block: %zu bytes.\n", stats_after.header_size);

    my_allocator_destroy();
    return 0;
}
