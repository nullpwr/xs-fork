/* event_loop.c - async event loop with epoll/kqueue/poll backends
 *
 * Platform selection:
 *   Linux  -> epoll
 *   macOS  -> kqueue
 *   other  -> poll (fallback)
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "runtime/event_loop.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#else
#include <winsock2.h>
#include <io.h>
#include <fcntl.h>
#endif
#include <time.h>

#if defined(__linux__)
#  include <sys/epoll.h>
#  define EVLOOP_EPOLL 1
#elif defined(__APPLE__)
#  include <sys/event.h>
#  define EVLOOP_KQUEUE 1
#elif defined(_WIN32)
#  include <winsock2.h>
#  define EVLOOP_POLL 1
#else
#  include <poll.h>
#  define EVLOOP_POLL 1
#endif

/* ----------------------------------------------------------------
 *  Time helpers
 * ---------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>
#endif

int64_t evloop_now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ----------------------------------------------------------------
 *  Internal: set fd non-blocking
 * ---------------------------------------------------------------- */

#ifdef _WIN32
static int set_nonblocking(int fd) {
    unsigned long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
}
#else
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

/* ----------------------------------------------------------------
 *  Timer min-heap operations
 * ---------------------------------------------------------------- */

static void heap_swap(TimerEntry *a, TimerEntry *b) {
    TimerEntry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_sift_up(TimerEntry *heap, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (heap[parent].when_ms <= heap[idx].when_ms) break;
        heap_swap(&heap[parent], &heap[idx]);
        idx = parent;
    }
}

static void heap_sift_down(TimerEntry *heap, int n, int idx) {
    while (1) {
        int smallest = idx;
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;

        if (left < n && heap[left].when_ms < heap[smallest].when_ms)
            smallest = left;
        if (right < n && heap[right].when_ms < heap[smallest].when_ms)
            smallest = right;

        if (smallest == idx) break;
        heap_swap(&heap[smallest], &heap[idx]);
        idx = smallest;
    }
}

static void heap_push(EventLoop *ev, TimerEntry *entry) {
    if (ev->ntimers >= ev->timer_cap) {
        ev->timer_cap = ev->timer_cap ? ev->timer_cap * 2 : 16;
        ev->timers = realloc(ev->timers, sizeof(TimerEntry) * ev->timer_cap);
    }
    ev->timers[ev->ntimers] = *entry;
    heap_sift_up(ev->timers, ev->ntimers);
    ev->ntimers++;
}

static TimerEntry heap_pop(EventLoop *ev) {
    TimerEntry top = ev->timers[0];
    ev->ntimers--;
    if (ev->ntimers > 0) {
        ev->timers[0] = ev->timers[ev->ntimers];
        heap_sift_down(ev->timers, ev->ntimers, 0);
    }
    return top;
}

static int heap_peek_ms(EventLoop *ev) {
    if (ev->ntimers == 0) return -1;
    int64_t now = evloop_now_ms();
    int64_t diff = ev->timers[0].when_ms - now;
    if (diff <= 0) return 0;
    if (diff > 60000) return 60000;
    return (int)diff;
}

/* ----------------------------------------------------------------
 *  Find source by fd
 * ---------------------------------------------------------------- */

static int find_source(EventLoop *ev, int fd) {
    for (int i = 0; i < ev->nsources; i++) {
        if (ev->sources[i].fd == fd && ev->sources[i].active)
            return i;
    }
    return -1;
}

/* ----------------------------------------------------------------
 *  Signal pipe
 * ---------------------------------------------------------------- */

#ifndef _WIN32
static volatile sig_atomic_t g_signal_received[64];
static EventLoop *g_signal_loop = NULL;

static void signal_handler(int signum) {
    if (signum >= 0 && signum < 64)
        g_signal_received[signum] = 1;
    if (g_signal_loop && g_signal_loop->signal_pipe[1] >= 0) {
        char c = (char)signum;
        (void)write(g_signal_loop->signal_pipe[1], &c, 1);
    }
}

static void process_signals(EventLoop *ev) {
    if (ev->signal_pipe[0] < 0) return;

    char buf[64];
    while (read(ev->signal_pipe[0], buf, sizeof(buf)) > 0)
        ;

    for (int i = 0; i < ev->nsig_handlers; i++) {
        int sig = ev->sig_handlers[i].signum;
        if (sig >= 0 && sig < 64 && g_signal_received[sig]) {
            g_signal_received[sig] = 0;
            if (ev->sig_handlers[i].callback) {
                ev->sig_handlers[i].callback(
                    sig, EV_SIGNAL, ev->sig_handlers[i].ctx);
            }
        }
    }
}
#else
static void process_signals(EventLoop *ev) { (void)ev; }
#endif

/* ================================================================
 *  EPOLL BACKEND (Linux)
 * ================================================================ */

#ifdef EVLOOP_EPOLL

static int backend_init(EventLoop *ev) {
    ev->backend_fd = epoll_create1(EPOLL_CLOEXEC);
    return ev->backend_fd >= 0 ? 0 : -1;
}

static void backend_destroy(EventLoop *ev) {
    if (ev->backend_fd >= 0) {
        close(ev->backend_fd);
        ev->backend_fd = -1;
    }
}

static int backend_add_fd(EventLoop *ev, int fd, EventType events) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.data.fd = fd;
    if (events & EV_READ)  ee.events |= EPOLLIN;
    if (events & EV_WRITE) ee.events |= EPOLLOUT;
    return epoll_ctl(ev->backend_fd, EPOLL_CTL_ADD, fd, &ee);
}

static int backend_mod_fd(EventLoop *ev, int fd, EventType events) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.data.fd = fd;
    if (events & EV_READ)  ee.events |= EPOLLIN;
    if (events & EV_WRITE) ee.events |= EPOLLOUT;
    return epoll_ctl(ev->backend_fd, EPOLL_CTL_MOD, fd, &ee);
}

static int backend_remove_fd(EventLoop *ev, int fd) {
    return epoll_ctl(ev->backend_fd, EPOLL_CTL_DEL, fd, NULL);
}

static int backend_poll(EventLoop *ev, int timeout_ms) {
    struct epoll_event events[128];
    int n = epoll_wait(ev->backend_fd, events, 128, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        int idx = find_source(ev, fd);
        if (idx < 0) continue;

        EventType fired = 0;
        if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            fired |= EV_READ;
        if (events[i].events & EPOLLOUT)
            fired |= EV_WRITE;

        if (ev->sources[idx].callback) {
            ev->sources[idx].callback(fd, fired, ev->sources[idx].ctx);
            ev->total_events++;
        }
    }
    return n;
}

#endif /* EVLOOP_EPOLL */

/* ================================================================
 *  KQUEUE BACKEND (macOS)
 * ================================================================ */

#ifdef EVLOOP_KQUEUE

static int backend_init(EventLoop *ev) {
    ev->backend_fd = kqueue();
    return ev->backend_fd >= 0 ? 0 : -1;
}

static void backend_destroy(EventLoop *ev) {
    if (ev->backend_fd >= 0) {
        close(ev->backend_fd);
        ev->backend_fd = -1;
    }
}

static int backend_add_fd(EventLoop *ev, int fd, EventType events) {
    struct kevent changes[2];
    int nchanges = 0;

    if (events & EV_READ) {
        EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        nchanges++;
    }
    if (events & EV_WRITE) {
        EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
        nchanges++;
    }
    return kevent(ev->backend_fd, changes, nchanges, NULL, 0, NULL);
}

static int backend_mod_fd(EventLoop *ev, int fd, EventType events) {
    struct kevent changes[2];
    int nchanges = 0;

    /* disable both first, then re-enable what we want */
    EV_SET(&changes[0], fd, EVFILT_READ,
           (events & EV_READ) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
    nchanges++;
    EV_SET(&changes[1], fd, EVFILT_WRITE,
           (events & EV_WRITE) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
    nchanges++;

    /* ignore errors from deleting filters that weren't added */
    kevent(ev->backend_fd, changes, nchanges, NULL, 0, NULL);
    return 0;
}

static int backend_remove_fd(EventLoop *ev, int fd) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(ev->backend_fd, changes, 2, NULL, 0, NULL);
    return 0;
}

static int backend_poll(EventLoop *ev, int timeout_ms) {
    struct kevent events[128];
    struct timespec ts;
    struct timespec *tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(ev->backend_fd, NULL, 0, events, 128, tsp);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int fd = (int)events[i].ident;
        int idx = find_source(ev, fd);
        if (idx < 0) continue;

        EventType fired = 0;
        if (events[i].filter == EVFILT_READ)  fired |= EV_READ;
        if (events[i].filter == EVFILT_WRITE) fired |= EV_WRITE;

        if (ev->sources[idx].callback) {
            ev->sources[idx].callback(fd, fired, ev->sources[idx].ctx);
            ev->total_events++;
        }
    }
    return n;
}

#endif /* EVLOOP_KQUEUE */

/* ================================================================
 *  POLL BACKEND (Fallback)
 * ================================================================ */

#ifdef EVLOOP_POLL

static int backend_init(EventLoop *ev) {
    ev->backend_fd = -1;
    return 0;
}

static void backend_destroy(EventLoop *ev) {
    ev->backend_fd = -1;
}

static int backend_add_fd(EventLoop *ev, int fd, EventType events) {
    (void)ev; (void)fd; (void)events;
    return 0;
}

static int backend_mod_fd(EventLoop *ev, int fd, EventType events) {
    (void)ev; (void)fd; (void)events;
    return 0;
}

static int backend_remove_fd(EventLoop *ev, int fd) {
    (void)ev; (void)fd;
    return 0;
}

static int backend_poll(EventLoop *ev, int timeout_ms) {
    if (ev->nsources == 0) {
        if (timeout_ms > 0) {
#ifdef _WIN32
            Sleep(timeout_ms);
#else
            struct timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
#endif
        }
        return 0;
    }

#ifdef _WIN32
    /* Windows: use select instead of poll */
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = 0;
    int *idx_map = calloc(ev->nsources, sizeof(int));
    int nfds = 0;

    for (int i = 0; i < ev->nsources; i++) {
        if (!ev->sources[i].active) continue;
        int fd = ev->sources[i].fd;
        if (ev->sources[i].events & EV_READ)  FD_SET(fd, &rfds);
        if (ev->sources[i].events & EV_WRITE) FD_SET(fd, &wfds);
        if (fd > maxfd) maxfd = fd;
        idx_map[nfds++] = i;
    }

    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int n = select(maxfd + 1, &rfds, &wfds, NULL, tvp);
    if (n > 0) {
        for (int i = 0; i < nfds; i++) {
            int idx = idx_map[i];
            int fd = ev->sources[idx].fd;
            EventType fired = 0;
            if (FD_ISSET(fd, &rfds)) fired |= EV_READ;
            if (FD_ISSET(fd, &wfds)) fired |= EV_WRITE;
            if (fired && ev->sources[idx].callback) {
                ev->sources[idx].callback(fd, fired, ev->sources[idx].ctx);
                ev->total_events++;
            }
        }
    }
    free(idx_map);
    return n > 0 ? n : 0;
#else
    struct pollfd *pfds = calloc(ev->nsources, sizeof(struct pollfd));
    int npfds = 0;
    int *idx_map = calloc(ev->nsources, sizeof(int));

    for (int i = 0; i < ev->nsources; i++) {
        if (!ev->sources[i].active) continue;
        pfds[npfds].fd = ev->sources[i].fd;
        pfds[npfds].events = 0;
        if (ev->sources[i].events & EV_READ)  pfds[npfds].events |= POLLIN;
        if (ev->sources[i].events & EV_WRITE) pfds[npfds].events |= POLLOUT;
        idx_map[npfds] = i;
        npfds++;
    }

    int n = poll(pfds, npfds, timeout_ms);
    if (n < 0 && errno != EINTR) {
        free(pfds);
        free(idx_map);
        return -1;
    }

    if (n > 0) {
        for (int i = 0; i < npfds; i++) {
            if (pfds[i].revents == 0) continue;
            int idx = idx_map[i];

            EventType fired = 0;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
                fired |= EV_READ;
            if (pfds[i].revents & POLLOUT)
                fired |= EV_WRITE;

            if (ev->sources[idx].callback) {
                ev->sources[idx].callback(
                    ev->sources[idx].fd, fired, ev->sources[idx].ctx);
                ev->total_events++;
            }
        }
    }

    free(pfds);
    free(idx_map);
    return n > 0 ? n : 0;
#endif
}

#endif /* EVLOOP_POLL */

/* ================================================================
 *  Public API
 * ================================================================ */

EventLoop *evloop_new(void) {
    EventLoop *ev = calloc(1, sizeof(EventLoop));
    if (!ev) return NULL;

    ev->backend_fd = -1;
    ev->signal_pipe[0] = -1;
    ev->signal_pipe[1] = -1;

    if (backend_init(ev) < 0) {
        free(ev);
        return NULL;
    }

    /* initial capacity */
    ev->cap = 64;
    ev->sources = calloc(ev->cap, sizeof(EventSource));
    ev->timer_cap = 16;
    ev->timers = calloc(ev->timer_cap, sizeof(TimerEntry));
    ev->next_timer_id = 1;

    /* signal self-pipe */
#ifdef _WIN32
    if (_pipe(ev->signal_pipe, 256, _O_BINARY) == 0) {
#else
    if (pipe(ev->signal_pipe) == 0) {
#endif
        set_nonblocking(ev->signal_pipe[0]);
        set_nonblocking(ev->signal_pipe[1]);
        /* register read end with the backend */
        EventSource src;
        memset(&src, 0, sizeof(src));
        src.fd = ev->signal_pipe[0];
        src.events = EV_READ;
        src.callback = NULL;   /* handled internally */
        src.active = 1;
        if (ev->nsources >= ev->cap) {
            ev->cap *= 2;
            ev->sources = realloc(ev->sources, sizeof(EventSource) * ev->cap);
        }
        ev->sources[ev->nsources++] = src;
        backend_add_fd(ev, ev->signal_pipe[0], EV_READ);
    }

    return ev;
}

void evloop_free(EventLoop *ev) {
    if (!ev) return;

#ifndef _WIN32
    /* restore default signal handlers */
    for (int i = 0; i < ev->nsig_handlers; i++) {
        signal(ev->sig_handlers[i].signum, SIG_DFL);
    }
    if (g_signal_loop == ev) g_signal_loop = NULL;
#endif

    backend_destroy(ev);

#ifdef _WIN32
    if (ev->signal_pipe[0] >= 0) _close(ev->signal_pipe[0]);
    if (ev->signal_pipe[1] >= 0) _close(ev->signal_pipe[1]);
#else
    if (ev->signal_pipe[0] >= 0) close(ev->signal_pipe[0]);
    if (ev->signal_pipe[1] >= 0) close(ev->signal_pipe[1]);
#endif

    free(ev->sources);
    free(ev->timers);
    free(ev);
}

int evloop_add_fd(EventLoop *ev, int fd, EventType events,
                  EventCallback cb, void *ctx) {
    if (!ev || fd < 0) return -1;

    /* check if already tracked */
    int idx = find_source(ev, fd);
    if (idx >= 0) return -1;  /* already registered */

    set_nonblocking(fd);

    /* find a free slot or expand */
    int slot = -1;
    for (int i = 0; i < ev->nsources; i++) {
        if (!ev->sources[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        if (ev->nsources >= ev->cap) {
            ev->cap = ev->cap ? ev->cap * 2 : 64;
            ev->sources = realloc(ev->sources, sizeof(EventSource) * ev->cap);
        }
        slot = ev->nsources++;
    }

    ev->sources[slot].fd = fd;
    ev->sources[slot].events = events;
    ev->sources[slot].callback = cb;
    ev->sources[slot].ctx = ctx;
    ev->sources[slot].active = 1;

    return backend_add_fd(ev, fd, events);
}

int evloop_mod_fd(EventLoop *ev, int fd, EventType events) {
    if (!ev) return -1;
    int idx = find_source(ev, fd);
    if (idx < 0) return -1;
    ev->sources[idx].events = events;
    return backend_mod_fd(ev, fd, events);
}

int evloop_remove_fd(EventLoop *ev, int fd) {
    if (!ev) return -1;
    int idx = find_source(ev, fd);
    if (idx < 0) return -1;

    ev->sources[idx].active = 0;
    ev->sources[idx].callback = NULL;
    ev->sources[idx].ctx = NULL;

    return backend_remove_fd(ev, fd);
}

int evloop_add_timer(EventLoop *ev, int ms, int repeat,
                     TimerCallback cb, void *ctx) {
    if (!ev || !cb || ms < 0) return -1;

    TimerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.when_ms = evloop_now_ms() + ms;
    entry.fn = cb;
    entry.ctx = ctx;
    entry.repeat_ms = repeat ? ms : 0;
    entry.active = 1;
    entry.id = ev->next_timer_id++;

    heap_push(ev, &entry);
    return entry.id;
}

int evloop_cancel_timer(EventLoop *ev, int timer_id) {
    if (!ev) return -1;
    for (int i = 0; i < ev->ntimers; i++) {
        if (ev->timers[i].id == timer_id) {
            ev->timers[i].active = 0;
            return 0;
        }
    }
    return -1;
}

int evloop_add_signal(EventLoop *ev, int signum,
                      EventCallback cb, void *ctx) {
    if (!ev || ev->nsig_handlers >= 32) return -1;

    ev->sig_handlers[ev->nsig_handlers].signum = signum;
    ev->sig_handlers[ev->nsig_handlers].callback = cb;
    ev->sig_handlers[ev->nsig_handlers].ctx = ctx;
    ev->nsig_handlers++;

#ifndef _WIN32
    g_signal_loop = ev;
#if !defined(__wasi__)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(signum, &sa, NULL);
#else
    signal(signum, signal_handler);
#endif
#endif /* _WIN32 */

    return 0;
}

/* fire expired timers, return count */
static int fire_timers(EventLoop *ev) {
    int fired = 0;
    int64_t now = evloop_now_ms();

    while (ev->ntimers > 0 && ev->timers[0].when_ms <= now) {
        TimerEntry entry = heap_pop(ev);
        if (!entry.active) continue;

        entry.fn(entry.ctx);
        fired++;
        ev->total_timers_fired++;

        /* re-queue repeating timers */
        if (entry.repeat_ms > 0) {
            entry.when_ms = now + entry.repeat_ms;
            heap_push(ev, &entry);
        }
    }
    return fired;
}

void evloop_run_once(EventLoop *ev, int timeout_ms) {
    if (!ev) return;

    /* calculate timeout from nearest timer */
    int timer_timeout = heap_peek_ms(ev);
    int actual_timeout;

    if (timeout_ms < 0) {
        actual_timeout = timer_timeout;
    } else if (timer_timeout < 0) {
        actual_timeout = timeout_ms;
    } else {
        actual_timeout = timeout_ms < timer_timeout ? timeout_ms : timer_timeout;
    }

    /* poll for I/O events */
    backend_poll(ev, actual_timeout);

    /* process signals */
    process_signals(ev);

    /* fire expired timers */
    fire_timers(ev);
}

void evloop_run(EventLoop *ev) {
    if (!ev) return;
    ev->running = 1;

    while (ev->running) {
        evloop_run_once(ev, 100);  /* 100ms default poll interval */
    }
}

void evloop_stop(EventLoop *ev) {
    if (ev) ev->running = 0;
}
