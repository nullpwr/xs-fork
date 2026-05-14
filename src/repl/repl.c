#define _POSIX_C_SOURCE 200809L
#include "repl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "core/xs.h"
#include "core/xs_compat.h"
#include "core/value.h"
#include "core/ast.h"
#include "core/env.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "diagnostic/diagnostic.h"
#include "runtime/interp.h"
#include "runtime/error.h"
#include "runtime/concurrent.h"

#ifndef XS_VERSION
#define XS_VERSION "dev"
#endif

#define BUF_INIT  1024
#define BUF_MAX   (1024 * 1024)

static const char *PROMPT = ">> ";
static const char *CONT   = ".. ";

/* Walk `buf` tracking bracket/string/comment nesting.
   Returns 1 if we need more input (unmatched open delimiter),
   0 if the buffer is a complete chunk ready to parse. */
static int input_incomplete(const char *buf) {
    int depth = 0;        /* net open parens/brackets/braces */
    int in_block = 0;     /* inside {- ... -} block comment */
    char in_str = 0;      /* 0, '"', '\'', '`' */
    int triple = 0;       /* 0 = not in string, 1 = single-quoted, 3 = triple-quoted */

    const char *p = buf;
    while (*p) {
        if (in_block) {
            if (p[0] == '-' && p[1] == '}') { in_block = 0; p += 2; continue; }
            p++;
            continue;
        }
        if (in_str) {
            if (triple == 3) {
                /* inside a triple-quoted string; look for closing triple */
                if (*p == in_str && p[1] == in_str && p[2] == in_str) {
                    in_str = 0; triple = 0; p += 3; continue;
                }
                p++;
                continue;
            }
            /* single-quoted string */
            if (*p == '\\') { p += 2; continue; }
            if (*p == in_str) { in_str = 0; triple = 0; p++; continue; }
            if (*p == '\n') {
                /* unterminated single-line string; treat as closed so we
                   don't hang - the parser will error */
                in_str = 0; triple = 0;
            }
            p++;
            continue;
        }

        /* line comment: -- to end of line */
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            continue;
        }

        /* block comment open */
        if (p[0] == '{' && p[1] == '-') {
            in_block = 1; p += 2; continue;
        }

        /* string open: check for triple first */
        if (*p == '"' || *p == '\'' || *p == '`') {
            char q = *p;
            if (p[1] == q && p[2] == q) {
                in_str = q; triple = 3; p += 3; continue;
            }
            in_str = q; triple = 1; p++; continue;
        }

        if (*p == '(' || *p == '[' || *p == '{') { depth++; p++; continue; }
        if (*p == ')' || *p == ']' || *p == '}') {
            depth--; if (depth < 0) depth = 0; p++; continue;
        }
        p++;
    }

    return (depth > 0 || in_str || in_block);
}

/* Read one logical "turn": one or more physical lines that form a
   complete XS chunk.  Prints prompt/cont appropriately.
   Returns a malloc'd string on success, NULL on EOF. */
static char *read_turn(void) {
    size_t cap = BUF_INIT;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;

    int first_line = 1;

    for (;;) {
        fputs(first_line ? PROMPT : CONT, stdout);
        fflush(stdout);

        char line[4096];
        if (!fgets(line, sizeof line, stdin)) {
            free(buf);
            return NULL;
        }

        size_t ll = strlen(line);

        /* grow buf to fit */
        while (len + ll + 2 > cap) {
            if (cap >= BUF_MAX) {
                fprintf(stderr, "\nxs: input too long (> 1 MB), discarding\n");
                free(buf);
                return NULL;
            }
            cap = cap * 2 > BUF_MAX ? BUF_MAX : cap * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }

        if (len > 0) {
            buf[len++] = '\n';
        }
        memcpy(buf + len, line, ll);
        len += ll;
        buf[len] = '\0';

        /* strip trailing newline for the completeness check */
        size_t check_len = len;
        if (check_len > 0 && buf[check_len-1] == '\n') {
            buf[check_len-1] = '\0';
            check_len--;
        }

        if (!input_incomplete(buf)) {
            /* restore: buf is already null-terminated at check_len */
            break;
        }

        /* restore the newline for the accumulated buffer */
        if (buf[check_len] == '\0' && check_len < len) {
            buf[check_len] = '\n';
        }
        first_line = 0;
    }

    return buf;
}

/* Print every name defined in the interpreter's global environment. */
static void cmd_env(Interp *interp) {
    Env *g = interp->globals;
    if (!g || g->len == 0) {
        printf("(no bindings)\n");
        return;
    }
    for (int i = 0; i < g->len; i++) {
        Binding *b = &g->bindings[i];
        if (!b->name || !b->value) continue;
        const char *kind = b->mutable ? "var" : "let";
        const char *tstr = value_type_str(b->value);
        if (b->value->tag == XS_FUNC || b->value->tag == XS_NATIVE ||
            b->value->tag == XS_OVERLOAD) {
            kind = "fn";
        } else if (b->value->tag == XS_MODULE) {
            kind = "module";
        } else if (b->value->tag == XS_STRUCT_VAL) {
            kind = "struct";
        } else if (b->value->tag == XS_ENUM_VAL) {
            kind = "enum";
        } else if (b->value->tag == XS_CLASS_VAL) {
            kind = "class";
        }
        printf("  %s %s: %s\n", kind, b->name, tstr);
    }
}

static void print_help(void) {
    printf(
        "REPL meta-commands:\n"
        "  :help, :h         show this list\n"
        "  :quit, :q         exit\n"
        "  :env              list all bindings in scope\n"
        "  :clear            reset environment (fresh interpreter)\n"
        "  :t <expr>         show the inferred type of <expr>\n"
    );
}

/* --- program list: keep AST nodes alive so function closures don't dangle --- */

typedef struct {
    Node **items;
    int    len, cap;
} ProgList;

static void proglist_push(ProgList *pl, Node *n) {
    if (pl->len >= pl->cap) {
        int nc = pl->cap ? pl->cap * 2 : 16;
        pl->items = realloc(pl->items, (size_t)nc * sizeof(Node*));
        pl->cap = nc;
    }
    pl->items[pl->len++] = n;
}

static void proglist_free(ProgList *pl) {
    for (int i = 0; i < pl->len; i++) {
        /* Don't call node_free: AST nodes may be referenced by function
           bodies in the interp. Leak intentionally; the REPL session
           is ending anyway. */
        (void)pl->items[i];
    }
    free(pl->items);
    pl->items = NULL;
    pl->len = pl->cap = 0;
}

/* --- :t implementation --- */

static void cmd_t(const char *expr_src, Interp *interp, int no_color,
                  ProgList *progs) {
    if (!expr_src || !*expr_src) {
        printf(":t requires an expression\n");
        return;
    }

    DiagContext *dctx = diag_context_new();
    diag_context_add_source(dctx, "<repl>", expr_src);
    Lexer lex; lexer_init(&lex, expr_src, "<repl>");
    TokenArray ta = lexer_tokenize(&lex);
    Parser psr; parser_init(&psr, &ta, "<repl>");
    psr.diag = dctx;
    Node *prog = parser_parse(&psr);
    int had_err = psr.had_error || diag_context_error_count(dctx) > 0;

    if (had_err) {
        diag_render_all(dctx);
    } else if (prog) {
        /* save for type eval */
        if (interp->last_expr_value) {
            value_decref(interp->last_expr_value);
            interp->last_expr_value = NULL;
        }
        if (interp->cf.signal) CF_CLEAR(interp);

        interp_run(interp, prog);
        proglist_push(progs, prog);
        prog = NULL; /* owned by progs now */

        int eval_ok = !(interp->cf.signal == CF_ERROR ||
                        interp->cf.signal == CF_PANIC ||
                        interp->cf.signal == CF_THROW);
        if (eval_ok && interp->last_expr_value) {
            const char *ts = value_type_str(interp->last_expr_value);
            printf("%s\n", ts ? ts : "?");
        } else if (!eval_ok) {
            if (!no_color) fprintf(stderr, "\033[31m");
            fprintf(stderr, "error: could not evaluate expression for :t\n");
            if (!no_color) fprintf(stderr, "\033[0m");
            if (interp->cf.signal) CF_CLEAR(interp);
        } else {
            printf("(declaration)\n");
        }
    }

    token_array_free(&ta);
    comment_list_free(&lex.comments);
    if (prog) node_free(prog);
    diag_context_free(dctx);
}

/* Returns 1 if `line` was a recognised meta-command, 0 otherwise.
   Sets *should_exit=1 if the command was :quit. */
static int handle_meta(const char *line, Interp **interp_ptr,
                       int no_color, int *should_exit, ProgList *progs) {
    const char *cmd = line + 1;
    char buf[512];
    size_t clen = strlen(cmd);
    if (clen >= sizeof buf) clen = sizeof buf - 1;
    memcpy(buf, cmd, clen);
    buf[clen] = '\0';
    /* rtrim */
    while (clen > 0 && (buf[clen-1] == '\n' || buf[clen-1] == ' ' ||
                        buf[clen-1] == '\r' || buf[clen-1] == '\t')) {
        buf[--clen] = '\0';
    }

    if (strcmp(buf, "q") == 0 || strcmp(buf, "quit") == 0) {
        *should_exit = 1;
        return 1;
    }
    if (strcmp(buf, "h") == 0 || strcmp(buf, "help") == 0) {
        print_help();
        return 1;
    }
    if (strcmp(buf, "env") == 0) {
        cmd_env(*interp_ptr);
        return 1;
    }
    if (strcmp(buf, "clear") == 0) {
        interp_free(*interp_ptr);
        /* free old programs that we were keeping alive */
        proglist_free(progs);
        *interp_ptr = interp_new("<repl>");
        if (!*interp_ptr) {
            fprintf(stderr, "xs: failed to reinitialize interpreter\n");
            *should_exit = 1;
        } else {
            Value *av = xs_array_new();
            env_define((*interp_ptr)->globals, "argv", av, 0);
            value_decref(av);
            printf("environment cleared\n");
        }
        return 1;
    }
    /* :t <expr> */
    if (strncmp(buf, "t ", 2) == 0 || strcmp(buf, "t") == 0) {
        const char *expr_src = buf + 1;
        while (*expr_src == ' ' || *expr_src == '\t') expr_src++;
        cmd_t(expr_src, *interp_ptr, no_color, progs);
        return 1;
    }

    return 0;
}

/* Returns 1 if the last statement in program is an expression statement. */
static int last_is_expr_stmt(Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return 0;
    int n = program->program.stmts.len;
    if (n == 0) return 0;
    Node *last = program->program.stmts.items[n - 1];
    return last && VAL_TAG(last) == NODE_EXPR_STMT;
}

int xs_repl_run(int no_color) {
    printf("xs %s\n", XS_VERSION);
    printf("type :help for commands, :quit to exit (or Ctrl-D)\n");
    fflush(stdout);

    Interp *interp = interp_new("<repl>");
    if (!interp) {
        fprintf(stderr, "xs: failed to init interpreter\n");
        return 1;
    }

    {
        Value *av = xs_array_new();
        env_define(interp->globals, "argv", av, 0);
        value_decref(av);
    }

    /* Keep all parsed ASTs alive so function closures don't dangle.
       Function bodies are AST nodes referenced by XSFunc; freeing the
       AST after a turn that defined a function would leave dangling
       pointers in any closure created during that turn. */
    ProgList progs = {0};

    int rc = 0;
    int should_exit = 0;

    while (!should_exit) {
        char *turn = read_turn();
        if (!turn) {
            printf("\n");
            break;
        }

        /* meta-command? */
        const char *trimmed = turn;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (trimmed[0] == ':') {
            if (!handle_meta(trimmed, &interp, no_color, &should_exit, &progs)) {
                const char *p = trimmed + 1;
                char name[64];
                int ni = 0;
                while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
                name[ni] = '\0';
                fprintf(stderr, "unknown REPL command: %s (try :help)\n", name);
            }
            free(turn);
            continue;
        }

        /* skip empty input */
        {
            const char *p = trimmed;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) { free(turn); continue; }
        }

        /* parse */
        DiagContext *dctx = diag_context_new();
        diag_context_add_source(dctx, "<repl>", turn);

        Lexer lex; lexer_init(&lex, turn, "<repl>");
        TokenArray ta = lexer_tokenize(&lex);

        Parser psr; parser_init(&psr, &ta, "<repl>");
        psr.source = turn;
        psr.diag = dctx;

        Node *prog = parser_parse(&psr);

        int parse_failed = !prog || psr.had_error ||
                           diag_context_error_count(dctx) > 0 ||
                           lex.n_errors > 0;

        if (parse_failed) {
            diag_render_all(dctx);
            token_array_free(&ta);
            comment_list_free(&lex.comments);
            if (prog) node_free(prog);
            diag_context_free(dctx);
            free(turn);
            continue;
        }
        diag_context_free(dctx);
        token_array_free(&ta);
        comment_list_free(&lex.comments);

        int will_print = last_is_expr_stmt(prog);

        if (interp->cf.signal) CF_CLEAR(interp);
        if (interp->last_expr_value) {
            value_decref(interp->last_expr_value);
            interp->last_expr_value = NULL;
        }

        interp_run(interp, prog);

        /* Keep the AST alive. Function closures hold pointers into it. */
        proglist_push(&progs, prog);

        int had_error = (interp->cf.signal == CF_ERROR ||
                         interp->cf.signal == CF_PANIC ||
                         interp->cf.signal == CF_THROW);

        if (had_error) {
            if (interp->cf.signal == CF_THROW && interp->cf.value) {
                char *msg = value_repr(interp->cf.value);
                if (!no_color) fprintf(stderr, "\033[31m");
                fprintf(stderr, "error: %s\n", msg ? msg : "(unknown)");
                if (!no_color) fprintf(stderr, "\033[0m");
                free(msg);
            }
            CF_CLEAR(interp);
        } else if (will_print && interp->last_expr_value) {
            char *s = value_repr(interp->last_expr_value);
            printf("=> %s\n", s ? s : "(null)");
            free(s);
        }
        fflush(stdout);

        free(turn);
    }

    interp_free(interp);
    proglist_free(&progs);
    return rc;
}
