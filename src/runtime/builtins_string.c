#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* string module */
static Value *native_str_pad_left(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR) return n>0?value_incref(a[0]):value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int width=(int)VAL_INT(a[1]); char fill=' ';
    if (n>=3&&VAL_TAG(a[2])==XS_STR&&a[2]->s[0]) fill=a[2]->s[0];
    int slen=(int)strlen(s);
    if (slen>=width) return value_incref(a[0]);
    char *r=xs_malloc(width+1);
    int pad=width-slen;
    for(int j=0;j<pad;j++) r[j]=fill;
    memcpy(r+pad,s,slen); r[width]='\0';
    Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_pad_right(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR) return n>0?value_incref(a[0]):value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int width=(int)VAL_INT(a[1]); char fill=' ';
    if (n>=3&&VAL_TAG(a[2])==XS_STR&&a[2]->s[0]) fill=a[2]->s[0];
    int slen=(int)strlen(s);
    if (slen>=width) return value_incref(a[0]);
    char *r=xs_malloc(width+1);
    memcpy(r,s,slen);
    for(int j=slen;j<width;j++) r[j]=fill;
    r[width]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_center(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR) return n>0?value_incref(a[0]):value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int width=(int)VAL_INT(a[1]); char fill=' ';
    if (n>=3&&VAL_TAG(a[2])==XS_STR&&a[2]->s[0]) fill=a[2]->s[0];
    int slen=(int)strlen(s);
    if (slen>=width) return value_incref(a[0]);
    int pad=width-slen; int lpad=pad/2; int rpad=pad-lpad;
    char *r=xs_malloc(width+1);
    for(int j=0;j<lpad;j++) r[j]=fill;
    memcpy(r+lpad,s,slen);
    for(int j=0;j<rpad;j++) r[lpad+slen+j]=fill;
    r[width]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_truncate(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR) return n>0?value_incref(a[0]):value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int width=(int)VAL_INT(a[1]);
    const char *suf=(n>=3&&VAL_TAG(a[2])==XS_STR)?a[2]->s:"...";
    int slen=(int)strlen(s); int suflen=(int)strlen(suf);
    if (slen<=width) return value_incref(a[0]);
    int keep=width-suflen; if(keep<0)keep=0;
    char *r=xs_malloc(keep+suflen+1);
    memcpy(r,s,keep); memcpy(r+keep,suf,suflen); r[keep+suflen]='\0';
    Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_camel_to_snake(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int slen=(int)strlen(s);
    char *r=xs_malloc(slen*2+1); int ri=0;
    for (int j=0;j<slen;j++) {
        if (isupper((unsigned char)s[j])&&j>0) r[ri++]='_';
        r[ri++]=tolower((unsigned char)s[j]);
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_snake_to_camel(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int slen=(int)strlen(s);
    char *r=xs_malloc(slen+1); int ri=0; int cap=0;
    for (int j=0;j<slen;j++) {
        if (s[j]=='_') { cap=1; continue; }
        r[ri++]=cap?toupper((unsigned char)s[j]):s[j]; cap=0;
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_escape_html(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s;
    int cap=256; char *r=xs_malloc(cap); int ri=0;
    for (const char *p=s;*p;p++) {
        const char *ent=NULL; int elen=0;
        if (*p=='&'){ent="&amp;";elen=5;}
        else if(*p=='<'){ent="&lt;";elen=4;}
        else if(*p=='>'){ent="&gt;";elen=4;}
        else if(*p=='"'){ent="&quot;";elen=6;}
        else if(*p=='\''){ent="&#39;";elen=5;}
        if (ri+elen+2>cap){cap=cap*2+elen+2;r=realloc(r,cap);}
        if (ent){memcpy(r+ri,ent,elen);ri+=elen;}
        else r[ri++]=(char)*p;
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_is_numeric(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    const char *s=a[0]->s; if (!*s) return value_incref(XS_FALSE_VAL);
    int j=0; if(s[j]=='+'||s[j]=='-') j++;
    int digits=0, dots=0;
    for(;s[j];j++){
        if (isdigit((unsigned char)s[j])) digits++;
        else if (s[j]=='.'&&!dots) dots++;
        else return value_incref(XS_FALSE_VAL);
    }
    return digits>0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_str_words(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    const char *s=a[0]->s; Value *arr=xs_array_new();
    const char *p=s;
    while (*p) {
        while (*p&&isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start=p;
        while (*p&&!isspace((unsigned char)*p)) p++;
        array_push(arr->arr,xs_str_n(start,(int)(p-start)));
    }
    return arr;
}
static int levenshtein_dist(const char *s1, const char *s2) {
    int l1=(int)strlen(s1), l2=(int)strlen(s2);
    int *dp=xs_malloc((l2+1)*sizeof(int));
    for(int j=0;j<=l2;j++) dp[j]=j;
    for(int i=1;i<=l1;i++){
        int prev=dp[0]; dp[0]=i;
        for(int j=1;j<=l2;j++){
            int old=dp[j];
            dp[j]=s1[i-1]==s2[j-1]?prev:1+( prev<dp[j-1]?(prev<dp[j]?prev:dp[j]):(dp[j-1]<dp[j]?dp[j-1]:dp[j]) );
            prev=old;
        }
    }
    int r=dp[l2]; free(dp); return r;
}
static Value *native_str_levenshtein(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return xs_int(0);
    return xs_int(levenshtein_dist(a[0]->s,a[1]->s));
}
static Value *native_str_similarity(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return xs_float(0.0);
    int l1=(int)strlen(a[0]->s), l2=(int)strlen(a[1]->s);
    int maxlen=l1>l2?l1:l2;
    if (maxlen==0) return xs_float(1.0);
    int d=levenshtein_dist(a[0]->s,a[1]->s);
    return xs_float(1.0-(double)d/maxlen);
}
static Value *native_str_repeat(Interp *i, Value **a, int n) {
    (void)i;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_INT) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int count=(int)VAL_INT(a[1]);
    if (count<=0) return xs_str("");
    int slen=(int)strlen(s);
    int rlen=slen*count;
    char *r=xs_malloc(rlen+1);
    for(int j=0;j<count;j++) memcpy(r+j*slen,s,slen);
    r[rlen]='\0';
    Value *v=xs_str(r); free(r); return v;
}
static Value *native_str_chars(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *s=a[0]->s; int slen=(int)strlen(s);
    Value *arr=xs_array_new();
    char buf[2]; buf[1]='\0';
    for(int j=0;j<slen;j++){
        buf[0]=s[j];
        Value *ch=xs_str(buf);
        array_push(arr->arr,ch);
    }
    return arr;
}
static Value *native_str_bytes(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const unsigned char *s=(const unsigned char*)a[0]->s; int slen=(int)strlen(a[0]->s);
    Value *arr=xs_array_new();
    for(int j=0;j<slen;j++){
        Value *b=xs_int((int64_t)s[j]);
        array_push(arr->arr,b);
    }
    return arr;
}
Value *make_string_module(void) {
    XSMap *m=map_new();
    map_take(m,"pad_left",       xs_native(native_str_pad_left));
    map_take(m,"pad_right",      xs_native(native_str_pad_right));
    map_take(m,"center",         xs_native(native_str_center));
    map_take(m,"truncate",       xs_native(native_str_truncate));
    map_take(m,"camel_to_snake", xs_native(native_str_camel_to_snake));
    map_take(m,"snake_to_camel", xs_native(native_str_snake_to_camel));
    map_take(m,"escape_html",    xs_native(native_str_escape_html));
    map_take(m,"is_numeric",     xs_native(native_str_is_numeric));
    map_take(m,"words",          xs_native(native_str_words));
    map_take(m,"levenshtein",    xs_native(native_str_levenshtein));
    map_take(m,"similarity",     xs_native(native_str_similarity));
    map_take(m,"repeat",         xs_native(native_str_repeat));
    map_take(m,"chars",          xs_native(native_str_chars));
    map_take(m,"bytes",          xs_native(native_str_bytes));
    return xs_module(m);
}
