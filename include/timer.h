#ifndef FULMEN_TIMER_H
#define FULMEN_TIMER_H

#include "types.h"

#define TIMER_HZ 100U /* 10 ms tick */

void timer_init(void);
void timer_handle_irq(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ms(void);
uint64_t timer_get_freq(void);

#endif /* FULMEN_TIMER_H */
