#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* reflect */

static Value *native_reflect_type_of(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_str("null");
    switch (VAL_TAG(a[0])) {
        case XS_NULL:       return xs_str("null");
        case XS_BOOL:       return xs_str("bool");
        case XS_INT:        return xs_str("int");
        case XS_FLOAT:      return xs_str("float");
        case XS_STR:        return xs_str("str");
        case XS_CHAR:       return xs_str("char");
        case XS_ARRAY:      return xs_str("array");
        case XS_MAP:        return xs_str("map");
        case XS_TUPLE:      return xs_str("tuple");
        case XS_FUNC:       return xs_str("fn");
        case XS_NATIVE:     return xs_str("native_fn");
        case XS_STRUCT_VAL: return xs_str("struct");
        case XS_ENUM_VAL:   return xs_str("enum");
        case XS_CLASS_VAL:  return xs_str("class");
        case XS_INST:       return xs_str("instance");
        case XS_RANGE:      return xs_str("range");
        case XS_MODULE:     return xs_str("module");
#ifdef XSC_ENABLE_VM
        case XS_CLOSURE:    return xs_str("closure");
#endif
        default:            return xs_str("unknown");
    }
}

static Value *native_reflect_fields(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_array_new();
    XSMap *m = NULL;
    if (VAL_TAG(a[0]) == XS_MAP || VAL_TAG(a[0]) == XS_MODULE) m = a[0]->map;
    else if (VAL_TAG(a[0]) == XS_STRUCT_VAL) m = a[0]->st->fields;
    else if (VAL_TAG(a[0]) == XS_INST) m = a[0]->inst->fields;
    if (!m) return xs_array_new();

    int klen;
    char **keys = map_keys(m, &klen);
    Value *arr = xs_array_new();
    for (int i = 0; i < klen; i++) {
        XSMap *pair = map_new();
        map_set(pair, "name", xs_str(keys[i]));
        Value *v = map_get(m, keys[i]);
        if (v) map_set(pair, "value", value_incref(v));
        else   map_set(pair, "value", value_incref(XS_NULL_VAL));
        Value *pm = xs_module(pair);
        array_push(arr->arr, pm);
        value_decref(pm);
        free(keys[i]);
    }
    free(keys);
    return arr;
}

static Value *native_reflect_methods(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_array_new();
    XSMap *m = NULL;
    if (VAL_TAG(a[0]) == XS_INST && a[0]->inst->methods)
        m = a[0]->inst->methods;
    else if (VAL_TAG(a[0]) == XS_CLASS_VAL && a[0]->cls->methods)
        m = a[0]->cls->methods;
    if (!m) return xs_array_new();

    int klen;
    char **keys = map_keys(m, &klen);
    Value *arr = xs_array_new();
    for (int i = 0; i < klen; i++) {
        Value *s = xs_str(keys[i]);
        array_push(arr->arr, s);
        value_decref(s);
        free(keys[i]);
    }
    free(keys);
    return arr;
}

static Value *native_reflect_is_instance(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return value_incref(XS_FALSE_VAL);
    if (VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    const char *type_name = a[1]->s;
    const char *actual = NULL;
    switch (VAL_TAG(a[0])) {
        case XS_NULL:   actual = "null"; break;
        case XS_BOOL:   actual = "bool"; break;
        case XS_INT:    actual = "int"; break;
        case XS_FLOAT:  actual = "float"; break;
        case XS_STR:    actual = "str"; break;
        case XS_ARRAY:  actual = "array"; break;
        case XS_MAP:    actual = "map"; break;
        case XS_FUNC:   actual = "fn"; break;
        case XS_NATIVE: actual = "native_fn"; break;
        case XS_INST:   actual = a[0]->inst->class_ ? a[0]->inst->class_->name : "instance"; break;
        case XS_STRUCT_VAL: actual = a[0]->st->type_name; break;
        case XS_ENUM_VAL:   actual = a[0]->en->type_name; break;
        default: actual = "unknown"; break;
    }
    return (actual && strcmp(actual, type_name) == 0)
        ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *make_reflect_module(void) {
    XSMap *m = map_new();
    map_take(m, "type_of",     xs_native(native_reflect_type_of));
    map_take(m, "fields",      xs_native(native_reflect_fields));
    map_take(m, "methods",     xs_native(native_reflect_methods));
    map_take(m, "is_instance", xs_native(native_reflect_is_instance));
    return xs_module(m);
}
