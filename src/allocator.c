#include "allocator.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

#define DEFAULT_POOL_SIZE (16U * 1024U * 1024U)
#define MIN_SPLIT_PAYLOAD alignof(max_align_t)

#define BLOCK_MAGIC_ALLOC 0x414C4F43U /* 'ALOC' */
#define BLOCK_MAGIC_FREE  0x46524545U /* 'FREE' */

#define CANARY_SIZE 4
static const uint8_t CANARY_PATTERN[CANARY_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF};

typedef struct block_header {
    uint32_t magic;                 /* BLOCK_MAGIC_ALLOC or BLOCK_MAGIC_FREE */
    uint32_t requested_size;         /* User-requested payload bytes (for canary check) */
    size_t size;                    /* Usable payload bytes capacity (aligned) */
    struct block_header *next;      /* Physical next block in pool */
    struct block_header *prev;      /* Physical previous block in pool */
    struct block_header *next_free; /* Free-list bin next block */
    struct block_header *prev_free; /* Free-list bin previous block */
    bool is_free;
    max_align_t alignment_guard;    /* Guarantees payload is max_align_t-aligned */
} block_header_t;

/* Segregated free list bins */
static const size_t BIN_SIZES[] = {
    32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
};
#define NUM_SIZE_CLASSES (sizeof(BIN_SIZES) / sizeof(BIN_SIZES[0]))
#define NUM_BINS (NUM_SIZE_CLASSES + 1)
#define LARGE_BIN_INDEX NUM_SIZE_CLASSES

static void *pool = NULL;
static size_t pool_size = 0;
static block_header_t *head = NULL;
static block_header_t *bins[NUM_BINS];
static pthread_mutex_t allocator_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t align_up(size_t value) {
    const size_t alignment = alignof(max_align_t);
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool valid_size(size_t size) {
    return size > 0 && size <= SIZE_MAX - sizeof(block_header_t) - alignof(max_align_t) - CANARY_SIZE;
}

static size_t get_bin_index(size_t size) {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (size <= BIN_SIZES[i]) {
            return i;
        }
    }
    return LARGE_BIN_INDEX;
}

static void bin_insert(block_header_t *block) {
    size_t bin_idx = get_bin_index(block->size);
    block->next_free = bins[bin_idx];
    block->prev_free = NULL;
    if (bins[bin_idx] != NULL) {
        bins[bin_idx]->prev_free = block;
    }
    bins[bin_idx] = block;
}

static void bin_remove(block_header_t *block) {
    size_t bin_idx = get_bin_index(block->size);
    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    } else if (bins[bin_idx] == block) {
        bins[bin_idx] = block->next_free;
    }
    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

static void merge_with_next(block_header_t *block) {
    block_header_t *next = block->next;
    if (next == NULL || !next->is_free) return;
    block->size += sizeof(block_header_t) + next->size;
    block->next = next->next;
    if (next->next != NULL) next->next->prev = block;
}

static void split_block(block_header_t *block, size_t requested) {
    if (block->size < requested + sizeof(block_header_t) + MIN_SPLIT_PAYLOAD) return;
    block_header_t *remainder = (block_header_t *)((char *)(block + 1) + requested);
    remainder->magic = BLOCK_MAGIC_FREE;
    remainder->requested_size = 0;
    remainder->size = block->size - requested - sizeof(block_header_t);
    remainder->next = block->next;
    remainder->prev = block;
    remainder->next_free = NULL;
    remainder->prev_free = NULL;
    remainder->is_free = true;
    if (remainder->next != NULL) remainder->next->prev = remainder;
    block->next = remainder;
    block->size = requested;
    bin_insert(remainder);
}

static void free_block_locked(block_header_t *block) {
    block->is_free = true;
    block->magic = BLOCK_MAGIC_FREE;
    block->requested_size = 0;
    if (block->next != NULL && block->next->is_free) {
        bin_remove(block->next);
        merge_with_next(block);
    }
    if (block->prev != NULL && block->prev->is_free) {
        bin_remove(block->prev);
        block_header_t *prev = block->prev;
        merge_with_next(prev);
        block = prev;
    }
    bin_insert(block);
}

static block_header_t *find_free_block(size_t needed_size) {
    size_t start_bin = get_bin_index(needed_size);
    if (start_bin < LARGE_BIN_INDEX) {
        for (block_header_t *b = bins[start_bin]; b != NULL; b = b->next_free) {
            if (b->size >= needed_size) {
                return b;
            }
        }
        for (size_t i = start_bin + 1; i < LARGE_BIN_INDEX; ++i) {
            if (bins[i] != NULL) {
                return bins[i];
            }
        }
    }
    for (block_header_t *b = bins[LARGE_BIN_INDEX]; b != NULL; b = b->next_free) {
        if (b->size >= needed_size) {
            return b;
        }
    }
    return NULL;
}

static bool is_valid_payload_ptr(const void *ptr) {
    if (ptr == NULL || pool == NULL) return false;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t pool_start = (uintptr_t)pool;
    uintptr_t pool_end = pool_start + pool_size;
    const size_t alignment = alignof(max_align_t);
    if ((p & (alignment - 1)) != 0) return false;
    if (p < pool_start + sizeof(block_header_t) || p >= pool_end) return false;
    return true;
}

static block_header_t *get_block_from_payload(void *ptr) {
    if (!is_valid_payload_ptr(ptr)) return NULL;
    block_header_t *block = ((block_header_t *)ptr) - 1;
    uintptr_t b = (uintptr_t)block;
    if (b < (uintptr_t)pool || b + sizeof(block_header_t) > (uintptr_t)pool + pool_size) {
        return NULL;
    }
    return block;
}

static int init_pool_locked(size_t requested_pool_size) {
    if (pool != NULL) return 0;
    if (requested_pool_size < sizeof(block_header_t) + MIN_SPLIT_PAYLOAD || !valid_size(requested_pool_size)) {
        errno = EINVAL;
        return -1;
    }
    size_t aligned_pool_size = align_up(requested_pool_size);
    void *mapped = mmap(NULL, aligned_pool_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) {
        errno = ENOMEM;
        return -1;
    }
    pool = mapped;
    pool_size = aligned_pool_size;
    head = (block_header_t *)pool;
    head->magic = BLOCK_MAGIC_FREE;
    head->requested_size = 0;
    head->size = pool_size - sizeof(block_header_t);
    head->next = NULL;
    head->prev = NULL;
    head->next_free = NULL;
    head->prev_free = NULL;
    head->is_free = true;

    for (size_t i = 0; i < NUM_BINS; ++i) {
        bins[i] = NULL;
    }
    bin_insert(head);
    return 0;
}

int my_allocator_init(size_t requested_pool_size) {
    pthread_mutex_lock(&allocator_lock);
    int res = init_pool_locked(requested_pool_size);
    pthread_mutex_unlock(&allocator_lock);
    return res;
}

void my_allocator_destroy(void) {
    pthread_mutex_lock(&allocator_lock);
    if (pool != NULL) {
        munmap(pool, pool_size);
    }
    pool = NULL;
    pool_size = 0;
    head = NULL;
    for (size_t i = 0; i < NUM_BINS; ++i) {
        bins[i] = NULL;
    }
    pthread_mutex_unlock(&allocator_lock);
}

void *my_malloc(size_t size) {
    if (!valid_size(size)) {
        if (size > 0) errno = ENOMEM;
        return NULL;
    }

    size_t needed = align_up(size + CANARY_SIZE);

    pthread_mutex_lock(&allocator_lock);
    if (pool == NULL) {
        if (init_pool_locked(DEFAULT_POOL_SIZE) != 0) {
            pthread_mutex_unlock(&allocator_lock);
            errno = ENOMEM;
            return NULL;
        }
    }

    block_header_t *block = find_free_block(needed);
    if (block != NULL) {
        bin_remove(block);
        split_block(block, needed);
        block->is_free = false;
        block->magic = BLOCK_MAGIC_ALLOC;
        block->requested_size = (uint32_t)size;
        memcpy((char *)(block + 1) + size, CANARY_PATTERN, CANARY_SIZE);
        pthread_mutex_unlock(&allocator_lock);
        return block + 1;
    }

    pthread_mutex_unlock(&allocator_lock);
    errno = ENOMEM;
    return NULL;
}

void my_free(void *ptr) {
    if (ptr == NULL) return;

    pthread_mutex_lock(&allocator_lock);
    block_header_t *block = get_block_from_payload(ptr);
    if (block == NULL) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EINVAL;
        return;
    }
    if (block->is_free || block->magic == BLOCK_MAGIC_FREE) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EINVAL; /* Double-free detected */
        return;
    }
    if (block->magic != BLOCK_MAGIC_ALLOC) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EFAULT; /* Heap metadata corrupted */
        return;
    }
    if (memcmp((char *)(block + 1) + block->requested_size, CANARY_PATTERN, CANARY_SIZE) != 0) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EFAULT; /* Buffer overflow / canary corrupted */
        return;
    }

    free_block_locked(block);
    pthread_mutex_unlock(&allocator_lock);
}

void *my_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return my_malloc(size);
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }
    if (!valid_size(size)) {
        errno = ENOMEM;
        return NULL;
    }

    size_t needed = align_up(size + CANARY_SIZE);

    pthread_mutex_lock(&allocator_lock);
    block_header_t *block = get_block_from_payload(ptr);
    if (block == NULL || block->is_free || block->magic != BLOCK_MAGIC_ALLOC) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EINVAL;
        return NULL;
    }
    if (memcmp((char *)(block + 1) + block->requested_size, CANARY_PATTERN, CANARY_SIZE) != 0) {
        pthread_mutex_unlock(&allocator_lock);
        errno = EFAULT;
        return NULL;
    }

    if (block->size >= needed) {
        if (block->size >= needed + sizeof(block_header_t) + MIN_SPLIT_PAYLOAD) {
            split_block(block, needed);
        }
        block->requested_size = (uint32_t)size;
        memcpy((char *)(block + 1) + size, CANARY_PATTERN, CANARY_SIZE);
        pthread_mutex_unlock(&allocator_lock);
        return ptr;
    }

    if (block->next != NULL && block->next->is_free &&
        block->size + sizeof(block_header_t) + block->next->size >= needed) {
        bin_remove(block->next);
        merge_with_next(block);
        if (block->size >= needed + sizeof(block_header_t) + MIN_SPLIT_PAYLOAD) {
            split_block(block, needed);
        }
        block->requested_size = (uint32_t)size;
        memcpy((char *)(block + 1) + size, CANARY_PATTERN, CANARY_SIZE);
        pthread_mutex_unlock(&allocator_lock);
        return ptr;
    }

    const size_t copy_bytes = (block->requested_size < size) ? block->requested_size : size;
    pthread_mutex_unlock(&allocator_lock);

    void *replacement = my_malloc(size);
    if (replacement == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(replacement, ptr, copy_bytes);
    my_free(ptr);
    return replacement;
}

void *my_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = count * size;
    void *ptr = my_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

allocator_stats_t my_allocator_stats(void) {
    allocator_stats_t stats = {0};
    pthread_mutex_lock(&allocator_lock);
    stats.pool_size = pool_size;
    stats.header_size = sizeof(block_header_t);
    for (block_header_t *block = head; block != NULL; block = block->next) {
        ++stats.block_count;
        if (block->is_free) {
            ++stats.free_block_count;
            stats.free_bytes += block->size;
            if (block->size > stats.largest_free_block) {
                stats.largest_free_block = block->size;
            }
        } else {
            stats.allocated_bytes += block->size;
        }
    }
    stats.header_overhead_bytes = stats.block_count * stats.header_size;
    if (stats.free_bytes > 0) {
        stats.fragmentation_metric = 1.0 - ((double)stats.largest_free_block / (double)stats.free_bytes);
    } else {
        stats.fragmentation_metric = 0.0;
    }
    pthread_mutex_unlock(&allocator_lock);
    return stats;
}

int my_heap_check(void) {
    pthread_mutex_lock(&allocator_lock);
    if (pool == NULL) {
        pthread_mutex_unlock(&allocator_lock);
        return 0;
    }

    if (head != (block_header_t *)pool) {
        pthread_mutex_unlock(&allocator_lock);
        return 1;
    }

    size_t total_bytes = 0;
    size_t count_free_blocks = 0;

    for (block_header_t *block = head; block != NULL; block = block->next) {
        uintptr_t b = (uintptr_t)block;
        uintptr_t p_start = (uintptr_t)pool;
        uintptr_t p_end = p_start + pool_size;

        if (b < p_start || b + sizeof(block_header_t) > p_end) {
            pthread_mutex_unlock(&allocator_lock);
            return 2;
        }

        if (((uintptr_t)(block + 1) % alignof(max_align_t)) != 0) {
            pthread_mutex_unlock(&allocator_lock);
            return 3;
        }

        if (block->is_free) {
            if (block->magic != BLOCK_MAGIC_FREE) {
                pthread_mutex_unlock(&allocator_lock);
                return 4;
            }
            count_free_blocks++;
        } else {
            if (block->magic != BLOCK_MAGIC_ALLOC) {
                pthread_mutex_unlock(&allocator_lock);
                return 5;
            }
            if (memcmp((char *)(block + 1) + block->requested_size, CANARY_PATTERN, CANARY_SIZE) != 0) {
                pthread_mutex_unlock(&allocator_lock);
                return 6;
            }
        }

        if (block->next != NULL) {
            if (block->next->prev != block) {
                pthread_mutex_unlock(&allocator_lock);
                return 7;
            }
            if ((char *)(block + 1) + block->size != (char *)block->next) {
                pthread_mutex_unlock(&allocator_lock);
                return 8;
            }
            if (block->is_free && block->next->is_free) {
                pthread_mutex_unlock(&allocator_lock);
                return 9;
            }
        }

        if (block->prev != NULL && block->prev->next != block) {
            pthread_mutex_unlock(&allocator_lock);
            return 10;
        }

        total_bytes += sizeof(block_header_t) + block->size;
    }

    if (total_bytes != pool_size) {
        pthread_mutex_unlock(&allocator_lock);
        return 11;
    }

    size_t bin_free_count = 0;
    for (size_t i = 0; i < NUM_BINS; ++i) {
        for (block_header_t *fb = bins[i]; fb != NULL; fb = fb->next_free) {
            if (!fb->is_free || fb->magic != BLOCK_MAGIC_FREE) {
                pthread_mutex_unlock(&allocator_lock);
                return 12;
            }
            if (fb->next_free != NULL && fb->next_free->prev_free != fb) {
                pthread_mutex_unlock(&allocator_lock);
                return 13;
            }
            if (fb->prev_free != NULL && fb->prev_free->next_free != fb) {
                pthread_mutex_unlock(&allocator_lock);
                return 14;
            }
            if (get_bin_index(fb->size) != i) {
                pthread_mutex_unlock(&allocator_lock);
                return 15;
            }
            bin_free_count++;
        }
    }

    if (bin_free_count != count_free_blocks) {
        pthread_mutex_unlock(&allocator_lock);
        return 16;
    }

    pthread_mutex_unlock(&allocator_lock);
    return 0;
}
