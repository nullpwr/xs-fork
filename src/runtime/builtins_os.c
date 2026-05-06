#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/triggers.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif

/* os module */
static Value *native_os_cwd(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    char buf[4096]; if (!getcwd(buf,sizeof(buf))) return value_incref(XS_NULL_VAL);
    return xs_str(buf);
}
static Value *native_os_chdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (chdir(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_home(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    const char *h=getenv("HOME"); return h?xs_str(h):xs_str("");
}
static Value *native_os_tempdir(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    const char *t=getenv("TMPDIR"); return xs_str(t?t:"/tmp");
}
static Value *native_os_mkdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    int r=xs_io_mkdirs(a[0]->s);
    return (r==0||errno==EEXIST)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_rmdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rmdir(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_remove(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (unlink(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_rename(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rename(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_exists(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (access(a[0]->s,F_OK)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_is_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_is_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_cpu_count(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    #ifdef _SC_NPROCESSORS_ONLN
    long c=sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__APPLE__)
    long c=1; /* fallback */
#else
    long c=1;
#endif
    return xs_int(c>0?c:1);
}
static Value *native_os_pid(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0);
#else
    return xs_int((int64_t)getpid());
#endif
}
static Value *native_os_ppid(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0);
#else
    return xs_int((int64_t)getppid());
#endif
}
static Value *native_os_exit(Interp *ig, Value **a, int n) {
    int code=(n>0&&VAL_TAG(a[0])==XS_INT)?(int)VAL_INT(a[0]):0;
    trigger_fire_on_exit(ig);
    exit(code);
}
static Value *native_os_list_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    DIR *d=opendir(a[0]->s); if (!d) return xs_array_new();
    Value *arr=xs_array_new();
    struct dirent *ent;
    while ((ent=readdir(d))!=NULL) {
        if (strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0) continue;
        array_push(arr->arr,xs_str(ent->d_name));
    }
    closedir(d); return arr;
}
static Value *native_os_glob(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    glob_t g; memset(&g,0,sizeof(g));
    Value *arr=xs_array_new();
    if (glob(a[0]->s,0,NULL,&g)==0) {
        for (size_t j=0;j<g.gl_pathc;j++) array_push(arr->arr,xs_str(g.gl_pathv[j]));
    }
    globfree(&g); return arr;
}
static Value *native_os_env_get(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *v=getenv(a[0]->s);
    if (!v) return (n>1)?value_incref(a[1]):value_incref(XS_NULL_VAL);
    return xs_str(v);
}
static Value *native_os_env_set(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (setenv(a[0]->s,a[1]->s,1)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_env_has(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return getenv(a[0]->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
#ifndef _WIN32
extern char **environ;
#endif
static Value *native_os_env_all(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *m=xs_map_new();
    for (char **ep=environ;ep&&*ep;ep++) {
        char *eq=strchr(*ep,'=');
        if (!eq) continue;
        char key[256]; int klen=(int)(eq-*ep);
        if (klen>=256) klen=255;
        strncpy(key,*ep,klen); key[klen]='\0';
        Value *v=xs_str(eq+1); map_set(m->map,key,v); value_decref(v);
    }
    return m;
}

Value *make_os_module(Interp *ig) {
    XSMap *m=map_new();
    Value *args_arr=xs_array_new();
    for (int ai = 0; ai < g_xs_argc; ai++) {
        Value *s = xs_str(g_xs_argv[ai]);
        array_push(args_arr->arr, s);
        value_decref(s);
    }
    map_set(m,"args",args_arr); value_decref(args_arr);
    map_take(m,"cwd",      xs_native(native_os_cwd));
    map_take(m,"chdir",    xs_native(native_os_chdir));
    map_take(m,"home",     xs_native(native_os_home));
    map_take(m,"tempdir",  xs_native(native_os_tempdir));
    map_take(m,"mkdir",    xs_native(native_os_mkdir));
    map_take(m,"rmdir",    xs_native(native_os_rmdir));
    map_take(m,"remove",   xs_native(native_os_remove));
    map_take(m,"rename",   xs_native(native_os_rename));
    map_take(m,"exists",   xs_native(native_os_exists));
    map_take(m,"is_file",  xs_native(native_os_is_file));
    map_take(m,"is_dir",   xs_native(native_os_is_dir));
    map_take(m,"cpu_count",xs_native(native_os_cpu_count));
    map_take(m,"pid",      xs_native(native_os_pid));
    map_take(m,"ppid",     xs_native(native_os_ppid));
    map_take(m,"exit",     xs_native(native_os_exit));
    map_take(m,"list_dir", xs_native(native_os_list_dir));
    map_take(m,"glob",     xs_native(native_os_glob));
    /* platform / sep */
#ifdef __APPLE__
    { Value *v=xs_str("darwin"); map_set(m,"platform",v); value_decref(v); }
#elif defined(_WIN32)
    { Value *v=xs_str("windows"); map_set(m,"platform",v); value_decref(v); }
#else
    { Value *v=xs_str("linux"); map_set(m,"platform",v); value_decref(v); }
#endif
#ifdef _WIN32
    { Value *v=xs_str("\\"); map_set(m,"sep",v); value_decref(v); }
#else
    { Value *v=xs_str("/"); map_set(m,"sep",v); value_decref(v); }
#endif
    /* env as a callable (getenv) + helper functions at top level */
    map_take(m,"env",      xs_native(native_os_env_get));
    map_take(m,"getenv",   xs_native(native_os_env_get));
    map_take(m,"setenv",   xs_native(native_os_env_set));
    map_take(m,"hasenv",   xs_native(native_os_env_has));
    map_take(m,"environ",  xs_native(native_os_env_all));
    (void)ig;
    return xs_module(m);
}
