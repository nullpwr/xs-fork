#ifndef XS_CONCURRENT_H
#define XS_CONCURRENT_H

#include "core/xs_thread.h"
#include "core/value.h"

/* Global interpreter lock. Every thread that touches XS state must hold
   this. Acquired by the main thread at startup. spawned threads acquire
   it before running their closure and release it before exiting.
   Blocking ops (channel.recv on empty, sleep) release the GIL while
   waiting and reacquire when ready. */
void xs_gil_init(void);
void xs_gil_acquire(void);
void xs_gil_release(void);

/* Spawn a new thread that will run `closure` to completion under the
   GIL. Returns a future-like map { _task_id, _status, _result }. */
struct Interp;
Value *xs_spawn_thread(struct Interp *parent, Value *closure);

/* Block until the given task finishes and return its result. Releases
   the GIL while waiting. */
Value *xs_await_task(int task_id);

/* Wait for every interp-spawned task that hasn't been awaited yet.
   atexit calls this so fire-and-forget spawns finish before the
   process exits. Mirrors vm_drain_tasks for the interp backend. */
void xs_drain_interp_tasks(void);

/* Channel primitives backed by mutex + condvar. Each channel value is
   a regular XS_MAP with `_buf` (FIFO array) and `_chan_id` (int index
   into a global mutex/condvar table; allocate with xs_chan_alloc). */
int    xs_chan_alloc(void);
Value *xs_chan_send(Value *ch, Value *v);
Value *xs_chan_recv(Value *ch, struct Interp *interp);
Value *xs_chan_try_recv(Value *ch);
int    xs_chan_len(Value *ch);

/* Sleep that releases the GIL for the duration. */
void xs_sleep_seconds(double secs);

/* Spawn a lazy-generator worker thread. The worker waits for the
   first .next() (which sends a token on resume_chan), then runs the
   closure with the thread-local yield/resume channel slots installed.
   When the closure returns, the worker sends an EOS sentinel map
   {_gen_eos: true} on yield_chan and exits. */
void xs_spawn_generator(struct Interp *parent, Value *closure,
                        Value *yield_chan, Value *resume_chan);

/* Per-thread generator handoff channels. NODE_YIELD reads these; the
   worker thread sets them on entry and restores them on exit. They
   have to be thread-local because all generator workers share the
   single Interp pointer under the GIL. */
Value *xs_gen_tls_yield_chan(void);
Value *xs_gen_tls_resume_chan(void);
void   xs_gen_tls_set(Value *yield_chan, Value *resume_chan);

#endif
