#include "sync.h"
#include "sched.h"
#include "irq.h"
#include "uart.h"
#include "string.h"

static kmutex_t *mutex_registry[MAX_MUTEXES];
static uint32_t next_mutex_id = 0;
static struct sync_stats g_sync_stats;
static bool sync_verbose = true;

static struct task_struct *find_task_by_pid(int pid) {
    size_t max_tasks = 0;
    struct task_struct *table = sched_get_task_table(&max_tasks);
    for (size_t i = 0; i < max_tasks; i++) {
        if (table[i].state != TASK_UNUSED &&
            table[i].state != TASK_ZOMBIE &&
            table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}

void sync_init(void) {
    memset(mutex_registry, 0, sizeof(mutex_registry));
    memset(&g_sync_stats, 0, sizeof(g_sync_stats));
    next_mutex_id = 0;
    sync_verbose = true;
}

void sync_set_verbose(bool verbose) {
    sync_verbose = verbose;
}

void kmutex_init(kmutex_t *m, const char *name, uint32_t lock_order_rank) {
    if (!m) return;
    uint64_t flags = irq_save();
    memset(m, 0, sizeof(*m));
    m->id = next_mutex_id % MAX_MUTEXES;
    next_mutex_id++;
    strncpy(m->name, name ? name : "mutex", sizeof(m->name) - 1);
    m->owner_pid = -1;
    m->lock_order_rank = lock_order_rank;
    mutex_registry[m->id] = m;
    irq_restore(flags);
}

static bool detect_wfg_cycle(struct task_struct *requester, kmutex_t *target) {
    int cursor_pid = target->owner_pid;
    for (size_t depth = 0; depth < MAX_TASKS; depth++) {
        if (cursor_pid < 0) return false;
        if (cursor_pid == requester->pid) return true;
        struct task_struct *t = find_task_by_pid(cursor_pid);
        if (!t || !t->waiting_on_mutex) return false;
        cursor_pid = t->waiting_on_mutex->owner_pid;
    }
    return false;
}

static void check_lock_order(struct task_struct *me, kmutex_t *m) {
    if (m->lock_order_rank == 0) return;
    for (uint32_t id = 0; id < MAX_MUTEXES; id++) {
        if ((me->held_mutex_mask & (1U << id)) && mutex_registry[id]) {
            kmutex_t *held = mutex_registry[id];
            if (held != m && held->lock_order_rank > m->lock_order_rank) {
                g_sync_stats.lock_order_warnings++;
                if (sync_verbose) {
                    kprintf(ANSI_YELLOW
                            "  [LOCK-ORDER WARNING] PID %d (%s) holds '%s'(rank=%u), requesting '%s'(rank=%u)\n"
                            ANSI_RESET,
                            me->pid, me->name, held->name, held->lock_order_rank,
                            m->name, m->lock_order_rank);
                }
            }
        }
    }
}

int kmutex_lock(kmutex_t *m) {
    if (!m) return -EINVAL;

    uint64_t flags = irq_save();
    struct task_struct *me = sched_current();

    while (m->locked) {
        if (m->owner_pid == me->pid) {
            irq_restore(flags);
            return -EDEADLK;
        }

        if (detect_wfg_cycle(me, m)) {
            g_sync_stats.deadlocks_prevented++;
            if (sync_verbose) {
                struct task_struct *owner = find_task_by_pid(m->owner_pid);
                kprintf(ANSI_RED
                        "  [DEADLOCK PREVENTED] Wait-For Graph cycle detected!\n"
                        "    Requester: PID %d (%s) -> wants '%s' (held by PID %d %s)\n"
                        "    Chain leads back to PID %d -> denying lock (-EDEADLK)\n"
                        ANSI_RESET,
                        me->pid, me->name, m->name, m->owner_pid,
                        owner ? owner->name : "?", me->pid);
            }
            irq_restore(flags);
            return -EDEADLK;
        }

        check_lock_order(me, m);

        g_sync_stats.mutex_contentions++;
        me->waiting_on_mutex = m;
        if (m->wait_count < ARRAY_SIZE(m->wait_queue)) {
            m->wait_queue[m->wait_count++] = me->pid;
        }
        me->state = TASK_BLOCKED;
        schedule();
        me->waiting_on_mutex = NULL;
    }

    check_lock_order(me, m);
    m->locked = 1;
    m->owner_pid = me->pid;
    me->held_mutex_mask |= (1U << m->id);
    g_sync_stats.mutex_acquires++;

    irq_restore(flags);
    return 0;
}

int kmutex_trylock(kmutex_t *m) {
    if (!m) return -EINVAL;
    uint64_t flags = irq_save();
    if (m->locked) {
        irq_restore(flags);
        return -EBUSY;
    }
    struct task_struct *me = sched_current();
    m->locked = 1;
    m->owner_pid = me->pid;
    me->held_mutex_mask |= (1U << m->id);
    g_sync_stats.mutex_acquires++;
    irq_restore(flags);
    return 0;
}

void kmutex_unlock(kmutex_t *m) {
    if (!m) return;
    uint64_t flags = irq_save();
    struct task_struct *me = sched_current();
    if (!m->locked || m->owner_pid != me->pid) {
        irq_restore(flags);
        return;
    }

    me->held_mutex_mask &= ~(1U << m->id);
    m->locked = 0;
    m->owner_pid = -1;

    /* Wake all blocked waiters so the scheduler runs the next eligible waiter */
    for (size_t i = 0; i < m->wait_count; i++) {
        struct task_struct *w = find_task_by_pid(m->wait_queue[i]);
        if (w && w->state == TASK_BLOCKED) {
            w->state = TASK_READY;
        }
    }
    m->wait_count = 0;

    irq_restore(flags);
}

void ksem_init(ksem_t *s, const char *name, int initial_count) {
    if (!s) return;
    uint64_t flags = irq_save();
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name ? name : "sem", sizeof(s->name) - 1);
    s->count = initial_count;
    irq_restore(flags);
}

void ksem_wait(ksem_t *s) {
    if (!s) return;
    uint64_t flags = irq_save();
    struct task_struct *me = sched_current();
    g_sync_stats.sem_waits++;

    while (s->count <= 0) {
        if (s->wait_count < ARRAY_SIZE(s->wait_queue)) {
            s->wait_queue[s->wait_count++] = me->pid;
        }
        me->state = TASK_BLOCKED;
        schedule();
    }
    s->count--;
    irq_restore(flags);
}

void ksem_post(ksem_t *s) {
    if (!s) return;
    uint64_t flags = irq_save();
    g_sync_stats.sem_posts++;
    s->count++;
    if (s->wait_count > 0) {
        int wake_pid = s->wait_queue[0];
        for (size_t i = 1; i < s->wait_count; i++) {
            s->wait_queue[i - 1] = s->wait_queue[i];
        }
        s->wait_count--;
        struct task_struct *w = find_task_by_pid(wake_pid);
        if (w && w->state == TASK_BLOCKED) {
            w->state = TASK_READY;
        }
    }
    irq_restore(flags);
}

void sync_get_stats(struct sync_stats *out) {
    if (!out) return;
    uint64_t flags = irq_save();
    *out = g_sync_stats;
    irq_restore(flags);
}
