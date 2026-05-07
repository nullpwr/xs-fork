#include "runtime/error.h"
#include "runtime/interp.h"
#include "diagnostic/diagnostic.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *g_current_source = NULL;

void xs_error_set_source(const char *source) {
    g_current_source = source;
}

extern Interp *g_current_interp;

Value *xs_error_new(const char *kind, const char *message, Value *cause) {
    Value *m = xs_map_new();
    Value *k   = xs_str(kind    ? kind    : "Error");
    Value *msg = xs_str(message ? message : "");
    map_set(m->map, "kind",    k);   value_decref(k);
    map_set(m->map, "message", msg); value_decref(msg);
    if (cause) map_set(m->map, "cause", cause);
    return m;
}

Value *xs_error_from_str(const char *message) {
    return xs_error_new("Error", message, NULL);
}

const char *xs_error_kind(Value *err) {
    if (!err || VAL_TAG(err) != XS_MAP) return "Error";
    Value *v = map_get(err->map, "kind");
    return (v && VAL_TAG(v) == XS_STR) ? v->s : "Error";
}

const char *xs_error_message(Value *err) {
    if (!err || VAL_TAG(err) != XS_MAP) return "";
    Value *v = map_get(err->map, "message");
    return (v && VAL_TAG(v) == XS_STR) ? v->s : "";
}

Value *xs_error_cause(Value *err) {
    if (!err || VAL_TAG(err) != XS_MAP) return NULL;
    return map_get(err->map, "cause");
}

/* runtime error reporting */

int g_xs_runtime_error_count = 0;
/* Thread-local because each spawn worker has its own try-frame stack
   and pending throw; sharing them across threads (the original layout)
   would let a worker park its exception on top of a sibling's. */
__thread int g_xs_in_try         = 0;
__thread Value *g_xs_pending_throw = NULL;
__thread int g_xs_throw_from_runtime = 0;

void xs_runtime_error(Span span, const char *label, const char *hint,
                      const char *fmt, ...) {
    int in_try = g_xs_in_try > 0
              || (g_current_interp && g_current_interp->try_depth > 0);
    /* Only count this against the process exit when there's no live
       try/catch frame to absorb it. A caught error must not poison
       the program's exit code. */
    if (!in_try) g_xs_runtime_error_count++;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    Diagnostic *d = diag_new(DIAG_ERROR, DIAG_PHASE_RUNTIME, NULL, "%s", buf);
    diag_annotate(d, span, 1, "%s", label ? label : buf);
    if (hint) diag_hint(d, "%s", hint);

    if (g_current_interp && g_current_interp->call_stack_len > 0) {
        int n = g_current_interp->call_stack_len;
        #define MAX_FRAMES 10
        /* collect local variable names from current env for innermost frame */
        char locals_buf[256] = {0};
        if (g_current_interp->env) {
            Env *e = g_current_interp->env;
            int pos = 0;
            for (int k = 0; k < e->len && pos < (int)sizeof(locals_buf) - 1; k++) {
                if (!e->bindings[k].name) continue;
                if (pos > 0 && pos < (int)sizeof(locals_buf) - 3)
                    pos += snprintf(locals_buf + pos, sizeof(locals_buf) - pos, ", ");
                pos += snprintf(locals_buf + pos, sizeof(locals_buf) - pos,
                                "%s", e->bindings[k].name);
            }
        }
        if (locals_buf[0]) {
            char hint_buf[300];
            snprintf(hint_buf, sizeof hint_buf, "locals: %s", locals_buf);
            diag_hint(d, "%s", hint_buf);
        }

        if (n <= MAX_FRAMES) {
            for (int j = n - 1; j >= 0; j--) {
                InterpFrame *f = &g_current_interp->call_stack[j];
                diag_push_frame(d,
                    f->call_span.file ? f->call_span.file : "<unknown>",
                    f->call_span.line, f->call_span.col, f->func_name);
            }
        } else {
            for (int j = n - 1; j >= n - 5; j--) {
                InterpFrame *f = &g_current_interp->call_stack[j];
                diag_push_frame(d,
                    f->call_span.file ? f->call_span.file : "<unknown>",
                    f->call_span.line, f->call_span.col, f->func_name);
            }
            char elide[64];
            snprintf(elide, sizeof elide, "... %d frames omitted ...", n - MAX_FRAMES);
            diag_push_frame(d, "", 0, 0, elide);
            for (int j = 4; j >= 0; j--) {
                InterpFrame *f = &g_current_interp->call_stack[j];
                diag_push_frame(d,
                    f->call_span.file ? f->call_span.file : "<unknown>",
                    f->call_span.line, f->call_span.col, f->func_name);
            }
        }
        #undef MAX_FRAMES
    }

    /* Make the error catchable: install a throw signal carrying a
       structured error value. If the surrounding code is inside a
       try/catch, the unwinder will pick this up and the catch block
       sees an error map with kind/message. If nothing catches it the
       process exits non-zero via g_xs_runtime_error_count. We do not
       overwrite an in-flight throw (e.g. one nested error during
       cleanup); the first error wins. */
    Value *err_value = NULL;
    if (g_current_interp && g_current_interp->cf.signal != CF_THROW) {
        err_value = xs_error_new(label ? label : "RuntimeError", buf, NULL);
        if (g_current_interp->cf.value) value_decref(g_current_interp->cf.value);
        g_current_interp->cf.signal = CF_THROW;
        g_current_interp->cf.value  = value_incref(err_value);
        g_xs_throw_from_runtime = 1;
    }
    /* Install a pending throw for the VM dispatcher. The VM has no
       interp pointer to set cf.signal on, so it polls this slot at the
       top of each instruction. Install whenever no interp is active
       (pure VM path) or we're already inside a try frame; the VM
       unwinder handles the no-catch case by printing "uncaught" and
       exiting with rc=1. */
    if ((g_xs_in_try > 0 || !g_current_interp) && !g_xs_pending_throw) {
        g_xs_pending_throw = err_value
            ? value_incref(err_value)
            : xs_error_new(label ? label : "RuntimeError", buf, NULL);
        g_xs_throw_from_runtime = 1;
    }
    if (err_value) value_decref(err_value);
    if (!in_try) diag_render_one(d, g_current_source, span.file);
    diag_free(d);
}
