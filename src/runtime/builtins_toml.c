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

/* toml module */
static void toml_skip_ws_inline(const char *s, int *p) {
    while (s[*p]==' '||s[*p]=='\t') (*p)++;
}
static void toml_skip_comment(const char *s, int *p) {
    if (s[*p]=='#') { while (s[*p]&&s[*p]!='\n') (*p)++; }
}
static char *toml_parse_key(const char *s, int *p) {
    if (s[*p]=='"'||s[*p]=='\'') {
        char q=s[*p]; (*p)++;
        int start=*p;
        while (s[*p]&&s[*p]!=q) (*p)++;
        int len=*p-start;
        char *k=xs_malloc(len+1);
        memcpy(k,s+start,len); k[len]='\0';
        if (s[*p]==q) (*p)++;
        return k;
    }
    int start=*p;
    while (s[*p]&&(isalnum((unsigned char)s[*p])||s[*p]=='_'||s[*p]=='-')) (*p)++;
    if (*p==start) return NULL;
    int len=*p-start;
    char *k=xs_malloc(len+1);
    memcpy(k,s+start,len); k[len]='\0';
    return k;
}
static Value *toml_parse_val(const char *s, int *p);
static Value *toml_parse_string(const char *s, int *p) {
    char q=s[*p]; (*p)++;
    int cap=64; char *buf=xs_malloc(cap); int ri=0;
    while (s[*p]&&s[*p]!=q) {
        if (ri+4>=cap){cap*=2;buf=xs_realloc(buf,cap);}
        if (s[*p]=='\\') {
            (*p)++;
            switch(s[*p]){
            case 'n': buf[ri++]='\n'; break;
            case 't': buf[ri++]='\t'; break;
            case 'r': buf[ri++]='\r'; break;
            case '\\': buf[ri++]='\\'; break;
            case '"': buf[ri++]='"'; break;
            default: buf[ri++]=s[*p]; break;
            }
        } else buf[ri++]=s[*p];
        (*p)++;
    }
    if (s[*p]==q) (*p)++;
    buf[ri]='\0'; Value *v=xs_str(buf); free(buf); return v;
}
static Value *toml_parse_array(const char *s, int *p) {
    (*p)++;
    Value *arr=xs_array_new();
    while (s[*p]) {
        toml_skip_ws_inline(s,p);
        while (s[*p]=='\n'||s[*p]=='\r') (*p)++;
        toml_skip_ws_inline(s,p);
        toml_skip_comment(s,p);
        if (s[*p]==']') { (*p)++; break; }
        Value *v=toml_parse_val(s,p);
        if (v) array_push(arr->arr,v);
        toml_skip_ws_inline(s,p);
        if (s[*p]==',') (*p)++;
    }
    return arr;
}
static Value *toml_parse_val(const char *s, int *p) {
    toml_skip_ws_inline(s,p);
    if (s[*p]=='"'||s[*p]=='\'') return toml_parse_string(s,p);
    if (s[*p]=='[') return toml_parse_array(s,p);
    if (strncmp(s+*p,"true",4)==0&&!isalnum((unsigned char)s[*p+4])){*p+=4;return value_incref(XS_TRUE_VAL);}
    if (strncmp(s+*p,"false",5)==0&&!isalnum((unsigned char)s[*p+5])){*p+=5;return value_incref(XS_FALSE_VAL);}
    int start=*p; int is_float=0;
    if (s[*p]=='-'||s[*p]=='+') (*p)++;
    while (isdigit((unsigned char)s[*p])||s[*p]=='_') (*p)++;
    if (s[*p]=='.'){is_float=1;(*p)++;while(isdigit((unsigned char)s[*p])||s[*p]=='_')(*p)++;}
    if (s[*p]=='e'||s[*p]=='E'){is_float=1;(*p)++;if(s[*p]=='+'||s[*p]=='-')(*p)++;while(isdigit((unsigned char)s[*p]))(*p)++;}
    if (*p>start) {
        int slen2=*p-start;
        char *tmp=xs_malloc(slen2+1); int ti=0;
        for(int j=start;j<*p;j++) if(s[j]!='_') tmp[ti++]=s[j];
        tmp[ti]='\0';
        Value *v;
        if (is_float) v=xs_float(strtod(tmp,NULL));
        else v=xs_int((int64_t)strtoll(tmp,NULL,10));
        free(tmp); return v;
    }
    return value_incref(XS_NULL_VAL);
}
static XSMap *toml_ensure_section(XSMap *root, const char *section) {
    char buf[512];
    int len=(int)strlen(section);
    if (len>=(int)sizeof(buf)-1) len=(int)sizeof(buf)-2;
    memcpy(buf,section,len); buf[len]='\0';
    XSMap *cur=root; char *pp=buf;
    while (*pp) {
        char *dot=strchr(pp,'.');
        if (dot) *dot='\0';
        Value *existing=map_get(cur,pp);
        if (existing&&(VAL_TAG(existing)==XS_MAP||VAL_TAG(existing)==XS_MODULE)) cur=existing->map;
        else { Value *nm=xs_map_new(); map_set(cur,pp,nm); cur=nm->map; value_decref(nm); }
        if (dot) pp=dot+1; else break;
    }
    return cur;
}
static Value *native_toml_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_map_new();
    const char *s=a[0]->s; int pos=0; int slen=(int)strlen(s);
    Value *root=xs_map_new(); XSMap *cur=root->map;
    while (pos<slen) {
        toml_skip_ws_inline(s,&pos);
        if (s[pos]=='\n'||s[pos]=='\r') { pos++; continue; }
        if (s[pos]=='#') { while(s[pos]&&s[pos]!='\n') pos++; continue; }
        if (!s[pos]) break;
        if (s[pos]=='[') {
            pos++; int dbl=0;
            if (s[pos]=='[') { dbl=1; pos++; }
            toml_skip_ws_inline(s,&pos);
            int kstart=pos;
            while (s[pos]&&s[pos]!=']') pos++;
            int klen=pos-kstart;
            while (klen>0&&(s[kstart+klen-1]==' '||s[kstart+klen-1]=='\t')) klen--;
            char *section=xs_malloc(klen+1);
            memcpy(section,s+kstart,klen); section[klen]='\0';
            cur=toml_ensure_section(root->map,section);
            free(section);
            if (s[pos]==']') pos++;
            if (dbl&&s[pos]==']') pos++;
            while (s[pos]&&s[pos]!='\n') pos++;
            continue;
        }
        char *key=toml_parse_key(s,&pos);
        if (!key) { while(s[pos]&&s[pos]!='\n') pos++; continue; }
        toml_skip_ws_inline(s,&pos);
        if (s[pos]!='=') { free(key); while(s[pos]&&s[pos]!='\n') pos++; continue; }
        pos++;
        toml_skip_ws_inline(s,&pos);
        Value *val=toml_parse_val(s,&pos);
        map_set(cur,key,val); value_decref(val); free(key);
        toml_skip_ws_inline(s,&pos);
        toml_skip_comment(s,&pos);
        while (s[pos]=='\n'||s[pos]=='\r') pos++;
    }
    return root;
}
Value *make_toml_module(void) {
    XSMap *m=map_new();
    map_take(m,"parse",xs_native(native_toml_parse));
    return xs_module(m);
}
