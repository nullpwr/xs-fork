/* gc_concurrent.h -- deferred-sweep concurrent collector.
 *
 * The synchronous trial-deletion algorithm in gc.c finishes by walking
 * every garbage object and calling free() on its arrays / maps /
 * payload. For heaps with thousands of garbage cycles that walk is
 * the dominant cost of a collection.
 *
 * gc_concurrent moves that walk to a background thread. The collector
 * still identifies garbage synchronously (sub-ms for typical heaps),
 * detaches it from the gen lists + node map, then queues the detached
 * GCNodes for the worker to free. The mutator returns to the next
 * statement without paying the free-time tax.
 *
 * Pause-time SLO (target):
 *   - synchronous algorithm phases: O(tracked) but fast; under 5 ms
 *     for heaps up to 100K objects
 *   - the multi-ms free walk: 0 ms in foreground (off-thread)
 *
 * Enable via gc_concurrent_enable() or XS_GC_CONCURRENT=1.
 */
#ifndef XS_GC_CONCURRENT_H
#define XS_GC_CONCURRENT_H

#include "core/gc.h"

void gc_concurrent_enable(void);
void gc_concurrent_disable(void);
int  gc_concurrent_is_enabled(void);

/* Queue a freshly-detached GCNode for the worker thread to free.
 * The node must already be off the gen list and removed from node_map.
 * No-op if concurrent GC is disabled (caller falls back to inline free). */
void gc_concurrent_queue(GCNode *node);

/* Block until the worker has freed everything currently queued. Called
 * at gc_shutdown and by anything that needs a clean state. */
void gc_concurrent_drain(void);

/* Stop the worker thread. Called at gc_shutdown. */
void gc_concurrent_stop(void);

/* Stats read off the worker thread (atomic loads). */
typedef struct {
    int64_t queued_total;
    int64_t freed_total;
    int     queue_depth;
    double  worker_idle_ratio;  /* 0.0 .. 1.0 */
} GCConcurrentStats;

GCConcurrentStats gc_concurrent_stats(void);

#endif
