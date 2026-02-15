#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <stdint.h>

/* random module */
static Value *native_random_int(Interp *ig, Value **args, int argc) {
    (void)ig;
    int64_t lo = 0, hi = 100;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) lo = VAL_INT(args[0]);
    if (argc >= 2 && VAL_TAG(args[1]) == XS_INT) hi = VAL_INT(args[1]);
    if (hi < lo) { int64_t tmp=lo; lo=hi; hi=tmp; }
    int64_t range2 = hi - lo + 1;
    return xs_int(lo + (range2 > 0 ? (int64_t)(rand() % (int)range2) : 0));
}
static Value *native_random_float(Interp *ig, Value **args, int argc) {
    (void)ig; (void)args; (void)argc;
    return xs_float((double)rand() / ((double)RAND_MAX + 1.0));
}
static Value *native_random_choice(Interp *ig, Value **args, int argc) {
    (void)ig;
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY || args[0]->arr->len == 0)
        return value_incref(XS_NULL_VAL);
    int idx = rand() % args[0]->arr->len;
    return value_incref(array_get(args[0]->arr, idx));
}
static Value *native_random_shuffle(Interp *ig, Value **args, int argc) {
    (void)ig;
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return value_incref(XS_NULL_VAL);
    Value *result = xs_array_new();
    for (int j2 = 0; j2 < args[0]->arr->len; j2++)
        array_push(result->arr, value_incref(array_get(args[0]->arr, j2)));
    for (int j2 = result->arr->len - 1; j2 > 0; j2--) {
        int k = rand() % (j2 + 1);
        Value *tmp2 = result->arr->items[j2];
        result->arr->items[j2] = result->arr->items[k];
        result->arr->items[k] = tmp2;
    }
    return result;
}
static Value *native_random_seed(Interp *ig, Value **args, int argc) {
    (void)ig;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) srand((unsigned)VAL_INT(args[0]));
    return value_incref(XS_NULL_VAL);
}
static Value *native_random_bool(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    return (rand()%2==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_random_choices(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_ARRAY||a[0]->arr->len==0) return xs_array_new();
    int64_t k=(VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):1;
    Value *arr=xs_array_new();
    XSArray *src=a[0]->arr;
    for (int64_t j=0;j<k;j++) {
        int idx=rand()%src->len;
        array_push(arr->arr,value_incref(src->items[idx]));
    }
    return arr;
}
static Value *native_random_shuffled(Interp *ig, Value **a, int n) {
    return native_random_shuffle(ig,a,n);
}
static Value *native_random_sample(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_ARRAY) return xs_array_new();
    XSArray *src=a[0]->arr;
    int64_t k=(VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):0;
    if (k>src->len) k=src->len;
    /* Fisher-Yates on a copy */
    Value *copy=xs_array_new();
    for (int j=0;j<src->len;j++) array_push(copy->arr,value_incref(src->items[j]));
    for (int j=copy->arr->len-1;j>0;j--) {
        int r=rand()%(j+1);
        Value *tmp=copy->arr->items[j];
        copy->arr->items[j]=copy->arr->items[r];
        copy->arr->items[r]=tmp;
    }
    /* Shrink to k */
    while (copy->arr->len>(int)k) {
        value_decref(copy->arr->items[--copy->arr->len]);
    }
    return copy;
}
static Value *native_random_gauss(Interp *ig, Value **a, int n) {
    (void)ig;
    double mu=(n>0)?(VAL_TAG(a[0])==XS_FLOAT?a[0]->f:(double)VAL_INT(a[0])):0.0;
    double sigma=(n>1)?(VAL_TAG(a[1])==XS_FLOAT?a[1]->f:(double)VAL_INT(a[1])):1.0;
    /* Box-Muller transform */
    double u1=((double)rand()+1.0)/((double)RAND_MAX+2.0);
    double u2=((double)rand()+1.0)/((double)RAND_MAX+2.0);
    double z=sqrt(-2.0*log(u1))*cos(2.0*M_PI*u2);
    return xs_float(mu+sigma*z);
}
static Value *native_random_uniform(Interp *ig, Value **a, int n) {
    (void)ig;
    double lo=(n>0)?(VAL_TAG(a[0])==XS_FLOAT?a[0]->f:(double)VAL_INT(a[0])):0.0;
    double hi=(n>1)?(VAL_TAG(a[1])==XS_FLOAT?a[1]->f:(double)VAL_INT(a[1])):1.0;
    double r=(double)rand()/((double)RAND_MAX+1.0);
    return xs_float(lo+r*(hi-lo));
}
static Value *native_random_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    int64_t cnt=(n>0&&VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):0;
    Value *arr=xs_array_new();
    for (int64_t j=0;j<cnt;j++) array_push(arr->arr,xs_int((int64_t)(rand()&0xff)));
    return arr;
}
static Value *native_random_hex_str(Interp *ig, Value **a, int n) {
    (void)ig;
    int64_t cnt=(n>0&&VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):0;
    char *buf=xs_malloc(cnt*2+1); buf[0]='\0';
    for (int64_t j=0;j<cnt;j++) snprintf(buf+j*2,3,"%02x",(unsigned)(rand()&0xff));
    Value *v=xs_str(buf); free(buf); return v;
}

Value *make_random_module(void) {
    XSMap *m = map_new();
    map_take(m, "int",     xs_native(native_random_int));
    map_take(m, "float",   xs_native(native_random_float));
    map_take(m, "bool",    xs_native(native_random_bool));
    map_take(m, "choice",  xs_native(native_random_choice));
    map_take(m, "choices", xs_native(native_random_choices));
    map_take(m, "shuffle", xs_native(native_random_shuffle));
    map_take(m, "shuffled",xs_native(native_random_shuffled));
    map_take(m, "sample",  xs_native(native_random_sample));
    map_take(m, "gauss",   xs_native(native_random_gauss));
    map_take(m, "uniform", xs_native(native_random_uniform));
    map_take(m, "bytes",   xs_native(native_random_bytes));
    map_take(m, "hex_str", xs_native(native_random_hex_str));
    map_take(m, "seed",    xs_native(native_random_seed));
    return xs_module(m);
}
