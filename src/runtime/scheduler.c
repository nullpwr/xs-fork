/* scheduler.c -- cooperative task scheduler for XS concurrency */
#include "runtime/scheduler.h"
#include "runtime/interp.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>

Scheduler *scheduler_new(void) {
    Scheduler *s = xs_calloc(1, sizeof(Scheduler));
    s->next_nursery = 1;
    return s;
}

void scheduler_free(Scheduler *s) {
    if (!s) return;
    Task *t = s->head;
    while (t) {
        Task *next = t->next;
        if (t->closure) value_decref(t->closure);
        if (t->result) value_decref(t->result);
        free(t);
        t = next;
    }
    free(s);
}

int scheduler_spawn(Scheduler *s, Value *closure, int nursery_id) {
    Task *t = xs_calloc(1, sizeof(Task));
    t->id = s->next_id++;
    t->closure = value_incref(closure);
    t->result = NULL;
    t->status = TASK_PENDING;
    t->nursery_id = nursery_id;
    t->next = NULL;

    if (s->tail) {
        s->tail->next = t;
        s->tail = t;
    } else {
        s->head = s->tail = t;
    }
    return t->id;
}

Task *scheduler_get_task(Scheduler *s, int task_id) {
    for (Task *t = s->head; t; t = t->next) {
        if (t->id == task_id) return t;
    }
    return NULL;
}

/* run one specific task */
int scheduler_run_task(Scheduler *s, int task_id, Interp *interp) {
    Task *t = scheduler_get_task(s, task_id);
    if (!t || t->status != TASK_PENDING) return 0;

    t->status = TASK_RUNNING;
    Task *prev_current = s->current;
    s->current = t;

    Value *r = call_value(interp, t->closure, NULL, 0, "spawn_task");

    s->current = prev_current;

    if (interp->cf.signal == CF_THROW || interp->cf.signal == CF_ERROR) {
        t->status = TASK_ERROR;
        t->result = interp->cf.value ? value_incref(interp->cf.value) : value_incref(XS_NULL_VAL);
        /* don't clear cf - let caller handle */
    } else {
        t->status = TASK_DONE;
        t->result = r ? r : value_incref(XS_NULL_VAL);
        /* clear any non-error signals that leaked (break/continue/return) */
        if (interp->cf.signal) {
            interp->cf.signal = 0;
            if (interp->cf.value) { value_decref(interp->cf.value); interp->cf.value = NULL; }
        }
        return 1;
    }

    if (r && t->status == TASK_ERROR) value_decref(r);
    return 1;
}

void scheduler_run_all(Scheduler *s, Interp *interp, int nursery_id) {
    for (Task *t = s->head; t; t = t->next) {
        if (t->status != TASK_PENDING) continue;
        if (nursery_id >= 0 && t->nursery_id != nursery_id) continue;
        if (interp->cf.signal) break;
        scheduler_run_task(s, t->id, interp);
    }
}

Value *scheduler_await(Scheduler *s, int task_id, Interp *interp) {
    Task *target = scheduler_get_task(s, task_id);
    if (!target) return value_incref(XS_NULL_VAL);

    /* if already done, return result */
    if (target->status == TASK_DONE || target->status == TASK_ERROR) {
        return target->result ? value_incref(target->result) : value_incref(XS_NULL_VAL);
    }

    /* run pending tasks in order up to and including the target */
    for (Task *t = s->head; t; t = t->next) {
        if (t->status != TASK_PENDING) continue;
        if (interp->cf.signal) break;
        scheduler_run_task(s, t->id, interp);
        if (t->id == task_id) break;
    }

    /* if target still pending (shouldn't happen), run it directly */
    if (target->status == TASK_PENDING) {
        scheduler_run_task(s, task_id, interp);
    }

    return target->result ? value_incref(target->result) : value_incref(XS_NULL_VAL);
}

Value *scheduler_make_future(int task_id) {
    Value *future = xs_map_new();
    Value *tid_v = xs_int(task_id);
    Value *is_future = xs_bool(1);
    Value *status_v = xs_str("pending");

    map_set(future->map, "_task_id", tid_v);
    map_set(future->map, "__is_future", is_future);
    map_set(future->map, "_status", status_v);

    value_decref(tid_v);
    value_decref(is_future);
    value_decref(status_v);
    return future;
}

int scheduler_is_future(Value *v, int *task_id_out) {
    if (!v || VAL_TAG(v) != XS_MAP) return 0;
    Value *marker = map_get(v->map, "__is_future");
    if (!marker || !value_truthy(marker)) return 0;
    Value *tid = map_get(v->map, "_task_id");
    if (!tid || VAL_TAG(tid) != XS_INT) return 0;
    if (task_id_out) *task_id_out = (int)VAL_INT(tid);
    return 1;
}
