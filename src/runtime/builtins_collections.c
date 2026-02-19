#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* collections module */
/* Stack: returns a map with _type="Stack" and _data=[] */
static Value *collections_stack_new(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    Value *stack=xs_map_new();
    Value *type=xs_str("Stack"); map_set(stack->map,"_type",type); value_decref(type);
    Value *data=xs_array_new(); map_set(stack->map,"_data",data); value_decref(data);
    return stack;
}
/* PriorityQueue: returns a map with _type="PriorityQueue" and _data=[] */
static Value *collections_pq_new(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    Value *pq=xs_map_new();
    Value *type=xs_str("PriorityQueue"); map_set(pq->map,"_type",type); value_decref(type);
    Value *data=xs_array_new(); map_set(pq->map,"_data",data); value_decref(data);
    return pq;
}
/* Simple counter: Counter(arr) -> map of {item: count, _type: "Counter"} */
static Value *collections_counter(Interp *i, Value **a, int n) {
    (void)i;
    Value *result=xs_map_new();
    Value *type=xs_str("Counter"); map_set(result->map,"_type",type); value_decref(type);
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    for(int j=0;j<arr->len;j++){
        char *key=value_str(arr->items[j]);
        Value *cur=map_get(result->map,key);
        Value *next=xs_int(cur?(VAL_TAG(cur)==XS_INT?VAL_INT(cur):0)+1:1);
        map_set(result->map,key,next); value_decref(next);
        free(key);
    }
    return result;
}
static Value *collections_deque_new(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *d=xs_map_new();
    Value *t=xs_str("Deque"); map_set(d->map,"_type",t); value_decref(t);
    Value *data=xs_array_new(); map_set(d->map,"_data",data); value_decref(data);
    return d;
}
static Value *collections_set_new(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *s=xs_map_new();
    Value *t=xs_str("Set"); map_set(s->map,"_type",t); value_decref(t);
    Value *data=xs_map_new(); map_set(s->map,"_data",data); value_decref(data);
    /* If array passed, pre-populate */
    if (n>0&&VAL_TAG(a[0])==XS_ARRAY) {
        Value *d2=map_get(s->map,"_data");
        XSArray *arr=a[0]->arr;
        for (int j=0;j<arr->len;j++){
            char *k=value_str(arr->items[j]);
            Value *tv=value_incref(XS_TRUE_VAL);
            map_set(d2->map,k,tv); value_decref(tv);
            free(k);
        }
    }
    return s;
}
static Value *collections_ordered_map_new(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *om=xs_map_new();
    Value *t=xs_str("OrderedMap"); map_set(om->map,"_type",t); value_decref(t);
    Value *keys=xs_array_new(); map_set(om->map,"_keys",keys); value_decref(keys);
    Value *data=xs_map_new(); map_set(om->map,"_data",data); value_decref(data);
    return om;
}

static Value *collections_set_simple(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *result=xs_array_new();
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    /* track seen keys in a temporary map for dedup */
    XSMap *seen=map_new();
    for(int j=0;j<arr->len;j++){
        char *k=value_str(arr->items[j]);
        if (!map_get(seen,k)){
            Value *tv=value_incref(XS_TRUE_VAL);
            map_set(seen,k,tv); value_decref(tv);
            array_push(result->arr,value_incref(arr->items[j]));
        }
        free(k);
    }
    map_free(seen);
    return result;
}
static Value *collections_deque_simple(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    return xs_array_new();
}
static Value *collections_counter_simple(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *result=xs_map_new();
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    for(int j=0;j<arr->len;j++){
        char *key=value_str(arr->items[j]);
        Value *cur=map_get(result->map,key);
        Value *next=xs_int(cur?(VAL_TAG(cur)==XS_INT?VAL_INT(cur):0)+1:1);
        map_set(result->map,key,next); value_decref(next);
        free(key);
    }
    return result;
}
Value *make_collections_module(void) {
    XSMap *m=map_new();
    map_take(m,"Counter",      xs_native(collections_counter));
    map_take(m,"Stack",        xs_native(collections_stack_new));
    map_take(m,"PriorityQueue",xs_native(collections_pq_new));
    map_take(m,"Deque",        xs_native(collections_deque_new));
    map_take(m,"Set",          xs_native(collections_set_new));
    map_take(m,"OrderedMap",   xs_native(collections_ordered_map_new));
    map_take(m,"set",          xs_native(collections_set_simple));
    map_take(m,"deque",        xs_native(collections_deque_simple));
    map_take(m,"counter",      xs_native(collections_counter_simple));
    return xs_module(m);
}
