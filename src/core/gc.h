/* gc.h - generational cycle-detecting garbage collector */
#ifndef XS_GC_H
#define XS_GC_H

#include "core/xs.h"
#include <stdint.h>

/* side-allocated tracking node (not embedded in Value) */
typedef struct GCNode {
    struct GCNode *gc_next;
    struct GCNode *gc_prev;
    Value         *value;
    int            gc_refs;   /* scratch field used during collection */
    int            generation; /* 0, 1, or 2 */
    int            frozen;     /* permanently excluded from collection */
} GCNode;

/* finalizer callback */
typedef void (*FinalizerFunc)(Value *obj);

/* finalizer registry entry */
typedef struct FinalizerEntry {
    Value                  *obj;
    FinalizerFunc           fn;
    struct FinalizerEntry  *next;
} FinalizerEntry;

/* weak reference entry */
typedef struct WeakRef {
    Value          *target;      /* NOT incref'd */
    int             alive;
    struct WeakRef *next;        /* global chain for quick invalidation */
} WeakRef;

/* per-generation list */
typedef struct {
    GCNode  head;       /* sentinel for doubly-linked list */
    int     count;      /* tracked objects in this generation */
    int     threshold;  /* collect when count exceeds this */
    int     collections; /* how many times this gen has been collected */
} GCGeneration;

/* cumulative stats */
typedef struct {
    int64_t total_collected;
    int64_t total_allocations;
    int     gen0_collections;
    int     gen1_collections;
    int     gen2_collections;
    double  total_gc_time_ms;
    int64_t peak_tracked;
} GCStats;

#define GC_NUM_GENERATIONS 3

void     gc_init(void);
void     gc_track(Value *v);
void     gc_untrack(Value *v);
int      gc_collect(void);
int      gc_collect_gen(int generation);
void     gc_enable(void);
void     gc_disable(void);
int      gc_is_enabled(void);
int      gc_tracked_count(void);
int      gc_total_collected(void);

void     gc_maybe_collect(void);

/* generational thresholds */
void     gc_set_threshold(int gen, int n);
int      gc_get_threshold(int gen);

/* freeze: permanently exclude from collection */
void     gc_freeze(Value *v);

/* finalizers */
void     gc_register_finalizer(Value *obj, FinalizerFunc fn);

/* weak references */
WeakRef *gc_weakref_create(Value *target);
Value   *gc_weakref_get(WeakRef *ref);
int      gc_weakref_alive(WeakRef *ref);
void     gc_weakref_free(WeakRef *ref);

/* stats */
GCStats  gc_get_stats(void);

/* debug mode */
void     gc_set_debug(int on);
int      gc_debug_enabled(void);

/* get all referrers of an object (caller frees returned array) */
Value  **gc_get_referrers(Value *obj, int *count_out);

/* lookup node for a value (NULL if not tracked) */
GCNode  *gc_find_node(Value *v);

#endif /* XS_GC_H */
