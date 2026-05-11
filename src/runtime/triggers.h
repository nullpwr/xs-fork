#ifndef XS_TRIGGERS_H
#define XS_TRIGGERS_H

#include "core/xs.h"

/* A single decorated fn registration: the decorator's name, its
   evaluated args, the fn value to call, and the @once book-keeping
   flags. The registry is shared by --interp and --vm: each proto-load
   walks the fn's decorator list and appends one entry per decorator. */
typedef struct {
    char    *name;
    Value  **args;
    int      n_args;
    Value   *fn;
    int      has_once;
    int      fired_once;

    /* Scheduling state for @every / @cron / @delayed. next_fire_ns is
       a CLOCK_MONOTONIC timestamp; 0 means "compute on first pass".
       interval_ns drives the @every cadence. The cron bitsets are
       compiled lazily from args[0] the first time the entry is seen
       in the scheduler. */
    int64_t  next_fire_ns;
    int64_t  interval_ns;
    uint64_t cron_min;
    uint32_t cron_hour;
    uint32_t cron_dom;
    uint16_t cron_mon;
    uint8_t  cron_dow;
    int      cron_compiled;

    /* @watch state. mtime and size of the last known snapshot of the
       watched path; the watcher fires when either changes. Linux
       prefers an inotify wd (>=0); other platforms poll the cached
       stat. wd_inotify == -2 marks "not yet initialised". */
    long long  watch_mtime_ns;
    long long  watch_size;
    int        watch_inotify_wd;
    int        watch_inotify_initialised;
} TriggerEntry;

void trigger_registry_clear(void);
void trigger_registry_register(const char *name, Value **args, int n_args,
                               Value *fn, int has_once);
int  trigger_registry_count(void);
TriggerEntry *trigger_registry_get(int i);

/* Reflection natives shared by --interp and --vm. */
Value *trigger_native_size(Interp *i, Value **args, int argc);
Value *trigger_native_name(Interp *i, Value **args, int argc);
Value *trigger_native_register(Interp *i, Value **args, int argc);

/* Lifecycle firing. on_start fires once after the top-level script
   finishes its first pass (so vars and fns are all visible) but
   before any persistent trigger work begins. on_exit fires when the
   process is about to leave the run loop. on_panic fires once if an
   unhandled exception bubbles to the top. */
void trigger_fire_on_start(Interp *i);
void trigger_fire_on_exit(Interp *i);
void trigger_fire_on_panic(Interp *i, Value *exc);

/* Signal plumbing. Install OS handlers for any registered @on_signal
   triggers; the handler sets a pending flag, and pump_signals fires
   the matching xs fn from a safe point inside the run loop. */
void trigger_install_signal_handlers(void);
void trigger_pump_signals(Interp *i);

/* Run the persistent-trigger event loop. Blocks while the registry
   has any @every / @cron / @delayed / @on_signal entry that hasn't
   spent itself, sleeping monotonic-clock until the next due event.
   Returns when the registry has nothing live left. */
void trigger_run_event_loop(Interp *i);

/* One pass of the periodic trigger machinery. Fires any @every /
   @cron / @delayed entries that are due, runs @watch checks, and
   drains the @on_signal pending flags. Long-running native loops
   (e.g. http.serve) call this between iterations so registered
   triggers don't starve. */
void trigger_pump_due(Interp *i);

#endif
