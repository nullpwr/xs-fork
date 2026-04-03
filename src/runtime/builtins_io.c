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
#include <errno.h>
#if !defined(__MINGW32__) && !defined(__wasi__)
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/select.h>      /* fd_set / FD_ZERO / FD_SET / select */
#  include <dirent.h>
#  include <glob.h>
#  include <fcntl.h>
#endif

/* io module */
static Value *native_io_read_file(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = fopen(args[0]->s, "r");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=xs_malloc(sz+1);
    long nr = (long)fread(buf,1,sz,f); fclose(f); buf[nr]='\0';
    Value *v=xs_str(buf); free(buf); return v;
}

static Value *native_io_write_file(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<2||VAL_TAG(args[0])!=XS_STR||VAL_TAG(args[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(args[0]->s,"w");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(args[1]->s,f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}



static Value *native_io_read_line(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR) printf("%s", args[0]->s);
    fflush(stdout);
    char buf[1024]; buf[0] = '\0';
    if (fgets(buf, sizeof(buf), stdin)) {
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
static Value *native_io_get_key_nowait(Interp *ig, Value **args, int argc) {
    (void)ig;
    int timeout_ms = 0;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) timeout_ms = (int)VAL_INT(args[0]);
#if defined(__wasi__)
    (void)timeout_ms;
    return value_incref(XS_NULL_VAL);
#elif defined(__MINGW32__)
    /* Windows: poll with kbhit in a loop */
    #include <conio.h>
    DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    while (GetTickCount() < deadline || timeout_ms == 0) {
        if (_kbhit()) {
            char buf[64]; buf[0]='\0'; int bi=0;
            int c = _getch();
            if (c == 0 || c == 0xE0) { /* special key prefix */
                int c2 = _getch();
                if (c2==72) return xs_str("UP");
                if (c2==80) return xs_str("DOWN");
                if (c2==75) return xs_str("LEFT");
                if (c2==77) return xs_str("RIGHT");
                return xs_str("UNKNOWN");
            }
            buf[bi++]=(char)c; buf[bi]='\0';
            return xs_str(buf);
        }
        if (timeout_ms == 0) break;
        Sleep(10);
    }
    return value_incref(XS_NULL_VAL);
#else
    /* POSIX: use select() */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) return value_incref(XS_NULL_VAL);
    char buf[64]; buf[0] = '\0';
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf)-1);
    if (n <= 0) return value_incref(XS_NULL_VAL);
    buf[n] = '\0';
    int blen = (int)n;
    while (blen > 0 && (buf[blen-1]=='\n'||buf[blen-1]=='\r')) buf[--blen]='\0';
    if (buf[0] == '\033') {
        if (strcmp(buf, "\033[A")==0) return xs_str("UP");
        if (strcmp(buf, "\033[B")==0) return xs_str("DOWN");
        if (strcmp(buf, "\033[C")==0) return xs_str("RIGHT");
        if (strcmp(buf, "\033[D")==0) return xs_str("LEFT");
        return xs_str("ESC");
    }
    return xs_str(buf);
#endif
}
static Value *native_io_append_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"a");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(a[1]->s,f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}
static Value *native_io_read_lines(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(a[0]->s,"r");
    if (!f) return value_incref(XS_NULL_VAL);
    Value *arr=xs_array_new();
    char buf[4096];
    while (fgets(buf,sizeof(buf),f)) {
        int len=(int)strlen(buf);
        while (len>0&&(buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
        array_push(arr->arr,xs_str(buf));
    }
    fclose(f); return arr;
}
static Value *native_io_write_lines(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_ARRAY) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"w");
    if (!f) return value_incref(XS_FALSE_VAL);
    XSArray *arr=a[1]->arr;
    for (int j=0;j<arr->len;j++) {
        char *s=value_str(arr->items[j]); fputs(s,f); fputc('\n',f); free(s);
    }
    fclose(f); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_read_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(a[0]->s,"rb");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *buf=xs_malloc(sz+1);
    long nr = (long)fread(buf,1,sz,f); fclose(f);
    Value *arr=xs_array_new();
    for (long j=0;j<nr;j++) array_push(arr->arr,xs_int((int64_t)buf[j]));
    free(buf); return arr;
}
static Value *native_io_write_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_ARRAY) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"wb");
    if (!f) return value_incref(XS_FALSE_VAL);
    XSArray *arr=a[1]->arr;
    for (int j=0;j<arr->len;j++) {
        if (VAL_TAG(arr->items[j])==XS_INT) {
            unsigned char b=(unsigned char)(VAL_INT(arr->items[j])&0xff);
            fwrite(&b,1,1,f);
        }
    }
    fclose(f); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_file_exists(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (access(a[0]->s,F_OK)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_file_size(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_int(-1);
    struct stat st; if (stat(a[0]->s,&st)!=0) return xs_int(-1);
    return xs_int((int64_t)st.st_size);
}
static Value *native_io_delete_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (remove(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_copy_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *src=fopen(a[0]->s,"rb"); if (!src) return value_incref(XS_FALSE_VAL);
    FILE *dst=fopen(a[1]->s,"wb"); if (!dst){fclose(src);return value_incref(XS_FALSE_VAL);}
    char buf[8192]; size_t r;
    while ((r=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,r,dst);
    fclose(src); fclose(dst); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_rename_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rename(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
int xs_io_mkdirs(const char *path) {
    char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",path);
    int len=(int)strlen(tmp);
    if (tmp[len-1]=='/') tmp[--len]='\0';
    for (int j=1;j<len;j++) {
        if (tmp[j]=='/') {
            tmp[j]='\0'; mkdir(tmp,0755); tmp[j]='/';
        }
    }
    return mkdir(tmp,0755);
}
static Value *native_io_make_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    int r=xs_io_mkdirs(a[0]->s);
    return (r==0||errno==EEXIST)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_list_dir(Interp *ig, Value **a, int n) {
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
static Value *native_io_stdin_read(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    size_t cap=256,pos=0; char *buf=xs_malloc(cap);
    int c;
    while ((c=fgetc(stdin))!=EOF) {
        if (pos+1>=cap){cap*=2;buf=xs_realloc(buf,cap);}
        buf[pos++]=(char)c;
    }
    buf[pos]='\0'; Value *v=xs_str(buf); free(buf); return v;
}
static Value *native_io_stdin_readline(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    char buf[4096]; buf[0]='\0';
    if (fgets(buf,sizeof(buf),stdin)){
        int len=(int)strlen(buf);
        while(len>0&&(buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
static Value *native_io_stdin_read_n(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return value_incref(XS_NULL_VAL);
    int64_t count = VAL_INT(a[0]);
    if (count <= 0) return xs_str("");
    char *buf = xs_malloc((size_t)count + 1);
    size_t total = 0;
    while (total < (size_t)count) {
        size_t r = fread(buf + total, 1, (size_t)count - total, stdin);
        if (r == 0) break;
        total += r;
    }
    buf[total] = '\0';
    Value *v = xs_str(buf);
    free(buf);
    return v;
}
static Value *native_io_is_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_is_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}

/* move_file: rename, falling back to copy+delete for cross-device moves */
static Value *native_io_move_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    if (rename(a[0]->s,a[1]->s)==0) return value_incref(XS_TRUE_VAL);
    /* cross-device fallback: copy then delete */
    FILE *src=fopen(a[0]->s,"rb"); if (!src) return value_incref(XS_FALSE_VAL);
    FILE *dst=fopen(a[1]->s,"wb"); if (!dst){fclose(src);return value_incref(XS_FALSE_VAL);}
    char buf[8192]; size_t r;
    while ((r=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,r,dst);
    fclose(src); fclose(dst);
    remove(a[0]->s);
    return value_incref(XS_TRUE_VAL);
}

/* temp_file: create a temp file, return its path */
static Value *native_io_temp_file(Interp *ig, Value **a, int n) {
    (void)ig;
    const char *suffix = (n>=1 && VAL_TAG(a[0])==XS_STR) ? a[0]->s : "";
    const char *prefix = (n>=2 && VAL_TAG(a[1])==XS_STR) ? a[1]->s : "xs_tmp_";
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char tmpl[4096];
    snprintf(tmpl,sizeof(tmpl),"%s/%sXXXXXX%s",tmpdir,prefix,suffix);
#if !defined(__MINGW32__) && !defined(__wasi__)
    int fd;
    if (suffix[0]) {
        #ifdef __APPLE__
        fd = mkstemp(tmpl);
#else
        fd = mkstemps(tmpl,(int)strlen(suffix));
#endif
    } else {
        fd = mkstemp(tmpl);
    }
    if (fd<0) return value_incref(XS_NULL_VAL);
    close(fd);
#else
    if (!_mktemp(tmpl)) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(tmpl,"w"); if (f) fclose(f);
#endif
    return xs_str(tmpl);
}

/* temp_dir: create a temp directory, return its path */
static Value *native_io_temp_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    const char *prefix = (n>=1 && VAL_TAG(a[0])==XS_STR) ? a[0]->s : "xs_tmpd_";
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char tmpl[4096];
    snprintf(tmpl,sizeof(tmpl),"%s/%sXXXXXX",tmpdir,prefix);
#if !defined(__MINGW32__) && !defined(__wasi__)
    #ifdef __APPLE__
    extern char *mkdtemp(char *);
#endif
    char *res = mkdtemp(tmpl);
    if (!res) return value_incref(XS_NULL_VAL);
    return xs_str(res);
#else
    if (!_mktemp(tmpl)) return value_incref(XS_NULL_VAL);
    mkdir(tmpl, 0700);
    return xs_str(tmpl);
#endif
}

/* file_info: return map with size, is_file, is_dir, modified, path */
static Value *native_io_file_info(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    struct stat st;
    if (stat(a[0]->s,&st)!=0) return value_incref(XS_NULL_VAL);
    Value *m = xs_map_new();
    Value *v;
    v=xs_int((int64_t)st.st_size); map_set(m->map,"size",v); value_decref(v);
    v=S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(m->map,"is_file",v); value_decref(v);
    v=S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(m->map,"is_dir",v); value_decref(v);
    v=xs_int((int64_t)st.st_mtime); map_set(m->map,"modified",v); value_decref(v);
    v=xs_str(a[0]->s); map_set(m->map,"path",v); value_decref(v);
    return m;
}

/* stdin_lines: read all stdin lines as array */
static Value *native_io_stdin_lines(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *arr=xs_array_new();
    char buf[4096];
    while (fgets(buf,sizeof(buf),stdin)) {
        int len2=(int)strlen(buf);
        while (len2>0&&(buf[len2-1]=='\n'||buf[len2-1]=='\r')) buf[--len2]='\0';
        array_push(arr->arr,xs_str(buf));
    }
    return arr;
}

/* glob: pattern matching for file paths */
static Value *native_io_glob(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
#if !defined(__wasi__)
    glob_t g; memset(&g,0,sizeof(g));
    Value *arr=xs_array_new();
    if (glob(a[0]->s,0,NULL,&g)==0) {
        for (size_t j=0;j<g.gl_pathc;j++) array_push(arr->arr,xs_str(g.gl_pathv[j]));
    }
    globfree(&g); return arr;
#else
    return xs_array_new();
#endif
}

/* symlink: create a symbolic link */
static Value *native_io_symlink(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    return (symlink(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

/* stdout/stderr sub-module helpers */
static Value *native_io_stdout_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) fputs(a[0]->s,stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stdout_writeln(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) { fputs(a[0]->s,stdout); fputc('\n',stdout); }
    else fputc('\n',stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stdout_flush(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    fflush(stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) fputs(a[0]->s,stderr);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_writeln(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) { fputs(a[0]->s,stderr); fputc('\n',stderr); }
    else fputc('\n',stderr);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_flush(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    fflush(stderr);
    return value_incref(XS_NULL_VAL);
}


static Value *native_io_wait_for_key(Interp *, Value **, int);
Value *make_io_module(void) {
    XSMap *m = map_new();
    /* file operations */
    map_take(m,"read_file",      xs_native(native_io_read_file));
    map_take(m,"write_file",     xs_native(native_io_write_file));
    map_take(m,"append_file",    xs_native(native_io_append_file));
    map_take(m,"read_lines",     xs_native(native_io_read_lines));
    map_take(m,"write_lines",    xs_native(native_io_write_lines));
    map_take(m,"read_bytes",     xs_native(native_io_read_bytes));
    map_take(m,"write_bytes",    xs_native(native_io_write_bytes));
    /* file info */
    map_take(m,"file_exists",    xs_native(native_io_file_exists));
    map_take(m,"exists",         xs_native(native_io_file_exists));
    map_take(m,"file_size",      xs_native(native_io_file_size));
    map_take(m,"size",           xs_native(native_io_file_size));
    map_take(m,"file_info",      xs_native(native_io_file_info));
    map_take(m,"is_file",        xs_native(native_io_is_file));
    map_take(m,"is_dir",         xs_native(native_io_is_dir));
    /* file manipulation */
    map_take(m,"delete_file",    xs_native(native_io_delete_file));
    map_take(m,"copy_file",      xs_native(native_io_copy_file));
    map_take(m,"move_file",      xs_native(native_io_move_file));
    map_take(m,"rename_file",    xs_native(native_io_rename_file));
    map_take(m,"symlink",        xs_native(native_io_symlink));
    /* directories */
    map_take(m,"make_dir",       xs_native(native_io_make_dir));
    map_take(m,"list_dir",       xs_native(native_io_list_dir));
    map_take(m,"glob",           xs_native(native_io_glob));
    /* temp files */
    map_take(m,"temp_file",      xs_native(native_io_temp_file));
    map_take(m,"temp_dir",       xs_native(native_io_temp_dir));
    /* stdin */
    map_take(m,"stdin_read",     xs_native(native_io_stdin_read));
    map_take(m,"stdin_readline", xs_native(native_io_stdin_readline));
    map_take(m,"stdin_read_n",  xs_native(native_io_stdin_read_n));
    map_take(m,"stdin_lines",    xs_native(native_io_stdin_lines));
    /* keyboard */
    map_take(m,"wait_for_key",   xs_native(native_io_wait_for_key));
    map_take(m,"read_line",      xs_native(native_io_read_line));
    map_take(m,"get_key_nowait", xs_native(native_io_get_key_nowait));
    /* stdout sub-module */
    Value *out_m=xs_map_new();
    map_take(out_m->map, "write", xs_native(native_io_stdout_write));
    map_take(out_m->map, "writeln", xs_native(native_io_stdout_writeln));
    map_take(out_m->map, "flush", xs_native(native_io_stdout_flush));
    map_set(m,"stdout",out_m); value_decref(out_m);
    /* stderr sub-module */
    Value *err_m=xs_map_new();
    map_take(err_m->map, "write", xs_native(native_io_stderr_write));
    map_take(err_m->map, "writeln", xs_native(native_io_stderr_writeln));
    map_take(err_m->map, "flush", xs_native(native_io_stderr_flush));
    map_set(m,"stderr",err_m); value_decref(err_m);
    return xs_module(m);
}


static Value *native_io_wait_for_key(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR) printf("%s", args[0]->s);
    fflush(stdout);
    char buf[256]; buf[0] = '\0';
    if (fgets(buf, sizeof(buf), stdin)) {
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
