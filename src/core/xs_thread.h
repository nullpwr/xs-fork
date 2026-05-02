/* xs_thread.h: cross-platform threads (Win32 / pthreads) */
#ifndef XS_THREAD_H
#define XS_THREAD_H

#include <stdlib.h>

#if defined(_WIN32) || defined(__MINGW32__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef HANDLE          xs_thread_t;
typedef CRITICAL_SECTION xs_mutex_t;
typedef CONDITION_VARIABLE xs_cond_t;

static inline int xs_cond_init(xs_cond_t *c)    { InitializeConditionVariable(c); return 0; }
static inline int xs_cond_destroy(xs_cond_t *c) { (void)c; return 0; }
static inline int xs_cond_wait(xs_cond_t *c, xs_mutex_t *m) {
    return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1;
}
/* Returns 0 on signal / broadcast, 1 on timeout, -1 on error. */
static inline int xs_cond_timedwait_ms(xs_cond_t *c, xs_mutex_t *m, int ms) {
    if (SleepConditionVariableCS(c, m, (DWORD)ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : -1;
}
static inline int xs_cond_signal(xs_cond_t *c)    { WakeConditionVariable(c); return 0; }
static inline int xs_cond_broadcast(xs_cond_t *c) { WakeAllConditionVariable(c); return 0; }

/* bridge POSIX-style callback to Win32 thread entry */
typedef struct {
    void *(*fn)(void *);
    void  *arg;
} xs_thread_trampoline_t;

static DWORD WINAPI xs_thread_trampoline_entry(LPVOID param) {
    xs_thread_trampoline_t *tramp = (xs_thread_trampoline_t *)param;
    void *(*fn)(void *) = tramp->fn;
    void *arg           = tramp->arg;
    free(tramp);
    fn(arg);
    return 0;
}

static inline int xs_thread_create(xs_thread_t *t, void *(*fn)(void *), void *arg) {
    xs_thread_trampoline_t *tramp = (xs_thread_trampoline_t *)malloc(sizeof(*tramp));
    if (!tramp) return -1;
    tramp->fn  = fn;
    tramp->arg = arg;
    HANDLE h = CreateThread(NULL, 0, xs_thread_trampoline_entry, tramp, 0, NULL);
    if (!h) {
        free(tramp);
        return -1;
    }
    *t = h;
    return 0;
}

static inline int xs_thread_join(xs_thread_t t, void **retval) {
    (void)retval;
    DWORD r = WaitForSingleObject(t, INFINITE);
    if (r != WAIT_OBJECT_0) return -1;
    CloseHandle(t);
    return 0;
}

static inline int xs_thread_detach(xs_thread_t t) {
    return CloseHandle(t) ? 0 : -1;
}

static inline unsigned long xs_thread_self_id(void) {
    return (unsigned long)GetCurrentThreadId();
}


static inline int xs_mutex_init(xs_mutex_t *m) {
    InitializeCriticalSection(m);
    return 0;
}

static inline int xs_mutex_lock(xs_mutex_t *m) {
    EnterCriticalSection(m);
    return 0;
}

static inline int xs_mutex_unlock(xs_mutex_t *m) {
    LeaveCriticalSection(m);
    return 0;
}

static inline int xs_mutex_trylock(xs_mutex_t *m) {
    return TryEnterCriticalSection(m) ? 0 : -1;
}

static inline int xs_mutex_destroy(xs_mutex_t *m) {
    DeleteCriticalSection(m);
    return 0;
}

static inline void xs_thread_sleep_ns(double secs) {
    DWORD ms = (DWORD)(secs * 1000.0);
    if (ms == 0 && secs > 0) ms = 1;
    Sleep(ms);
}

#else /* POSIX */

#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

typedef pthread_t       xs_thread_t;
typedef pthread_mutex_t xs_mutex_t;
typedef pthread_cond_t  xs_cond_t;

static inline int xs_cond_init(xs_cond_t *c)    { return pthread_cond_init(c, NULL); }
static inline int xs_cond_destroy(xs_cond_t *c) { return pthread_cond_destroy(c); }
static inline int xs_cond_wait(xs_cond_t *c, xs_mutex_t *m) { return pthread_cond_wait(c, m); }
static inline int xs_cond_timedwait_ms(xs_cond_t *c, xs_mutex_t *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == 0) return 0;
    if (rc == ETIMEDOUT) return 1;
    return -1;
}
static inline int xs_cond_signal(xs_cond_t *c)    { return pthread_cond_signal(c); }
static inline int xs_cond_broadcast(xs_cond_t *c) { return pthread_cond_broadcast(c); }

static inline int xs_thread_create(xs_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static inline int xs_thread_join(xs_thread_t t, void **retval) {
    return pthread_join(t, retval);
}

static inline int xs_thread_detach(xs_thread_t t) {
    return pthread_detach(t);
}

static inline unsigned long xs_thread_self_id(void) {
    return (unsigned long)(uintptr_t)pthread_self();
}


static inline int xs_mutex_init(xs_mutex_t *m) {
    return pthread_mutex_init(m, NULL);
}

static inline int xs_mutex_lock(xs_mutex_t *m) {
    return pthread_mutex_lock(m);
}

static inline int xs_mutex_unlock(xs_mutex_t *m) {
    return pthread_mutex_unlock(m);
}

static inline int xs_mutex_trylock(xs_mutex_t *m) {
    return pthread_mutex_trylock(m);
}

static inline int xs_mutex_destroy(xs_mutex_t *m) {
    return pthread_mutex_destroy(m);
}

static inline void xs_thread_sleep_ns(double secs) {
    struct timespec ts;
    ts.tv_sec  = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

#endif /* _WIN32 || __MINGW32__ */

#define XS_HAS_THREADS 1

#endif /* XS_THREAD_H */
