#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "irq.h"
#include "uart.h"
#include "string.h"

extern void task_trampoline(void);

static struct task_struct tasks[MAX_TASKS];
static int current_idx = 0;
static int next_pid = 1;
static enum sched_policy active_policy = SCHED_MLFQ;
static volatile bool need_resched = false;

static const uint32_t mlfq_quanta[MLFQ_LEVELS] = { 2U, 4U, 8U, 16U };
static const uint32_t rr_quantum = 4U;

/* Trace log for sched_test visualization */
#define TRACE_MAX 64U
struct sched_trace_entry {
    uint64_t tick;
    int pid;
    char name[16];
    uint8_t level;
    uint8_t prio;
};

static bool trace_enabled = false;
static struct sched_trace_entry trace_log[TRACE_MAX];
static size_t trace_count = 0;

static uint32_t quantum_for_task(const struct task_struct *t) {
    if (active_policy == SCHED_MLFQ) {
        uint8_t lvl = t->mlfq_level;
        if (lvl >= MLFQ_LEVELS) lvl = MLFQ_LEVELS - 1;
        return mlfq_quanta[lvl];
    }
    return rr_quantum;
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));
    current_idx = 0;
    next_pid = 1;
    active_policy = SCHED_MLFQ;
    need_resched = false;

    /* Task 0 is the boot/idle thread running on the initial kernel stack */
    tasks[0].pid = 0;
    strcpy(tasks[0].name, "idle");
    tasks[0].state = TASK_RUNNING;
    tasks[0].priority = 7;
    tasks[0].mlfq_level = 3;
    tasks[0].time_slice = rr_quantum;
    tasks[0].ticks_remaining = rr_quantum;
    tasks[0].stack_base = NULL; /* Boot stack */
}

void sched_reap_zombies(void) {
    uint64_t flags = irq_save();
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE) {
            if (tasks[i].stack_base) {
                kfree(tasks[i].stack_base);
            }
            memset(&tasks[i], 0, sizeof(tasks[i]));
            tasks[i].state = TASK_UNUSED;
        }
    }
    irq_restore(flags);
}

int task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority) {
    sched_reap_zombies();
    uint64_t flags = irq_save();

    int slot = -1;
    for (int i = 1; i < (int)MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        irq_restore(flags);
        return -ENOMEM;
    }

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        irq_restore(flags);
        return -ENOMEM;
    }
    memset(stack, 0, TASK_STACK_SIZE);

    struct task_struct *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    strncpy(t->name, name ? name : "thread", sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->priority = (priority > 7U) ? 7U : priority;
    t->mlfq_level = 0;
    t->time_slice = quantum_for_task(t);
    t->ticks_remaining = t->time_slice;
    t->entry = entry;
    t->arg = arg;
    t->stack_base = stack;

    uint64_t stack_top = ALIGN_DOWN((uint64_t)(stack + TASK_STACK_SIZE), 16);
    t->ctx.sp = stack_top;
    t->ctx.fp = 0;
    t->ctx.x19 = (uint64_t)entry;
    t->ctx.x20 = (uint64_t)arg;
    t->ctx.pc = (uint64_t)task_trampoline;

    irq_restore(flags);
    return t->pid;
}

void task_exit(void) {
    irq_disable();
    struct task_struct *cur = &tasks[current_idx];
    cur->state = TASK_ZOMBIE;
    schedule();
    for (;;) __asm__ volatile("wfi");
}

int task_kill(int pid) {
    if (pid <= 1) return -EPERM; /* Protect idle (0) and shell (1) */
    uint64_t flags = irq_save();
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            if ((int)i == current_idx) {
                irq_restore(flags);
                task_exit();
                return 0;
            }
            tasks[i].state = TASK_ZOMBIE;
            irq_restore(flags);
            sched_reap_zombies();
            return 0;
        }
    }
    irq_restore(flags);
    return -ESRCH;
}

void task_yield(void) {
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task_sleep_ms(uint64_t ms) {
    uint64_t ticks = (ms * TIMER_HZ + 999UL) / 1000UL;
    if (ticks == 0) ticks = 1;
    uint64_t flags = irq_save();
    struct task_struct *cur = &tasks[current_idx];
    if (current_idx != 0) {
        cur->wake_tick = timer_get_ticks() + ticks;
        cur->state = TASK_SLEEPING;
        schedule();
    }
    irq_restore(flags);
}

static int pick_next_index(void) {
    if (active_policy == SCHED_RR) {
        for (int offset = 1; offset <= (int)MAX_TASKS; offset++) {
            int idx = (current_idx + offset) % (int)MAX_TASKS;
            if (idx == 0) continue;
            if (tasks[idx].state == TASK_READY) return idx;
        }
        return 0;
    }

    if (active_policy == SCHED_PRIO) {
        uint8_t best_prio = 255;
        int best_idx = 0;
        for (int offset = 1; offset <= (int)MAX_TASKS; offset++) {
            int idx = (current_idx + offset) % (int)MAX_TASKS;
            if (idx == 0) continue;
            if (tasks[idx].state == TASK_READY && tasks[idx].priority < best_prio) {
                best_prio = tasks[idx].priority;
                best_idx = idx;
            }
        }
        return best_idx;
    }

    /* SCHED_MLFQ: choose lowest queue level (0 is highest) */
    uint8_t best_lvl = 255;
    int best_idx = 0;
    for (int offset = 1; offset <= (int)MAX_TASKS; offset++) {
        int idx = (current_idx + offset) % (int)MAX_TASKS;
        if (idx == 0) continue;
        if (tasks[idx].state == TASK_READY && tasks[idx].mlfq_level < best_lvl) {
            best_lvl = tasks[idx].mlfq_level;
            best_idx = idx;
        }
    }
    return best_idx;
}

void scheduler_tick(void) {
    uint64_t now = timer_get_ticks();
    struct task_struct *cur = &tasks[current_idx];
    cur->total_ticks++;

    if (trace_enabled && trace_count < TRACE_MAX && current_idx != 0) {
        struct sched_trace_entry *e = &trace_log[trace_count++];
        e->tick = now;
        e->pid = cur->pid;
        strncpy(e->name, cur->name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->level = cur->mlfq_level;
        e->prio = cur->priority;
    }

    bool higher_woke = false;
    bool any_ready = false;

    /* Wake up sleeping tasks */
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= now) {
            tasks[i].state = TASK_READY;
            tasks[i].ticks_remaining = quantum_for_task(&tasks[i]);
            any_ready = true;
            if (active_policy == SCHED_PRIO && tasks[i].priority < cur->priority) {
                higher_woke = true;
            }
            if (active_policy == SCHED_MLFQ && tasks[i].mlfq_level < cur->mlfq_level) {
                higher_woke = true;
            }
        } else if (tasks[i].state == TASK_READY) {
            any_ready = true;
        }
    }

    /* MLFQ anti-starvation priority boost */
    if (active_policy == SCHED_MLFQ && (now % MLFQ_BOOST_TICKS) == 0) {
        for (size_t i = 1; i < MAX_TASKS; i++) {
            if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE) {
                tasks[i].mlfq_level = 0;
                tasks[i].time_slice = mlfq_quanta[0];
                if (tasks[i].ticks_remaining > mlfq_quanta[0]) {
                    tasks[i].ticks_remaining = mlfq_quanta[0];
                }
            }
        }
    }

    if (current_idx == 0) {
        if (any_ready) need_resched = true;
        return;
    }

    if (higher_woke) {
        need_resched = true;
    }

    if (cur->ticks_remaining > 0) {
        cur->ticks_remaining--;
    }

    if (cur->ticks_remaining == 0) {
        if (active_policy == SCHED_MLFQ && (uint32_t)cur->mlfq_level + 1U < MLFQ_LEVELS) {
            cur->mlfq_level++;
        }
        cur->time_slice = quantum_for_task(cur);
        cur->ticks_remaining = cur->time_slice;
        need_resched = true;
    }
}

bool sched_should_preempt(void) {
    return need_resched;
}

void schedule(void) {
    uint64_t flags = irq_save();
    need_resched = false;

    int prev_idx = current_idx;
    struct task_struct *prev = &tasks[prev_idx];

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }

    int next_idx = pick_next_index();
    struct task_struct *next = &tasks[next_idx];

    next->state = TASK_RUNNING;
    if (next->ticks_remaining == 0) {
        next->time_slice = quantum_for_task(next);
        next->ticks_remaining = next->time_slice;
    }

    if (next_idx != prev_idx) {
        next->switches++;
        current_idx = next_idx;
        cpu_switch_to(&prev->ctx, &next->ctx);
    }

    irq_restore(flags);
}

void sched_set_policy(enum sched_policy policy) {
    uint64_t flags = irq_save();
    active_policy = policy;
    for (size_t i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_ZOMBIE) {
            tasks[i].mlfq_level = 0;
            tasks[i].time_slice = quantum_for_task(&tasks[i]);
            tasks[i].ticks_remaining = tasks[i].time_slice;
        }
    }
    irq_restore(flags);
}

enum sched_policy sched_get_policy(void) {
    return active_policy;
}

const char *sched_policy_name(enum sched_policy policy) {
    switch (policy) {
        case SCHED_RR:   return "RR";
        case SCHED_PRIO: return "PRIO";
        case SCHED_MLFQ: return "MLFQ";
        default:         return "UNK";
    }
}

struct task_struct *sched_current(void) {
    return &tasks[current_idx];
}

struct task_struct *sched_get_task_table(size_t *max_count) {
    if (max_count) *max_count = MAX_TASKS;
    return tasks;
}

void sched_trace_start(bool enable) {
    uint64_t flags = irq_save();
    if (enable) trace_count = 0;
    trace_enabled = enable;
    irq_restore(flags);
}

void sched_trace_dump(void) {
    kprintf("  Tick Timeline (%lu recorded ticks under %s):\n",
            trace_count, sched_policy_name(active_policy));
    for (size_t i = 0; i < trace_count; i++) {
        kprintf("    [Tick %3lu] PID %-2d %-14s (Prio=%u, MLFQ=Q%u)\n",
                trace_log[i].tick,
                trace_log[i].pid,
                trace_log[i].name,
                trace_log[i].prio,
                trace_log[i].level);
    }
}
