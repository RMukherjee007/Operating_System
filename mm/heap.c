#include "heap.h"
#include "pmm.h"
#include "irq.h"
#include "uart.h"
#include "string.h"

#define HEAP_MAGIC 0xF01DCAFEU

struct heap_block {
    uint32_t magic;
    uint32_t is_free;
    size_t size; /* Payload size in bytes */
    struct heap_block *next;
    struct heap_block *prev;
} __attribute__((aligned(16)));

static struct heap_block *heap_head = NULL;
static size_t stat_alloc_calls = 0;
static size_t stat_free_calls = 0;

void heap_init(void) {
    heap_head = (struct heap_block *)KERNEL_HEAP_BASE;
    heap_head->magic = HEAP_MAGIC;
    heap_head->is_free = 1;
    heap_head->size = KERNEL_HEAP_SIZE - sizeof(struct heap_block);
    heap_head->next = NULL;
    heap_head->prev = NULL;
    stat_alloc_calls = 0;
    stat_free_calls = 0;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size, 16);

    uint64_t flags = irq_save();
    struct heap_block *cur = heap_head;
    while (cur) {
        if (cur->magic != HEAP_MAGIC) {
            irq_restore(flags);
            kprintf(ANSI_RED "[HEAP CORRUPTION] Invalid block magic at %p\n" ANSI_RESET, cur);
            return NULL;
        }
        if (cur->is_free && cur->size >= size) {
            /* Split block if remaining space can hold a new header + >=16 bytes */
            if (cur->size >= size + sizeof(struct heap_block) + 16) {
                struct heap_block *split = (struct heap_block *)((uint8_t *)cur + sizeof(struct heap_block) + size);
                split->magic = HEAP_MAGIC;
                split->is_free = 1;
                split->size = cur->size - size - sizeof(struct heap_block);
                split->next = cur->next;
                split->prev = cur;
                if (cur->next) cur->next->prev = split;
                cur->next = split;
                cur->size = size;
            }
            cur->is_free = 0;
            stat_alloc_calls++;
            irq_restore(flags);
            return (void *)((uint8_t *)cur + sizeof(struct heap_block));
        }
        cur = cur->next;
    }
    irq_restore(flags);
    return NULL;
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct heap_block *blk = (struct heap_block *)((uint8_t *)ptr - sizeof(struct heap_block));
    if (blk->magic != HEAP_MAGIC) return NULL;
    if (blk->size >= new_size) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, blk->size);
    kfree(ptr);
    return new_ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t flags = irq_save();
    struct heap_block *blk = (struct heap_block *)((uint8_t *)ptr - sizeof(struct heap_block));
    if (blk->magic != HEAP_MAGIC) {
        irq_restore(flags);
        kprintf(ANSI_RED "[HEAP ERROR] kfree bad pointer %p\n" ANSI_RESET, ptr);
        return;
    }
    if (blk->is_free) {
        irq_restore(flags);
        return;
    }

    blk->is_free = 1;
    stat_free_calls++;

    /* Coalesce with next block if free */
    if (blk->next && blk->next->is_free) {
        struct heap_block *nxt = blk->next;
        blk->size += sizeof(struct heap_block) + nxt->size;
        blk->next = nxt->next;
        if (nxt->next) nxt->next->prev = blk;
    }

    /* Coalesce with previous block if free */
    if (blk->prev && blk->prev->is_free) {
        struct heap_block *prv = blk->prev;
        prv->size += sizeof(struct heap_block) + blk->size;
        prv->next = blk->next;
        if (blk->next) blk->next->prev = prv;
    }

    irq_restore(flags);
}

void heap_get_stats(struct heap_stats *out) {
    if (!out) return;
    uint64_t flags = irq_save();
    memset(out, 0, sizeof(*out));
    out->total_bytes = KERNEL_HEAP_SIZE;
    out->alloc_calls = stat_alloc_calls;
    out->free_calls = stat_free_calls;

    struct heap_block *cur = heap_head;
    while (cur && cur->magic == HEAP_MAGIC) {
        out->block_count++;
        if (cur->is_free) {
            out->free_bytes += cur->size;
            out->free_block_count++;
        } else {
            out->used_bytes += cur->size + sizeof(struct heap_block);
        }
        cur = cur->next;
    }
    irq_restore(flags);
}

void heap_dump(void) {
    uint64_t flags = irq_save();
    kprintf("Kernel Heap Blocks (Base=0x%08lx, Size=%lu KB):\n",
            KERNEL_HEAP_BASE, KERNEL_HEAP_SIZE / 1024UL);
    struct heap_block *cur = heap_head;
    int idx = 0;
    while (cur && cur->magic == HEAP_MAGIC && idx < 20) {
        kprintf("  #%02d [%p] %-4s size=%lu bytes\n",
                idx++, cur, cur->is_free ? "FREE" : "USED", cur->size);
        cur = cur->next;
    }
    if (cur) kprintf("  ... (more blocks)\n");
    irq_restore(flags);
}
