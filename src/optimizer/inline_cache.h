/* inline_cache.h -- polymorphic inline caching for method dispatch. */
#ifndef INLINE_CACHE_H
#define INLINE_CACHE_H

#include "core/xs.h"

typedef struct {
    int64_t  type_tag;       /* receiver ValueTag */
    uint32_t method_hash;    /* hash of method name */
    Value   *cached_method;  /* the resolved method (not owned) */
    int      hits;           /* cache hit counter */
    int      misses;         /* cache miss counter */
} InlineCacheEntry;

#define IC_MAX_ENTRIES    1024
#define IC_POLY_SLOTS     4     /* polymorphic: up to 4 types per call site */

typedef struct {
    InlineCacheEntry entries[IC_MAX_ENTRIES][IC_POLY_SLOTS];
    int              slot_count[IC_MAX_ENTRIES]; /* how many slots used per site */
    int              total_hits;
    int              total_misses;
} InlineCacheTable;

/* global inline cache */
extern InlineCacheTable g_ic_table;

/* init/reset the cache */
void ic_init(void);
void ic_reset(void);

/* compute a call site ID from file/line or node_id */
static inline int ic_site_id(int node_id) {
    return (node_id & 0x7FFFFFFF) % IC_MAX_ENTRIES;
}

/* compute method name hash */
static inline uint32_t ic_method_hash(const char *name) {
    uint32_t h = 5381;
    while (*name) h = h * 33 + (unsigned char)*name++;
    return h;
}

/* lookup a cached method for the given receiver type and method name.
   returns the cached Value* or NULL on miss. */
Value *ic_lookup(int site_id, int64_t type_tag, uint32_t mhash);

/* insert/update a cache entry after a successful method resolution. */
void ic_update(int site_id, int64_t type_tag, uint32_t mhash, Value *method);

/* get cache stats */
void ic_stats(int *hits, int *misses);

#endif /* INLINE_CACHE_H */
