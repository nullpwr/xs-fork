#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* json module */
typedef struct { const char *s; int pos; } JsonParser;
static Value *json_parse_value(JsonParser *p);
static void json_skip_ws(JsonParser *p) {
    while (p->s[p->pos]==' '||p->s[p->pos]=='\t'||p->s[p->pos]=='\n'||p->s[p->pos]=='\r') p->pos++;
}
static Value *json_parse_string(JsonParser *p) {
    if (p->s[p->pos]!='"') return NULL;
    p->pos++;
    int cap=64; char *buf=xs_malloc(cap); int ri=0;
    while (p->s[p->pos]&&p->s[p->pos]!='"') {
        if (ri+8>=cap){cap*=2;buf=xs_realloc(buf,cap);}
        if (p->s[p->pos]=='\\') {
            p->pos++;
            switch (p->s[p->pos]) {
            case '"': buf[ri++]='"'; break;
            case '\\': buf[ri++]='\\'; break;
            case '/': buf[ri++]='/'; break;
            case 'n': buf[ri++]='\n'; break;
            case 't': buf[ri++]='\t'; break;
            case 'r': buf[ri++]='\r'; break;
            case 'b': buf[ri++]='\b'; break;
            case 'f': buf[ri++]='\f'; break;
            case 'u': {
                /* \uXXXX */
                char hex[5]={p->s[p->pos+1],p->s[p->pos+2],p->s[p->pos+3],p->s[p->pos+4],0};
                unsigned cp=(unsigned)strtoul(hex,NULL,16);
                p->pos+=4;
                /* Handle surrogate pairs: \uD800-\uDBFF followed by \uDC00-\uDFFF */
                if (cp>=0xD800 && cp<=0xDBFF && p->s[p->pos+1]=='\\' && p->s[p->pos+2]=='u') {
                    char hex2[5]={p->s[p->pos+3],p->s[p->pos+4],p->s[p->pos+5],p->s[p->pos+6],0};
                    unsigned lo=(unsigned)strtoul(hex2,NULL,16);
                    if (lo>=0xDC00 && lo<=0xDFFF) {
                        cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00);
                        p->pos+=6; /* skip \uXXXX of low surrogate */
                    }
                }
                /* Encode codepoint as UTF-8 */
                if (ri+4>=cap){cap*=2;buf=xs_realloc(buf,cap);}
                if (cp<0x80) {
                    buf[ri++]=(char)cp;
                } else if (cp<0x800) {
                    buf[ri++]=(char)(0xC0|(cp>>6));
                    buf[ri++]=(char)(0x80|(cp&0x3F));
                } else if (cp<0x10000) {
                    buf[ri++]=(char)(0xE0|(cp>>12));
                    buf[ri++]=(char)(0x80|((cp>>6)&0x3F));
                    buf[ri++]=(char)(0x80|(cp&0x3F));
                } else if (cp<=0x10FFFF) {
                    buf[ri++]=(char)(0xF0|(cp>>18));
                    buf[ri++]=(char)(0x80|((cp>>12)&0x3F));
                    buf[ri++]=(char)(0x80|((cp>>6)&0x3F));
                    buf[ri++]=(char)(0x80|(cp&0x3F));
                } else {
                    buf[ri++]='?'; /* invalid codepoint */
                }
                break;
            }
            default: buf[ri++]=p->s[p->pos]; break;
            }
        } else {
            buf[ri++]=p->s[p->pos];
        }
        p->pos++;
    }
    if (p->s[p->pos]=='"') p->pos++;
    buf[ri]='\0'; Value *v=xs_str(buf); free(buf); return v;
}
static Value *json_parse_number(JsonParser *p) {
    const char *start=p->s+p->pos;
    int is_float=0;
    if (p->s[p->pos]=='-') p->pos++;
    while (isdigit((unsigned char)p->s[p->pos])) p->pos++;
    if (p->s[p->pos]=='.'){is_float=1;p->pos++;while(isdigit((unsigned char)p->s[p->pos]))p->pos++;}
    if (p->s[p->pos]=='e'||p->s[p->pos]=='E'){is_float=1;p->pos++;if(p->s[p->pos]=='+'||p->s[p->pos]=='-')p->pos++;while(isdigit((unsigned char)p->s[p->pos]))p->pos++;}
    if (is_float) return xs_float(strtod(start,NULL));
    return xs_int((int64_t)strtoll(start,NULL,10));
}
static Value *json_parse_array(JsonParser *p) {
    p->pos++; /* skip [ */
    json_skip_ws(p);
    Value *arr=xs_array_new();
    if (p->s[p->pos]==']'){p->pos++;return arr;}
    while (p->s[p->pos]) {
        json_skip_ws(p);
        Value *v=json_parse_value(p);
        if (!v) break;
        array_push(arr->arr,v);
        json_skip_ws(p);
        if (p->s[p->pos]==','){p->pos++;continue;}
        if (p->s[p->pos]==']'){p->pos++;break;}
        break;
    }
    return arr;
}
static Value *json_parse_object(JsonParser *p) {
    p->pos++; /* skip { */
    json_skip_ws(p);
    Value *m=xs_map_new();
    if (p->s[p->pos]=='}'){p->pos++;return m;}
    while (p->s[p->pos]) {
        json_skip_ws(p);
        if (p->s[p->pos]!='"') break;
        Value *key=json_parse_string(p);
        if (!key) break;
        json_skip_ws(p);
        if (p->s[p->pos]!=':'){value_decref(key);break;}
        p->pos++;
        json_skip_ws(p);
        Value *val=json_parse_value(p);
        if (!val) val=value_incref(XS_NULL_VAL);
        map_set(m->map,key->s,val); value_decref(val);
        value_decref(key);
        json_skip_ws(p);
        if (p->s[p->pos]==','){p->pos++;continue;}
        if (p->s[p->pos]=='}'){p->pos++;break;}
        break;
    }
    return m;
}
static Value *json_parse_value(JsonParser *p) {
    json_skip_ws(p);
    char c=p->s[p->pos];
    if (c=='"') return json_parse_string(p);
    if (c=='[') return json_parse_array(p);
    if (c=='{') return json_parse_object(p);
    if (c=='-'||isdigit((unsigned char)c)) return json_parse_number(p);
    if (strncmp(p->s+p->pos,"true",4)==0){p->pos+=4;return value_incref(XS_TRUE_VAL);}
    if (strncmp(p->s+p->pos,"false",5)==0){p->pos+=5;return value_incref(XS_FALSE_VAL);}
    if (strncmp(p->s+p->pos,"null",4)==0){p->pos+=4;return value_incref(XS_NULL_VAL);}
    return NULL;
}

static void json_stringify_val(Value *v, int indent, int depth, char **out, int *len, int *cap);
static void json_append(char **out, int *len, int *cap, const char *s, int slen) {
    if (*len+slen+1>*cap){*cap=(*cap)*2+slen+64;*out=xs_realloc(*out,*cap);}
    memcpy(*out+*len,s,slen); *len+=slen; (*out)[*len]='\0';
}
static void json_append_str_escaped(const char *s, char **out, int *len, int *cap) {
    json_append(out,len,cap,"\"",1);
    for (const char *p=s;*p;p++){
        if (*p=='"'){json_append(out,len,cap,"\\\"",2);}
        else if(*p=='\\'){json_append(out,len,cap,"\\\\",2);}
        else if(*p=='\n'){json_append(out,len,cap,"\\n",2);}
        else if(*p=='\t'){json_append(out,len,cap,"\\t",2);}
        else if(*p=='\r'){json_append(out,len,cap,"\\r",2);}
        else { char cb[2]={*p,0}; json_append(out,len,cap,cb,1);}
    }
    json_append(out,len,cap,"\"",1);
}
static void json_indent_line(int indent, int depth, char **out, int *len, int *cap) {
    if (indent<=0) return;
    json_append(out,len,cap,"\n",1);
    for (int j=0;j<depth*indent;j++) json_append(out,len,cap," ",1);
}
static void json_stringify_val(Value *v, int indent, int depth, char **out, int *len, int *cap) {
    if (!v||VAL_TAG(v)==XS_NULL){json_append(out,len,cap,"null",4);return;}
    if (VAL_TAG(v)==XS_BOOL){
        if (VAL_INT(v)) json_append(out,len,cap,"true",4);
        else json_append(out,len,cap,"false",5);
        return;
    }
    if (VAL_TAG(v)==XS_INT){
        char buf[32]; int bl=snprintf(buf,sizeof(buf),"%lld",(long long)VAL_INT(v));
        json_append(out,len,cap,buf,bl); return;
    }
    if (VAL_TAG(v)==XS_FLOAT){
        char buf[64]; int bl=snprintf(buf,sizeof(buf),"%g",v->f);
        json_append(out,len,cap,buf,bl); return;
    }
    if (VAL_TAG(v)==XS_STR){json_append_str_escaped(v->s,out,len,cap);return;}
    if (VAL_TAG(v)==XS_CHAR){
        char cb[2]={v->s?v->s[0]:0,0};
        json_append_str_escaped(cb,out,len,cap); return;
    }
    if (VAL_TAG(v)==XS_ARRAY||VAL_TAG(v)==XS_TUPLE){
        json_append(out,len,cap,"[",1);
        XSArray *arr=v->arr;
        for (int j=0;j<arr->len;j++){
            if (j) json_append(out,len,cap,",",1);
            if (indent>0) json_indent_line(indent,depth+1,out,len,cap);
            json_stringify_val(arr->items[j],indent,depth+1,out,len,cap);
        }
        if (indent>0&&arr->len>0) json_indent_line(indent,depth,out,len,cap);
        json_append(out,len,cap,"]",1); return;
    }
    if (VAL_TAG(v)==XS_MAP||VAL_TAG(v)==XS_MODULE){
        json_append(out,len,cap,"{",1);
        int nk=0; char **ks=map_keys(v->map,&nk);
        for (int j=0;j<nk;j++){
            if (j) json_append(out,len,cap,",",1);
            if (indent>0) json_indent_line(indent,depth+1,out,len,cap);
            json_append_str_escaped(ks[j],out,len,cap);
            json_append(out,len,cap,":",1);
            if (indent>0) json_append(out,len,cap," ",1);
            Value *mv=map_get(v->map,ks[j]);
            json_stringify_val(mv,indent,depth+1,out,len,cap);
            free(ks[j]);
        }
        free(ks);
        if (indent>0&&nk>0) json_indent_line(indent,depth,out,len,cap);
        json_append(out,len,cap,"}",1); return;
    }
    json_append(out,len,cap,"null",4);
}

static Value *native_json_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    JsonParser p={a[0]->s,0};
    Value *v=json_parse_value(&p);
    return v?v:value_incref(XS_NULL_VAL);
}
static Value *native_json_stringify(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("null");
    int indent=(n>=2&&VAL_TAG(a[1])==XS_INT)?(int)VAL_INT(a[1]):0;
    int cap=256,len2=0; char *out=xs_malloc(cap); out[0]='\0';
    json_stringify_val(a[0],indent,0,&out,&len2,&cap);
    Value *v=xs_str(out); free(out); return v;
}
static Value *native_json_pretty(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("null");
    int cap=256,len2=0; char *out=xs_malloc(cap); out[0]='\0';
    json_stringify_val(a[0],2,0,&out,&len2,&cap);
    Value *v=xs_str(out); free(out); return v;
}
static Value *native_json_valid(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    JsonParser p={a[0]->s,0};
    Value *v=json_parse_value(&p);
    if (v){value_decref(v);return value_incref(XS_TRUE_VAL);}
    return value_incref(XS_FALSE_VAL);
}
static Value *native_json_parse_safe(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    JsonParser p={a[0]->s,0};
    Value *v=json_parse_value(&p);
    return v?v:value_incref(XS_NULL_VAL);
}
Value *make_json_module(void) {
    XSMap *m=map_new();
    map_take(m,"parse",       xs_native(native_json_parse));
    map_take(m,"stringify",   xs_native(native_json_stringify));
    map_take(m,"pretty",      xs_native(native_json_pretty));
    map_take(m,"valid",       xs_native(native_json_valid));
    map_take(m,"parse_safe",  xs_native(native_json_parse_safe));
    return xs_module(m);
}

/* io.read_json / io.write_json: defined here because they depend on json helpers */
Value *native_io_read_json(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(a[0]->s,"r");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=xs_malloc(sz+1);
    long nr=(long)fread(buf,1,sz,f); fclose(f); buf[nr]='\0';
    JsonParser p={buf,0};
    json_skip_ws(&p);
    Value *v=json_parse_value(&p);
    free(buf);
    return v?v:value_incref(XS_NULL_VAL);
}
Value *native_io_write_json(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    int indent=(n>=3&&VAL_TAG(a[2])==XS_INT)?(int)VAL_INT(a[2]):2;
    char *out=xs_malloc(256); int len2=0,cap=256;
    json_stringify_val(a[1],indent,0,&out,&len2,&cap);
    out[len2]='\0';
    FILE *f=fopen(a[0]->s,"w");
    if (!f){free(out);return value_incref(XS_FALSE_VAL);}
    fputs(out,f); fputc('\n',f); fclose(f); free(out);
    return value_incref(XS_TRUE_VAL);
}
