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

/* url module */
static int url_hex_val(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
static Value *native_url_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    const char *s=a[0]->s; int cap=256; char *r=xs_malloc(cap); int ri=0;
    for (const unsigned char *p=(const unsigned char*)s;*p;p++){
        if (isalnum(*p)||*p=='-'||*p=='_'||*p=='.'||*p=='~'){
            if (ri+2>cap){cap*=2;r=xs_realloc(r,cap);}
            r[ri++]=(char)*p;
        } else {
            if (ri+4>cap){cap*=2;r=xs_realloc(r,cap);}
            snprintf(r+ri,4,"%%%02X",*p); ri+=3;
        }
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_url_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_str("");
    const char *s=a[0]->s; int slen=(int)strlen(s);
    char *r=xs_malloc(slen+1); int ri=0;
    for (int j=0;j<slen;) {
        if (s[j]=='%'&&j+2<slen){
            int h1=url_hex_val(s[j+1]),h2=url_hex_val(s[j+2]);
            if (h1>=0&&h2>=0){r[ri++]=(char)(h1*16+h2);j+=3;}
            else r[ri++]=s[j++];
        } else if (s[j]=='+') { r[ri++]=' '; j++; }
        else r[ri++]=s[j++];
    }
    r[ri]='\0'; Value *v=xs_str(r); free(r); return v;
}
static Value *native_url_encode_query(Interp *ig, Value **a, int n) {
    if (n<1||(VAL_TAG(a[0])!=XS_MAP&&VAL_TAG(a[0])!=XS_MODULE)) return xs_str("");
    int nk=0; char **ks=map_keys(a[0]->map,&nk);
    int cap=256; char *out=xs_malloc(cap); int oi=0; out[0]='\0';
    for (int j=0;j<nk;j++){
        Value *kv=xs_str(ks[j]); Value *args2[1]={kv};
        Value *ek=native_url_encode(ig,args2,1); value_decref(kv);
        Value *vv=map_get(a[0]->map,ks[j]);
        Value *vs=(vv&&VAL_TAG(vv)==XS_STR)?value_incref(vv):xs_str("");
        Value *args3[1]={vs};
        Value *ev=native_url_encode(ig,args3,1); value_decref(vs);
        int ekl=(int)strlen(ek->s),evl=(int)strlen(ev->s);
        if (oi+ekl+evl+4>cap){cap=(cap+ekl+evl)*2;out=xs_realloc(out,cap);}
        if (oi) out[oi++]='&';
        memcpy(out+oi,ek->s,ekl); oi+=ekl;
        out[oi++]='=';
        memcpy(out+oi,ev->s,evl); oi+=evl;
        out[oi]='\0';
        value_decref(ek); value_decref(ev); free(ks[j]);
    }
    free(ks); Value *v=xs_str(out); free(out); return v;
}
static Value *native_url_parse_query(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_map_new();
    Value *m=xs_map_new();
    const char *s=a[0]->s;
    while (*s) {
        const char *eq=strchr(s,'='); if (!eq) break;
        const char *amp=strchr(eq,'&');
        int klen=(int)(eq-s);
        int vlen=amp?(int)(amp-eq-1):(int)strlen(eq+1);
        char *key=xs_strndup(s,klen);
        char *val=xs_strndup(eq+1,vlen);
        /* decode key */
        Value *kv=xs_str(key); Value *args2[1]={kv};
        Value *dk=native_url_decode(ig,args2,1); value_decref(kv); free(key);
        Value *vv=xs_str(val); args2[0]=vv;
        Value *dv=native_url_decode(ig,args2,1); value_decref(vv); free(val);
        map_set(m->map,dk->s,dv); value_decref(dv); value_decref(dk);
        s=amp?amp+1:"";
    }
    return m;
}
static Value *native_url_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_map_new();
    Value *m=xs_map_new();
    const char *url=a[0]->s;
    /* scheme */
    const char *p=strstr(url,"://");
    if (p) {
        Value *v=xs_str_n(url,(int)(p-url)); map_set(m->map,"scheme",v); value_decref(v);
        url=p+3;
    } else {
        Value *v=xs_str(""); map_set(m->map,"scheme",v); value_decref(v);
    }
    /* host */
    const char *slash=strchr(url,'/');
    const char *qmark=strchr(url,'?');
    const char *end_host=slash?(qmark&&qmark<slash?qmark:slash):qmark;
    if (end_host){
        Value *v=xs_str_n(url,(int)(end_host-url)); map_set(m->map,"host",v); value_decref(v);
        url=end_host;
    } else {
        Value *v=xs_str(url); map_set(m->map,"host",v); value_decref(v); url="";
    }
    /* path */
    const char *qm=strchr(url,'?'); const char *hash=strchr(url,'#');
    const char *path_end=qm?qm:(hash?hash:url+strlen(url));
    { Value *v=xs_str_n(url,(int)(path_end-url)); map_set(m->map,"path",v); value_decref(v); }
    if (qm) {
        url=qm+1; const char *frag=strchr(url,'#');
        int qlen=frag?(int)(frag-url):(int)strlen(url);
        Value *v=xs_str_n(url,qlen); map_set(m->map,"query",v); value_decref(v);
        if (frag) { Value *fv=xs_str(frag+1); map_set(m->map,"fragment",fv); value_decref(fv); }
        else { Value *fv=xs_str(""); map_set(m->map,"fragment",fv); value_decref(fv); }
    } else {
        Value *v=xs_str(""); map_set(m->map,"query",v); value_decref(v);
        if (hash) { Value *fv=xs_str(hash+1); map_set(m->map,"fragment",fv); value_decref(fv); }
        else { Value *fv=xs_str(""); map_set(m->map,"fragment",fv); value_decref(fv); }
    }
    return m;
}
Value *make_url_module(void) {
    XSMap *m=map_new();
    map_take(m,"encode",       xs_native(native_url_encode));
    map_take(m,"decode",       xs_native(native_url_decode));
    map_take(m,"encode_query", xs_native(native_url_encode_query));
    map_take(m,"parse_query",  xs_native(native_url_parse_query));
    map_take(m,"parse",        xs_native(native_url_parse));
    return xs_module(m);
}
