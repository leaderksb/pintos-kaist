#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/palloc.h"

/* See [8254] for hardware details of the 8254 timer chip. */
// 8254 타이머 칩은 IBM PC 호환 컴퓨터에서 시스템 타이머로 일반적으로 사용되는 장치
// 타이머가 초당 발생시키는 인터럽트의 횟수, 즉 타이머의 주파수를 설정하는 값
// 권장은 19 이상, 1000 이하로 설정
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

//

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
// void timer_init(void)
// {
// 	/* 8254 input frequency divided by TIMER_FREQ, rounded to
// 	   nearest. */
// 	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

// 	outb(0x43, 0x34); /* CW: counter 0, LSB then MSB, mode 2, binary. */
// 	outb(0x40, count & 0xff);
// 	outb(0x40, count >> 8);

// 	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
// }
void timer_init(void)
{
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb(0x43, 0x34); /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb(0x40, count & 0xff);
	outb(0x40, count >> 8);

	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
// 시스템의 타이머를 보정
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10; // unsigned 정수를 왼쪽으로 10 비트 이동시키는 연산 = 1024
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	// 프로세서가 초당 수행할 수 있는 루프의 수
	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
/* OS 부팅 이후 타이머 틱 수를 반환 */
// ticks 변수는 시스템 타이머 인터럽트 핸들러에서 각 타이머 틱마다 증가시키는 값
int64_t
timer_ticks(void)
{
	// 현재의 인터럽트 레벨을 비활성화하여 인터럽트가 발생하지 않도록 하고
	enum intr_level old_level = intr_disable();
	// 전역 변수 ticks 저장
	int64_t t = ticks;
	// 원래의 인터럽트 레벨을 복원
	intr_set_level(old_level);
	barrier();
	// ticks 값 반환
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
// 입력받은 시간과 현재 시간 사이에 경과한 타이머 틱의 수를 반환
int64_t
timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
// 프로그램의 실행을 대략적으로 ticks 타이머 틱 동안 정지
void timer_sleep(int64_t ticks_much)
{
	int64_t ticks_now = timer_ticks();

	ASSERT(intr_get_level() == INTR_ON);
	// 양보를 얼마나 했는가
	// while (timer_elapsed(start) < ticks)
	// {
	// 	thread_yield();
	// }
	if (timer_elapsed(ticks_now) < ticks)
	{
		thread_sleep(ticks_now + ticks_much);
	}
}

/* Suspends execution for approximately MS milliseconds. */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
/* 타이머 통계를 출력합니다.*/
// 운영 체제가 부팅된 이후부터 현재까지의 타이머 틱 수를 출력
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks()); // 타이머 틱 수
}

/* Timer interrupt handler. */
// 타이머 인터럽트가 발생할 때마다 스레드 관련 통계를 업데이트하고 스레드 선점을 수행
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	// ticks 변수를 증가 시켜
	// 각 타이머 틱마다 호출되어 스레드 관련 통계를 업데이트한 후
	// 필요한 경우 스레드의 선점을 수행
	ticks++;
	thread_tick();

	// int64_t ticks_now = timer_ticks();
	thread_wake(ticks);
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
// 시스템의 수행 능력을 판단
// true : 주어진 loops 수의 루프를 수행하는데 한 타이머 틱보다 더 많은 시간이 걸렸다
// false: 주어진 loops 수의 루프를 수행하는데 한 타이머 틱보다 적거나 같은 시간이 걸렸다는 것이므로,
// 이는 시스템의 수행 능력이 상대적으로 높다는 것
static bool
too_many_loops(unsigned loops)
{
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	barrier();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep(int64_t num, int32_t denom)
{
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep(ticks);
	}
	else
	{
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
