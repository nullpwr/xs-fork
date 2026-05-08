#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include "core/xs_bigint.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif

/* shared builtins defined in builtins.c; also registered as globals */
Value *builtin_abs(Interp *, Value **, int);
Value *builtin_pow(Interp *, Value **, int);
Value *builtin_log(Interp *, Value **, int);
Value *builtin_sqrt(Interp *, Value **, int);
Value *builtin_floor(Interp *, Value **, int);
Value *builtin_ceil(Interp *, Value **, int);
Value *builtin_round(Interp *, Value **, int);
Value *builtin_sin(Interp *, Value **, int);
Value *builtin_cos(Interp *, Value **, int);
Value *builtin_tan(Interp *, Value **, int);

/* Math.* one-arg wrappers */
#define MATH1(fname, cfunc) \
static Value *native_math_##fname(Interp *ig, Value **a, int n) { \
    (void)ig; \
    double v=(n>0&&VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(n>0&&VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):0.0; \
    return xs_float(cfunc(v)); \
}
MATH1(cbrt, cbrt)
MATH1(log2_fn, log2)
MATH1(log10_fn, log10)
MATH1(sinh_fn, sinh)
MATH1(cosh_fn, cosh)
MATH1(tanh_fn, tanh)
MATH1(asin_fn, asin)
MATH1(acos_fn, acos)
MATH1(atan_fn, atan)
MATH1(exp_fn, exp)
#undef MATH1

static Value *native_math_gcd(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_int(0);
    int64_t x=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    int64_t y=(VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):(int64_t)a[1]->f;
    if (x<0) x=-x;
    if (y<0) y=-y;
    while (y) { int64_t t=y; y=x%y; x=t; }
    return xs_int(x);
}
static Value *native_math_lcm(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_int(0);
    int64_t x=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    int64_t y=(VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):(int64_t)a[1]->f;
    if (x<0) x=-x;
    if (y<0) y=-y;
    if (x==0||y==0) return xs_int(0);
    int64_t gcd=x, b2=y;
    while (b2) { int64_t t=b2; b2=gcd%b2; gcd=t; }
    return xs_int(x/gcd*y);
}
static Value *native_math_factorial(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_int(1);
    int64_t k=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    if (k<0) return xs_int(0);
    /* 20! still fits in int64; from 21! we promote to bigint so the
       result stays correct instead of silently wrapping into the
       negative range like factorial(30) = -8764578968847253504. */
    if (k <= 20) {
        int64_t r = 1;
        for (int64_t j = 2; j <= k; j++) r *= j;
        return xs_int(r);
    }
    XSBigInt *bi = bigint_from_i64(1);
    for (int64_t j = 2; j <= k; j++) {
        XSBigInt *jb = bigint_from_i64(j);
        XSBigInt *next = bigint_mul(bi, jb);
        bigint_free(bi); bigint_free(jb);
        bi = next;
    }
    return xs_bigint_val(bi);
}
static Value *native_math_sign(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_int(0);
    if (VAL_TAG(a[0])==XS_INT) return xs_int(VAL_INT(a[0])>0?1:VAL_INT(a[0])<0?-1:0);
    double v=a[0]->f; return xs_int(v>0.0?1:v<0.0?-1:0);
}
static Value *native_math_degrees(Interp *ig, Value **a, int n) {
    (void)ig;
    double v=(n>0&&VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(n>0&&VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):0.0;
    return xs_float(v*180.0/M_PI);
}
static Value *native_math_radians(Interp *ig, Value **a, int n) {
    (void)ig;
    double v=(n>0&&VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(n>0&&VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):0.0;
    return xs_float(v*M_PI/180.0);
}
static Value *native_math_trunc(Interp *ig, Value **a, int n) {
    (void)ig;
    double v=(n>0&&VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(n>0&&VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):0.0;
    return xs_float(trunc(v));
}
static Value *native_math_lerp(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<3) return xs_float(0.0);
    double av=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double bv=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    double tv=(VAL_TAG(a[2])==XS_FLOAT)?a[2]->f:(double)VAL_INT(a[2]);
    return xs_float(av+(bv-av)*tv);
}
static Value *native_math_clamp(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<3) return xs_float(0.0);
    double val=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double lo =(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    double hi =(VAL_TAG(a[2])==XS_FLOAT)?a[2]->f:(double)VAL_INT(a[2]);
    if (val<lo) return xs_float(lo);
    if (val>hi) return xs_float(hi);
    return xs_float(val);
}
static Value *native_math_atan2(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_float(0.0);
    double y=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double x=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    return xs_float(atan2(y,x));
}
static Value *native_math_hypot(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_float(0.0);
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double y=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    return xs_float(hypot(x,y));
}
static Value *native_math_isnan(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return value_incref(XS_FALSE_VAL);
    double v=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    return isnan(v)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_math_isinf(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return value_incref(XS_FALSE_VAL);
    double v=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    return isinf(v)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}

#define MATH1_EXTRA(fname, cfunc) \
static Value *native_math_##fname(Interp *ig, Value **a, int n) { \
    (void)ig; \
    double v=(n>0&&VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(n>0&&VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):0.0; \
    return xs_float(cfunc(v)); \
}
MATH1_EXTRA(asinh_fn, asinh)
MATH1_EXTRA(acosh_fn, acosh)
MATH1_EXTRA(atanh_fn, atanh)
MATH1_EXTRA(expm1_fn, expm1)
MATH1_EXTRA(log1p_fn, log1p)
MATH1_EXTRA(erf_fn,   erf)
MATH1_EXTRA(erfc_fn,  erfc)
MATH1_EXTRA(gamma_fn, tgamma)
MATH1_EXTRA(lgamma_fn,lgamma)
#undef MATH1_EXTRA

static Value *native_math_fmod(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_float(0.0);
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double y=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    return xs_float(fmod(x,y));
}

static Value *native_math_modf(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_array_new();
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double ipart;
    double fpart=modf(x,&ipart);
    Value *arr=xs_array_new();
    Value *vi=xs_float(ipart); array_push(arr->arr,vi); value_decref(vi);
    Value *vf=xs_float(fpart); array_push(arr->arr,vf); value_decref(vf);
    return arr;
}

static Value *native_math_copysign(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_float(0.0);
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double y=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    return xs_float(copysign(x,y));
}

static Value *native_math_isclose(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return value_incref(XS_FALSE_VAL);
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    double y=(VAL_TAG(a[1])==XS_FLOAT)?a[1]->f:(double)VAL_INT(a[1]);
    double rel_tol=(n>=3)?((VAL_TAG(a[2])==XS_FLOAT)?a[2]->f:(double)VAL_INT(a[2])):1e-9;
    double abs_tol=(n>=4)?((VAL_TAG(a[3])==XS_FLOAT)?a[3]->f:(double)VAL_INT(a[3])):0.0;
    double diff=fabs(x-y);
    if (diff<=abs_tol) return value_incref(XS_TRUE_VAL);
    double mx=fabs(x)>fabs(y)?fabs(x):fabs(y);
    return (diff<=rel_tol*mx)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}

static Value *native_math_frexp(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_array_new();
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    int exp_val;
    double mant=frexp(x,&exp_val);
    Value *arr=xs_array_new();
    Value *vm=xs_float(mant); array_push(arr->arr,vm); value_decref(vm);
    Value *ve=xs_int(exp_val); array_push(arr->arr,ve); value_decref(ve);
    return arr;
}

static Value *native_math_ldexp(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_float(0.0);
    double x=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    int exp_val=(VAL_TAG(a[1])==XS_INT)?(int)VAL_INT(a[1]):(int)a[1]->f;
    return xs_float(ldexp(x,exp_val));
}

static Value *native_math_comb(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_int(0);
    int64_t nn=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    int64_t kk=(VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):(int64_t)a[1]->f;
    if (kk<0||kk>nn||nn<0) return xs_int(0);
    if (kk>nn-kk) kk=nn-kk;
    int64_t r=1;
    for (int64_t j=0;j<kk;j++) { r=r*(nn-j)/(j+1); }
    return xs_int(r);
}

static Value *native_math_perm(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_int(0);
    int64_t nn=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    int64_t kk=(n>=2)?((VAL_TAG(a[1])==XS_INT)?VAL_INT(a[1]):(int64_t)a[1]->f):nn;
    if (kk<0||kk>nn||nn<0) return xs_int(0);
    int64_t r=1;
    for (int64_t j=0;j<kk;j++) r*=(nn-j);
    return xs_int(r);
}

static double math_to_double(Value *v) {
    if (VAL_TAG(v)==XS_FLOAT) return v->f;
    if (VAL_TAG(v)==XS_INT)   return (double)VAL_INT(v);
    return 0.0;
}

static Value *native_math_prod(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return xs_float(1.0);
    XSArray *arr=a[0]->arr;
    double r=1.0;
    for (int j=0;j<arr->len;j++) r*=math_to_double(arr->items[j]);
    return xs_float(r);
}

static Value *native_math_sum(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return xs_float(0.0);
    XSArray *arr=a[0]->arr;
    double r=0.0;
    for (int j=0;j<arr->len;j++) r+=math_to_double(arr->items[j]);
    return xs_float(r);
}

static int args_all_ints(Value **a, int n) {
    for (int j = 0; j < n; j++) if (!a[j] || VAL_TAG(a[j]) != XS_INT) return 0;
    return 1;
}

static Value *native_math_min_arr(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n == 0) return value_incref(XS_NULL_VAL);
    if (n == 1 && VAL_TAG(a[0]) == XS_ARRAY) {
        XSArray *arr = a[0]->arr;
        if (arr->len == 0) return value_incref(XS_NULL_VAL);
        double r = math_to_double(arr->items[0]);
        int all_int = VAL_TAG(arr->items[0]) == XS_INT;
        for (int j = 1; j < arr->len; j++) {
            double v = math_to_double(arr->items[j]);
            if (v < r) r = v;
            if (VAL_TAG(arr->items[j]) != XS_INT) all_int = 0;
        }
        return all_int ? xs_int((int64_t)r) : xs_float(r);
    }
    double r = math_to_double(a[0]);
    for (int j = 1; j < n; j++) { double v = math_to_double(a[j]); if (v < r) r = v; }
    return args_all_ints(a, n) ? xs_int((int64_t)r) : xs_float(r);
}

static Value *native_math_max_arr(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n == 0) return value_incref(XS_NULL_VAL);
    if (n == 1 && VAL_TAG(a[0]) == XS_ARRAY) {
        XSArray *arr = a[0]->arr;
        if (arr->len == 0) return value_incref(XS_NULL_VAL);
        double r = math_to_double(arr->items[0]);
        int all_int = VAL_TAG(arr->items[0]) == XS_INT;
        for (int j = 1; j < arr->len; j++) {
            double v = math_to_double(arr->items[j]);
            if (v > r) r = v;
            if (VAL_TAG(arr->items[j]) != XS_INT) all_int = 0;
        }
        return all_int ? xs_int((int64_t)r) : xs_float(r);
    }
    double r = math_to_double(a[0]);
    for (int j = 1; j < n; j++) { double v = math_to_double(a[j]); if (v > r) r = v; }
    return args_all_ints(a, n) ? xs_int((int64_t)r) : xs_float(r);
}

static Value *native_math_mean(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY||a[0]->arr->len==0) return xs_float(0.0);
    XSArray *arr=a[0]->arr;
    double r=0.0;
    for (int j=0;j<arr->len;j++) r+=math_to_double(arr->items[j]);
    return xs_float(r/(double)arr->len);
}

Value *make_math_module(void) {
    XSMap *m = map_new();

#define MATH_CONST(name, val) do { \
    Value *cv = xs_float(val); map_set(m, name, cv); value_decref(cv); \
} while(0)
    MATH_CONST("PI",     M_PI);
    MATH_CONST("pi",     M_PI);
    MATH_CONST("E",      M_E);
    MATH_CONST("e",      M_E);
    MATH_CONST("TAU",    2*M_PI);
    MATH_CONST("tau",    2*M_PI);
    MATH_CONST("INF",    HUGE_VAL);
    MATH_CONST("inf",    HUGE_VAL);
    MATH_CONST("NAN",    0.0/0.0);
    MATH_CONST("nan",    0.0/0.0);
#undef MATH_CONST

#define REG(name, fn) map_take(m, name, xs_native(fn))

    REG("sin",       builtin_sin);
    REG("cos",       builtin_cos);
    REG("tan",       builtin_tan);
    REG("asin",      native_math_asin_fn);
    REG("acos",      native_math_acos_fn);
    REG("atan",      native_math_atan_fn);
    REG("atan2",     native_math_atan2);
    REG("sinh",      native_math_sinh_fn);
    REG("cosh",      native_math_cosh_fn);
    REG("tanh",      native_math_tanh_fn);
    REG("asinh",     native_math_asinh_fn);
    REG("acosh",     native_math_acosh_fn);
    REG("atanh",     native_math_atanh_fn);

    REG("sqrt",      builtin_sqrt);
    REG("cbrt",      native_math_cbrt);
    REG("exp",       native_math_exp_fn);
    REG("expm1",     native_math_expm1_fn);
    REG("log",       builtin_log);
    REG("log2",      native_math_log2_fn);
    REG("log10",     native_math_log10_fn);
    REG("log1p",     native_math_log1p_fn);

    REG("floor",     builtin_floor);
    REG("ceil",      builtin_ceil);
    REG("round",     builtin_round);
    REG("trunc",     native_math_trunc);

    REG("abs",       builtin_abs);
    REG("pow",       builtin_pow);
    REG("hypot",     native_math_hypot);
    REG("gcd",       native_math_gcd);
    REG("lcm",       native_math_lcm);
    REG("factorial", native_math_factorial);
    REG("is_nan",    native_math_isnan);
    REG("is_inf",    native_math_isinf);
    REG("isnan",     native_math_isnan);
    REG("isinf",     native_math_isinf);
    REG("clamp",     native_math_clamp);
    REG("lerp",      native_math_lerp);
    REG("sign",      native_math_sign);
    REG("degrees",   native_math_degrees);
    REG("radians",   native_math_radians);
    REG("fmod",      native_math_fmod);
    REG("modf",      native_math_modf);
    REG("copysign",  native_math_copysign);
    REG("isclose",   native_math_isclose);
    REG("frexp",     native_math_frexp);
    REG("ldexp",     native_math_ldexp);

    REG("comb",      native_math_comb);
    REG("perm",      native_math_perm);

    REG("prod",      native_math_prod);
    REG("sum",       native_math_sum);
    REG("min",       native_math_min_arr);
    REG("max",       native_math_max_arr);
    REG("mean",      native_math_mean);

    REG("erf",       native_math_erf_fn);
    REG("erfc",      native_math_erfc_fn);
    REG("gamma",     native_math_gamma_fn);
    REG("lgamma",    native_math_lgamma_fn);

#undef REG
    return xs_module(m);
}
