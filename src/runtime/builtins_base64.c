#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* base64 module */
static const char b64_table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static Value *native_b64_encode(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const unsigned char *s=(const unsigned char*)a[0]->s;
    int slen=(int)strlen(a[0]->s);
    int rlen=((slen+2)/3)*4; char *r=xs_malloc(rlen+1); int ri=0;
    for(int j=0;j<slen;j+=3){
        unsigned int v=(unsigned)s[j]<<16|(j+1<slen?(unsigned)s[j+1]:0)<<8|(j+2<slen?(unsigned)s[j+2]:0);
        r[ri++]=b64_table[(v>>18)&63]; r[ri++]=b64_table[(v>>12)&63];
        r[ri++]=(j+1<slen)?b64_table[(v>>6)&63]:'=';
        r[ri++]=(j+2<slen)?b64_table[v&63]:'=';
    }
    r[ri]='\0'; Value *v2=xs_str(r); free(r); return v2;
}
static Value *native_b64_decode(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int slen=(int)strlen(s);
    char *r=xs_malloc(slen); int ri=0;
    static const signed char b64_inv[256]={
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    for(int j=0;j+3<slen;j+=4){
        int a0=b64_inv[(unsigned char)s[j]],a1=b64_inv[(unsigned char)s[j+1]];
        int a2=b64_inv[(unsigned char)s[j+2]],a3=b64_inv[(unsigned char)s[j+3]];
        if(a0<0||a1<0) break;
        r[ri++]=(char)((a0<<2)|(a1>>4));
        if(a2>=0) r[ri++]=(char)((a1<<4)|(a2>>2));
        if(a3>=0) r[ri++]=(char)((a2<<6)|a3);
    }
    r[ri]='\0'; Value *v2=xs_str(r); free(r); return v2;
}
Value *make_base64_module(void) {
    XSMap *m=map_new();
    map_take(m,"encode",xs_native(native_b64_encode));
    map_take(m,"decode",xs_native(native_b64_decode));
    return xs_module(m);
}
