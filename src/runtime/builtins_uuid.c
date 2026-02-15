#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* uuid module */
static Value *native_uuid_v4(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    unsigned char b[16];
    for(int j=0;j<16;j++) b[j]=(unsigned char)(rand()&0xff);
    b[6]=(b[6]&0x0f)|0x40; b[8]=(b[8]&0x3f)|0x80;
    char buf[37];
    snprintf(buf,sizeof(buf),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return xs_str(buf);
}
static Value *native_uuid_is_valid(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    const char *s=a[0]->s; int l=(int)strlen(s);
    if (l!=36) return value_incref(XS_FALSE_VAL);
    for(int j=0;j<36;j++){
        if(j==8||j==13||j==18||j==23){if(s[j]!='-')return value_incref(XS_FALSE_VAL);}
        else if(!isxdigit((unsigned char)s[j])) return value_incref(XS_FALSE_VAL);
    }
    return value_incref(XS_TRUE_VAL);
}
Value *make_uuid_module(void) {
    XSMap *m=map_new();
    map_take(m,"v4",      xs_native(native_uuid_v4));
    map_take(m,"is_valid",xs_native(native_uuid_is_valid));
    return xs_module(m);
}
