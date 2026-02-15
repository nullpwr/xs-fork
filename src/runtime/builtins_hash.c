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

/* implemented in the crypto module section of builtins.c */
void xs_md5_hash(const uint8_t *data, size_t len, uint8_t out[16]);
void xs_sha256_hash(const uint8_t *data, size_t len, uint8_t out[32]);

static Value *native_hash_md5(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    uint8_t hash[16];
    xs_md5_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char buf[33];
    for(int j=0;j<16;j++) sprintf(buf+j*2,"%02x",hash[j]);
    buf[32]='\0';
    return xs_str(buf);
}
static Value *native_hash_sha256(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    uint8_t hash[32];
    xs_sha256_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char buf[65];
    for(int j=0;j<32;j++) sprintf(buf+j*2,"%02x",hash[j]);
    buf[64]='\0';
    return xs_str(buf);
}
Value *make_hash_module(void) {
    XSMap *m=map_new();
    map_take(m,"md5",   xs_native(native_hash_md5));
    map_take(m,"sha256",xs_native(native_hash_sha256));
    return xs_module(m);
}
