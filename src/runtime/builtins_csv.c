#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* csv module */
static Value *csv_parse_row(const char *s, int *pos2, char delim) {
    Value *row=xs_array_new();
    while (s[*pos2]&&s[*pos2]!='\n'&&s[*pos2]!='\r') {
        int start=*pos2;
        if (s[*pos2]=='"') {
            /* quoted field */
            (*pos2)++;
            int cap=64; char *buf=xs_malloc(cap); int ri=0;
            while (s[*pos2]&&!(s[*pos2]=='"'&&s[*pos2+1]!='"')) {
                if (ri+2>=cap){cap*=2;buf=xs_realloc(buf,cap);}
                if (s[*pos2]=='"'&&s[*pos2+1]=='"'){buf[ri++]='"';(*pos2)+=2;}
                else buf[ri++]=s[(*pos2)++];
            }
            if (s[*pos2]=='"') (*pos2)++;
            buf[ri]='\0'; array_push(row->arr,xs_str(buf)); free(buf);
        } else {
            /* unquoted field */
            while (s[*pos2]&&s[*pos2]!=delim&&s[*pos2]!='\n'&&s[*pos2]!='\r') (*pos2)++;
            array_push(row->arr,xs_str_n(s+start,*pos2-start));
        }
        if (s[*pos2]==delim) (*pos2)++;
        else break;
    }
    return row;
}
static Value *native_csv_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    char delim=',';
    if (n>=2&&VAL_TAG(a[1])==XS_STR&&a[1]->s[0]) delim=a[1]->s[0];
    const char *s=a[0]->s; int pos2=0; int slen=(int)strlen(s);
    Value *rows=xs_array_new();
    while (pos2<slen) {
        Value *row=csv_parse_row(s,&pos2,delim);
        array_push(rows->arr,row);
        while (s[pos2]=='\r'||s[pos2]=='\n') pos2++;
    }
    return rows;
}
static Value *native_csv_parse_with_headers(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    char delim=',';
    if (n>=2&&VAL_TAG(a[1])==XS_STR&&a[1]->s[0]) delim=a[1]->s[0];
    const char *s=a[0]->s; int pos2=0; int slen=(int)strlen(s);
    if (pos2>=slen) return xs_array_new();
    /* First row = headers */
    Value *hdr_row=csv_parse_row(s,&pos2,delim);
    while (s[pos2]=='\r'||s[pos2]=='\n') pos2++;
    Value *result=xs_array_new();
    while (pos2<slen) {
        Value *row=csv_parse_row(s,&pos2,delim);
        Value *rec=xs_map_new();
        for (int j=0;j<hdr_row->arr->len&&j<row->arr->len;j++){
            Value *hk=hdr_row->arr->items[j];
            char *ks=value_str(hk);
            Value *cell=value_incref(row->arr->items[j]);
            map_set(rec->map,ks,cell); value_decref(cell);
            free(ks);
        }
        value_decref(row);
        array_push(result->arr,rec);
        while (s[pos2]=='\r'||s[pos2]=='\n') pos2++;
    }
    value_decref(hdr_row);
    return result;
}
static Value *native_csv_stringify(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return xs_str("");
    char delim=',';
    if (n>=2&&VAL_TAG(a[1])==XS_STR&&a[1]->s[0]) delim=a[1]->s[0];
    int cap=256,len2=0; char *out=xs_malloc(cap); out[0]='\0';
    XSArray *rows=a[0]->arr;
    for (int r=0;r<rows->len;r++){
        if (r) {
            if (len2+2>cap){cap*=2;out=xs_realloc(out,cap);}
            out[len2++]='\n'; out[len2]='\0';
        }
        if (VAL_TAG(rows->items[r])!=XS_ARRAY) continue;
        XSArray *row=rows->items[r]->arr;
        for (int c=0;c<row->len;c++){
            if (c){
                if (len2+2>cap){cap*=2;out=xs_realloc(out,cap);}
                out[len2++]=delim; out[len2]='\0';
            }
            char *s=value_str(row->items[c]); int sl=(int)strlen(s);
            /* Quote if contains delimiter, quote, or newline */
            int need_quote=0;
            for(int j=0;s[j];j++) if(s[j]==delim||s[j]=='"'||s[j]=='\n'){need_quote=1;break;}
            if (need_quote) {
                if (len2+sl*2+4>cap){cap=cap*2+sl*2+4;out=xs_realloc(out,cap);}
                out[len2++]='"';
                for(int j=0;s[j];j++){if(s[j]=='"'){out[len2++]='"';}out[len2++]=s[j];}
                out[len2++]='"'; out[len2]='\0';
            } else {
                if (len2+sl+2>cap){cap=cap*2+sl+2;out=xs_realloc(out,cap);}
                memcpy(out+len2,s,sl); len2+=sl; out[len2]='\0';
            }
            free(s);
        }
    }
    Value *v=xs_str(out); free(out); return v;
}
static Value *native_csv_stringify_with_headers(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_ARRAY||VAL_TAG(a[1])!=XS_ARRAY) return xs_str("");
    /* Build a new array with headers prepended */
    Value *all=xs_array_new();
    array_push(all->arr,value_incref(a[0]));
    XSArray *rows=a[1]->arr;
    for (int j=0;j<rows->len;j++) array_push(all->arr,value_incref(rows->items[j]));
    Value *args2[1]={all};
    Value *r=native_csv_stringify(ig,args2,1);
    value_decref(all); return r;
}
Value *make_csv_module(void) {
    XSMap *m=map_new();
    map_take(m,"parse",               xs_native(native_csv_parse));
    map_take(m,"parse_with_headers",  xs_native(native_csv_parse_with_headers));
    map_take(m,"stringify",           xs_native(native_csv_stringify));
    map_take(m,"stringify_with_headers",xs_native(native_csv_stringify_with_headers));
    return xs_module(m);
}
