#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include "core/xs_regex.h"
#ifndef re_nsub
#define re_nsub nsub
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* re (regex) module */
/* Convert PCRE-style shorthand escapes to POSIX ERE equivalents */
static char *re_to_posix(const char *pat) {
    /* Allocate worst-case: each \X can expand to ~12 chars */
    size_t plen = strlen(pat);
    char *out = xs_malloc(plen * 12 + 1);
    char *p = out;
    for (size_t i = 0; i < plen; i++) {
        if (pat[i] == '\\' && i+1 < plen) {
            char c = pat[++i];
            switch (c) {
            case 'd': memcpy(p,"[0-9]",5);     p+=5; break;
            case 'D': memcpy(p,"[^0-9]",6);    p+=6; break;
            case 'w': memcpy(p,"[A-Za-z0-9_]",12); p+=12; break;
            case 'W': memcpy(p,"[^A-Za-z0-9_]",13); p+=13; break;
            case 's': memcpy(p,"[ \\t\\n\\r\\f\\v]",14); p+=14; break;
            case 'S': memcpy(p,"[^ \\t\\n\\r\\f\\v]",15); p+=15; break;
            default:  *p++='\\'; *p++=c; break;
            }
        } else {
            *p++ = pat[i];
        }
    }
    *p = '\0';
    return out;
}

static Value *native_re_test(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return value_incref(XS_FALSE_VAL);
    int r=(regexec(&re,a[1]->s,0,NULL,0)==0);
    regfree(&re); return r?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_re_match(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_NULL_VAL);
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return value_incref(XS_NULL_VAL);
    regmatch_t m; Value *res=value_incref(XS_NULL_VAL);
    if (regexec(&re,a[1]->s,1,&m,0)==0) {
        value_decref(res);
        res=xs_str_n(a[1]->s+m.rm_so,(int)(m.rm_eo-m.rm_so));
    }
    regfree(&re); return res;
}
static Value *native_re_find_all(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return xs_array_new();
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return xs_array_new();
    Value *arr=xs_array_new();
    const char *s=a[1]->s;
    regmatch_t m;
    while (*s&&regexec(&re,s,1,&m,0)==0) {
        array_push(arr->arr,xs_str_n(s+m.rm_so,(int)(m.rm_eo-m.rm_so)));
        if (m.rm_eo==m.rm_so) { s++; continue; }
        s+=m.rm_eo;
    }
    regfree(&re); return arr;
}
static Value *native_re_replace(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<3||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR||VAL_TAG(a[2])!=XS_STR) return n>1?value_incref(a[1]):xs_str("");
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return value_incref(a[1]);
    const char *s=a[1]->s; const char *rep=a[2]->s;
    regmatch_t m;
    if (regexec(&re,s,1,&m,0)!=0){regfree(&re);return value_incref(a[1]);}
    int replen=(int)strlen(rep);
    int rlen=(int)m.rm_so+replen+(int)strlen(s+m.rm_eo)+1;
    char *r=xs_malloc(rlen);
    memcpy(r,s,(size_t)m.rm_so);
    memcpy(r+m.rm_so,rep,(size_t)replen);
    strcpy(r+m.rm_so+replen,s+m.rm_eo);
    Value *v=xs_str(r); free(r); regfree(&re); return v;
}
static Value *native_re_replace_all(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<3||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR||VAL_TAG(a[2])!=XS_STR) return n>1?value_incref(a[1]):xs_str("");
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return value_incref(a[1]);
    const char *s=a[1]->s; const char *rep=a[2]->s; int replen=(int)strlen(rep);
    int cap=256; char *out=xs_malloc(cap); int oi=0;
    regmatch_t m;
    while (*s&&regexec(&re,s,1,&m,0)==0){
        int plen=(int)m.rm_so;
        if (oi+plen+replen+2>cap){cap=(cap+plen+replen)*2;out=xs_realloc(out,cap);}
        memcpy(out+oi,s,(size_t)plen); oi+=plen;
        memcpy(out+oi,rep,(size_t)replen); oi+=replen;
        if (m.rm_eo==m.rm_so){
            if (oi+2>cap){cap*=2;out=xs_realloc(out,cap);}
            out[oi++]=*s++;
        } else s+=m.rm_eo;
    }
    int sl=(int)strlen(s);
    if (oi+sl+2>cap){cap=oi+sl+2;out=xs_realloc(out,cap);}
    memcpy(out+oi,s,(size_t)sl); oi+=sl; out[oi]='\0';
    Value *v=xs_str(out); free(out); regfree(&re); return v;
}
static Value *native_re_split(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return xs_array_new();
    char *pat=re_to_posix(a[0]->s);
    regex_t re; int rc=regcomp(&re,pat,REG_EXTENDED); free(pat);
    if (rc!=0) return xs_array_new();
    Value *arr=xs_array_new();
    const char *s=a[1]->s;
    regmatch_t m;
    while (*s&&regexec(&re,s,1,&m,0)==0){
        array_push(arr->arr,xs_str_n(s,(int)m.rm_so));
        if (m.rm_eo==m.rm_so){s++;continue;}
        s+=m.rm_eo;
    }
    array_push(arr->arr,xs_str(s));
    regfree(&re); return arr;
}
static Value *native_re_groups(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return xs_array_new();
    regex_t re; if (regcomp(&re,a[0]->s,REG_EXTENDED)!=0) return xs_array_new();
    int ng=(int)re.re_nsub+1; if(ng<1)ng=1;
    regmatch_t *m=xs_malloc(ng*sizeof(regmatch_t));
    Value *arr=xs_array_new();
    if (regexec(&re,a[1]->s,(size_t)ng,m,0)==0){
        for(int j=1;j<ng;j++){
            if (m[j].rm_so<0) array_push(arr->arr,value_incref(XS_NULL_VAL));
            else array_push(arr->arr,xs_str_n(a[1]->s+m[j].rm_so,(int)(m[j].rm_eo-m[j].rm_so)));
        }
    }
    free(m); regfree(&re); return arr;
}
Value *make_re_module(void) {
    XSMap *m=map_new();
    map_take(m,"test",        xs_native(native_re_test));
    map_take(m,"is_match",    xs_native(native_re_test));
    map_take(m,"match",       xs_native(native_re_match));
    map_take(m,"find_all",    xs_native(native_re_find_all));
    map_take(m,"replace",     xs_native(native_re_replace));
    map_take(m,"replace_all", xs_native(native_re_replace_all));
    map_take(m,"split",       xs_native(native_re_split));
    map_take(m,"groups",      xs_native(native_re_groups));
    return xs_module(m);
}
