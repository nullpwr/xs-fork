#include "dap/dap.h"
#include "core/xs.h"
#include "core/ast.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/value.h"
#include "core/env.h"
#include "runtime/interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DAP_MAX_BREAKPOINTS   256
#define DAP_MAX_STACK_FRAMES  128
#define DAP_MAX_SOURCE_LINES  65536
#define DAP_MAX_WATCHES       32
#define DAP_MAX_DATA_BPS      32
#define DAP_BUF_SMALL         1024
#define DAP_BUF_MEDIUM        4096
#define DAP_BUF_LARGE         16384

/* scope reference encoding:
   frame_id * 100 + kind
   kind: 1=locals, 2=globals, 3=closure, 4=watch */
#define SCOPE_LOCALS   1
#define SCOPE_GLOBALS  2
#define SCOPE_CLOSURE  3
#define SCOPE_WATCH    4



typedef struct {
    int  line;
    char condition[256];    /* XS expression, or empty = unconditional */
    char log_message[256];  /* logpoint message, or empty = normal bp */
    int  hit_count;         /* times this bp has been hit */
    int  hit_condition;     /* break only when hit_count reaches this (0 = always) */
    int  enabled;
} DapBreakpoint;

typedef struct {
    char          *source_path;
    DapBreakpoint  bps[DAP_MAX_BREAKPOINTS];
    int            n_bps;
} DapBreakpointSet;



typedef struct {
    int   id;
    char  name[256];
    char  source[512];
    int   line;
    int   col;
    Env  *env;       /* scope at this frame */
    Env  *closure;   /* closure env if this is a closure call, or NULL */
} DapStackFrame;



typedef enum {
    STEP_NONE = 0,
    STEP_CONTINUE,
    STEP_NEXT,
    STEP_IN,
    STEP_OUT,
} StepMode;



typedef struct {
    char *expression;
    int   id;
} WatchEntry;

typedef struct {
    char     *var_name;
    uint64_t  last_hash;
    int       enabled;
    int       id;
} DataBreakpoint;



typedef struct {
    DapBreakpointSet bp_set;
    char            *program_path;
    char            *source_text;
    int              n_source_lines;

    /* Execution state */
    Interp          *interp;
    Node            *program_ast;
    int              running;
    int              terminated;
    int              current_line;
    int              seq;

    /* Stepping */
    StepMode         step_mode;
    int              step_depth;

    /* Call stack */
    DapStackFrame    frames[DAP_MAX_STACK_FRAMES];
    int              n_frames;

    /* Statement execution index for stepping */
    int              stmt_index;
    int              stop_requested;
    int              stop_on_entry;

    /* Watch expressions */
    WatchEntry       watches[DAP_MAX_WATCHES];
    int              n_watches;
    int              next_watch_id;

    /* Exception breakpoints */
    int              break_on_throw;
    int              break_on_uncaught;

    /* Data breakpoints (watchpoints) */
    DataBreakpoint   data_bps[DAP_MAX_DATA_BPS];
    int              n_data_bps;
    int              next_data_bp_id;

    /* Last stop reason for exception reporting */
    const char      *last_exception_text;
} DapState;



static char *dap_json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p += 2;
        else p++;
    }
    size_t len = (size_t)(p - start);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            i++;
            switch (start[i]) {
                case 'n': result[j++] = '\n'; break;
                case 't': result[j++] = '\t'; break;
                case '"': result[j++] = '"'; break;
                case '\\': result[j++] = '\\'; break;
                default: result[j++] = start[i]; break;
            }
        } else {
            result[j++] = start[i];
        }
    }
    result[j] = '\0';
    return result;
}

static int dap_json_get_int(const char *json, const char *key) {
    if (!json || !key) return -1;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return atoi(p);
}



static int dap_json_get_bool(const char *json, const char *key) {
    if (!json || !key) return 0;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return (strncmp(p, "true", 4) == 0) ? 1 : 0;
}

/* extract a JSON string array from "key": ["a","b",...] */
static int dap_json_get_string_array(const char *json, const char *key,
                                     char **out, int max_out) {
    if (!json || !key) return 0;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_out) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p++;
        if (*p == '"') {
            p++;
            const char *s = p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) p += 2;
                else p++;
            }
            size_t len = (size_t)(p - s);
            out[count] = malloc(len + 1);
            if (out[count]) {
                memcpy(out[count], s, len);
                out[count][len] = '\0';
                count++;
            }
            if (*p == '"') p++;
        } else if (*p == ']') {
            break;
        } else {
            p++;
        }
    }
    return count;
}

static void json_escape_into(char *dst, size_t dstsz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        default:
            if (c < 0x20) {
                j += (size_t)snprintf(dst + j, dstsz - j, "\\u%04x", c);
            } else {
                dst[j++] = (char)c;
            }
            break;
        }
    }
    dst[j] = '\0';
}



static char *dap_read_message(void) {
    char header[512];
    int content_length = -1;
    while (fgets(header, sizeof(header), stdin)) {
        size_t len = strlen(header);
        while (len > 0 && (header[len-1] == '\r' || header[len-1] == '\n'))
            header[--len] = '\0';
        if (len == 0) break;
        if (strncmp(header, "Content-Length:", 15) == 0)
            content_length = atoi(header + 15);
    }
    if (content_length <= 0) return NULL;
    char *body = malloc((size_t)content_length + 1);
    if (!body) return NULL;
    size_t nread = fread(body, 1, (size_t)content_length, stdin);
    body[nread] = '\0';
    if ((int)nread < content_length) { free(body); return NULL; }
    return body;
}

static void dap_write_message(const char *json) {
    int len = (int)strlen(json);
    fprintf(stdout, "Content-Length: %d\r\n\r\n%s", len, json);
    fflush(stdout);
}



static void dap_send_response(DapState *st, int request_seq, const char *command, const char *body_json) {
    size_t blen = body_json ? strlen(body_json) : 2;
    char *buf = malloc(blen + 512);
    if (!buf) return;
    st->seq++;
    snprintf(buf, blen + 512,
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
        "\"success\":true,\"command\":\"%s\",\"body\":%s}",
        st->seq, request_seq, command, body_json ? body_json : "{}");
    dap_write_message(buf);
    free(buf);
}

static void dap_send_error_response(DapState *st, int request_seq, const char *command, const char *message) {
    char escaped[512];
    json_escape_into(escaped, sizeof(escaped), message);
    char buf[DAP_BUF_MEDIUM];
    st->seq++;
    snprintf(buf, sizeof(buf),
        "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
        "\"success\":false,\"command\":\"%s\",\"message\":\"%s\"}",
        st->seq, request_seq, command, escaped);
    dap_write_message(buf);
}

static void dap_send_event(DapState *st, const char *event, const char *body_json) {
    size_t blen = body_json ? strlen(body_json) : 2;
    char *buf = malloc(blen + 256);
    if (!buf) return;
    st->seq++;
    sprintf(buf,
        "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}",
        st->seq, event, body_json ? body_json : "{}");
    dap_write_message(buf);
    free(buf);
}

/* send an output event (for logpoints and debug console) */
static void dap_send_output(DapState *st, const char *category, const char *text) {
    char escaped[DAP_BUF_SMALL];
    json_escape_into(escaped, sizeof(escaped), text);
    char buf[DAP_BUF_MEDIUM];
    snprintf(buf, sizeof(buf),
        "{\"category\":\"%s\",\"output\":\"%s\\n\"}",
        category, escaped);
    dap_send_event(st, "output", buf);
}



static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)(sz + 1));
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    fclose(f);
    return buf;
}

static int count_lines(const char *text) {
    if (!text) return 0;
    int n = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') n++;
    }
    return n;
}

static int load_program(DapState *st) {
    if (!st->program_path) return -1;

    free(st->source_text);
    st->source_text = read_file(st->program_path);
    if (!st->source_text) return -1;

    st->n_source_lines = count_lines(st->source_text);

    Lexer lex;
    lexer_init(&lex, st->source_text, st->program_path);
    TokenArray ta = lexer_tokenize(&lex);

    Parser parser;
    parser_init(&parser, &ta, st->program_path);
    Node *prog = parser_parse(&parser);
    token_array_free(&ta);

    if (!prog || parser.had_error) {
        if (prog) node_free(prog);
        return -1;
    }

    if (st->program_ast) node_free(st->program_ast);
    st->program_ast = prog;

    if (st->interp) interp_free(st->interp);
    st->interp = interp_new(st->program_path);

    return 0;
}

/* check if a source line has executable code (not blank, not comment-only) */
static int line_is_executable(const char *source_text, int line) {
    if (!source_text || line < 1) return 0;
    const char *p = source_text;
    int cur = 1;
    while (*p && cur < line) {
        if (*p == '\n') cur++;
        p++;
    }
    /* p now points to start of 'line' */
    while (*p && *p != '\n') {
        if (*p == '/' && *(p+1) == '/') return 0; /* rest is comment */
        if (*p != ' ' && *p != '\t' && *p != '\r') return 1;
        p++;
    }
    return 0; /* blank line */
}

/* find nearest executable line at or after 'line' */
static int find_executable_line(const char *source_text, int line, int max_lines) {
    for (int l = line; l <= max_lines; l++) {
        if (line_is_executable(source_text, l)) return l;
    }
    /* try backwards */
    for (int l = line - 1; l >= 1; l--) {
        if (line_is_executable(source_text, l)) return l;
    }
    return line; /* give up, return original */
}



static void push_frame(DapState *st, const char *name, const char *source,
                        int line, int col, Env *env, Env *closure) {
    if (st->n_frames >= DAP_MAX_STACK_FRAMES) return;
    DapStackFrame *f = &st->frames[st->n_frames];
    f->id = st->n_frames + 1;
    snprintf(f->name, sizeof(f->name), "%s", name ? name : "<anonymous>");
    snprintf(f->source, sizeof(f->source), "%s", source ? source : "<unknown>");
    f->line = line;
    f->col = col;
    f->env = env;
    f->closure = closure;
    st->n_frames++;
}

static void pop_frame(DapState *st) {
    if (st->n_frames > 0) st->n_frames--;
}

static void update_top_frame(DapState *st, int line, int col) {
    if (st->n_frames > 0) {
        st->frames[st->n_frames - 1].line = line;
        st->frames[st->n_frames - 1].col = col;
    }
}

/* simple hash for data breakpoint change detection */
static uint64_t value_hash_simple(Value *v) {
    if (!v) return 0;
    uint64_t h = (uint64_t)v->tag * 2654435761ULL;
    switch (v->tag) {
    case XS_INT:   h ^= (uint64_t)v->i; break;
    case XS_FLOAT: { uint64_t bits; memcpy(&bits, &v->f, sizeof(bits)); h ^= bits; } break;
    case XS_BOOL:  h ^= (uint64_t)v->i; break;
    case XS_STR:   if (v->s) { for (const char *p = v->s; *p; p++) h = h * 31 + (unsigned char)*p; } break;
    case XS_NULL:  break;
    default: h ^= (uint64_t)(uintptr_t)v; break; /* pointer identity for complex types */
    }
    return h;
}

/* evaluate an expression string in the current interpreter scope.
   Returns a new ref (caller must decref), or NULL on error. */
static Value *eval_expression(DapState *st, const char *expr_str) {
    if (!st->interp || !expr_str || !expr_str[0]) return NULL;

    Lexer lex;
    lexer_init(&lex, expr_str, "<eval>");
    TokenArray ta = lexer_tokenize(&lex);

    Parser p;
    parser_init(&p, &ta, "<eval>");
    Node *prog = parser_parse(&p);
    token_array_free(&ta);

    if (!prog || p.had_error) {
        if (prog) node_free(prog);
        return NULL;
    }

    Value *result = NULL;
    if (prog->tag == NODE_PROGRAM && prog->program.stmts.len == 1) {
        Node *stmt = prog->program.stmts.items[0];
        if (stmt->tag == NODE_EXPR_STMT && stmt->expr_stmt.expr) {
            result = interp_eval(st->interp, stmt->expr_stmt.expr);
            if (result) value_incref(result);
        } else {
            interp_exec(st->interp, stmt);
            result = st->interp->cf.value;
            if (result) value_incref(result);
        }
    } else {
        interp_run(st->interp, prog);
        result = st->interp->cf.value;
        if (result) value_incref(result);
    }
    node_free(prog);

    /* clear any error state from the eval */
    if (st->interp->cf.signal == CF_ERROR || st->interp->cf.signal == CF_PANIC) {
        if (result) { value_decref(result); result = NULL; }
    }
    if (st->interp->cf.value) {
        if (!result || result != st->interp->cf.value)
            value_decref(st->interp->cf.value);
        st->interp->cf.value = NULL;
    }
    st->interp->cf.signal = 0;

    return result;
}

/* interpolate {expr} in a logpoint message string */
static void interpolate_log_message(DapState *st, const char *msg, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; msg[i] && j + 1 < outsz; i++) {
        if (msg[i] == '{') {
            /* find closing brace */
            const char *end = strchr(msg + i + 1, '}');
            if (end) {
                size_t elen = (size_t)(end - (msg + i + 1));
                char expr_buf[256];
                if (elen >= sizeof(expr_buf)) elen = sizeof(expr_buf) - 1;
                memcpy(expr_buf, msg + i + 1, elen);
                expr_buf[elen] = '\0';

                Value *val = eval_expression(st, expr_buf);
                char *repr = val ? value_repr(val) : NULL;
                const char *s = repr ? repr : "null";
                while (*s && j + 1 < outsz) out[j++] = *s++;
                free(repr);
                if (val) value_decref(val);

                i = (size_t)(end - msg); /* skip past } */
                continue;
            }
        }
        out[j++] = msg[i];
    }
    out[j] = '\0';
}

/* check data breakpoints - return 1 if any variable changed */
static int check_data_breakpoints(DapState *st) {
    if (!st->interp || st->n_data_bps == 0) return 0;

    for (int i = 0; i < st->n_data_bps; i++) {
        DataBreakpoint *db = &st->data_bps[i];
        if (!db->enabled || !db->var_name) continue;

        Value *val = env_get(st->interp->env, db->var_name);
        if (!val) val = env_get(st->interp->globals, db->var_name);

        uint64_t h = value_hash_simple(val);
        if (h != db->last_hash) {
            db->last_hash = h;
            return 1;
        }
    }
    return 0;
}



static int check_breakpoint(DapState *st, int line) {
    for (int i = 0; i < st->bp_set.n_bps; i++) {
        DapBreakpoint *bp = &st->bp_set.bps[i];
        if (bp->line != line || !bp->enabled) continue;

        /* increment hit count */
        bp->hit_count++;

        /* check hit condition if set */
        if (bp->hit_condition > 0 && bp->hit_count < bp->hit_condition) continue;

        /* check condition if set */
        if (bp->condition[0] != '\0' && st->interp) {
            Value *cv = eval_expression(st, bp->condition);
            int truthy = cv ? value_truthy(cv) : 0;
            if (cv) value_decref(cv);
            if (!truthy) continue;
        }

        /* logpoint: print message and continue without breaking */
        if (bp->log_message[0] != '\0') {
            char interpolated[DAP_BUF_SMALL];
            interpolate_log_message(st, bp->log_message, interpolated, sizeof(interpolated));
            dap_send_output(st, "console", interpolated);
            return 0; /* don't break */
        }

        return 1;
    }
    return 0;
}

/* step-aware execution */

static int exec_stmt_with_debug(DapState *st, Node *stmt) {
    if (!stmt) return 0;

    int line = stmt->span.line;
    int col  = stmt->span.col > 0 ? stmt->span.col : 1;

    st->current_line = line;
    update_top_frame(st, line, col);

    int should_stop = 0;

    if (check_breakpoint(st, line))
        should_stop = 1;

    /* check data breakpoints */
    if (!should_stop && check_data_breakpoints(st))
        should_stop = 1;

    switch (st->step_mode) {
    case STEP_NEXT:
        if (st->n_frames <= st->step_depth)
            should_stop = 1;
        break;
    case STEP_IN:
        should_stop = 1;
        break;
    case STEP_OUT:
        if (st->n_frames < st->step_depth)
            should_stop = 1;
        break;
    default:
        break;
    }

    if (should_stop) {
        st->stop_requested = 1;
        st->step_mode = STEP_NONE;
        return 1;
    }

    /* Execute the statement */
    interp_exec(st->interp, stmt);

    /* check for exception breakpoints */
    if (st->interp->cf.signal == CF_THROW && st->break_on_throw) {
        Value *err = st->interp->cf.value;
        char *repr = err ? value_repr(err) : NULL;
        st->last_exception_text = repr ? repr : "exception";
        st->stop_requested = 1;
        st->step_mode = STEP_NONE;
        /* don't clear the signal - let the caller handle it */
        return 1;
    }

    return 0;
}

/*
 * Run program statements with debug support.
 * Returns the stop reason string, or NULL if completed.
 */
static const char *run_program_debug(DapState *st, int from_stmt) {
    if (!st->program_ast || st->program_ast->tag != NODE_PROGRAM) return NULL;

    NodeList *stmts = &st->program_ast->program.stmts;

    for (int i = from_stmt; i < stmts->len; i++) {
        Node *stmt = stmts->items[i];
        st->stmt_index = i;

        if (exec_stmt_with_debug(st, stmt)) {
            if (st->interp->cf.signal == CF_THROW)
                return "exception";
            return st->stop_requested ? "breakpoint" : "step";
        }

        /* Check for runtime errors */
        if (st->interp->cf.signal == CF_ERROR || st->interp->cf.signal == CF_PANIC) {
            Value *err = st->interp->cf.value;
            {
                char body[DAP_BUF_MEDIUM];
                char escaped[DAP_BUF_SMALL];
                const char *emsg = (err && err->tag == XS_STR) ? err->s : "runtime error";
                json_escape_into(escaped, sizeof(escaped), emsg);
                snprintf(body, sizeof(body),
                    "{\"reason\":\"exception\",\"description\":\"%s\","
                    "\"text\":\"%s\","
                    "\"threadId\":1,\"allThreadsStopped\":true}",
                    escaped, escaped);
                dap_send_event(st, "stopped", body);
            }
            if (st->interp->cf.value) {
                value_decref(st->interp->cf.value);
                st->interp->cf.value = NULL;
            }
            st->interp->cf.signal = 0;
            return "exception";
        }

        if (st->interp->cf.signal == CF_RETURN) {
            if (st->interp->cf.value) {
                value_decref(st->interp->cf.value);
                st->interp->cf.value = NULL;
            }
            st->interp->cf.signal = 0;
            return NULL;
        }
    }

    return NULL;
}



static void dap_parse_breakpoints(DapState *st, const char *msg) {
    st->bp_set.n_bps = 0;

    const char *bp = strstr(msg, "\"breakpoints\"");
    if (!bp) return;

    const char *p = bp;
    while (*p && st->bp_set.n_bps < DAP_MAX_BREAKPOINTS) {
        const char *line_key = strstr(p, "\"line\"");
        if (!line_key) break;
        line_key += 6;
        while (*line_key == ' ' || *line_key == ':' || *line_key == '\t') line_key++;
        int line = atoi(line_key);

        if (line > 0) {
            DapBreakpoint *b = &st->bp_set.bps[st->bp_set.n_bps];
            b->line = line;
            b->condition[0] = '\0';
            b->log_message[0] = '\0';
            b->hit_count = 0;
            b->hit_condition = 0;
            b->enabled = 1;

            const char *next_line = strstr(line_key + 1, "\"line\"");

            /* Look for condition */
            const char *cond_key = strstr(line_key, "\"condition\"");
            if (cond_key && (!next_line || cond_key < next_line)) {
                const char *q = cond_key + 11;
                while (*q == ' ' || *q == ':' || *q == '\t') q++;
                if (*q == '"') {
                    q++;
                    const char *cs = q;
                    while (*q && *q != '"') {
                        if (*q == '\\' && *(q+1)) q += 2;
                        else q++;
                    }
                    size_t clen = (size_t)(q - cs);
                    if (clen >= sizeof(b->condition)) clen = sizeof(b->condition) - 1;
                    memcpy(b->condition, cs, clen);
                    b->condition[clen] = '\0';
                }
            }

            /* Look for logMessage */
            const char *log_key = strstr(line_key, "\"logMessage\"");
            if (log_key && (!next_line || log_key < next_line)) {
                const char *q = log_key + 12;
                while (*q == ' ' || *q == ':' || *q == '\t') q++;
                if (*q == '"') {
                    q++;
                    const char *cs = q;
                    while (*q && *q != '"') {
                        if (*q == '\\' && *(q+1)) q += 2;
                        else q++;
                    }
                    size_t clen = (size_t)(q - cs);
                    if (clen >= sizeof(b->log_message)) clen = sizeof(b->log_message) - 1;
                    memcpy(b->log_message, cs, clen);
                    b->log_message[clen] = '\0';
                }
            }

            /* Look for hitCondition */
            const char *hit_key = strstr(line_key, "\"hitCondition\"");
            if (hit_key && (!next_line || hit_key < next_line)) {
                const char *q = hit_key + 14;
                while (*q == ' ' || *q == ':' || *q == '\t') q++;
                if (*q == '"') {
                    q++;
                    b->hit_condition = atoi(q);
                } else {
                    b->hit_condition = atoi(q);
                }
            }

            st->bp_set.n_bps++;
        }
        p = line_key + 1;
    }
}



static const char *value_type_name(Value *v) {
    if (!v) return "null";
    switch (v->tag) {
    case XS_NULL:       return "null";
    case XS_BOOL:       return "Bool";
    case XS_INT:        return "Int";
    case XS_FLOAT:      return "Float";
    case XS_STR:        return "String";
    case XS_CHAR:       return "Char";
    case XS_ARRAY:      return "Array";
    case XS_MAP:        return "Map";
    case XS_TUPLE:      return "Tuple";
    case XS_FUNC:       return "Fn";
    case XS_NATIVE:     return "NativeFn";
    case XS_STRUCT_VAL: return "Struct";
    case XS_ENUM_VAL:   return "Enum";
    case XS_RANGE:      return "Range";
    case XS_MODULE:     return "Module";
    case XS_CLASS_VAL:  return "Class";
    case XS_INST:       return "Instance";
    default:            return "unknown";
    }
}

static int build_variables_json(Env *env, char *buf, size_t bufsz, int ref) {
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "{\"variables\":[");

    if (env) {
        int first = 1;
        Env *e = env;
        int kind = ref % 100;
        int max_depth = (kind == SCOPE_GLOBALS) ? 100 : 1;
        int depth = 0;

        while (e && depth < max_depth) {
            for (int i = 0; i < e->len; i++) {
                Binding *b = &e->bindings[i];
                if (!b->name) continue;

                if (!first) off += snprintf(buf + off, bufsz - (size_t)off, ",");
                first = 0;

                char escaped_name[256];
                json_escape_into(escaped_name, sizeof(escaped_name), b->name);

                char *repr = b->value ? value_repr(b->value) : NULL;
                char escaped_val[DAP_BUF_SMALL];
                json_escape_into(escaped_val, sizeof(escaped_val),
                                 repr ? repr : "null");
                free(repr);

                const char *type_str = value_type_name(b->value);

                int var_ref = 0;
                if (b->value && (b->value->tag == XS_ARRAY || b->value->tag == XS_MAP ||
                                 b->value->tag == XS_TUPLE || b->value->tag == XS_STRUCT_VAL ||
                                 b->value->tag == XS_INST)) {
                    var_ref = 1000 + i + depth * 100;
                }

                off += snprintf(buf + off, bufsz - (size_t)off,
                    "{\"name\":\"%s\",\"value\":\"%s\",\"type\":\"%s\","
                    "\"variablesReference\":%d}",
                    escaped_name, escaped_val, type_str, var_ref);

                if (off + 256 >= (int)bufsz) break;
            }
            e = e->parent;
            depth++;
        }
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]}");
    return off;
}

/* build watch expressions as variables */
static int build_watch_variables_json(DapState *st, char *buf, size_t bufsz) {
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "{\"variables\":[");

    int first = 1;
    for (int i = 0; i < st->n_watches; i++) {
        WatchEntry *w = &st->watches[i];
        if (!w->expression) continue;

        if (!first) off += snprintf(buf + off, bufsz - (size_t)off, ",");
        first = 0;

        char escaped_name[256];
        json_escape_into(escaped_name, sizeof(escaped_name), w->expression);

        Value *val = eval_expression(st, w->expression);
        char *repr = val ? value_repr(val) : NULL;
        const char *display = repr ? repr : "<error>";
        const char *type_str = val ? value_type_name(val) : "error";

        char escaped_val[DAP_BUF_SMALL];
        json_escape_into(escaped_val, sizeof(escaped_val), display);
        free(repr);
        if (val) value_decref(val);

        off += snprintf(buf + off, bufsz - (size_t)off,
            "{\"name\":\"%s\",\"value\":\"%s\",\"type\":\"%s\","
            "\"variablesReference\":0}",
            escaped_name, escaped_val, type_str);

        if (off + 256 >= (int)bufsz) break;
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]}");
    return off;
}

/* collect variable names from an env for completions */
static int collect_var_names(Env *env, char **names, int max_names) {
    int count = 0;
    Env *e = env;
    while (e && count < max_names) {
        for (int i = 0; i < e->len && count < max_names; i++) {
            if (!e->bindings[i].name) continue;
            /* deduplicate */
            int dup = 0;
            for (int j = 0; j < count; j++) {
                if (strcmp(names[j], e->bindings[i].name) == 0) { dup = 1; break; }
            }
            if (!dup) names[count++] = e->bindings[i].name;
        }
        e = e->parent;
    }
    return count;
}

/* handlers */

static void dap_handle_initialize(DapState *st, int req_seq) {
    const char *caps =
        "{"
        "\"supportsConfigurationDoneRequest\":true,"
        "\"supportsFunctionBreakpoints\":false,"
        "\"supportsConditionalBreakpoints\":true,"
        "\"supportsHitConditionalBreakpoints\":true,"
        "\"supportsLogPoints\":true,"
        "\"supportsStepBack\":false,"
        "\"supportsSetVariable\":false,"
        "\"supportsRestartFrame\":false,"
        "\"supportsGotoTargetsRequest\":false,"
        "\"supportsStepInTargetsRequest\":false,"
        "\"supportsCompletionsRequest\":true,"
        "\"supportsModulesRequest\":false,"
        "\"supportsExceptionOptions\":true,"
        "\"supportsExceptionInfoRequest\":true,"
        "\"supportsEvaluateForHovers\":true,"
        "\"supportsDataBreakpoints\":true,"
        "\"supportsTerminateRequest\":true,"
        "\"exceptionBreakpointFilters\":["
        "{\"filter\":\"all\",\"label\":\"All Exceptions\",\"default\":false},"
        "{\"filter\":\"uncaught\",\"label\":\"Uncaught Exceptions\",\"default\":true}"
        "]"
        "}";
    dap_send_response(st, req_seq, "initialize", caps);
    dap_send_event(st, "initialized", "{}");
}

static void dap_handle_launch(DapState *st, int req_seq, const char *msg) {
    char *program = dap_json_get_string(msg, "program");
    if (program) {
        free(st->program_path);
        st->program_path = program;
    }

    st->running = 0;
    st->terminated = 0;
    st->current_line = 1;
    st->stmt_index = 0;
    st->step_mode = STEP_NONE;
    st->n_frames = 0;
    st->stop_on_entry = dap_json_get_bool(msg, "stopOnEntry");

    if (load_program(st) != 0) {
        dap_send_error_response(st, req_seq, "launch",
            st->program_path ? "Failed to load program" : "No program specified");
        return;
    }

    /* Push initial frame */
    push_frame(st, "main", st->program_path, 1, 1,
               st->interp ? st->interp->globals : NULL, NULL);

    dap_send_response(st, req_seq, "launch", "{}");
}

static void dap_handle_set_breakpoints(DapState *st, int req_seq, const char *msg) {
    char *source_path = NULL;
    const char *src = strstr(msg, "\"source\"");
    if (src) source_path = dap_json_get_string(src, "path");

    free(st->bp_set.source_path);
    st->bp_set.source_path = source_path;
    dap_parse_breakpoints(st, msg);

    /* Verify breakpoints - adjust lines to nearest executable code */
    char buf[DAP_BUF_MEDIUM];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"breakpoints\":[");
    for (int i = 0; i < st->bp_set.n_bps; i++) {
        if (i > 0) off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");

        int orig_line = st->bp_set.bps[i].line;
        int adj_line = find_executable_line(st->source_text, orig_line, st->n_source_lines);
        int verified = (adj_line == orig_line || line_is_executable(st->source_text, adj_line));

        /* update the breakpoint to the adjusted line */
        st->bp_set.bps[i].line = adj_line;

        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "{\"id\":%d,\"verified\":%s,\"line\":%d}",
            i + 1, verified ? "true" : "false", adj_line);
    }
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
    dap_send_response(st, req_seq, "setBreakpoints", buf);
}

static void dap_handle_set_exception_breakpoints(DapState *st, int req_seq, const char *msg) {
    st->break_on_throw = 0;
    st->break_on_uncaught = 0;

    /* parse "filters": ["all", "uncaught"] */
    char *filters[8];
    int nf = dap_json_get_string_array(msg, "filters", filters, 8);
    for (int i = 0; i < nf; i++) {
        if (strcmp(filters[i], "all") == 0) st->break_on_throw = 1;
        if (strcmp(filters[i], "uncaught") == 0) st->break_on_uncaught = 1;
        free(filters[i]);
    }

    dap_send_response(st, req_seq, "setExceptionBreakpoints", "{\"breakpoints\":[]}");
}

static void dap_handle_exception_info(DapState *st, int req_seq) {
    const char *desc = st->last_exception_text ? st->last_exception_text : "exception";
    char escaped[DAP_BUF_SMALL];
    json_escape_into(escaped, sizeof(escaped), desc);
    char buf[DAP_BUF_MEDIUM];
    snprintf(buf, sizeof(buf),
        "{\"exceptionId\":\"exception\",\"description\":\"%s\","
        "\"breakMode\":\"always\"}", escaped);
    dap_send_response(st, req_seq, "exceptionInfo", buf);
}

static void dap_handle_configuration_done(DapState *st, int req_seq) {
    st->running = 1;
    st->stop_requested = 0;
    dap_send_response(st, req_seq, "configurationDone", "{}");

    if (st->stop_on_entry) {
        st->step_mode = STEP_NEXT;
        st->step_depth = 0;
        dap_send_event(st, "stopped",
            "{\"reason\":\"entry\",\"threadId\":1,\"allThreadsStopped\":true}");
        st->running = 0;
        return;
    }

    st->step_mode = STEP_CONTINUE;
    const char *reason = run_program_debug(st, 0);

    if (st->stop_requested) {
        char body[DAP_BUF_SMALL];
        snprintf(body, sizeof(body),
            "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
            reason ? reason : "breakpoint");
        dap_send_event(st, "stopped", body);
        st->running = 0;
    } else if (!reason || strcmp(reason, "exception") != 0) {
        dap_send_event(st, "terminated", "{}");
        st->terminated = 1;
        st->running = 0;
    }
}

static void dap_handle_threads(DapState *st, int req_seq) {
    dap_send_response(st, req_seq, "threads",
        "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
}

static void dap_handle_stack_trace(DapState *st, int req_seq) {
    char *buf = malloc(DAP_BUF_LARGE);
    if (!buf) { dap_send_response(st, req_seq, "stackTrace", "{\"stackFrames\":[],\"totalFrames\":0}"); return; }

    int off = 0;
    off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off, "{\"stackFrames\":[");

    for (int i = st->n_frames - 1; i >= 0; i--) {
        DapStackFrame *f = &st->frames[i];
        if (i < st->n_frames - 1) off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off, ",");

        char escaped_name[512];
        json_escape_into(escaped_name, sizeof(escaped_name), f->name);
        char escaped_source[1024];
        json_escape_into(escaped_source, sizeof(escaped_source), f->source);

        const char *basename = f->source;
        const char *slash = strrchr(f->source, '/');
        if (slash) basename = slash + 1;
        char escaped_basename[256];
        json_escape_into(escaped_basename, sizeof(escaped_basename), basename);

        off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off,
            "{"
            "\"id\":%d,"
            "\"name\":\"%s\","
            "\"source\":{\"name\":\"%s\",\"path\":\"%s\"},"
            "\"line\":%d,"
            "\"column\":%d"
            "}",
            f->id, escaped_name,
            escaped_basename, escaped_source,
            f->line > 0 ? f->line : 1,
            f->col > 0 ? f->col : 1);
    }

    off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off,
        "],\"totalFrames\":%d}", st->n_frames);

    dap_send_response(st, req_seq, "stackTrace", buf);
    free(buf);
}

static void dap_handle_scopes(DapState *st, int req_seq, const char *msg) {
    int frame_id = dap_json_get_int(msg, "frameId");

    int locals_ref  = frame_id * 100 + SCOPE_LOCALS;
    int globals_ref = frame_id * 100 + SCOPE_GLOBALS;
    int closure_ref = frame_id * 100 + SCOPE_CLOSURE;
    int watch_ref   = frame_id * 100 + SCOPE_WATCH;

    /* check if this frame has closure variables */
    Env *closure_env = NULL;
    for (int i = 0; i < st->n_frames; i++) {
        if (st->frames[i].id == frame_id) {
            closure_env = st->frames[i].closure;
            break;
        }
    }

    char buf[DAP_BUF_SMALL];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"scopes\":[");
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
        "{\"name\":\"Locals\",\"variablesReference\":%d,\"expensive\":false}", locals_ref);

    if (closure_env) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            ",{\"name\":\"Closure\",\"variablesReference\":%d,\"expensive\":false}", closure_ref);
    }

    if (st->n_watches > 0) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            ",{\"name\":\"Watch\",\"variablesReference\":%d,\"expensive\":false}", watch_ref);
    }

    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
        ",{\"name\":\"Globals\",\"variablesReference\":%d,\"expensive\":false}", globals_ref);

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
    dap_send_response(st, req_seq, "scopes", buf);
}

static void dap_handle_variables(DapState *st, int req_seq, const char *msg) {
    int var_ref = dap_json_get_int(msg, "variablesReference");

    char *buf = malloc(DAP_BUF_LARGE);
    if (!buf) { dap_send_response(st, req_seq, "variables", "{\"variables\":[]}"); return; }

    if (!st->interp) {
        snprintf(buf, DAP_BUF_LARGE, "{\"variables\":[]}");
    } else {
        int kind     = var_ref % 100;
        int frame_id = var_ref / 100;

        if (kind == SCOPE_WATCH) {
            build_watch_variables_json(st, buf, DAP_BUF_LARGE);
        } else if (kind == SCOPE_GLOBALS) {
            build_variables_json(st->interp->globals, buf, DAP_BUF_LARGE, var_ref);
        } else if (kind == SCOPE_CLOSURE) {
            /* find the frame's closure env */
            Env *closure_env = NULL;
            for (int i = 0; i < st->n_frames; i++) {
                if (st->frames[i].id == frame_id) {
                    closure_env = st->frames[i].closure;
                    break;
                }
            }
            if (closure_env) {
                build_variables_json(closure_env, buf, DAP_BUF_LARGE, var_ref);
            } else {
                snprintf(buf, DAP_BUF_LARGE, "{\"variables\":[]}");
            }
        } else if (kind == SCOPE_LOCALS) {
            Env *frame_env = NULL;
            for (int i = 0; i < st->n_frames; i++) {
                if (st->frames[i].id == frame_id) {
                    frame_env = st->frames[i].env;
                    break;
                }
            }
            if (frame_env) {
                build_variables_json(frame_env, buf, DAP_BUF_LARGE, var_ref);
            } else {
                build_variables_json(st->interp->env, buf, DAP_BUF_LARGE, var_ref);
            }
        } else {
            snprintf(buf, DAP_BUF_LARGE, "{\"variables\":[]}");
        }
    }

    dap_send_response(st, req_seq, "variables", buf);
    free(buf);
}

static void dap_handle_continue(DapState *st, int req_seq) {
    dap_send_response(st, req_seq, "continue", "{\"allThreadsContinued\":true}");

    if (st->terminated) {
        dap_send_event(st, "terminated", "{}");
        return;
    }

    st->stop_requested = 0;
    st->step_mode = STEP_CONTINUE;
    const char *reason = run_program_debug(st, st->stmt_index + 1);

    if (st->stop_requested) {
        char body[DAP_BUF_SMALL];
        snprintf(body, sizeof(body),
            "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
            reason ? reason : "breakpoint");
        dap_send_event(st, "stopped", body);
    } else if (!reason || strcmp(reason, "exception") != 0) {
        dap_send_event(st, "terminated", "{}");
        st->terminated = 1;
    }
}

static void dap_handle_next(DapState *st, int req_seq) {
    dap_send_response(st, req_seq, "next", "{}");

    if (st->terminated) {
        dap_send_event(st, "terminated", "{}");
        return;
    }

    st->stop_requested = 0;
    st->step_mode = STEP_NEXT;
    st->step_depth = st->n_frames;
    const char *reason = run_program_debug(st, st->stmt_index + 1);

    if (st->stop_requested) {
        dap_send_event(st, "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
    } else if (!reason || strcmp(reason, "exception") != 0) {
        dap_send_event(st, "terminated", "{}");
        st->terminated = 1;
    }
}

static void dap_handle_step_in(DapState *st, int req_seq) {
    dap_send_response(st, req_seq, "stepIn", "{}");

    if (st->terminated) {
        dap_send_event(st, "terminated", "{}");
        return;
    }

    st->stop_requested = 0;
    st->step_mode = STEP_IN;
    st->step_depth = st->n_frames;
    const char *reason = run_program_debug(st, st->stmt_index + 1);

    if (st->stop_requested) {
        dap_send_event(st, "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
    } else if (!reason || strcmp(reason, "exception") != 0) {
        dap_send_event(st, "terminated", "{}");
        st->terminated = 1;
    }
}

static void dap_handle_step_out(DapState *st, int req_seq) {
    dap_send_response(st, req_seq, "stepOut", "{}");

    if (st->terminated) {
        pop_frame(st);
        dap_send_event(st, "terminated", "{}");
        return;
    }

    st->stop_requested = 0;
    st->step_mode = STEP_OUT;
    st->step_depth = st->n_frames;
    const char *reason = run_program_debug(st, st->stmt_index + 1);

    if (st->stop_requested) {
        dap_send_event(st, "stopped",
            "{\"reason\":\"step\",\"threadId\":1,\"allThreadsStopped\":true}");
    } else if (!reason || strcmp(reason, "exception") != 0) {
        dap_send_event(st, "terminated", "{}");
        st->terminated = 1;
    }
}



static void dap_handle_evaluate(DapState *st, int req_seq, const char *msg) {
    char *expression = dap_json_get_string(msg, "expression");
    if (!expression || strlen(expression) == 0) {
        dap_send_error_response(st, req_seq, "evaluate", "Empty expression");
        free(expression);
        return;
    }

    if (!st->interp) {
        dap_send_error_response(st, req_seq, "evaluate", "No interpreter active");
        free(expression);
        return;
    }

    char *context = dap_json_get_string(msg, "context");
    int is_watch = (context && strcmp(context, "watch") == 0);
    int is_hover = (context && strcmp(context, "hover") == 0);
    (void)is_watch; /* watch context: safe eval only */
    (void)is_hover;
    free(context);

    /* Parse the expression */
    Lexer lex;
    lexer_init(&lex, expression, "<eval>");
    TokenArray ta = lexer_tokenize(&lex);

    Parser parser;
    parser_init(&parser, &ta, "<eval>");
    Node *prog = parser_parse(&parser);
    token_array_free(&ta);

    if (!prog || parser.had_error) {
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), "Parse error: %s",
                 parser.error.msg[0] ? parser.error.msg : "syntax error");
        dap_send_error_response(st, req_seq, "evaluate", err_buf);
        if (prog) node_free(prog);
        free(expression);
        return;
    }

    Value *result = NULL;
    if (prog->tag == NODE_PROGRAM && prog->program.stmts.len == 1) {
        Node *stmt = prog->program.stmts.items[0];
        if (stmt->tag == NODE_EXPR_STMT && stmt->expr_stmt.expr) {
            result = interp_eval(st->interp, stmt->expr_stmt.expr);
            if (result) value_incref(result);
        } else {
            interp_exec(st->interp, stmt);
        }
    } else {
        interp_run(st->interp, prog);
    }
    node_free(prog);

    if (st->interp->cf.signal == CF_ERROR || st->interp->cf.signal == CF_PANIC) {
        Value *err = st->interp->cf.value;
        char err_msg[DAP_BUF_SMALL];
        if (err && err->tag == XS_STR) {
            snprintf(err_msg, sizeof(err_msg), "Error: %s", err->s);
        } else {
            char *repr = err ? value_repr(err) : NULL;
            snprintf(err_msg, sizeof(err_msg), "Error: %s", repr ? repr : "unknown");
            free(repr);
        }
        if (st->interp->cf.value) {
            value_decref(st->interp->cf.value);
            st->interp->cf.value = NULL;
        }
        st->interp->cf.signal = 0;
        dap_send_error_response(st, req_seq, "evaluate", err_msg);
    } else {
        if (!result) result = st->interp->cf.value;
        char *repr = result ? value_repr(result) : NULL;
        char escaped[DAP_BUF_SMALL];
        json_escape_into(escaped, sizeof(escaped), repr ? repr : "null");
        free(repr);

        const char *type_str = value_type_name(result);

        /* provide variablesReference for compound types */
        int var_ref = 0;
        if (result && (result->tag == XS_ARRAY || result->tag == XS_MAP ||
                       result->tag == XS_TUPLE || result->tag == XS_STRUCT_VAL ||
                       result->tag == XS_INST)) {
            var_ref = 9000; /* special eval ref */
        }

        char body[DAP_BUF_MEDIUM];
        snprintf(body, sizeof(body),
            "{\"result\":\"%s\",\"type\":\"%s\",\"variablesReference\":%d}",
            escaped, type_str, var_ref);
        dap_send_response(st, req_seq, "evaluate", body);

        if (result && result != st->interp->cf.value) value_decref(result);
        if (st->interp->cf.value) {
            value_decref(st->interp->cf.value);
            st->interp->cf.value = NULL;
        }
        st->interp->cf.signal = 0;
    }

    free(expression);
}

/* setExpression - add/update a watch expression */
static void dap_handle_set_expression(DapState *st, int req_seq, const char *msg) {
    char *expression = dap_json_get_string(msg, "expression");
    if (!expression || !expression[0]) {
        dap_send_error_response(st, req_seq, "setExpression", "Empty expression");
        free(expression);
        return;
    }

    if (st->n_watches >= DAP_MAX_WATCHES) {
        dap_send_error_response(st, req_seq, "setExpression", "Too many watch expressions");
        free(expression);
        return;
    }

    /* add the watch */
    WatchEntry *w = &st->watches[st->n_watches];
    w->expression = expression; /* takes ownership */
    w->id = ++st->next_watch_id;
    st->n_watches++;

    /* evaluate it now */
    Value *val = eval_expression(st, expression);
    char *repr = val ? value_repr(val) : NULL;
    char escaped[DAP_BUF_SMALL];
    json_escape_into(escaped, sizeof(escaped), repr ? repr : "null");
    const char *type_str = value_type_name(val);

    char body[DAP_BUF_MEDIUM];
    snprintf(body, sizeof(body),
        "{\"value\":\"%s\",\"type\":\"%s\",\"variablesReference\":0}",
        escaped, type_str);
    dap_send_response(st, req_seq, "setExpression", body);

    free(repr);
    if (val) value_decref(val);
}

/* data breakpoints */
static void dap_handle_data_breakpoint_info(DapState *st, int req_seq, const char *msg) {
    char *name = dap_json_get_string(msg, "name");
    if (!name) {
        dap_send_response(st, req_seq, "dataBreakpointInfo",
            "{\"dataId\":null,\"description\":\"No variable specified\",\"accessTypes\":[]}");
        return;
    }

    /* check if variable exists */
    int found = 0;
    if (st->interp) {
        Value *v = env_get(st->interp->env, name);
        if (!v) v = env_get(st->interp->globals, name);
        if (v) found = 1;
    }

    char escaped_name[256];
    json_escape_into(escaped_name, sizeof(escaped_name), name);

    char body[DAP_BUF_SMALL];
    if (found) {
        snprintf(body, sizeof(body),
            "{\"dataId\":\"%s\",\"description\":\"Watch '%s' for changes\","
            "\"accessTypes\":[\"write\"]}",
            escaped_name, escaped_name);
    } else {
        snprintf(body, sizeof(body),
            "{\"dataId\":null,\"description\":\"Variable '%s' not found\","
            "\"accessTypes\":[]}", escaped_name);
    }
    dap_send_response(st, req_seq, "dataBreakpointInfo", body);
    free(name);
}

static void dap_handle_set_data_breakpoints(DapState *st, int req_seq, const char *msg) {
    /* clear existing data breakpoints */
    for (int i = 0; i < st->n_data_bps; i++) {
        free(st->data_bps[i].var_name);
        st->data_bps[i].var_name = NULL;
    }
    st->n_data_bps = 0;

    /* parse breakpoints array */
    const char *bps = strstr(msg, "\"breakpoints\"");
    if (!bps) {
        dap_send_response(st, req_seq, "setDataBreakpoints", "{\"breakpoints\":[]}");
        return;
    }

    /* find each dataId in the breakpoints array */
    const char *p = bps;
    char buf[DAP_BUF_MEDIUM];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"breakpoints\":[");
    int first = 1;

    while (*p && st->n_data_bps < DAP_MAX_DATA_BPS) {
        const char *did = strstr(p, "\"dataId\"");
        if (!did) break;
        char *data_id = dap_json_get_string(did, "dataId");
        if (!data_id) { p = did + 8; continue; }

        DataBreakpoint *db = &st->data_bps[st->n_data_bps];
        db->var_name = data_id;
        db->enabled = 1;
        db->id = ++st->next_data_bp_id;

        /* snapshot current value */
        db->last_hash = 0;
        if (st->interp) {
            Value *v = env_get(st->interp->env, data_id);
            if (!v) v = env_get(st->interp->globals, data_id);
            db->last_hash = value_hash_simple(v);
        }
        st->n_data_bps++;

        if (!first) off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");
        first = 0;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "{\"id\":%d,\"verified\":true}", db->id);

        p = did + 8;
    }

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
    dap_send_response(st, req_seq, "setDataBreakpoints", buf);
}

/* completions in debug console */
static void dap_handle_completions(DapState *st, int req_seq, const char *msg) {
    char *text = dap_json_get_string(msg, "text");
    int col = dap_json_get_int(msg, "column");
    if (col < 0) col = text ? (int)strlen(text) : 0;

    char *buf = malloc(DAP_BUF_LARGE);
    if (!buf) {
        dap_send_response(st, req_seq, "completions", "{\"targets\":[]}");
        free(text);
        return;
    }

    int off = 0;
    off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off, "{\"targets\":[");

    if (st->interp && text) {
        /* find the prefix to complete: last word fragment */
        const char *prefix = text;
        int prefix_len = (int)strlen(text);
        /* walk backward from cursor to find word start */
        int start = col - 1;
        if (start < 0) start = 0;
        if (start > prefix_len) start = prefix_len;
        int word_start = start;
        while (word_start > 0 && (isalnum((unsigned char)text[word_start-1]) || text[word_start-1] == '_'))
            word_start--;

        char word[256];
        int wlen = start - word_start;
        if (wlen < 0) wlen = 0;
        if (wlen >= (int)sizeof(word)) wlen = (int)sizeof(word) - 1;
        memcpy(word, text + word_start, (size_t)wlen);
        word[wlen] = '\0';

        /* collect variable names */
        char *names[256];
        int nnames = collect_var_names(st->interp->env, names, 256);
        /* also globals */
        if (st->interp->globals != st->interp->env) {
            nnames += collect_var_names(st->interp->globals, names + nnames, 256 - nnames);
        }

        int first = 1;
        for (int i = 0; i < nnames && off + 256 < DAP_BUF_LARGE; i++) {
            if (wlen > 0 && strncmp(names[i], word, (size_t)wlen) != 0) continue;
            if (!first) off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off, ",");
            first = 0;

            char esc[256];
            json_escape_into(esc, sizeof(esc), names[i]);
            off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off,
                "{\"label\":\"%s\",\"type\":\"variable\",\"start\":%d,\"length\":%d}",
                esc, word_start, wlen);
        }
    }

    off += snprintf(buf + off, DAP_BUF_LARGE - (size_t)off, "]}");
    dap_send_response(st, req_seq, "completions", buf);
    free(text);
    free(buf);
}



int dap_run(void) {
    DapState state;
    memset(&state, 0, sizeof(state));
    state.seq = 0;
    state.current_line = 1;
    state.next_watch_id = 0;
    state.next_data_bp_id = 0;
    state.break_on_uncaught = 1; /* default: break on uncaught */

    fprintf(stderr, "xs-dap: starting DAP server v0.3.0\n");

    while (1) {
        char *msg = dap_read_message();
        if (!msg) break;

        char *command = dap_json_get_string(msg, "command");
        int req_seq = dap_json_get_int(msg, "seq");

        if (!command) {
            free(msg);
            continue;
        }

        if (strcmp(command, "initialize") == 0) {
            dap_handle_initialize(&state, req_seq);
        } else if (strcmp(command, "launch") == 0) {
            dap_handle_launch(&state, req_seq, msg);
        } else if (strcmp(command, "setBreakpoints") == 0) {
            dap_handle_set_breakpoints(&state, req_seq, msg);
        } else if (strcmp(command, "setExceptionBreakpoints") == 0) {
            dap_handle_set_exception_breakpoints(&state, req_seq, msg);
        } else if (strcmp(command, "exceptionInfo") == 0) {
            dap_handle_exception_info(&state, req_seq);
        } else if (strcmp(command, "configurationDone") == 0) {
            dap_handle_configuration_done(&state, req_seq);
        } else if (strcmp(command, "threads") == 0) {
            dap_handle_threads(&state, req_seq);
        } else if (strcmp(command, "stackTrace") == 0) {
            dap_handle_stack_trace(&state, req_seq);
        } else if (strcmp(command, "scopes") == 0) {
            dap_handle_scopes(&state, req_seq, msg);
        } else if (strcmp(command, "variables") == 0) {
            dap_handle_variables(&state, req_seq, msg);
        } else if (strcmp(command, "continue") == 0) {
            dap_handle_continue(&state, req_seq);
        } else if (strcmp(command, "next") == 0) {
            dap_handle_next(&state, req_seq);
        } else if (strcmp(command, "stepIn") == 0) {
            dap_handle_step_in(&state, req_seq);
        } else if (strcmp(command, "stepOut") == 0) {
            dap_handle_step_out(&state, req_seq);
        } else if (strcmp(command, "evaluate") == 0) {
            dap_handle_evaluate(&state, req_seq, msg);
        } else if (strcmp(command, "setExpression") == 0) {
            dap_handle_set_expression(&state, req_seq, msg);
        } else if (strcmp(command, "dataBreakpointInfo") == 0) {
            dap_handle_data_breakpoint_info(&state, req_seq, msg);
        } else if (strcmp(command, "setDataBreakpoints") == 0) {
            dap_handle_set_data_breakpoints(&state, req_seq, msg);
        } else if (strcmp(command, "completions") == 0) {
            dap_handle_completions(&state, req_seq, msg);
        } else if (strcmp(command, "disconnect") == 0 || strcmp(command, "terminate") == 0) {
            dap_send_response(&state, req_seq, command, "{}");
            free(command);
            free(msg);
            break;
        } else {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Unknown command: %s", command);
            dap_send_error_response(&state, req_seq, command, err_msg);
        }

        free(command);
        free(msg);
    }

    /* Cleanup */
    free(state.program_path);
    free(state.source_text);
    free(state.bp_set.source_path);
    if (state.program_ast) node_free(state.program_ast);
    if (state.interp) interp_free(state.interp);

    /* free watches */
    for (int i = 0; i < state.n_watches; i++)
        free(state.watches[i].expression);

    /* free data breakpoints */
    for (int i = 0; i < state.n_data_bps; i++)
        free(state.data_bps[i].var_name);

    return 0;
}
