#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/concurrent.h"
#include "runtime/triggers.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
#include <strings.h>          /* strcasecmp */
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define strcasecmp _stricmp
#elif defined(__wasi__)
#include <unistd.h>
#include <signal.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

/* process module */
static Value *native_process_pid(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0); /* WASI has no process identity */
#else
    return xs_int((int64_t)getpid());
#endif
}
static Value *native_process_run(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = popen(a[0]->s, "r");
    Value *result = xs_map_new();
    if (!f) {
        Value *ok=value_incref(XS_FALSE_VAL); map_set(result->map,"ok",ok); value_decref(ok);
        Value *out=xs_str(""); map_set(result->map,"stdout",out); value_decref(out);
        Value *code=xs_int(-1); map_set(result->map,"code",code); value_decref(code);
        return result;
    }
    size_t cap=256, pos=0;
    char *buf=xs_malloc(cap);
    int c;
    while ((c=fgetc(f))!=EOF) {
        if (pos+1>=cap) { cap*=2; buf=xs_realloc(buf,cap); }
        buf[pos++]=(char)c;
    }
    buf[pos]='\0';
    int status=pclose(f);
    int code2=(status==-1)?-1:(status>>8)&0xff;
    Value *ok=code2==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(result->map,"ok",ok); value_decref(ok);
    Value *out=xs_str(buf); free(buf); map_set(result->map,"stdout",out); value_decref(out);
    Value *cv=xs_int(code2); map_set(result->map,"code",cv); value_decref(cv);
    return result;
}
/* process.exec(cmd_string) - run shell command, return exit code */
static Value *native_process_exec(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_int(-1);
    int rc = system(a[0]->s);
    return xs_int(rc);
}

/* process.spawn(cmd, args, opts) - spawn with pipe access */
static Value *native_process_spawn_stdin_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "_stdin_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_FALSE_VAL);
    if (VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    ssize_t w = write((int)VAL_INT(fdv), a[1]->s, strlen(a[1]->s));
    return (w >= 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

#if !defined(__MINGW32__) && !defined(__wasi__)
static Value *native_process_spawn_stdout_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_stdout_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_NULL_VAL);
    int maxn = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxn = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxn + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxn);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
}

static Value *native_process_spawn_stderr_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_stderr_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_NULL_VAL);
    int maxn = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxn = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxn + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxn);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
}

static Value *native_process_spawn_wait(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return xs_int(-1);
    Value *pidv = map_get(a[0]->map, "pid");
    if (!pidv || VAL_TAG(pidv) != XS_INT) return xs_int(-1);
    int status = 0;
    waitpid((pid_t)VAL_INT(pidv), &status, 0);
    /* close remaining fds */
    Value *si = map_get(a[0]->map, "_stdin_fd");
    if (si && VAL_TAG(si) == XS_INT && VAL_INT(si) > 0) { close((int)VAL_INT(si)); map_take(a[0]->map, "_stdin_fd", xs_int(0)); }
    Value *so = map_get(a[0]->map, "_stdout_fd");
    if (so && VAL_TAG(so) == XS_INT && VAL_INT(so) > 0) { close((int)VAL_INT(so)); map_take(a[0]->map, "_stdout_fd", xs_int(0)); }
    Value *se = map_get(a[0]->map, "_stderr_fd");
    if (se && VAL_TAG(se) == XS_INT && VAL_INT(se) > 0) { close((int)VAL_INT(se)); map_take(a[0]->map, "_stderr_fd", xs_int(0)); }
    if (WIFEXITED(status)) return xs_int(WEXITSTATUS(status));
    return xs_int(-1);
}
#endif

static Value *native_process_spawn_kill(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_FALSE_VAL);
    Value *pidv = map_get(a[0]->map, "pid");
    if (!pidv || VAL_TAG(pidv) != XS_INT) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    int sig = SIGTERM;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) sig = (int)VAL_INT(a[1]);
    return (kill((pid_t)VAL_INT(pidv), sig) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

#if defined(__MINGW32__)
/* Windows spawn via _popen - captures stdout, uses pclose for wait */
static Value *native_process_spawn_stdout_read_win(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fpv = map_get(a[0]->map, "_fp");
    if (!fpv || VAL_TAG(fpv) != XS_INT || VAL_INT(fpv) == 0) return value_incref(XS_NULL_VAL);
    FILE *fp = (FILE*)(uintptr_t)VAL_INT(fpv);
    char buf[8192]; int total = 0;
    while (total < (int)sizeof(buf)-1) {
        int c = fgetc(fp);
        if (c == EOF) break;
        buf[total++] = (char)c;
    }
    buf[total] = '\0';
    return xs_str(buf);
}
static Value *native_process_spawn_wait_win(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return xs_int(-1);
    Value *fpv = map_get(a[0]->map, "_fp");
    if (!fpv || VAL_TAG(fpv) != XS_INT || VAL_INT(fpv) == 0) return xs_int(-1);
    FILE *fp = (FILE*)(uintptr_t)VAL_INT(fpv);
    int rc = _pclose(fp);
    map_take(a[0]->map, "_fp", xs_int(0));
    return xs_int(rc);
}
#endif

static Value *native_process_spawn(Interp *ig, Value **a, int n) {
    (void)ig;
#if defined(__MINGW32__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *cmd = a[0]->s;
    char cmdline[4096];
    if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
        int pos = snprintf(cmdline, sizeof(cmdline), "%s", cmd);
        for (int j = 0; j < a[1]->arr->len && pos < (int)sizeof(cmdline)-2; j++) {
            Value *av = a[1]->arr->items[j];
            pos += snprintf(cmdline+pos, sizeof(cmdline)-pos, " %s",
                           (VAL_TAG(av) == XS_STR) ? av->s : "");
        }
    } else {
        snprintf(cmdline, sizeof(cmdline), "%s", cmd);
    }
    FILE *fp = _popen(cmdline, "r");
    if (!fp) return value_incref(XS_NULL_VAL);
    XSMap *proc = map_new();
    map_take(proc, "pid", xs_int(0));
    map_take(proc, "_fp", xs_int((int64_t)(uintptr_t)fp));
    map_take(proc, "stdout_read", xs_native(native_process_spawn_stdout_read_win));
    map_take(proc, "stderr_read", xs_native(native_process_spawn_stdout_read_win));
    map_take(proc, "stdin_write", xs_native(native_process_spawn_stdin_write));
    map_take(proc, "wait", xs_native(native_process_spawn_wait_win));
    map_take(proc, "kill", xs_native(native_process_spawn_kill));
    return xs_module(proc);
#elif !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *cmd = a[0]->s;

    /* collect args */
    int nargs = 0;
    char **argv_list = NULL;
    if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
        nargs = a[1]->arr->len;
        argv_list = xs_malloc(sizeof(char*) * (nargs + 2));
        argv_list[0] = (char*)cmd;
        for (int j = 0; j < nargs; j++) {
            Value *av = a[1]->arr->items[j];
            argv_list[j+1] = (VAL_TAG(av) == XS_STR) ? av->s : "";
        }
        argv_list[nargs+1] = NULL;
    } else {
        argv_list = xs_malloc(sizeof(char*) * 4);
        argv_list[0] = "/bin/sh";
        argv_list[1] = "-c";
        argv_list[2] = (char*)cmd;
        argv_list[3] = NULL;
    }

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        free(argv_list);
        return value_incref(XS_NULL_VAL);
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(argv_list);
        return value_incref(XS_NULL_VAL);
    }
    if (pid == 0) {
        /* child */
        close(stdin_pipe[1]);  dup2(stdin_pipe[0], 0);  close(stdin_pipe[0]);
        close(stdout_pipe[0]); dup2(stdout_pipe[1], 1); close(stdout_pipe[1]);
        close(stderr_pipe[0]); dup2(stderr_pipe[1], 2); close(stderr_pipe[1]);
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY)
            execvp(cmd, argv_list);
        else
            execvp("/bin/sh", argv_list);
        _exit(127);
    }
    /* parent */
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    free(argv_list);

    XSMap *proc = map_new();
    map_take(proc, "pid", xs_int((int64_t)pid));
    map_take(proc, "_stdin_fd", xs_int((int64_t)stdin_pipe[1]));
    map_take(proc, "_stdout_fd", xs_int((int64_t)stdout_pipe[0]));
    map_take(proc, "_stderr_fd", xs_int((int64_t)stderr_pipe[0]));
    map_take(proc, "stdin_write", xs_native(native_process_spawn_stdin_write));
    map_take(proc, "stdout_read", xs_native(native_process_spawn_stdout_read));
    map_take(proc, "stderr_read", xs_native(native_process_spawn_stderr_read));
    map_take(proc, "wait", xs_native(native_process_spawn_wait));
    map_take(proc, "kill", xs_native(native_process_spawn_kill));
    return xs_module(proc);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* process.on_signal(sig_name, callback) */
#if !defined(__MINGW32__) && !defined(__wasi__)
static Interp *g_signal_interp = NULL;
static Value  *g_signal_handlers[32] = {0};

static void xs_signal_handler(int sig) {
    if (sig >= 0 && sig < 32 && g_signal_handlers[sig] && g_signal_interp) {
        Value *sv = xs_int(sig);
        Value *args[1] = { sv };
        Value *r = call_value(g_signal_interp, g_signal_handlers[sig], args, 1, "on_signal");
        if (r) value_decref(r);
        value_decref(sv);
    }
}
#endif

static Value *native_process_on_signal(Interp *ig, Value **a, int n) {
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[1]) != XS_FUNC && VAL_TAG(a[1]) != XS_NATIVE))
        return value_incref(XS_FALSE_VAL);
    int sig = -1;
    if (VAL_TAG(a[0]) == XS_INT) sig = (int)VAL_INT(a[0]);
    else if (VAL_TAG(a[0]) == XS_STR) {
        if (strcasecmp(a[0]->s, "SIGINT") == 0 || strcasecmp(a[0]->s, "INT") == 0) sig = SIGINT;
        else if (strcasecmp(a[0]->s, "SIGTERM") == 0 || strcasecmp(a[0]->s, "TERM") == 0) sig = SIGTERM;
        else if (strcasecmp(a[0]->s, "SIGHUP") == 0 || strcasecmp(a[0]->s, "HUP") == 0) sig = SIGHUP;
        else if (strcasecmp(a[0]->s, "SIGUSR1") == 0 || strcasecmp(a[0]->s, "USR1") == 0) sig = SIGUSR1;
        else if (strcasecmp(a[0]->s, "SIGUSR2") == 0 || strcasecmp(a[0]->s, "USR2") == 0) sig = SIGUSR2;
    }
    if (sig < 0 || sig >= 32) return value_incref(XS_FALSE_VAL);
    g_signal_interp = ig;
    if (g_signal_handlers[sig]) value_decref(g_signal_handlers[sig]);
    g_signal_handlers[sig] = value_incref(a[1]);
    signal(sig, xs_signal_handler);
    return value_incref(XS_TRUE_VAL);
#else
    (void)ig; (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* process.env(name) / process.env(name, value) */
static Value *native_process_env(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    if (n >= 2 && VAL_TAG(a[1]) == XS_STR) {
        setenv(a[0]->s, a[1]->s, 1);
        return value_incref(XS_TRUE_VAL);
    }
    const char *v = getenv(a[0]->s);
    return v ? xs_str(v) : value_incref(XS_NULL_VAL);
}

/* process.cwd() */
static Value *native_process_cwd(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return xs_str(buf);
    return xs_str(".");
}

/* process.exit(code) */
static Value *native_process_exit(Interp *ig, Value **a, int n) {
    int code = 0;
    if (n >= 1 && VAL_TAG(a[0]) == XS_INT) code = (int)VAL_INT(a[0]);
    trigger_fire_on_exit(ig);
    exit(code);
    return value_incref(XS_NULL_VAL);
}

Value *make_process_module(void) {
    XSMap *m=map_new();
    map_take(m,"pid",       xs_native(native_process_pid));
    map_take(m,"run",       xs_native(native_process_run));
    map_take(m,"exec",      xs_native(native_process_exec));
    map_take(m,"spawn",     xs_native(native_process_spawn));
    map_take(m,"on_signal", xs_native(native_process_on_signal));
    map_take(m,"env",       xs_native(native_process_env));
    map_take(m,"cwd",       xs_native(native_process_cwd));
    map_take(m,"exit",      xs_native(native_process_exit));
    return xs_module(m);
}

