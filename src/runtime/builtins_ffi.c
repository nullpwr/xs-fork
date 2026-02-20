#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ffi (dlopen/dlsym) */
#ifdef XSC_ENABLE_PLUGINS
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

static Value *native_ffi_load(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
#ifdef _WIN32
    void *handle = (void *)LoadLibraryA(a[0]->s);
#else
    void *handle = dlopen(a[0]->s, RTLD_LAZY);
#endif
    if (!handle) return value_incref(XS_NULL_VAL);
    XSMap *h = map_new();
    map_take(h, "_handle", xs_int((int64_t)(uintptr_t)handle));
    map_set(h, "path", value_incref(a[0]));
    return xs_module(h);
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available (compile with XSC_ENABLE_PLUGINS=1)");
#endif
}

static Value *native_ffi_sym(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    Value *hp = map_get(a[0]->map, "_handle");
    if (!hp || VAL_TAG(hp) != XS_INT) return value_incref(XS_NULL_VAL);
    void *handle = (void*)(uintptr_t)VAL_INT(hp);
#ifdef _WIN32
    void *sym = (void *)GetProcAddress((HMODULE)handle, a[1]->s);
#else
    void *sym = dlsym(handle, a[1]->s);
#endif
    if (!sym) return value_incref(XS_NULL_VAL);
    XSMap *s = map_new();
    map_take(s, "_sym", xs_int((int64_t)(uintptr_t)sym));
    map_set(s, "name", value_incref(a[1]));
    return xs_module(s);
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available");
#endif
}

static Value *native_ffi_call(Interp *ig, Value **a, int n) {
    (void)ig;
    /* ffi.call(sym_handle, args_array): call a foreign function.
       If the sym has a _fn field pointing to a native function wrapper, call it.
       Otherwise return an error. */
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: ffi.call requires a symbol handle");

    /* Check for a native function wrapper in _fn field */
    Value *fn_v = map_get(a[0]->map, "_fn");
    if (fn_v && VAL_TAG(fn_v) == XS_NATIVE) {
        /* Pass args_array items as arguments */
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
            return fn_v->native(ig, a[1]->arr->items, a[1]->arr->len);
        }
        return fn_v->native(ig, NULL, 0);
    }
    if (fn_v && VAL_TAG(fn_v) == XS_FUNC) {
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
            return call_value(ig, fn_v, a[1]->arr->items, a[1]->arr->len, "ffi.call");
        }
        return call_value(ig, fn_v, NULL, 0, "ffi.call");
    }

    return xs_str("error: symbol has no callable _fn wrapper; generic FFI requires libffi");
}

static Value *native_ffi_close(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: ffi.close requires a library handle");
    Value *hp = map_get(a[0]->map, "_handle");
    if (!hp || VAL_TAG(hp) != XS_INT)
        return xs_str("error: invalid library handle");
    void *handle = (void *)(uintptr_t)VAL_INT(hp);
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
    /* Invalidate the handle */
    map_take(a[0]->map, "_handle", xs_int(0));
    map_set(a[0]->map, "_closed", value_incref(XS_TRUE_VAL));
    return xs_str("ok");
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available (compile with XSC_ENABLE_PLUGINS=1)");
#endif
}

static Value *native_ffi_typeof(Interp *ig, Value **a, int n) {
    (void)ig;
    /* ffi.typeof(sym_handle): return the type string of a symbol */
    if (n < 1) return xs_str("null");
    if (VAL_TAG(a[0]) == XS_MAP || VAL_TAG(a[0]) == XS_MODULE) {
        /* Check if it has a _sym field (it's a symbol handle) */
        Value *sym = map_get(a[0]->map, "_sym");
        if (sym) return xs_str("ffi_symbol");
        /* Check if it has a _handle field (it's a library handle) */
        Value *h = map_get(a[0]->map, "_handle");
        if (h) return xs_str("ffi_library");
        return xs_str("map");
    }
    /* For non-map values, return the XS type */
    switch (VAL_TAG(a[0])) {
        case XS_NULL:   return xs_str("null");
        case XS_BOOL:   return xs_str("bool");
        case XS_INT:    return xs_str("int");
        case XS_FLOAT:  return xs_str("float");
        case XS_STR:    return xs_str("str");
        case XS_FUNC:   return xs_str("fn");
        case XS_NATIVE: return xs_str("native_fn");
        default:        return xs_str("unknown");
    }
}

Value *make_ffi_module(void) {
    XSMap *m = map_new();
    map_take(m, "load",   xs_native(native_ffi_load));
    map_take(m, "sym",    xs_native(native_ffi_sym));
    map_take(m, "call",   xs_native(native_ffi_call));
    map_take(m, "close",  xs_native(native_ffi_close));
    map_take(m, "typeof", xs_native(native_ffi_typeof));
    return xs_module(m);
}
