#ifndef TIMER_H
#define TIMER_H

#define TIME_LT(a,b)		((long)((a)-(b)) < 0)
#define TIME_LEQ(a,b)		((long)((a)-(b)) <= 0)
#define TIME_GT(a,b)		((long)((a)-(b)) > 0)
#define TIME_GEQ(a,b)		((long)((a)-(b)) >= 0)

typedef	unsigned long timeq_t;
typedef timeq_t clock_time_t;
extern timeq_t Now;

#define CLOCK_SECOND	16UL

struct timer {
	clock_time_t start;
	clock_time_t interval;
};

int timer_init(void);
timeq_t get_time(void);
void timer_set(struct timer *t, clock_time_t interval);
void timer_reset(struct timer *t);
void timer_restart(struct timer *t);
int timer_expired(struct timer *t);

#endif
