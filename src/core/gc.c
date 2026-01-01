#define _POSIX_C_SOURCE 199309L

/*
 * gc.c - generational cycle-detecting garbage collector
 *
 * Three generations (CPython-style):
 *   gen0: newly allocated, collected often (threshold ~700 allocs)
 *   gen1: survived one collection, collected every ~10 gen0 runs
 *   gen2: long-lived, collected every ~10 gen1 runs
 *
 * Side-allocation model: GCNode is malloc'd separately and points
 * back to the Value. A hash table maps Value* -> GCNode* for O(1)
 * lookup. The cycle detection algorithm is the standard
 * trial-deletion approach used by CPython.
 */

#include "core/gc.h"
#include "core/value.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---- hash table: Value* -> GCNode* ---- */

#define HT_INIT_CAP 256

typedef struct {
    Value  **keys;
    GCNode **vals;
    int      cap;
    int      len;
} NodeMap;

static NodeMap node_map = {0};

static void nodemap_init(void) {
    if (node_map.keys) return;
    node_map.cap = HT_INIT_CAP;
    node_map.keys = calloc(node_map.cap, sizeof(Value *));
    node_map.vals = calloc(node_map.cap, sizeof(GCNode *));
    node_map.len = 0;
}

static unsigned ptr_hash(Value *p) {
    uintptr_t x = (uintptr_t)p;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return (unsigned)x;
}

static void nodemap_grow(void);

static void nodemap_set(Value *key, GCNode *val) {
    if (node_map.len * 2 >= node_map.cap) nodemap_grow();
    unsigned h = ptr_hash(key) & (node_map.cap - 1);
    while (node_map.keys[h]) {
        if (node_map.keys[h] == key) {
            node_map.vals[h] = val;
            return;
        }
        h = (h + 1) & (node_map.cap - 1);
    }
    node_map.keys[h] = key;
    node_map.vals[h] = val;
    node_map.len++;
}

static GCNode *nodemap_get(Value *key) {
    if (!node_map.keys || !key) return NULL;
    unsigned h = ptr_hash(key) & (node_map.cap - 1);
    while (node_map.keys[h]) {
        if (node_map.keys[h] == key) return node_map.vals[h];
        h = (h + 1) & (node_map.cap - 1);
    }
    return NULL;
}

static void nodemap_del(Value *key) {
    if (!node_map.keys) return;
    unsigned h = ptr_hash(key) & (node_map.cap - 1);
    while (node_map.keys[h]) {
        if (node_map.keys[h] == key) {
            /* robin-hood deletion: shift following entries back */
            node_map.keys[h] = NULL;
            node_map.vals[h] = NULL;
            node_map.len--;
            unsigned j = (h + 1) & (node_map.cap - 1);
            while (node_map.keys[j]) {
                unsigned ideal = ptr_hash(node_map.keys[j]) & (node_map.cap - 1);
                /* if ideal slot is behind h (wrapping), it needs to move */
                if ((j > h && (ideal <= h || ideal > j)) ||
                    (j < h && ideal <= h && ideal > j)) {
                    node_map.keys[h] = node_map.keys[j];
                    node_map.vals[h] = node_map.vals[j];
                    node_map.keys[j] = NULL;
                    node_map.vals[j] = NULL;
                    h = j;
                }
                j = (j + 1) & (node_map.cap - 1);
            }
            return;
        }
        h = (h + 1) & (node_map.cap - 1);
    }
}

static void nodemap_grow(void) {
    int old_cap = node_map.cap;
    Value **old_keys = node_map.keys;
    GCNode **old_vals = node_map.vals;

    node_map.cap = old_cap * 2;
    node_map.keys = calloc(node_map.cap, sizeof(Value *));
    node_map.vals = calloc(node_map.cap, sizeof(GCNode *));
    node_map.len = 0;

    for (int i = 0; i < old_cap; i++) {
        if (old_keys[i]) nodemap_set(old_keys[i], old_vals[i]);
    }
    free(old_keys);
    free(old_vals);
}

/* ---- global state ---- */

static GCGeneration gc_gens[GC_NUM_GENERATIONS];
static int gc_enabled     = 1;
static int gc_collecting  = 0;
static int gc_debug       = 0;
static int gc_initialized = 0;

static GCStats gc_stats = {0};

/* finalizer chain */
static FinalizerEntry *finalizer_head = NULL;

/* weak reference chain */
static WeakRef *weakref_head = NULL;

/* ---- init ---- */

static void init_gen(GCGeneration *g) {
    g->head.gc_next = &g->head;
    g->head.gc_prev = &g->head;
    g->head.value = NULL;
    g->count = 0;
    g->collections = 0;
}

void gc_init(void) {
    if (gc_initialized) return;
    gc_initialized = 1;
    nodemap_init();

    init_gen(&gc_gens[0]);
    gc_gens[0].threshold = 700;

    init_gen(&gc_gens[1]);
    gc_gens[1].threshold = 10;

    init_gen(&gc_gens[2]);
    gc_gens[2].threshold = 10;

    memset(&gc_stats, 0, sizeof(gc_stats));
}

/* ensure init has run (lazy) */
static void ensure_init(void) {
    if (!gc_initialized) gc_init();
}

/* ---- container check ---- */

static int is_container(Value *v) {
    if (!v) return 0;
    switch (VAL_TAG(v)) {
        case XS_ARRAY:
        case XS_TUPLE:
        case XS_MAP:
        case XS_MODULE:
        case XS_FUNC:
        case XS_STRUCT_VAL:
        case XS_ENUM_VAL:
        case XS_CLASS_VAL:
        case XS_INST:
        case XS_SIGNAL:
        case XS_ACTOR:
        case XS_OVERLOAD:
#ifdef XSC_ENABLE_VM
        case XS_CLOSURE:
#endif
            return 1;
        default:
            return 0;
    }
}

/* ---- list manipulation ---- */

static void list_insert(GCGeneration *gen, GCNode *node) {
    node->gc_next = gen->head.gc_next;
    node->gc_prev = &gen->head;
    gen->head.gc_next->gc_prev = node;
    gen->head.gc_next = node;
    gen->count++;
}

static void list_remove(GCNode *node) {
    if (!node->gc_next || !node->gc_prev) return;
    node->gc_prev->gc_next = node->gc_next;
    node->gc_next->gc_prev = node->gc_prev;
    node->gc_next = NULL;
    node->gc_prev = NULL;
}

/* ---- public tracking API ---- */

void gc_track(Value *v) {
    if (!v) return;
    ensure_init();
    if (nodemap_get(v)) return; /* already tracked */

    GCNode *node = malloc(sizeof(GCNode));
    if (!node) return;
    node->value = v;
    node->gc_refs = 0;
    node->generation = 0;
    node->frozen = 0;
    node->gc_next = NULL;
    node->gc_prev = NULL;

    list_insert(&gc_gens[0], node);
    nodemap_set(v, node);
    gc_stats.total_allocations++;

    int64_t total = 0;
    for (int i = 0; i < GC_NUM_GENERATIONS; i++) total += gc_gens[i].count;
    if (total > gc_stats.peak_tracked) gc_stats.peak_tracked = total;
}

void gc_untrack(Value *v) {
    if (!v) return;
    ensure_init();
    GCNode *node = nodemap_get(v);
    if (!node) return;

    int gen = node->generation;
    list_remove(node);
    if (gen >= 0 && gen < GC_NUM_GENERATIONS) gc_gens[gen].count--;

    nodemap_del(v);
    free(node);
}

GCNode *gc_find_node(Value *v) {
    if (!v) return NULL;
    ensure_init();
    return nodemap_get(v);
}

void gc_disable(void)    { gc_enabled = 0; }
void gc_enable(void)     { gc_enabled = 1; }
int  gc_is_enabled(void) { return gc_enabled; }

int gc_tracked_count(void) {
    ensure_init();
    int total = 0;
    for (int i = 0; i < GC_NUM_GENERATIONS; i++) total += gc_gens[i].count;
    return total;
}

int gc_total_collected(void) { return (int)gc_stats.total_collected; }

/* ---- visitor infrastructure ---- */

typedef void (*visit_fn)(Value *child, void *ctx);

static void visit_refs(Value *v, visit_fn visitor, void *ctx) {
    if (!v) return;
    switch (VAL_TAG(v)) {
        case XS_ARRAY:
        case XS_TUPLE:
            if (v->arr) {
                for (int i = 0; i < v->arr->len; i++)
                    if (v->arr->items[i]) visitor(v->arr->items[i], ctx);
            }
            break;
        case XS_MAP:
        case XS_MODULE:
            if (v->map) {
                for (int i = 0; i < v->map->cap; i++) {
                    if (v->map->keys[i] && v->map->vals[i])
                        visitor(v->map->vals[i], ctx);
                }
            }
            break;
        case XS_STRUCT_VAL:
            if (v->st && v->st->fields) {
                for (int i = 0; i < v->st->fields->cap; i++) {
                    if (v->st->fields->keys[i] && v->st->fields->vals[i])
                        visitor(v->st->fields->vals[i], ctx);
                }
            }
            break;
        case XS_ENUM_VAL:
            if (v->en) {
                if (v->en->arr_data) {
                    for (int i = 0; i < v->en->arr_data->len; i++)
                        if (v->en->arr_data->items[i])
                            visitor(v->en->arr_data->items[i], ctx);
                }
                if (v->en->map_data) {
                    for (int i = 0; i < v->en->map_data->cap; i++) {
                        if (v->en->map_data->keys[i] && v->en->map_data->vals[i])
                            visitor(v->en->map_data->vals[i], ctx);
                    }
                }
            }
            break;
        case XS_CLASS_VAL:
            if (v->cls) {
                if (v->cls->fields) {
                    for (int i = 0; i < v->cls->fields->cap; i++)
                        if (v->cls->fields->keys[i] && v->cls->fields->vals[i])
                            visitor(v->cls->fields->vals[i], ctx);
                }
                if (v->cls->methods) {
                    for (int i = 0; i < v->cls->methods->cap; i++)
                        if (v->cls->methods->keys[i] && v->cls->methods->vals[i])
                            visitor(v->cls->methods->vals[i], ctx);
                }
                if (v->cls->static_methods) {
                    for (int i = 0; i < v->cls->static_methods->cap; i++)
                        if (v->cls->static_methods->keys[i] && v->cls->static_methods->vals[i])
                            visitor(v->cls->static_methods->vals[i], ctx);
                }
            }
            break;
        case XS_INST:
            if (v->inst) {
                if (v->inst->fields) {
                    for (int i = 0; i < v->inst->fields->cap; i++)
                        if (v->inst->fields->keys[i] && v->inst->fields->vals[i])
                            visitor(v->inst->fields->vals[i], ctx);
                }
                if (v->inst->methods) {
                    for (int i = 0; i < v->inst->methods->cap; i++)
                        if (v->inst->methods->keys[i] && v->inst->methods->vals[i])
                            visitor(v->inst->methods->vals[i], ctx);
                }
            }
            break;
        case XS_SIGNAL:
            if (v->signal) {
                if (v->signal->value) visitor(v->signal->value, ctx);
                for (int i = 0; i < v->signal->nsubs; i++)
                    if (v->signal->subscribers[i])
                        visitor(v->signal->subscribers[i], ctx);
                if (v->signal->compute) visitor(v->signal->compute, ctx);
            }
            break;
        case XS_ACTOR:
            if (v->actor) {
                if (v->actor->state) {
                    for (int i = 0; i < v->actor->state->cap; i++)
                        if (v->actor->state->keys[i] && v->actor->state->vals[i])
                            visitor(v->actor->state->vals[i], ctx);
                }
                if (v->actor->methods) {
                    for (int i = 0; i < v->actor->methods->cap; i++)
                        if (v->actor->methods->keys[i] && v->actor->methods->vals[i])
                            visitor(v->actor->methods->vals[i], ctx);
                }
            }
            break;
        case XS_OVERLOAD:
            if (v->overload) {
                for (int i = 0; i < v->overload->len; i++)
                    if (v->overload->items[i])
                        visitor(v->overload->items[i], ctx);
            }
            break;
        case XS_FUNC:
            /* func closures can hold refs via default values, but those
               are AST nodes not Values - nothing to visit */
            break;
        default:
            break;
    }
}

/* ---- cycle detection visitors ---- */

/* phase 1: subtract internal references */
static void subtract_ref(Value *child, void *ctx) {
    (void)ctx;
    if (!child || !is_container(child)) return;
    GCNode *cn = nodemap_get(child);
    if (!cn) return;
    cn->gc_refs--;
}

/* phase 3: mark reachable from roots */
static void mark_reachable(Value *v);

static void mark_child(Value *child, void *ctx) {
    (void)ctx;
    if (!child || !is_container(child)) return;
    GCNode *cn = nodemap_get(child);
    if (!cn) return;
    if (cn->gc_refs > 0) return; /* already known reachable */
    cn->gc_refs = 1;
    mark_reachable(child);
}

static void mark_reachable(Value *v) {
    visit_refs(v, mark_child, NULL);
}

/* ---- finalizer management ---- */

void gc_register_finalizer(Value *obj, FinalizerFunc fn) {
    if (!obj || !fn) return;
    ensure_init();
    FinalizerEntry *e = malloc(sizeof(FinalizerEntry));
    if (!e) return;
    e->obj = obj;
    e->fn = fn;
    e->next = finalizer_head;
    finalizer_head = e;
}

static FinalizerFunc find_finalizer(Value *obj) {
    for (FinalizerEntry *e = finalizer_head; e; e = e->next) {
        if (e->obj == obj) return e->fn;
    }
    return NULL;
}

static void remove_finalizer(Value *obj) {
    FinalizerEntry **pp = &finalizer_head;
    while (*pp) {
        if ((*pp)->obj == obj) {
            FinalizerEntry *old = *pp;
            *pp = old->next;
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ---- weak reference management ---- */

WeakRef *gc_weakref_create(Value *target) {
    if (!target) return NULL;
    ensure_init();
    WeakRef *w = malloc(sizeof(WeakRef));
    if (!w) return NULL;
    w->target = target;
    w->alive = 1;
    w->next = weakref_head;
    weakref_head = w;
    return w;
}

Value *gc_weakref_get(WeakRef *ref) {
    if (!ref || !ref->alive) return NULL;
    return ref->target;
}

int gc_weakref_alive(WeakRef *ref) {
    if (!ref) return 0;
    return ref->alive;
}

void gc_weakref_free(WeakRef *ref) {
    if (!ref) return;
    WeakRef **pp = &weakref_head;
    while (*pp) {
        if (*pp == ref) {
            *pp = ref->next;
            free(ref);
            return;
        }
        pp = &(*pp)->next;
    }
    /* not found in chain, still free it */
    free(ref);
}

/* invalidate all weak refs pointing at a value about to be freed */
static void invalidate_weakrefs(Value *v) {
    for (WeakRef *w = weakref_head; w; w = w->next) {
        if (w->target == v && w->alive) {
            w->alive = 0;
            w->target = NULL;
        }
    }
}

/* ---- freeze ---- */

void gc_freeze(Value *v) {
    if (!v) return;
    ensure_init();
    GCNode *node = nodemap_get(v);
    if (node) node->frozen = 1;
}

/* ---- stats ---- */

GCStats gc_get_stats(void) {
    ensure_init();
    gc_stats.gen0_collections = gc_gens[0].collections;
    gc_stats.gen1_collections = gc_gens[1].collections;
    gc_stats.gen2_collections = gc_gens[2].collections;
    return gc_stats;
}

/* ---- thresholds ---- */

void gc_set_threshold(int gen, int n) {
    ensure_init();
    if (gen < 0 || gen >= GC_NUM_GENERATIONS) return;
    if (n < 1) n = 1;
    gc_gens[gen].threshold = n;
}

int gc_get_threshold(int gen) {
    ensure_init();
    if (gen < 0 || gen >= GC_NUM_GENERATIONS) return 0;
    return gc_gens[gen].threshold;
}

/* ---- debug ---- */

void gc_set_debug(int on) { gc_debug = on; }
int  gc_debug_enabled(void) { return gc_debug; }

/* ---- referrer search ---- */

Value **gc_get_referrers(Value *obj, int *count_out) {
    ensure_init();
    *count_out = 0;
    int cap = 8;
    int len = 0;
    Value **results = malloc(cap * sizeof(Value *));
    if (!results) return NULL;

    /* scan all generations */
    for (int g = 0; g < GC_NUM_GENERATIONS; g++) {
        GCGeneration *gen = &gc_gens[g];
        for (GCNode *n = gen->head.gc_next; n != &gen->head; n = n->gc_next) {
            Value *v = n->value;
            if (!v || v == obj) continue;
            /* check if v references obj */
            int found = 0;
            /* inline quick visitor */
            switch (VAL_TAG(v)) {
                case XS_ARRAY:
                case XS_TUPLE:
                    if (v->arr) {
                        for (int i = 0; i < v->arr->len && !found; i++)
                            if (v->arr->items[i] == obj) found = 1;
                    }
                    break;
                case XS_MAP:
                case XS_MODULE:
                    if (v->map) {
                        for (int i = 0; i < v->map->cap && !found; i++)
                            if (v->map->keys[i] && v->map->vals[i] == obj) found = 1;
                    }
                    break;
                case XS_STRUCT_VAL:
                    if (v->st && v->st->fields) {
                        for (int i = 0; i < v->st->fields->cap && !found; i++)
                            if (v->st->fields->keys[i] && v->st->fields->vals[i] == obj)
                                found = 1;
                    }
                    break;
                case XS_INST:
                    if (v->inst) {
                        if (v->inst->fields) {
                            for (int i = 0; i < v->inst->fields->cap && !found; i++)
                                if (v->inst->fields->keys[i] && v->inst->fields->vals[i] == obj)
                                    found = 1;
                        }
                        if (!found && v->inst->methods) {
                            for (int i = 0; i < v->inst->methods->cap && !found; i++)
                                if (v->inst->methods->keys[i] && v->inst->methods->vals[i] == obj)
                                    found = 1;
                        }
                    }
                    break;
                default:
                    break;
            }
            if (found) {
                if (len >= cap) {
                    cap *= 2;
                    results = realloc(results, cap * sizeof(Value *));
                    if (!results) { *count_out = 0; return NULL; }
                }
                results[len++] = v;
            }
        }
    }
    *count_out = len;
    return results;
}

/* ---- timing helper ---- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* ---- the collector ---- */

/*
 * Collect a specific generation (and all younger ones).
 * When collecting gen N, we merge gen 0..N into one workspace,
 * run cycle detection, free garbage, and promote survivors to gen N+1.
 */
int gc_collect_gen(int generation) {
    if (gc_collecting) return 0;
    gc_collecting = 1;
    ensure_init();

    if (generation < 0) generation = 0;
    if (generation >= GC_NUM_GENERATIONS) generation = GC_NUM_GENERATIONS - 1;

    double t0 = 0;
    if (gc_debug) t0 = now_ms();

    /*
     * Build a temporary merged list of all objects in gen 0..generation.
     * We use a flat array for the actual cycle detection so we
     * don't mess with the linked lists during the algorithm.
     */
    int work_cap = 64;
    int work_len = 0;
    GCNode **work = malloc(work_cap * sizeof(GCNode *));
    if (!work) { gc_collecting = 0; return 0; }

    for (int g = 0; g <= generation; g++) {
        GCGeneration *gen = &gc_gens[g];
        for (GCNode *n = gen->head.gc_next; n != &gen->head; n = n->gc_next) {
            if (n->frozen) continue;
            if (work_len >= work_cap) {
                work_cap *= 2;
                work = realloc(work, work_cap * sizeof(GCNode *));
                if (!work) { gc_collecting = 0; return 0; }
            }
            work[work_len++] = n;
        }
    }

    if (work_len == 0) {
        free(work);
        /* still count as a collection for gen-scheduling purposes */
        for (int g = 0; g <= generation; g++)
            gc_gens[g].collections++;
        gc_collecting = 0;
        return 0;
    }

    /* phase 1: copy real refcounts into gc_refs */
    for (int i = 0; i < work_len; i++) {
        work[i]->gc_refs = work[i]->value->refcount;
    }

    /* phase 2: subtract internal references between tracked objects */
    for (int i = 0; i < work_len; i++) {
        visit_refs(work[i]->value, subtract_ref, NULL);
    }

    /* phase 3: objects with gc_refs > 0 are roots, mark reachable */
    for (int i = 0; i < work_len; i++) {
        if (work[i]->gc_refs > 0) {
            mark_reachable(work[i]->value);
        }
    }

    /* phase 4: identify garbage (gc_refs == 0) */
    int garbage_cap = 32;
    int garbage_len = 0;
    GCNode **garbage = malloc(garbage_cap * sizeof(GCNode *));
    if (!garbage) { free(work); gc_collecting = 0; return 0; }

    int survivor_cap = 32;
    int survivor_len = 0;
    GCNode **survivors = malloc(survivor_cap * sizeof(GCNode *));
    if (!survivors) { free(work); free(garbage); gc_collecting = 0; return 0; }

    for (int i = 0; i < work_len; i++) {
        if (work[i]->gc_refs == 0 && !work[i]->frozen) {
            if (garbage_len >= garbage_cap) {
                garbage_cap *= 2;
                garbage = realloc(garbage, garbage_cap * sizeof(GCNode *));
                if (!garbage) { free(work); free(survivors); gc_collecting = 0; return 0; }
            }
            garbage[garbage_len++] = work[i];
        } else {
            if (survivor_len >= survivor_cap) {
                survivor_cap *= 2;
                survivors = realloc(survivors, survivor_cap * sizeof(GCNode *));
                if (!survivors) { free(work); free(garbage); gc_collecting = 0; return 0; }
            }
            survivors[survivor_len++] = work[i];
        }
    }

    /* promote survivors to next generation */
    int next_gen = generation + 1;
    if (next_gen >= GC_NUM_GENERATIONS) next_gen = GC_NUM_GENERATIONS - 1;
    for (int i = 0; i < survivor_len; i++) {
        GCNode *s = survivors[i];
        if (s->generation < next_gen) {
            /* remove from current gen list */
            int old_gen = s->generation;
            list_remove(s);
            gc_gens[old_gen].count--;
            /* add to new gen */
            s->generation = next_gen;
            list_insert(&gc_gens[next_gen], s);
        }
    }
    free(survivors);

    /* phase 5: run finalizers on garbage (objects still alive during call) */
    for (int i = 0; i < garbage_len; i++) {
        Value *v = garbage[i]->value;
        FinalizerFunc fin = find_finalizer(v);
        if (fin) {
            fin(v);
            remove_finalizer(v);
        }
    }

    /* phase 6: invalidate weak refs and clean up internal references */
    for (int i = 0; i < garbage_len; i++) {
        Value *v = garbage[i]->value;
        invalidate_weakrefs(v);
    }

    /* build a quick lookup set for garbage values */
    /* use a simple sorted array + binary search for medium sets,
       or linear scan for small sets */
    int use_linear = (garbage_len < 64);

    /* for cycle-breaking: null out internal refs between garbage objects.
       external children get decref'd normally. */
    for (int i = 0; i < garbage_len; i++) {
        Value *v = garbage[i]->value;
        switch (VAL_TAG(v)) {
            case XS_ARRAY:
            case XS_TUPLE:
                if (v->arr) {
                    for (int j = 0; j < v->arr->len; j++) {
                        Value *child = v->arr->items[j];
                        if (child) {
                            int in_garbage = 0;
                            if (use_linear) {
                                for (int k = 0; k < garbage_len; k++)
                                    if (garbage[k]->value == child) { in_garbage = 1; break; }
                            } else {
                                for (int k = 0; k < garbage_len; k++)
                                    if (garbage[k]->value == child) { in_garbage = 1; break; }
                            }
                            if (!in_garbage)
                                value_decref(child);
                        }
                        v->arr->items[j] = NULL;
                    }
                    v->arr->len = 0;
                }
                break;
            case XS_MAP:
            case XS_MODULE:
                if (v->map) {
                    for (int j = 0; j < v->map->cap; j++) {
                        if (v->map->keys[j]) {
                            Value *child = v->map->vals[j];
                            if (child) {
                                int in_garbage = 0;
                                for (int k = 0; k < garbage_len; k++)
                                    if (garbage[k]->value == child) { in_garbage = 1; break; }
                                if (!in_garbage)
                                    value_decref(child);
                            }
                            v->map->vals[j] = NULL;
                            free(v->map->keys[j]);
                            v->map->keys[j] = NULL;
                        }
                    }
                    v->map->len = 0;
                }
                break;
            case XS_STRUCT_VAL:
                if (v->st && v->st->fields) {
                    for (int j = 0; j < v->st->fields->cap; j++) {
                        if (v->st->fields->keys[j]) {
                            Value *child = v->st->fields->vals[j];
                            if (child) {
                                int in_garbage = 0;
                                for (int k = 0; k < garbage_len; k++)
                                    if (garbage[k]->value == child) { in_garbage = 1; break; }
                                if (!in_garbage)
                                    value_decref(child);
                            }
                            v->st->fields->vals[j] = NULL;
                            free(v->st->fields->keys[j]);
                            v->st->fields->keys[j] = NULL;
                        }
                    }
                    v->st->fields->len = 0;
                }
                break;
            case XS_OVERLOAD:
                if (v->overload) {
                    for (int j = 0; j < v->overload->len; j++) {
                        Value *child = v->overload->items[j];
                        if (child) {
                            int in_garbage = 0;
                            for (int k = 0; k < garbage_len; k++)
                                if (garbage[k]->value == child) { in_garbage = 1; break; }
                            if (!in_garbage)
                                value_decref(child);
                        }
                        v->overload->items[j] = NULL;
                    }
                    v->overload->len = 0;
                }
                break;
            default:
                break;
        }
    }

    /* phase 7: untrack and free garbage objects */
    int freed = garbage_len;
    for (int i = 0; i < garbage_len; i++) {
        GCNode *node = garbage[i];
        Value *v = node->value;
        int gen = node->generation;

        /* unlink from gen list */
        list_remove(node);
        if (gen >= 0 && gen < GC_NUM_GENERATIONS) gc_gens[gen].count--;
        nodemap_del(v);

        /* free container internals then the Value */
        switch (VAL_TAG(v)) {
            case XS_ARRAY:
            case XS_TUPLE:
                if (v->arr) {
                    free(v->arr->items);
                    free(v->arr);
                }
                break;
            case XS_MAP:
            case XS_MODULE:
                if (v->map) {
                    free(v->map->keys);
                    free(v->map->vals);
                    free(v->map);
                }
                break;
            case XS_OVERLOAD:
                if (v->overload) {
                    free(v->overload->items);
                    free(v->overload);
                }
                break;
            case XS_FUNC:
                if (v->fn) {
                    v->fn->refcount--;
                    if (v->fn->refcount <= 0) {
                        free(v->fn->name);
                        free(v->fn->deprecated_msg);
                        free(v->fn->params);
                        free(v->fn->default_vals);
                        free(v->fn->variadic_flags);
                        if (v->fn->param_type_names) {
                            for (int j = 0; j < v->fn->nparams; j++)
                                free(v->fn->param_type_names[j]);
                            free(v->fn->param_type_names);
                        }
                        free(v->fn->ret_type_name);
                        free(v->fn->param_contracts);
                        free(v->fn);
                    }
                }
                break;
            case XS_STRUCT_VAL:
                if (v->st) {
                    v->st->refcount--;
                    if (v->st->refcount <= 0) {
                        free(v->st->type_name);
                        if (v->st->fields) {
                            free(v->st->fields->keys);
                            free(v->st->fields->vals);
                            free(v->st->fields);
                        }
                        free(v->st);
                    }
                }
                break;
            default:
                break;
        }
        free(v);
        free(node);
    }

    gc_stats.total_collected += freed;

    /* update collection counts */
    for (int g = 0; g <= generation; g++)
        gc_gens[g].collections++;

    if (gc_debug) {
        double elapsed = now_ms() - t0;
        gc_stats.total_gc_time_ms += elapsed;
        const char *gen_name[] = {"gen0", "gen1", "gen2"};
        fprintf(stderr, "gc: %s collected %d objects in %.1fms (tracked: %d)\n",
                gen_name[generation], freed, elapsed, gc_tracked_count());
    } else {
        double elapsed = now_ms() - t0;
        gc_stats.total_gc_time_ms += elapsed;
    }

    free(garbage);
    free(work);
    gc_collecting = 0;
    return freed;
}

/* collect all generations */
int gc_collect(void) {
    return gc_collect_gen(GC_NUM_GENERATIONS - 1);
}

/* automatic collection trigger (called from allocation paths) */
void gc_maybe_collect(void) {
    if (!gc_enabled) return;
    ensure_init();

    gc_gens[0].count++; /* approximate: count allocations as gen0 pressure */
    gc_gens[0].count--; /* undo - actual count managed by gc_track */

    gc_stats.total_allocations++;

    /* check if gen0 needs collecting */
    if (gc_gens[0].count > gc_gens[0].threshold) {
        /* check if we should also run higher generations */
        int target_gen = 0;

        if (gc_gens[0].collections > 0 &&
            (gc_gens[0].collections % gc_gens[1].threshold) == 0) {
            target_gen = 1;
            if (gc_gens[1].collections > 0 &&
                (gc_gens[1].collections % gc_gens[2].threshold) == 0) {
                target_gen = 2;
            }
        }

        gc_collect_gen(target_gen);
    }
}
