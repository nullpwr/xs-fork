#ifndef XS_LIMITS_H
#define XS_LIMITS_H

#include <stdint.h>
#include <stddef.h>

/* Runtime resource limits for the VM / interpreter / JIT.
   Limits are thread-local; each context running on its own thread
   gets independent counters. A value of 0 means "no limit". */

typedef enum {
    XS_LIMIT_NONE       = 0,
    XS_LIMIT_INSTRUCTIONS,
    XS_LIMIT_WALL_TIME,
    XS_LIMIT_MEMORY
} XSLimitKind;

/* Reset all counters and clear any pending exceeded flag. Does not
   clear the configured caps. Call at the start of every top-level
   eval / vm_run so budgets are per-invocation, not per-process. */
void    xs_limits_reset(void);

/* Clear both counters and configured caps. Returns to "no limits". */
void    xs_limits_clear(void);

/* Configure caps. 0 disables that particular limit. */
void    xs_limits_set_instructions(uint64_t budget);
void    xs_limits_set_wall_time_ms(uint64_t ms);
void    xs_limits_set_memory_bytes(size_t bytes);

/* Query current configuration and usage. */
uint64_t xs_limits_get_instructions_budget(void);
uint64_t xs_limits_get_instructions_used(void);
uint64_t xs_limits_get_wall_time_budget_ms(void);
size_t   xs_limits_get_memory_budget(void);
size_t   xs_limits_get_memory_rss(void);

/* Returns non-zero (the XSLimitKind) if any limit has been exceeded.
   Once tripped, stays tripped until xs_limits_reset(). */
int     xs_limits_exceeded(void);

/* Hot-path tick. Increments the instruction counter and, every N
   ticks, checks wall-time and memory caps. Returns non-zero if a
   limit was just exceeded; the caller should raise a catchable
   runtime error (see xs_limits_throw). */
int     xs_limits_tick(void);

/* Slow-path check. Verifies all configured limits. Use this at
   coarse boundaries (statement head in the interpreter, every few
   hundred VM opcodes). Returns non-zero if exceeded. */
int     xs_limits_check(void);

/* Raise the tripped limit through the normal runtime-error path, so
   it's catchable from XS with try/catch. Safe to call even when no
   limit is tripped (no-op in that case). */
void    xs_limits_throw_if_exceeded(void);

/* Human-readable name of the tripped limit, or "none". */
const char *xs_limits_exceeded_name(void);

#endif
