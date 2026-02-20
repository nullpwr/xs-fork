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
#include <time.h>
#ifndef __wasi__
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_kdf.h"
#include "bearssl_block.h"
#include "bearssl_aead.h"
#endif

/* crypto */

/* SHA-256 implementation */
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t sha256_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]<<24) | ((uint32_t)block[i*4+1]<<16) |
               ((uint32_t)block[i*4+2]<<8) | (uint32_t)block[i*4+3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i-15],7) ^ sha256_rotr(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = sha256_rotr(w[i-2],17) ^ sha256_rotr(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

void xs_sha256_hash(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t i;
    /* process full blocks */
    for (i = 0; i + 64 <= len; i += 64)
        sha256_transform(state, data + i);
    /* final block with padding */
    size_t rem = len - i;
    memset(block, 0, 64);
    if (rem > 0) memcpy(block, data + i, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        sha256_transform(state, block);
        memset(block, 0, 64);
    }
    /* length in bits (big-endian) */
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 7; j >= 0; j--) {
        block[56 + (7 - j)] = (uint8_t)(bits >> (j * 8));
    }
    sha256_transform(state, block);
    /* output */
    for (i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(state[i]>>24);
        out[i*4+1] = (uint8_t)(state[i]>>16);
        out[i*4+2] = (uint8_t)(state[i]>>8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}

/* MD5 implementation */
static const uint32_t md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
static const uint32_t md5_k_tab[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static uint32_t md5_leftrotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

void xs_md5_hash(const uint8_t *data, size_t len, uint8_t out[16]) {
    uint32_t a0=0x67452301, b0=0xefcdab89, c0=0x98badcfe, d0=0x10325476;
    /* pad message */
    size_t new_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = xs_calloc(new_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    memcpy(msg + new_len - 8, &bits, 8); /* little-endian */

    for (size_t off = 0; off < new_len; off += 64) {
        uint32_t *M = (uint32_t*)(msg + off);
        uint32_t A=a0, B=b0, C=c0, D=d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F, g;
            if (i < 16)      { F = (B&C)|(~B&D); g = (uint32_t)i; }
            else if (i < 32) { F = (D&B)|(~D&C); g = (5*(uint32_t)i+1)%16; }
            else if (i < 48) { F = B^C^D;        g = (3*(uint32_t)i+5)%16; }
            else              { F = C^(B|~D);     g = (7*(uint32_t)i)%16; }
            F = F + A + md5_k_tab[i] + M[g];
            A = D; D = C; C = B;
            B = B + md5_leftrotate(F, md5_s[i]);
        }
        a0+=A; b0+=B; c0+=C; d0+=D;
    }
    free(msg);
    memcpy(out+0,  &a0, 4);
    memcpy(out+4,  &b0, 4);
    memcpy(out+8,  &c0, 4);
    memcpy(out+12, &d0, 4);
}

static Value *native_crypto_sha256(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    uint8_t hash[32];
    xs_sha256_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[64] = '\0';
    return xs_str(hex);
}

static Value *native_crypto_md5(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    uint8_t hash[16];
    xs_md5_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[32] = '\0';
    return xs_str(hex);
}

static Value *native_crypto_random_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return xs_str("");
    int count = (int)VAL_INT(a[0]);
    if (count <= 0 || count > 65536) return xs_str("");
    uint8_t *buf = xs_malloc((size_t)count);
#if defined(__wasi__)
    for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff);
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(buf, 1, (size_t)count, f) < (size_t)count) { /* partial read ok */ } fclose(f); }
    else { for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff); }
#else
    /* Windows: use RtlGenRandom (SystemFunction036) from advapi32 */
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(buf, (ULONG)count)) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff);
        }
    }
#endif
    /* return as hex string */
    char *hex = xs_malloc((size_t)count * 2 + 1);
    for (int i = 0; i < count; i++) sprintf(hex + i*2, "%02x", buf[i]);
    hex[count*2] = '\0';
    free(buf);
    Value *r = xs_str(hex);
    free(hex);
    return r;
}

static Value *native_crypto_random_int(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return xs_int(0);
    int64_t lo = (VAL_TAG(a[0]) == XS_INT) ? VAL_INT(a[0]) : 0;
    int64_t hi = (VAL_TAG(a[1]) == XS_INT) ? VAL_INT(a[1]) : 0;
    if (hi <= lo) return xs_int(lo);
    uint64_t r;
#if defined(__wasi__)
    r = (uint64_t)rand();
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&r, sizeof r, 1, f) < 1) { r = (uint64_t)rand(); } fclose(f); }
    else { r = (uint64_t)rand(); }
#else
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(&r, (ULONG)sizeof(r))) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            r = (uint64_t)rand();
        }
    }
#endif
    return xs_int(lo + (int64_t)(r % (uint64_t)(hi - lo)));
}

static Value *native_crypto_uuid4(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    uint8_t bytes[16];
#if defined(__wasi__)
    for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff);
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(bytes, 1, 16, f) < 16) { /* partial read ok */ } fclose(f); }
    else { for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff); }
#else
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(bytes, 16)) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff);
        }
    }
#endif
    bytes[6] = (bytes[6] & 0x0f) | 0x40; /* version 4 */
    bytes[8] = (bytes[8] & 0x3f) | 0x80; /* variant 1 */
    char uuid[37];
    snprintf(uuid, sizeof uuid,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],
        bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],
        bytes[12],bytes[13],bytes[14],bytes[15]);
    return xs_str(uuid);
}

static Value *native_crypto_hash(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_int(0);
    /* djb2 hash */
    const char *s = a[0]->s;
    uint64_t h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return xs_int((int64_t)h);
}

static Value *native_crypto_hex_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    int slen = (int)strlen(s);
    char *hex = xs_malloc(slen * 2 + 1);
    for (int i = 0; i < slen; i++) sprintf(hex + i*2, "%02x", (unsigned char)s[i]);
    hex[slen * 2] = '\0';
    Value *r = xs_str(hex); free(hex); return r;
}

static Value *native_crypto_hex_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    int slen = (int)strlen(s);
    if (slen % 2 != 0) return xs_str("");
    int olen = slen / 2;
    char *out = xs_malloc(olen + 1);
    for (int i = 0; i < olen; i++) {
        unsigned int byte;
        if (sscanf(s + i*2, "%2x", &byte) != 1) { free(out); return xs_str(""); }
        out[i] = (char)byte;
    }
    out[olen] = '\0';
    Value *r = xs_str(out); free(out); return r;
}

static const char xs_b64_tbl[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static Value *native_crypto_base64_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const unsigned char *s = (const unsigned char*)a[0]->s;
    int slen = (int)strlen(a[0]->s);
    int rlen = ((slen + 2) / 3) * 4;
    char *r = xs_malloc(rlen + 1); int ri = 0;
    for (int j = 0; j < slen; j += 3) {
        unsigned int v = (unsigned)s[j]<<16 | (j+1<slen?(unsigned)s[j+1]:0)<<8 | (j+2<slen?(unsigned)s[j+2]:0);
        r[ri++] = xs_b64_tbl[(v>>18)&63]; r[ri++] = xs_b64_tbl[(v>>12)&63];
        r[ri++] = (j+1<slen) ? xs_b64_tbl[(v>>6)&63] : '=';
        r[ri++] = (j+2<slen) ? xs_b64_tbl[v&63] : '=';
    }
    r[ri] = '\0'; Value *v2 = xs_str(r); free(r); return v2;
}

static Value *native_crypto_base64_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s; int slen = (int)strlen(s);
    static const signed char inv[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    char *r = xs_malloc(slen); int ri = 0;
    for (int j = 0; j+3 < slen; j += 4) {
        int a0=inv[(unsigned char)s[j]], a1=inv[(unsigned char)s[j+1]];
        int a2=inv[(unsigned char)s[j+2]], a3=inv[(unsigned char)s[j+3]];
        if (a0<0||a1<0) break;
        r[ri++] = (char)((a0<<2)|(a1>>4));
        if (a2>=0) r[ri++] = (char)((a1<<4)|(a2>>2));
        if (a3>=0) r[ri++] = (char)((a2<<6)|a3);
    }
    r[ri] = '\0'; Value *v2 = xs_str(r); free(r); return v2;
}

#ifndef __wasi__
/* crypto.sha1(data) -> hex string using BearSSL */
static Value *native_crypto_sha1(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    br_sha1_context ctx;
    br_sha1_init(&ctx);
    br_sha1_update(&ctx, a[0]->s, strlen(a[0]->s));
    uint8_t hash[20];
    br_sha1_out(&ctx, hash);
    char hex[41];
    for (int i = 0; i < 20; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[40] = '\0';
    return xs_str(hex);
}

/* crypto.hmac_sha256(key, data) -> hex string using BearSSL */
static Value *native_crypto_hmac_sha256(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, a[0]->s, strlen(a[0]->s));
    br_hmac_context hctx;
    br_hmac_init(&hctx, &kc, 0);
    br_hmac_update(&hctx, a[1]->s, strlen(a[1]->s));
    uint8_t mac[32];
    br_hmac_out(&hctx, mac);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", mac[i]);
    hex[64] = '\0';
    return xs_str(hex);
}

/* crypto.hkdf(ikm, salt, info, length) -> hex string using BearSSL */
static Value *native_crypto_hkdf(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 4 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[3]) != XS_INT) return xs_str("");
    int out_len = (int)VAL_INT(a[3]);
    if (out_len <= 0 || out_len > 255 * 32) return xs_str("");
    const void *salt = BR_HKDF_NO_SALT;
    size_t salt_len = 0;
    if (VAL_TAG(a[1]) == XS_STR && strlen(a[1]->s) > 0) {
        salt = a[1]->s; salt_len = strlen(a[1]->s);
    }
    br_hkdf_context hc;
    br_hkdf_init(&hc, &br_sha256_vtable, salt, salt_len);
    br_hkdf_inject(&hc, a[0]->s, strlen(a[0]->s));
    br_hkdf_flip(&hc);
    const void *info = "";
    size_t info_len = 0;
    if (n >= 3 && VAL_TAG(a[2]) == XS_STR) { info = a[2]->s; info_len = strlen(a[2]->s); }
    uint8_t *out = xs_malloc(out_len);
    br_hkdf_produce(&hc, info, info_len, out, out_len);
    char *hex = xs_malloc(out_len * 2 + 1);
    for (int i = 0; i < out_len; i++) sprintf(hex + i*2, "%02x", out[i]);
    hex[out_len * 2] = '\0';
    free(out);
    Value *r = xs_str(hex); free(hex); return r;
}

/* crypto.pbkdf2(password, salt, iterations, key_len) -> hex string
   PBKDF2-HMAC-SHA256, hand-rolled using BearSSL HMAC primitives */
static Value *native_crypto_pbkdf2(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 4 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR
        || VAL_TAG(a[2]) != XS_INT || VAL_TAG(a[3]) != XS_INT) return xs_str("");
    const char *pw = a[0]->s;
    size_t pw_len = strlen(pw);
    const char *salt = a[1]->s;
    size_t salt_len = strlen(salt);
    int iters = (int)VAL_INT(a[2]);
    int dklen = (int)VAL_INT(a[3]);
    if (iters <= 0 || dklen <= 0 || dklen > 1024) return xs_str("");

    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, pw_len);

    uint8_t *dk = xs_calloc(dklen, 1);
    int blocks = (dklen + 31) / 32;
    for (int blk = 1; blk <= blocks; blk++) {
        /* U1 = HMAC(pw, salt || INT_BE(blk)) */
        uint8_t salt_blk[4];
        salt_blk[0] = (uint8_t)(blk >> 24);
        salt_blk[1] = (uint8_t)(blk >> 16);
        salt_blk[2] = (uint8_t)(blk >> 8);
        salt_blk[3] = (uint8_t)(blk);

        br_hmac_context hctx;
        br_hmac_init(&hctx, &kc, 0);
        br_hmac_update(&hctx, salt, salt_len);
        br_hmac_update(&hctx, salt_blk, 4);
        uint8_t u[32], t[32];
        br_hmac_out(&hctx, u);
        memcpy(t, u, 32);

        for (int i = 1; i < iters; i++) {
            br_hmac_init(&hctx, &kc, 0);
            br_hmac_update(&hctx, u, 32);
            br_hmac_out(&hctx, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }

        int off = (blk - 1) * 32;
        int cp = dklen - off;
        if (cp > 32) cp = 32;
        memcpy(dk + off, t, cp);
    }

    char *hex = xs_malloc(dklen * 2 + 1);
    for (int i = 0; i < dklen; i++) sprintf(hex + i*2, "%02x", dk[i]);
    hex[dklen * 2] = '\0';
    free(dk);
    Value *r = xs_str(hex); free(hex); return r;
}

/* crypto.aes_encrypt(key_hex, plaintext, mode) -> ciphertext hex
   Supports "cbc" and "gcm". Key must be hex-encoded (32/48/64 chars for 128/192/256). */
static int hex_decode_bytes(const char *hex, uint8_t *out, int max) {
    int len = (int)strlen(hex);
    if (len % 2 != 0) return -1;
    int n2 = len / 2;
    if (n2 > max) n2 = max;
    for (int i = 0; i < n2; i++) {
        unsigned int b;
        if (sscanf(hex + i*2, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return n2;
}

static Value *native_crypto_aes_encrypt(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    const char *mode_str = (n >= 3 && VAL_TAG(a[2]) == XS_STR) ? a[2]->s : "gcm";

    uint8_t key[32];
    int klen = hex_decode_bytes(a[0]->s, key, 32);
    if (klen != 16 && klen != 24 && klen != 32) return xs_str("");

    const uint8_t *pt = (const uint8_t*)a[1]->s;
    size_t pt_len = strlen(a[1]->s);

    if (strcmp(mode_str, "cbc") == 0) {
        /* PKCS7 padding */
        int pad = 16 - (int)(pt_len % 16);
        size_t padded_len = pt_len + (size_t)pad;
        uint8_t *buf = xs_malloc(padded_len);
        memcpy(buf, pt, pt_len);
        for (int i = 0; i < pad; i++) buf[pt_len + i] = (uint8_t)pad;

        /* random IV */
        uint8_t iv[16];
        FILE *rng = fopen("/dev/urandom", "rb");
        if (rng) { if (fread(iv, 1, 16, rng) < 16) {} fclose(rng); }
        else { for (int i=0;i<16;i++) iv[i]=(uint8_t)(rand()&0xff); }

        br_aes_ct_cbcenc_keys ctx;
        br_aes_ct_cbcenc_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcenc_run(&ctx, iv, buf, padded_len);

        /* output: IV || ciphertext, hex encoded */
        size_t out_len = 16 + padded_len;
        char *hex = xs_malloc(out_len * 2 + 1);
        uint8_t iv_save[16];
        /* we need original IV, but it was modified. regenerate */
        /* actually, openssl CBC mode modifies IV in place, so we need to save it.
           let's re-encrypt properly */
        free(buf); free(hex);

        /* redo: save IV first */
        uint8_t iv2[16];
        if (rng) { rng = fopen("/dev/urandom","rb"); if(rng){if(fread(iv2,1,16,rng)<16){}fclose(rng);} }
        else { for (int i=0;i<16;i++) iv2[i]=(uint8_t)(rand()&0xff); }
        memcpy(iv_save, iv2, 16);

        buf = xs_malloc(padded_len);
        memcpy(buf, pt, pt_len);
        for (int i = 0; i < pad; i++) buf[pt_len + i] = (uint8_t)pad;

        br_aes_ct_cbcenc_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcenc_run(&ctx, iv2, buf, padded_len);

        out_len = 16 + padded_len;
        hex = xs_malloc(out_len * 2 + 1);
        for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", iv_save[i]);
        for (size_t i = 0; i < padded_len; i++) sprintf(hex + 32 + i*2, "%02x", buf[i]);
        hex[out_len * 2] = '\0';
        free(buf);
        Value *r = xs_str(hex); free(hex); return r;
    } else {
        /* GCM mode */
        uint8_t iv[12];
        FILE *rng = fopen("/dev/urandom", "rb");
        if (rng) { if (fread(iv, 1, 12, rng) < 12) {} fclose(rng); }
        else { for (int i=0;i<12;i++) iv[i]=(uint8_t)(rand()&0xff); }

        uint8_t *buf = xs_malloc(pt_len > 0 ? pt_len : 1);
        memcpy(buf, pt, pt_len);

        br_aes_ct_ctr_keys ctr_keys;
        br_aes_ct_ctr_init(&ctr_keys, key, (size_t)klen);
        br_gcm_context gc;
        br_gcm_init(&gc, &ctr_keys.vtable, br_ghash_ctmul);
        br_gcm_reset(&gc, iv, 12);
        br_gcm_flip(&gc);
        br_gcm_run(&gc, 1, buf, pt_len);
        uint8_t tag[16];
        br_gcm_get_tag(&gc, tag);

        /* output: IV(12) || ciphertext || tag(16), hex encoded */
        size_t out_len = 12 + pt_len + 16;
        char *hex = xs_malloc(out_len * 2 + 1);
        for (int i = 0; i < 12; i++) sprintf(hex + i*2, "%02x", iv[i]);
        for (size_t i = 0; i < pt_len; i++) sprintf(hex + 24 + i*2, "%02x", buf[i]);
        for (int i = 0; i < 16; i++) sprintf(hex + 24 + pt_len*2 + i*2, "%02x", tag[i]);
        hex[out_len * 2] = '\0';
        free(buf);
        Value *r = xs_str(hex); free(hex); return r;
    }
}

static Value *native_crypto_aes_decrypt(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    const char *mode_str = (n >= 3 && VAL_TAG(a[2]) == XS_STR) ? a[2]->s : "gcm";

    uint8_t key[32];
    int klen = hex_decode_bytes(a[0]->s, key, 32);
    if (klen != 16 && klen != 24 && klen != 32) return xs_str("");

    int ct_hex_len = (int)strlen(a[1]->s);
    if (ct_hex_len % 2 != 0) return xs_str("");
    int ct_bytes = ct_hex_len / 2;

    if (strcmp(mode_str, "cbc") == 0) {
        if (ct_bytes < 32) return xs_str(""); /* need at least IV + 1 block */
        uint8_t *raw = xs_malloc(ct_bytes);
        if (hex_decode_bytes(a[1]->s, raw, ct_bytes) != ct_bytes) { free(raw); return xs_str(""); }

        uint8_t iv[16];
        memcpy(iv, raw, 16);
        uint8_t *ct = raw + 16;
        int ct_len = ct_bytes - 16;

        br_aes_ct_cbcdec_keys ctx;
        br_aes_ct_cbcdec_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcdec_run(&ctx, iv, ct, (size_t)ct_len);

        /* remove PKCS7 padding */
        int pad = ct[ct_len - 1];
        if (pad < 1 || pad > 16) { free(raw); return xs_str(""); }
        ct_len -= pad;
        Value *r = xs_str_n((const char*)ct, ct_len);
        free(raw);
        return r;
    } else {
        /* GCM: IV(12) || ciphertext || tag(16) */
        if (ct_bytes < 28) return xs_str(""); /* 12 + 0 + 16 minimum */
        uint8_t *raw = xs_malloc(ct_bytes);
        if (hex_decode_bytes(a[1]->s, raw, ct_bytes) != ct_bytes) { free(raw); return xs_str(""); }

        uint8_t iv[12];
        memcpy(iv, raw, 12);
        int data_len = ct_bytes - 12 - 16;
        if (data_len < 0) { free(raw); return xs_str(""); }
        uint8_t *ct = raw + 12;
        uint8_t *tag = raw + 12 + data_len;

        br_aes_ct_ctr_keys ctr_keys;
        br_aes_ct_ctr_init(&ctr_keys, key, (size_t)klen);
        br_gcm_context gc;
        br_gcm_init(&gc, &ctr_keys.vtable, br_ghash_ctmul);
        br_gcm_reset(&gc, iv, 12);
        br_gcm_flip(&gc);
        br_gcm_run(&gc, 0, ct, (size_t)data_len);

        if (br_gcm_check_tag(&gc, tag) != 1) {
            free(raw); return xs_str("");
        }
        Value *r = xs_str_n((const char*)ct, data_len);
        free(raw);
        return r;
    }
}

#endif /* __wasi__ */

/* crypto.constant_time_eq(a, b) -> bool */
static Value *native_crypto_constant_time_eq(Interp *ig, Value **a2, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a2[0]) != XS_STR || VAL_TAG(a2[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    size_t la = strlen(a2[0]->s), lb = strlen(a2[1]->s);
    if (la != lb) return value_incref(XS_FALSE_VAL);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (uint8_t)((unsigned char)a2[0]->s[i] ^ (unsigned char)a2[1]->s[i]);
    return (diff == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *make_crypto_module(void) {
    XSMap *m = map_new();
    map_take(m, "sha256",           xs_native(native_crypto_sha256));
#ifndef __wasi__
    map_take(m, "sha1",             xs_native(native_crypto_sha1));
    map_take(m, "hmac_sha256",      xs_native(native_crypto_hmac_sha256));
    map_take(m, "hkdf",             xs_native(native_crypto_hkdf));
    map_take(m, "pbkdf2",           xs_native(native_crypto_pbkdf2));
    map_take(m, "aes_encrypt",      xs_native(native_crypto_aes_encrypt));
    map_take(m, "aes_decrypt",      xs_native(native_crypto_aes_decrypt));
#endif
    map_take(m, "md5",              xs_native(native_crypto_md5));
    map_take(m, "hash",             xs_native(native_crypto_hash));
    map_take(m, "hex_encode",       xs_native(native_crypto_hex_encode));
    map_take(m, "hex_decode",       xs_native(native_crypto_hex_decode));
    map_take(m, "base64_encode",    xs_native(native_crypto_base64_encode));
    map_take(m, "base64_decode",    xs_native(native_crypto_base64_decode));
    map_take(m, "random_bytes",     xs_native(native_crypto_random_bytes));
    map_take(m, "random_int",       xs_native(native_crypto_random_int));
    map_take(m, "uuid4",            xs_native(native_crypto_uuid4));
    map_take(m, "constant_time_eq", xs_native(native_crypto_constant_time_eq));
    return xs_module(m);
}
