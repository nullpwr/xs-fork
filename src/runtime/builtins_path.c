#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>

/* path module */
static Value *native_path_basename(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s;
    const char *sl=strrchr(s,'/');
    const char *bs=strrchr(s,'\\');
    const char *last=sl>bs?sl:bs;
    return xs_str(last?last+1:s);
}
static Value *native_path_dirname(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str(".");
    const char *s=a[0]->s;
    const char *sl=strrchr(s,'/');
    const char *bs=strrchr(s,'\\');
    const char *last=sl>bs?sl:bs;
    if (!last) return xs_str(".");
    int dlen=(int)(last-s);
    if (dlen==0) return xs_str("/");
    char *r=xs_strndup(s,dlen); Value *v=xs_str(r); free(r); return v;
}
static Value *native_path_ext(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    const char *s=a[0]->s;
    const char *sl=strrchr(s,'/');
    const char *bs=strrchr(s,'\\');
    const char *last=sl>bs?sl:bs;
    const char *base=last?last+1:s;
    const char *dot=strrchr(base,'.');
    return xs_str(dot?dot:"");
}
static Value *native_path_stem(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    const char *s=a[0]->s;
    const char *sl=strrchr(s,'/'); const char *bs=strrchr(s,'\\');
    const char *last=sl>bs?sl:bs; const char *base=last?last+1:s;
    const char *dot=strrchr(base,'.');
    if (!dot) return xs_str(base);
    char *r=xs_strndup(base,(int)(dot-base)); Value *v=xs_str(r); free(r); return v;
}
static Value *native_path_join(Interp *i, Value **a, int argc) {
    (void)i;
    if (argc==0) return xs_str("");
    int total=0;
    for(int j=0;j<argc;j++) if(VAL_TAG(a[j])==XS_STR) total+=(int)strlen(a[j]->s)+1;
    char *r=xs_malloc(total+2); int ri=0;
    for(int j=0;j<argc;j++){
        if(VAL_TAG(a[j])!=XS_STR) continue;
        const char *s=a[j]->s;
        if(ri>0&&s[0]!='/'&&s[0]!='\\') r[ri++]='/';
        int slen=(int)strlen(s);
        memcpy(r+ri,s,slen); ri+=slen;
        /* remove trailing slash */
        while(ri>1&&(r[ri-1]=='/'||r[ri-1]=='\\')) ri--;
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
Value *make_path_module(void) {
    XSMap *m=map_new();
    map_take(m,"basename", xs_native(native_path_basename));
    map_take(m,"dirname",  xs_native(native_path_dirname));
    map_take(m,"ext",      xs_native(native_path_ext));
    map_take(m,"stem",     xs_native(native_path_stem));
    map_take(m,"join",     xs_native(native_path_join));
    { Value *v=xs_str("/"); map_set(m,"sep",v); value_decref(v); }
    return xs_module(m);
}
