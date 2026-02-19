#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* test module */
static int test_passed_count = 0;
static int test_failed_count = 0;

static Value *native_test_assert(Interp *ig, Value **a, int n) {
    (void)ig;
    int cond=(n>0&&value_truthy(a[0]));
    const char *msg=(n>1&&VAL_TAG(a[1])==XS_STR)?a[1]->s:"assertion failed";
    if (cond) { test_passed_count++; }
    else { test_failed_count++; fprintf(stderr,"[FAIL] %s\n",msg); }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_eq(Interp *ig, Value **a, int n) {
    (void)ig;
    int eq=(n>=2&&value_equal(a[0],a[1]));
    const char *msg=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:"assert_eq failed";
    if (eq) { test_passed_count++; }
    else {
        test_failed_count++;
        char *s1=value_repr(a[0]); char *s2=value_repr(a[1]);
        fprintf(stderr,"[FAIL] %s: %s != %s\n",msg,s1,s2);
        free(s1); free(s2);
    }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_ne(Interp *ig, Value **a, int n) {
    (void)ig;
    int ne=(n>=2&&!value_equal(a[0],a[1]));
    const char *msg=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:"assert_ne failed";
    if (ne) test_passed_count++;
    else {
        test_failed_count++;
        char *s1=value_repr(a[0]);
        fprintf(stderr,"[FAIL] %s: values are equal: %s\n",msg,s1);
        free(s1);
    }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_gt(Interp *ig, Value **a, int n) {
    (void)ig;
    int ok=(n>=2&&value_cmp(a[0],a[1])>0);
    const char *msg=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:"assert_gt failed";
    if (ok) test_passed_count++;
    else { test_failed_count++; fprintf(stderr,"[FAIL] %s\n",msg); }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_lt(Interp *ig, Value **a, int n) {
    (void)ig;
    int ok=(n>=2&&value_cmp(a[0],a[1])<0);
    const char *msg=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:"assert_lt failed";
    if (ok) test_passed_count++;
    else { test_failed_count++; fprintf(stderr,"[FAIL] %s\n",msg); }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_close(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<3) return value_incref(XS_NULL_VAL);
    double av=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double bv=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    double eps=(VAL_TAG(a[2])==XS_FLOAT)?a[2]->f:(double)VAL_INT(a[2]);
    const char *msg=(n>=4&&VAL_TAG(a[3])==XS_STR)?a[3]->s:"assert_close failed";
    double diff=av-bv; if(diff<0)diff=-diff;
    if (diff<=eps) test_passed_count++;
    else { test_failed_count++; fprintf(stderr,"[FAIL] %s: |%g - %g| = %g > %g\n",msg,av,bv,diff,eps); }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_assert_throws(Interp *ig, Value **a, int n) {
    if (n<1) return value_incref(XS_NULL_VAL);
    const char *msg=(n>=2&&VAL_TAG(a[1])==XS_STR)?a[1]->s:"assert_throws failed";
    Value *fn=a[0];
    if (VAL_TAG(fn)!=XS_FUNC&&VAL_TAG(fn)!=XS_NATIVE) {
        test_failed_count++;
        fprintf(stderr,"[FAIL] %s: not a callable\n",msg);
        return value_incref(XS_NULL_VAL);
    }
    Value *res=call_value(ig,fn,NULL,0,"assert_throws");
    int threw=(ig->cf.signal==CF_THROW||ig->cf.signal==CF_ERROR||ig->cf.signal==CF_PANIC);
    if (threw) { CF_CLEAR(ig); test_passed_count++; }
    else { test_failed_count++; fprintf(stderr,"[FAIL] %s: expected throw\n",msg); }
    if (res) value_decref(res);
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_run(Interp *ig, Value **a, int n) {
    if (n>=1&&(VAL_TAG(a[0])==XS_MAP||VAL_TAG(a[0])==XS_MODULE)) {
        int nk=0; char **ks=map_keys(a[0]->map,&nk);
        int run_pass=0,run_fail=0;
        for (int j=0;j<nk;j++){
            Value *fn=map_get(a[0]->map,ks[j]);
            if (fn&&(VAL_TAG(fn)==XS_FUNC||VAL_TAG(fn)==XS_NATIVE)) {
                fprintf(stderr,"[RUN] %s\n",ks[j]);
                int before_fail=test_failed_count;
                Value *res=call_value(ig,fn,NULL,0,ks[j]);
                if (ig->cf.signal) CF_CLEAR(ig);
                if (res) value_decref(res);
                if (test_failed_count>before_fail) { run_fail++; }
                else { run_pass++; fprintf(stderr,"[PASS] %s\n",ks[j]); }
            }
            free(ks[j]);
        }
        free(ks);
        fprintf(stderr,"[SUMMARY] %d passed, %d failed\n",run_pass,run_fail);
        return xs_int(run_fail);
    }
    const char *name=(n>0&&VAL_TAG(a[0])==XS_STR)?a[0]->s:"test";
    fprintf(stderr,"[RUN] %s\n",name);
    if (n>=2&&(VAL_TAG(a[1])==XS_NATIVE||VAL_TAG(a[1])==XS_FUNC)) {
        Value *res=call_value(ig,a[1],NULL,0,name);
        if (ig->cf.signal) CF_CLEAR(ig);
        if (res) value_decref(res);
    }
    return value_incref(XS_NULL_VAL);
}
static Value *native_test_summary(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    fprintf(stderr,"[SUMMARY] %d passed, %d failed\n",test_passed_count,test_failed_count);
    return xs_int(test_failed_count);
}
Value *make_test_module(void) {
    XSMap *m=map_new();
    map_take(m,"assert",       xs_native(native_test_assert));
    map_take(m,"assert_eq",    xs_native(native_test_assert_eq));
    map_take(m,"assert_ne",    xs_native(native_test_assert_ne));
    map_take(m,"assert_gt",    xs_native(native_test_assert_gt));
    map_take(m,"assert_lt",    xs_native(native_test_assert_lt));
    map_take(m,"assert_close", xs_native(native_test_assert_close));
    map_take(m,"assert_throws",xs_native(native_test_assert_throws));
    map_take(m,"run",          xs_native(native_test_run));
    map_take(m,"summary",      xs_native(native_test_summary));
    return xs_module(m);
}
