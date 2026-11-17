#ifndef FULMEN_HEAP_H
#define FULMEN_HEAP_H

#include "types.h"

struct heap_stats {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t block_count;
    size_t free_block_count;
    size_t alloc_calls;
    size_t free_calls;
};

void heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *ptr, size_t new_size);
void kfree(void *ptr);

void heap_get_stats(struct heap_stats *out);
void heap_dump(void);

#endif /* FULMEN_HEAP_H */
