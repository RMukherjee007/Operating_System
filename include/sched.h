#ifndef FULMEN_SCHED_H
#define FULMEN_SCHED_H

#include "types.h"

#define MAX_TASKS        32U
#define TASK_STACK_SIZE  65536U
#define MLFQ_LEVELS      4U
#define MLFQ_BOOST_TICKS 40U

enum sched_policy {
    SCHED_RR = 0,
    SCHED_PRIO = 1,
    SCHED_MLFQ = 2
};

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_ZOMBIE
};

/* Callee-saved register context for cpu_switch_to (must match boot/context.S) */
struct cpu_context {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t fp;  /* x29 */
    uint64_t sp;
    uint64_t pc;  /* x30 / lr */
};

struct kmutex;

struct task_struct {
    int pid;
    char name[32];
    enum task_state state;
    uint8_t priority;         /* 0 = highest, 7 = lowest */
    uint8_t mlfq_level;       /* 0..3 */
    uint32_t time_slice;      /* configured quantum */
    uint32_t ticks_remaining; /* ticks left in current quantum */
    uint64_t total_ticks;     /* total CPU ticks accumulated */
    uint64_t wake_tick;       /* tick to wake if sleeping */
    uint32_t switches;        /* context switches into this task */

    void (*entry)(void *);
    void *arg;

    struct kmutex *waiting_on_mutex;
    uint32_t held_mutex_mask;

    struct cpu_context ctx;
    uint8_t *stack_base;
};

/* Assembly helpers in boot/context.S */
void cpu_switch_to(struct cpu_context *prev, struct cpu_context *next);
int enter_el0(uint64_t entry, uint64_t user_sp, uint64_t argc, uint64_t argv);
void exit_el0(int exit_code);

void sched_init(void);
int task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority);
void task_exit(void);
int task_kill(int pid);
void task_yield(void);
void task_sleep_ms(uint64_t ms);

void scheduler_tick(void);
bool sched_should_preempt(void);
void schedule(void);

void sched_set_policy(enum sched_policy policy);
enum sched_policy sched_get_policy(void);
const char *sched_policy_name(enum sched_policy policy);
struct task_struct *sched_current(void);
struct task_struct *sched_get_task_table(size_t *max_count);
void sched_reap_zombies(void);

/* Optional execution trace log for sched_test visualization */
void sched_trace_start(bool enable);
void sched_trace_dump(void);

#endif /* FULMEN_SCHED_H */
