#ifndef FULMEN_SYNC_H
#define FULMEN_SYNC_H

#include "types.h"

#define MAX_MUTEXES 32U

typedef struct kmutex {
    uint32_t id;
    char name[24];
    volatile int locked;
    int owner_pid;
    uint32_t lock_order_rank; /* Lower rank must be acquired before higher rank */
    int wait_queue[32];
    size_t wait_count;
} kmutex_t;

typedef struct ksem {
    char name[24];
    volatile int count;
    int wait_queue[32];
    size_t wait_count;
} ksem_t;

struct sync_stats {
    uint64_t mutex_acquires;
    uint64_t mutex_contentions;
    uint64_t deadlocks_prevented;
    uint64_t lock_order_warnings;
    uint64_t sem_waits;
    uint64_t sem_posts;
};

void sync_init(void);
void kmutex_init(kmutex_t *m, const char *name, uint32_t lock_order_rank);
int kmutex_lock(kmutex_t *m);
int kmutex_trylock(kmutex_t *m);
void kmutex_unlock(kmutex_t *m);

void ksem_init(ksem_t *s, const char *name, int initial_count);
void ksem_wait(ksem_t *s);
void ksem_post(ksem_t *s);

void sync_get_stats(struct sync_stats *out);
void sync_set_verbose(bool verbose);

#endif /* FULMEN_SYNC_H */
