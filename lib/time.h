#ifndef LIB_TIME_H
#define LIB_TIME_H
#if SYSTEM_DARWIN
typedef long time_t;
typedef int suseconds_t;
#else
// Same for Linux, FreeBSD and OpenBSD
typedef long time_t;
typedef long suseconds_t;
#endif
typedef long clock_t;
typedef int clockid_t;
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
int clock_gettime(clockid_t clock_id, struct timespec *tp);
#define CLOCKS_PER_SEC 1000000L
clock_t clock(void);
#endif
