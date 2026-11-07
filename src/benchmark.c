#include "allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { ALLOCATION_COUNT = 100000, SLOT_COUNT = 1024 };

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static double run_system_allocator(void) {
    void *slots[SLOT_COUNT] = {0};
    uint32_t seed = 7;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ALLOCATION_COUNT; ++i) {
        size_t slot = next_random(&seed) % SLOT_COUNT;
        free(slots[slot]);
        slots[slot] = malloc(16 + (next_random(&seed) % 1008));
        if (slots[slot] == NULL) exit(1);
    }
    for (size_t i = 0; i < SLOT_COUNT; ++i) free(slots[i]);
    clock_gettime(CLOCK_MONOTONIC, &end);
    return elapsed_ms(start, end);
}

static double run_custom_allocator(void) {
    void *slots[SLOT_COUNT] = {0};
    uint32_t seed = 7;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ALLOCATION_COUNT; ++i) {
        size_t slot = next_random(&seed) % SLOT_COUNT;
        my_free(slots[slot]);
        slots[slot] = my_malloc(16 + (next_random(&seed) % 1008));
        if (slots[slot] == NULL) exit(1);
    }
    for (size_t i = 0; i < SLOT_COUNT; ++i) my_free(slots[i]);
    clock_gettime(CLOCK_MONOTONIC, &end);
    return elapsed_ms(start, end);
}

int main(void) {
    if (my_allocator_init(16U * 1024U * 1024U) != 0) return 1;
    assert(my_heap_check() == 0);

    double system_ms = run_system_allocator();
    double custom_ms = run_custom_allocator();

    assert(my_heap_check() == 0);
    allocator_stats_t stats = my_allocator_stats();

    printf("Mixed-size allocation benchmark (%d allocation/replacement operations)\n\n", ALLOCATION_COUNT);
    printf("Allocator              Time (ms)    Operations/s\n");
    printf("system malloc          %9.2f    %.0f\n", system_ms, ALLOCATION_COUNT * 1000.0 / system_ms);
    printf("custom segregated-bins %9.2f    %.0f\n", custom_ms, ALLOCATION_COUNT * 1000.0 / custom_ms);
    printf("\nCustom allocator metadata:\n");
    printf("  Header size: %zu bytes\n", stats.header_size);
    printf("  Total header overhead: %zu bytes (%zu blocks)\n", stats.header_overhead_bytes, stats.block_count);
    printf("  Free blocks after coalescing: %zu\n", stats.free_block_count);
    printf("  Largest free block: %zu bytes\n", stats.largest_free_block);
    printf("  External fragmentation metric: %.4f\n", stats.fragmentation_metric);

    void *first = my_malloc(4096);
    void *second = my_malloc(4096);
    void *third = my_malloc(4096);
    my_free(first);
    my_free(second);
    allocator_stats_t fragmented = my_allocator_stats();
    printf("\nFragmentation check:\n");
    printf("  Largest adjacent-free region = %zu bytes\n", fragmented.largest_free_block);
    printf("  External fragmentation before merge = %.4f\n", fragmented.fragmentation_metric);

    void *merged_allocation = my_malloc(7000);
    printf("  7,000-byte allocation after coalescing: %s\n", merged_allocation ? "succeeded" : "FAILED");
    assert(merged_allocation != NULL);

    my_free(merged_allocation);
    my_free(third);

    assert(my_heap_check() == 0);
    allocator_stats_t final_stats = my_allocator_stats();
    printf("  External fragmentation after full coalescing: %.4f\n", final_stats.fragmentation_metric);

    my_allocator_destroy();
    return 0;
}
