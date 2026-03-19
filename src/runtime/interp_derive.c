/* interp_derive.c -- builtins implementing #[derive(...)] traits.
   Wired into class instances at construction time when the source
   contained a derive attribute. Each one matches the corresponding
   trait's method shape (Debug.to_string(self), Clone.clone(self),
   Eq.eq(self, other)). */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "core/value.h"
#include "core/gc.h"
#include <stdlib.h>
#include <string.h>

Value *builtin_debug_to_string(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("<debug>");
    Value *self = args[0];
    char *repr = value_repr(self);
    Value *result = xs_str(repr);
    free(repr);
    return result;
}

Value *builtin_clone(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || VAL_TAG(args[0]) != XS_INST) return value_incref(XS_NULL_VAL);
    Value *self = args[0];
    XSInst *inst = xs_calloc(1, sizeof(XSInst));
    inst->class_ = self->inst->class_;
    if (inst->class_) inst->class_->refcount++;
    inst->fields = map_new();
    inst->methods = map_new();
    inst->refcount = 1;
    if (self->inst->fields) {
        int nk = 0; char **ks = map_keys(self->inst->fields, &nk);
        for (int j = 0; j < nk; j++) {
            Value *fv = map_get(self->inst->fields, ks[j]);
            if (fv) map_set(inst->fields, ks[j], value_incref(fv));
            free(ks[j]);
        }
        free(ks);
    }
    if (self->inst->methods) {
        int nk = 0; char **ks = map_keys(self->inst->methods, &nk);
        for (int j = 0; j < nk; j++) {
            Value *mv = map_get(self->inst->methods, ks[j]);
            if (mv) map_set(inst->methods, ks[j], value_incref(mv));
            free(ks[j]);
        }
        free(ks);
    }
    Value *result = xs_calloc(1, sizeof(Value));
    result->tag = XS_INST; result->refcount = 1;
    result->inst = inst;
    return result;
}

Value *builtin_struct_eq(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return value_incref(XS_FALSE_VAL);
    Value *self = args[0], *other = args[1];
    if (VAL_TAG(self) != XS_INST || VAL_TAG(other) != XS_INST) return value_incref(XS_FALSE_VAL);
    if (self->inst->class_ != other->inst->class_) return value_incref(XS_FALSE_VAL);
    if (!self->inst->fields || !other->inst->fields) return value_incref(XS_FALSE_VAL);
    int nk = 0; char **ks = map_keys(self->inst->fields, &nk);
    int eq = 1;
    for (int j = 0; j < nk; j++) {
        Value *a = map_get(self->inst->fields, ks[j]);
        Value *b = map_get(other->inst->fields, ks[j]);
        if (!b || !value_equal(a, b)) { eq = 0; }
        free(ks[j]);
    }
    free(ks);
    return eq ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}
