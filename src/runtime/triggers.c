#define _POSIX_C_SOURCE 200809L
#include "runtime/triggers.h"
#include "core/value.h"
#include "core/xs.h"
#include "runtime/interp.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif
#if defined(__linux__)
#  include <sys/inotify.h>
#endif

/* Pending signal flags: a signal handler can only set sig_atomic_t,
   so the run loop polls them and fires the xs handler from a safe
   point. SIGTERM and SIGINT cover the common graceful-shutdown case;
   the unix-only ones are conditional and stay zero on platforms that
   don't expose them. */
static volatile sig_atomic_t g_sig_int  = 0;
static volatile sig_atomic_t g_sig_term = 0;
static volatile sig_atomic_t g_sig_hup  = 0;
static volatile sig_atomic_t g_sig_usr1 = 0;
static volatile sig_atomic_t g_sig_usr2 = 0;

static TriggerEntry *g_items = NULL;
static int g_len = 0, g_cap = 0;

void trigger_registry_clear(void) {
    for (int i = 0; i < g_len; i++) {
        free(g_items[i].name);
        for (int j = 0; j < g_items[i].n_args; j++) {
            if (g_items[i].args[j]) value_decref(g_items[i].args[j]);
        }
        free(g_items[i].args);
        if (g_items[i].fn) value_decref(g_items[i].fn);
    }
    free(g_items);
    g_items = NULL;
    g_len = g_cap = 0;
}

/* Forward declaration used by the on_start fire-on-register path. */
extern Value *call_value(Interp *i, Value *callee, Value **args, int argc,
                         const char *call_site);
extern Interp *g_current_interp;

static void install_signal_handler_for(const char *name);

void trigger_registry_register(const char *name, Value **args, int n_args,
                               Value *fn, int has_once) {
    if (g_len == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8;
        g_items = realloc(g_items, sizeof(TriggerEntry) * g_cap);
    }
    TriggerEntry *e = &g_items[g_len++];
    memset(e, 0, sizeof(*e));
    e->name = name ? xs_strdup(name) : NULL;
    e->args = args;
    e->n_args = n_args;
    e->fn = fn ? value_incref(fn) : NULL;
    e->has_once = has_once;

    /* @on_signal binds an OS handler at registration. The actual xs
       call happens from trigger_pump_signals once the run loop
       reaches a safe point. */
    if (name && strcmp(name, "on_signal") == 0 && n_args >= 1
        && args[0] && VAL_TAG(args[0]) == XS_STR && args[0]->s) {
        install_signal_handler_for(args[0]->s);
    }
}

int trigger_registry_count(void) { return g_len; }
TriggerEntry *trigger_registry_get(int i) {
    if (i < 0 || i >= g_len) return NULL;
    return &g_items[i];
}

Value *trigger_native_size(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    return xs_int((int64_t)trigger_registry_count());
}

Value *trigger_native_name(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || VAL_TAG(args[0]) != XS_INT) return value_incref(XS_NULL_VAL);
    TriggerEntry *e = trigger_registry_get((int)VAL_INT(args[0]));
    return e && e->name ? xs_str(e->name) : value_incref(XS_NULL_VAL);
}

/* Used by the VM compiler to register a decorated fn at proto-load
   time: __register_decorator(fn, has_once_bool, name_str, args...) */
Value *trigger_native_register(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 3) return value_incref(XS_NULL_VAL);
    Value *fn = args[0];
    int has_once = value_truthy(args[1]);
    if (VAL_TAG(args[2]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *name = args[2]->s;
    int n = argc - 3;
    Value **dec_args = n > 0 ? xs_calloc(n, sizeof(Value*)) : NULL;
    for (int k = 0; k < n; k++) dec_args[k] = value_incref(args[k+3]);
    trigger_registry_register(name, dec_args, n, fn, has_once);
    return value_incref(XS_NULL_VAL);
}

/* ---- Lifecycle firing ---- */

static void fire_named(Interp *i, const char *name, Value *opt_arg) {
    int n_args = opt_arg ? 1 : 0;
    Value *args[1];
    if (opt_arg) args[0] = opt_arg;
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name || strcmp(e->name, name) != 0) continue;
        if (e->has_once && e->fired_once) continue;
        if (e->fn) {
            Value *r = call_value(i, e->fn, opt_arg ? args : NULL, n_args, name);
            if (r) value_decref(r);
        }
        e->fired_once = 1;
    }
}

void trigger_fire_on_start(Interp *i) { fire_named(i, "on_start", NULL); }
void trigger_fire_on_exit(Interp *i)  { fire_named(i, "on_exit", NULL); }
void trigger_fire_on_panic(Interp *i, Value *exc) {
    fire_named(i, "on_panic", exc);
}

/* ---- Signals ---- */

static void sigint_handler(int sig)  { (void)sig; g_sig_int  = 1; }
static void sigterm_handler(int sig) { (void)sig; g_sig_term = 1; }
#ifdef SIGHUP
static void sighup_handler(int sig)  { (void)sig; g_sig_hup  = 1; }
#endif
#ifdef SIGUSR1
static void sigusr1_handler(int sig) { (void)sig; g_sig_usr1 = 1; }
#endif
#ifdef SIGUSR2
static void sigusr2_handler(int sig) { (void)sig; g_sig_usr2 = 1; }
#endif

static void install_signal_handler_for(const char *name) {
    if (!name) return;
    if (strcmp(name, "INT") == 0)  signal(SIGINT,  sigint_handler);
    else if (strcmp(name, "TERM") == 0) signal(SIGTERM, sigterm_handler);
#ifdef SIGHUP
    else if (strcmp(name, "HUP") == 0)  signal(SIGHUP,  sighup_handler);
#endif
#ifdef SIGUSR1
    else if (strcmp(name, "USR1") == 0) signal(SIGUSR1, sigusr1_handler);
#endif
#ifdef SIGUSR2
    else if (strcmp(name, "USR2") == 0) signal(SIGUSR2, sigusr2_handler);
#endif
}

void trigger_install_signal_handlers(void) {
    /* Idempotent: each register call already wires its handler.
       This entry point exists so the run loop can reinstall a
       sane state if the host process clobbered the table. */
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (e->name && strcmp(e->name, "on_signal") == 0 &&
            e->n_args >= 1 && e->args[0] && VAL_TAG(e->args[0]) == XS_STR) {
            install_signal_handler_for(e->args[0]->s);
        }
    }
}

static void fire_signal(Interp *i, const char *which) {
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name || strcmp(e->name, "on_signal") != 0) continue;
        if (e->n_args < 1 || !e->args[0] || VAL_TAG(e->args[0]) != XS_STR) continue;
        if (strcmp(e->args[0]->s, which) != 0) continue;
        if (e->has_once && e->fired_once) continue;
        if (e->fn) {
            Value *r = call_value(i, e->fn, NULL, 0, "on_signal");
            if (r) value_decref(r);
        }
        e->fired_once = 1;
    }
}

void trigger_pump_signals(Interp *i) {
    if (g_sig_int)  { g_sig_int  = 0; fire_signal(i, "INT");  }
    if (g_sig_term) { g_sig_term = 0; fire_signal(i, "TERM"); }
    if (g_sig_hup)  { g_sig_hup  = 0; fire_signal(i, "HUP");  }
    if (g_sig_usr1) { g_sig_usr1 = 0; fire_signal(i, "USR1"); }
    if (g_sig_usr2) { g_sig_usr2 = 0; fire_signal(i, "USR2"); }
}

/* ---- Event loop ---- */

static int64_t now_mono_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int64_t entry_duration_ns(TriggerEntry *e) {
    if (e->n_args < 1 || !e->args[0]) return 0;
    Value *v = e->args[0];
    if (VAL_TAG(v) == XS_DURATION) return v->i;
    if (VAL_TAG(v) == XS_INT) return VAL_INT(v) * 1000000LL;
    if (VAL_TAG(v) == XS_FLOAT) return (int64_t)(v->f * 1e6);
    return 0;
}

/* Cron field parser: bitset-fills out from a single field token.
   Supports star, star slash N, single number, comma list, and
   lo-hi ranges. lo/hi are clamped to [field_min, field_max]. */
static int cron_parse_field(const char *spec, int field_min, int field_max,
                            uint64_t *out) {
    *out = 0;
    if (!spec || !*spec) return -1;
    /* split on commas */
    const char *p = spec;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) return -1;

        char buf[32];
        if (len >= sizeof(buf)) return -1;
        memcpy(buf, start, len); buf[len] = 0;

        /* step: "X/N" */
        int step = 1;
        char *slash = strchr(buf, '/');
        if (slash) { *slash = 0; step = atoi(slash + 1); if (step <= 0) return -1; }

        int lo, hi;
        if (strcmp(buf, "*") == 0) {
            lo = field_min; hi = field_max;
        } else if (strchr(buf, '-')) {
            char *dash = strchr(buf, '-');
            *dash = 0;
            lo = atoi(buf);
            hi = atoi(dash + 1);
        } else {
            lo = hi = atoi(buf);
        }
        if (lo < field_min) lo = field_min;
        if (hi > field_max) hi = field_max;
        for (int v = lo; v <= hi; v += step) {
            if (v >= 0 && v < 64) *out |= ((uint64_t)1 << v);
        }
        if (*p == ',') p++;
    }
    return 0;
}

static int compile_cron(TriggerEntry *e) {
    if (e->cron_compiled) return 0;
    if (e->n_args < 1 || !e->args[0] || VAL_TAG(e->args[0]) != XS_STR) return -1;
    const char *spec = e->args[0]->s;
    /* split into 5 whitespace-delimited fields */
    const char *fields[5] = {0};
    size_t lens[5] = {0};
    int fi = 0;
    const char *p = spec;
    while (*p && fi < 5) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        fields[fi] = s; lens[fi] = (size_t)(p - s);
        fi++;
    }
    if (fi != 5) return -1;
    char buf[32];
    uint64_t bits[5];
    int ranges[5][2] = {{0,59},{0,23},{1,31},{1,12},{0,6}};
    for (int k = 0; k < 5; k++) {
        if (lens[k] >= sizeof(buf)) return -1;
        memcpy(buf, fields[k], lens[k]); buf[lens[k]] = 0;
        if (cron_parse_field(buf, ranges[k][0], ranges[k][1], &bits[k]) < 0)
            return -1;
    }
    e->cron_min  = bits[0];
    e->cron_hour = (uint32_t)bits[1];
    e->cron_dom  = (uint32_t)bits[2];
    e->cron_mon  = (uint16_t)bits[3];
    e->cron_dow  = (uint8_t)bits[4];
    e->cron_compiled = 1;
    return 0;
}

/* Given a wall-clock time, find the next minute-aligned moment that
   matches the cron spec. Standard 5-field semantics: a timestamp
   matches if minute, hour, month all match and *either* DOM or DOW
   matches (POSIX dom/dow union, when both fields are restricted).
   Returns the resulting absolute wall-clock time as a time_t. */
static time_t cron_next_after(TriggerEntry *e, time_t after) {
    after += 60 - (after % 60); /* round up to next minute */
    for (int tries = 0; tries < 366*24*60; tries++) {
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &after);
#else
        localtime_r(&after, &tm);
#endif
        int min  = tm.tm_min;
        int hour = tm.tm_hour;
        int dom  = tm.tm_mday;
        int mon  = tm.tm_mon + 1;
        int dow  = tm.tm_wday;
        int dom_restricted = (e->cron_dom & 0xFFFFFFFEu) != 0xFFFFFFFEu;
        int dow_restricted = (e->cron_dow & 0x7Fu) != 0x7Fu;
        int dom_ok = (e->cron_dom >> dom) & 1;
        int dow_ok = (e->cron_dow >> dow) & 1;
        int day_ok = (dom_restricted && dow_restricted)
            ? (dom_ok || dow_ok)
            : (dom_ok && dow_ok);
        if (((e->cron_min  >> min ) & 1) &&
            ((e->cron_hour >> hour) & 1) &&
            ((e->cron_mon  >> mon ) & 1) &&
            day_ok) {
            return after;
        }
        after += 60;
    }
    return after;
}

/* ---- @watch ---- */

#if defined(__linux__)
static int g_inotify_fd = -1;
#endif

static void watch_snapshot(const char *path, long long *mtime_ns, long long *sz) {
    struct stat st;
    if (stat(path, &st) != 0) {
        *mtime_ns = -1;
        *sz = -1;
        return;
    }
    /* Use st_mtime (second resolution) for portability. POSIX 2008's
       st_mtim is hidden behind _POSIX_C_SOURCE on darwin, and the
       non-standard st_mtimespec / st_mtim variants disagree across
       libc / SDK / build flag combinations. The watcher only needs
       to notice that a file moved, so second resolution is enough. */
    *mtime_ns = (long long)st.st_mtime * 1000000000LL;
    *sz = (long long)st.st_size;
}

#if defined(__linux__)
static void inotify_setup_entry(TriggerEntry *e) {
    if (e->watch_inotify_initialised) return;
    e->watch_inotify_initialised = 1;
    e->watch_inotify_wd = -1;
    if (e->n_args < 1 || !e->args[0] || VAL_TAG(e->args[0]) != XS_STR) return;
    if (g_inotify_fd < 0) {
        g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_inotify_fd < 0) return;
    }
    int wd = inotify_add_watch(g_inotify_fd, e->args[0]->s,
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
        IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd >= 0) e->watch_inotify_wd = wd;
}

static int inotify_drain_events(void) {
    if (g_inotify_fd < 0) return 0;
    char buf[4096] __attribute__((aligned(8)));
    int saw_any = 0;
    for (;;) {
        ssize_t n = read(g_inotify_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        saw_any = 1;
        char *p = buf;
        while (p < buf + n) {
            struct inotify_event *ev = (struct inotify_event *)p;
            for (int k = 0; k < g_len; k++) {
                TriggerEntry *e = &g_items[k];
                if (e->name && strcmp(e->name, "watch") == 0 &&
                    e->watch_inotify_wd == ev->wd) {
                    /* Mark entry by negating watch_size; the dispatcher
                       compares against the cached snapshot to coalesce
                       multiple events that arrive in the same burst. */
                    long long m, s;
                    if (e->n_args >= 1 && e->args[0] && VAL_TAG(e->args[0]) == XS_STR)
                        watch_snapshot(e->args[0]->s, &m, &s);
                    else { m = s = -1; }
                    e->watch_mtime_ns = m;
                    e->watch_size = s;
                    /* mark as needing fire by reusing next_fire_ns = -1 */
                    e->next_fire_ns = -1;
                }
            }
            p += sizeof(*ev) + ev->len;
        }
    }
    return saw_any;
}
#endif

static void check_watches(Interp *i) {
#if defined(__linux__)
    /* Linux: use inotify when available, otherwise fall through to
       the portable polling path. */
    inotify_drain_events();
#endif
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name || strcmp(e->name, "watch") != 0) continue;
        if (e->has_once && e->fired_once) continue;
        if (e->n_args < 1 || !e->args[0] || VAL_TAG(e->args[0]) != XS_STR) continue;

#if defined(__linux__)
        if (!e->watch_inotify_initialised) {
            inotify_setup_entry(e);
            long long m, s; watch_snapshot(e->args[0]->s, &m, &s);
            e->watch_mtime_ns = m; e->watch_size = s;
            continue; /* don't fire on the initial snapshot */
        }
        int should_fire = (e->next_fire_ns == -1);
        if (e->watch_inotify_wd < 0) {
            /* Fall back to polling for paths inotify rejected. */
            long long m, s; watch_snapshot(e->args[0]->s, &m, &s);
            if (m != e->watch_mtime_ns || s != e->watch_size) {
                e->watch_mtime_ns = m; e->watch_size = s;
                should_fire = 1;
            }
        }
#else
        long long m, s; watch_snapshot(e->args[0]->s, &m, &s);
        int should_fire = 0;
        if (!e->watch_inotify_initialised) {
            e->watch_inotify_initialised = 1;
            e->watch_mtime_ns = m; e->watch_size = s;
            continue;
        }
        if (m != e->watch_mtime_ns || s != e->watch_size) {
            e->watch_mtime_ns = m; e->watch_size = s;
            should_fire = 1;
        }
#endif
        if (should_fire) {
            if (e->fn) {
                Value *r = call_value(i, e->fn, NULL, 0, "watch");
                if (r) value_decref(r);
            }
            e->fired_once = 1;
            e->next_fire_ns = 0;
        }
    }
}

/* Returns 1 if the registry contains anything that needs the loop. */
static int has_persistent_triggers(void) {
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name) continue;
        if (e->has_once && e->fired_once) continue;
        if (strcmp(e->name, "every") == 0)   return 1;
        if (strcmp(e->name, "cron") == 0)    return 1;
        if (strcmp(e->name, "delayed") == 0 && !e->fired_once) return 1;
        if (strcmp(e->name, "on_signal") == 0) return 1;
        if (strcmp(e->name, "watch") == 0)    return 1;
    }
    return 0;
}

static void compute_initial_fire(TriggerEntry *e, int64_t now_ns) {
    if (e->next_fire_ns) return;
    if (strcmp(e->name, "every") == 0) {
        e->interval_ns = entry_duration_ns(e);
        if (e->interval_ns <= 0) e->interval_ns = 1000000000LL;
        e->next_fire_ns = now_ns + e->interval_ns;
    } else if (strcmp(e->name, "delayed") == 0) {
        int64_t d = entry_duration_ns(e);
        e->next_fire_ns = now_ns + (d > 0 ? d : 0);
    } else if (strcmp(e->name, "cron") == 0) {
        if (compile_cron(e) < 0) {
            e->next_fire_ns = INT64_MAX;
            return;
        }
        time_t now_wall = time(NULL);
        time_t next = cron_next_after(e, now_wall);
        e->next_fire_ns = now_ns + (int64_t)(next - now_wall) * 1000000000LL;
    }
}

static int64_t soonest_fire_ns(int64_t now_ns) {
    int64_t best = INT64_MAX;
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name) continue;
        if (e->has_once && e->fired_once) continue;
        if (strcmp(e->name, "every") != 0 &&
            strcmp(e->name, "cron") != 0 &&
            strcmp(e->name, "delayed") != 0) continue;
        compute_initial_fire(e, now_ns);
        if (e->next_fire_ns < best) best = e->next_fire_ns;
    }
    return best;
}

static void fire_due(Interp *i, int64_t now_ns) {
    for (int k = 0; k < g_len; k++) {
        TriggerEntry *e = &g_items[k];
        if (!e->name) continue;
        if (e->has_once && e->fired_once) continue;
        int is_periodic = strcmp(e->name, "every") == 0 ||
                          strcmp(e->name, "cron")  == 0;
        int is_delayed  = strcmp(e->name, "delayed") == 0;
        if (!is_periodic && !is_delayed) continue;
        if (is_delayed && e->fired_once) continue;
        if (e->next_fire_ns > now_ns) continue;
        if (e->fn) {
            Value *r = call_value(i, e->fn, NULL, 0, e->name);
            if (r) value_decref(r);
        }
        e->fired_once = 1;
        if (is_periodic) {
            if (strcmp(e->name, "every") == 0) {
                e->next_fire_ns += e->interval_ns;
                if (e->next_fire_ns <= now_ns) e->next_fire_ns = now_ns + e->interval_ns;
            } else {
                time_t next_wall = cron_next_after(e, time(NULL));
                e->next_fire_ns = now_ns + (int64_t)(next_wall - time(NULL)) * 1000000000LL;
            }
        }
    }
}

static void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)(ns / 1000000));
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    nanosleep(&ts, NULL);
#endif
}

void trigger_run_event_loop(Interp *i) {
    if (!has_persistent_triggers()) return;
    while (has_persistent_triggers()) {
        int64_t now = now_mono_ns();
        int64_t next = soonest_fire_ns(now);
        if (next == INT64_MAX) {
            /* nothing time-based; just wait briefly for signals so a
               script with @on_signal can quiesce quickly when one
               arrives. 100ms is short enough to feel responsive. */
            sleep_ns(100000000LL);
        } else if (next > now) {
            int64_t budget = next - now;
            if (budget > 100000000LL) budget = 100000000LL;
            sleep_ns(budget);
        }
        trigger_pump_signals(i);
        check_watches(i);
        fire_due(i, now_mono_ns());
    }
}
