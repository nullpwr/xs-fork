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
#include <stdint.h>

/* fmt module */
static Value *native_fmt_number(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0");
    double v=(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]);
    int dec=(n>=2&&VAL_TAG(a[1])==XS_INT)?(int)VAL_INT(a[1]):2;
    char fmt2[32]; snprintf(fmt2,sizeof(fmt2),"%%.%df",dec);
    char buf[128]; snprintf(buf,sizeof(buf),fmt2,v);
    return xs_str(buf);
}
static Value *native_fmt_hex(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0x0");
    int64_t v=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    char buf[64]; snprintf(buf,sizeof(buf),"0x%llx",(long long)v);
    return xs_str(buf);
}
static Value *native_fmt_bin(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0b0");
    int64_t v=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    if (v==0) return xs_str("0b0");
    /* buf: sign(1) + "0b"(2) + 64 bits + null = 68 */
    char buf[68]; int pos=66; buf[66]='\0';
    uint64_t uv=(uint64_t)(v<0?-v:v);
    while (uv){buf[--pos]=(uv&1)?'1':'0';uv>>=1;}
    buf[--pos]='b'; buf[--pos]='0';
    if (v<0) buf[--pos]='-';
    return xs_str(buf+pos);
}
static Value *native_fmt_pad(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return n>0?(value_incref(a[0])):xs_str("");
    const char *s=(VAL_TAG(a[0])==XS_STR)?a[0]->s:"";
    int width=(VAL_TAG(a[1])==XS_INT)?(int)VAL_INT(a[1]):0;
    char fill=(n>=3&&VAL_TAG(a[2])==XS_STR&&a[2]->s[0])?a[2]->s[0]:' ';
    int slen=(int)strlen(s);
    if (slen>=width) return xs_str(s);
    char *r=xs_malloc(width+1);
    int pad=width-slen;
    for(int j=0;j<pad;j++) r[j]=fill;
    memcpy(r+pad,s,slen); r[width]='\0';
    Value *v=xs_str(r); free(r); return v;
}
static Value *native_fmt_comma(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0");
    char buf[64];
    if (VAL_TAG(a[0])==XS_INT) snprintf(buf,sizeof(buf),"%lld",(long long)VAL_INT(a[0]));
    else snprintf(buf,sizeof(buf),"%.0f",(VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:0.0);
    /* Insert commas */
    int len2=(int)strlen(buf);
    int neg=(buf[0]=='-'); int start=neg?1:0;
    int digits=len2-start;
    int commas=(digits-1)/3;
    char *r=xs_malloc(len2+commas+1); int ri=0;
    if (neg) r[ri++]='-';
    int first=digits%3; if(first==0)first=3;
    for(int j=start;j<len2;j++){
        if(j>start&&(j-start)%3==first%3&&commas>0){r[ri++]=',';commas--;}
        r[ri++]=buf[j];
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_fmt_filesize(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0 B");
    double v=(VAL_TAG(a[0])==XS_INT)?(double)VAL_INT(a[0]):a[0]->f;
    const char *units[]={"B","KB","MB","GB","TB","PB"};
    int ui=0;
    while (v>=1024.0&&ui<5){v/=1024.0;ui++;}
    char buf[64];
    if (ui==0) snprintf(buf,sizeof(buf),"%.0f %s",v,units[ui]);
    else snprintf(buf,sizeof(buf),"%.2f %s",v,units[ui]);
    return xs_str(buf);
}
static Value *native_fmt_ordinal(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("0th");
    int64_t v=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    const char *suf;
    int64_t abs_v=v<0?-v:v;
    if (abs_v%100>=11&&abs_v%100<=13) suf="th";
    else switch(abs_v%10){
        case 1: suf="st"; break;
        case 2: suf="nd"; break;
        case 3: suf="rd"; break;
        default: suf="th"; break;
    }
    char buf[32]; snprintf(buf,sizeof(buf),"%lld%s",(long long)v,suf);
    return xs_str(buf);
}
static Value *native_fmt_pluralize(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2) return xs_str("");
    int64_t cnt=(VAL_TAG(a[0])==XS_INT)?VAL_INT(a[0]):(int64_t)a[0]->f;
    const char *word=(VAL_TAG(a[1])==XS_STR)?a[1]->s:"";
    const char *plural=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:NULL;
    char buf[512];
    if (cnt==1) snprintf(buf,sizeof(buf),"%lld %s",(long long)cnt,word);
    else if (plural) snprintf(buf,sizeof(buf),"%lld %s",(long long)cnt,plural);
    else snprintf(buf,sizeof(buf),"%lld %ss",(long long)cnt,word);
    return xs_str(buf);
}
static Value *native_fmt_sprintf(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *fmt = a[0]->s;
    int flen = (int)strlen(fmt);
    /* estimate output size */
    int cap = flen + 256;
    char *buf = xs_malloc(cap);
    int bi = 0, ai = 1;
    for (int i = 0; i < flen; i++) {
        if (fmt[i] == '{' && i+1 < flen && fmt[i+1] == '}') {
            if (ai < n) {
                char *s = value_repr(a[ai++]);
                int slen = (int)strlen(s);
                /* strip quotes from string repr */
                if (slen >= 2 && s[0] == '"' && s[slen-1] == '"') {
                    s[slen-1] = '\0';
                    char *inner = s + 1;
                    slen -= 2;
                    while (bi + slen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + bi, inner, slen); bi += slen;
                } else {
                    while (bi + slen + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                    memcpy(buf + bi, s, slen); bi += slen;
                }
                free(s);
            }
            i++; /* skip the '}' */
        } else {
            if (bi + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[bi++] = fmt[i];
        }
    }
    buf[bi] = '\0';
    Value *r = xs_str(buf); free(buf); return r;
}

static Value *native_fmt_pad_left(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return n > 0 ? value_incref(a[0]) : xs_str("");
    const char *s = (VAL_TAG(a[0]) == XS_STR) ? a[0]->s : "";
    int width = (VAL_TAG(a[1]) == XS_INT) ? (int)VAL_INT(a[1]) : 0;
    char fill = (n >= 3 && VAL_TAG(a[2]) == XS_STR && a[2]->s[0]) ? a[2]->s[0] : ' ';
    int slen = (int)strlen(s);
    if (slen >= width) return xs_str(s);
    char *r = xs_malloc(width + 1);
    int pad = width - slen;
    for (int j = 0; j < pad; j++) r[j] = fill;
    memcpy(r + pad, s, slen); r[width] = '\0';
    Value *v = xs_str(r); free(r); return v;
}

static Value *native_fmt_pad_right(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return n > 0 ? value_incref(a[0]) : xs_str("");
    const char *s = (VAL_TAG(a[0]) == XS_STR) ? a[0]->s : "";
    int width = (VAL_TAG(a[1]) == XS_INT) ? (int)VAL_INT(a[1]) : 0;
    char fill = (n >= 3 && VAL_TAG(a[2]) == XS_STR && a[2]->s[0]) ? a[2]->s[0] : ' ';
    int slen = (int)strlen(s);
    if (slen >= width) return xs_str(s);
    char *r = xs_malloc(width + 1);
    memcpy(r, s, slen);
    for (int j = slen; j < width; j++) r[j] = fill;
    r[width] = '\0';
    Value *v = xs_str(r); free(r); return v;
}

static Value *native_fmt_center(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return n > 0 ? value_incref(a[0]) : xs_str("");
    const char *s = (VAL_TAG(a[0]) == XS_STR) ? a[0]->s : "";
    int width = (VAL_TAG(a[1]) == XS_INT) ? (int)VAL_INT(a[1]) : 0;
    char fill = (n >= 3 && VAL_TAG(a[2]) == XS_STR && a[2]->s[0]) ? a[2]->s[0] : ' ';
    int slen = (int)strlen(s);
    if (slen >= width) return xs_str(s);
    char *r = xs_malloc(width + 1);
    int total_pad = width - slen;
    int left_pad = total_pad / 2;
    int right_pad = total_pad - left_pad;
    for (int j = 0; j < left_pad; j++) r[j] = fill;
    memcpy(r + left_pad, s, slen);
    for (int j = 0; j < right_pad; j++) r[left_pad + slen + j] = fill;
    r[width] = '\0';
    Value *v = xs_str(r); free(r); return v;
}

static Value *native_fmt_oct(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_str("0o0");
    int64_t v = (VAL_TAG(a[0]) == XS_INT) ? VAL_INT(a[0]) : (int64_t)a[0]->f;
    char buf[64]; snprintf(buf, sizeof(buf), "0o%llo", (long long)v);
    return xs_str(buf);
}

Value *make_fmt_module(void) {
    XSMap *m=map_new();
    map_take(m,"sprintf",   xs_native(native_fmt_sprintf));
    map_take(m,"pad_left",  xs_native(native_fmt_pad_left));
    map_take(m,"pad_right", xs_native(native_fmt_pad_right));
    map_take(m,"center",    xs_native(native_fmt_center));
    map_take(m,"number",    xs_native(native_fmt_number));
    map_take(m,"hex",       xs_native(native_fmt_hex));
    map_take(m,"bin",       xs_native(native_fmt_bin));
    map_take(m,"oct",       xs_native(native_fmt_oct));
    map_take(m,"pad",       xs_native(native_fmt_pad));
    map_take(m,"comma",     xs_native(native_fmt_comma));
    map_take(m,"filesize",  xs_native(native_fmt_filesize));
    map_take(m,"ordinal",   xs_native(native_fmt_ordinal));
    map_take(m,"pluralize", xs_native(native_fmt_pluralize));
    return xs_module(m);
}
