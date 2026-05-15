#include "core/xs_compat.h"
#include "core/parser.h"
#include "core/xs_utils.h"
#include "plugins/plugin_parse.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

/* plugin syntax handler callbacks (set by interp.c) */
Node *(*g_plugin_try_syntax_handler)(Parser *p, Token *tok) = NULL;
Node *(*g_plugin_try_syntax_expr_handler)(Parser *p, Token *tok) = NULL;
int (*g_plugin_is_keyword)(const char *word) = NULL;
int (*g_parser_eager_load)(const char *path, const char *containing_file) = NULL;
Node *(*g_plugin_try_parser_override)(Parser *p, const char *keyword) = NULL;
const char *g_plugin_forced_id = NULL;

typedef struct { const char *typo; const char *suggestion; } TypoEntry;
static const TypoEntry KEYWORD_TYPOS[] = {
    {"function",  "fn"},
    {"fucntion",  "fn"},
    {"funciton",  "fn"},
    {"func",      "fn"},
    {"def",       "fn"},
    {"fun",       "fn"},
    {"elseif",    "else if"},
    {"elsif",     "else if"},
    {"switch",    "match"},
    {"foreach",   "for"},
    {"void",      "fn (XS functions return unit by default)"},
    {"puts",      "println"},
    {"echo",      "println"},
    {"printf",    "println (or use fmt module)"},
    {"string",    "str"},
    {"boolean",   "bool"},
    {"case",      "match"},
    {"lambda",    "fn (XS uses fn for lambdas too)"},
    {"nil",       "null"},
    {"none",      "null"},
    {"undefined", "null"},
    {"extends",   ": (use class Foo : Bar)"},
    {"interface", "trait"},
    {"abstract",  "trait"},
    {"require",   "import or use"},
    {"include",   "import or use"},
    {"raise",     "throw"},
    {"except",    "catch"},
    {"rescue",    "catch"},
    {"ensure",    "finally"},
    {"unless",    "if ! (XS has no unless)"},
    {"until",     "while ! (XS has no until)"},
    {"then",      "{ (XS uses braces, not then)"},
    {NULL, NULL}
};

static const char *suggest_keyword_exact(const char *ident) {
    if (!ident) return NULL;
    for (int i = 0; KEYWORD_TYPOS[i].typo; i++) {
        if (strcmp(ident, KEYWORD_TYPOS[i].typo) == 0)
            return KEYWORD_TYPOS[i].suggestion;
    }
    return NULL;
}

static const char *suggest_keyword(const char *ident) {
    if (!ident) return NULL;
    const char *exact = suggest_keyword_exact(ident);
    if (exact) return exact;
    static const char *xs_keywords[] = {
        "fn", "let", "var", "const", "if", "else", "while", "for", "in",
        "loop", "break", "continue", "return", "match", "when", "yield",
        "struct", "enum", "class", "trait", "impl", "import", "module",
        "pub", "mut", "static", "async", "await", "spawn", "try", "catch",
        "throw", "assert", "defer", "true", "false", "null",
        NULL
    };
    const char *best = NULL;
    int best_dist = 3; /* threshold: suggest only if distance <= 2 */
    for (int i = 0; xs_keywords[i]; i++) {
        int d = xs_edit_distance(ident, xs_keywords[i]);
        if (d > 0 && d < best_dist) {
            best_dist = d;
            best = xs_keywords[i];
        }
    }
    return best;
}

/* precedence */
static int prec_of(TokenKind k) {
    switch (k) {
    case TK_ASSIGN: case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN:
    case TK_STAR_ASSIGN: case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
    case TK_AND_ASSIGN: case TK_OR_ASSIGN: case TK_XOR_ASSIGN:
    case TK_SHL_ASSIGN: case TK_SHR_ASSIGN: case TK_POWER_ASSIGN:
        return 1;
    case TK_PIPE_ARROW:     return 2;
    case TK_NULL_COALESCE:  return 3;
    case TK_DOTDOT: case TK_DOTDOTEQ: return 3;
    case TK_OP_OR: case TK_OR: return 4;
    case TK_OP_AND: case TK_AND: return 5;
    case TK_EQ: case TK_NEQ: return 6;
    case TK_LT: case TK_GT: case TK_LE: case TK_GE:
    case TK_SPACESHIP:      return 7;
    case TK_IS:             return 7;
    case TK_IN:             return 7;
    case TK_PIPE:           return 8;
    case TK_CARET:          return 9;
    case TK_AMP:            return 10;
    case TK_SHL: case TK_SHR: return 11;
    case TK_PLUS: case TK_MINUS: case TK_CONCAT: return 12;
    case TK_STAR: case TK_SLASH: case TK_PERCENT:
    case TK_FLOORDIV:       return 13;
    case TK_POWER:          return 14;
    case TK_AS:             return 15; /* cast: highest binary prec */
    case TK_OP_NOT:         return 2;  /* actor ! msg send operator */
    default:                return 0;
    }
}

static int is_assign_op(TokenKind k) {
    return k==TK_ASSIGN||k==TK_PLUS_ASSIGN||k==TK_MINUS_ASSIGN||
           k==TK_STAR_ASSIGN||k==TK_SLASH_ASSIGN||k==TK_PERCENT_ASSIGN||
           k==TK_AND_ASSIGN||k==TK_OR_ASSIGN||k==TK_XOR_ASSIGN||
           k==TK_SHL_ASSIGN||k==TK_SHR_ASSIGN||k==TK_POWER_ASSIGN;
}

static const char *assign_op_str(TokenKind k) {
    switch (k) {
    case TK_ASSIGN:         return "=";
    case TK_PLUS_ASSIGN:    return "+=";
    case TK_MINUS_ASSIGN:   return "-=";
    case TK_STAR_ASSIGN:    return "*=";
    case TK_SLASH_ASSIGN:   return "/=";
    case TK_PERCENT_ASSIGN: return "%=";
    case TK_AND_ASSIGN:     return "&=";
    case TK_OR_ASSIGN:      return "|=";
    case TK_XOR_ASSIGN:     return "^=";
    case TK_SHL_ASSIGN:     return "<<=";
    case TK_SHR_ASSIGN:     return ">>=";
    case TK_POWER_ASSIGN:   return "**=";
    default:                return "=";
    }
}

void parser_init(Parser *p, TokenArray *ta, const char *filename) {
    p->tokens      = ta->items;
    p->ntokens     = ta->len;
    p->pos         = 0;
    p->filename    = filename ? filename : "<stdin>";
    p->source      = NULL;
    p->had_error   = 0;
    p->error_count = 0;
    p->max_errors  = 10;
    p->panic_mode  = 0;
    p->no_arrow_lambda = 0;
    p->diag        = NULL;
    memset(&p->error, 0, sizeof(p->error));
}

static Token *pp_peek(Parser *p, int off) {
    int idx = p->pos + off;
    if (idx >= p->ntokens) return &p->tokens[p->ntokens - 1]; /* EOF */
    return &p->tokens[idx];
}

/* Any keyword can stand in for an identifier in name-only positions —
 * after `.` (field access, method call), after `::` (path), as a struct
 * field name, as a map-literal key. Those slots are already disambiguated
 * by the surrounding syntax, so the parser has no use for the keyword
 * meaning there. Centralised here so we don't drift across call sites. */
static int is_ident_or_kw(TokenKind k) {
    return k == TK_IDENT || (k >= TK_IF && k <= TK_LOAD);
}

static Token *pp_advance(Parser *p) {
    Token *t = &p->tokens[p->pos];
    if (p->pos < p->ntokens - 1) p->pos++;
    return t;
}

static int pp_check(Parser *p, TokenKind k) {
    return pp_peek(p, 0)->kind == k;
}

static int pp_check2(Parser *p, TokenKind a, TokenKind b) {
    TokenKind k = pp_peek(p, 0)->kind;
    return k == a || k == b;
}

static Token *pp_match(Parser *p, TokenKind k) {
    if (pp_check(p, k)) return pp_advance(p);
    /* Generic-close splitting: `Foo<Bar<T>>` lexes the trailing `>>`
       as one TK_SHR. When a parser callsite wants a single TK_GT to
       close its generic, treat a leading SHR as if it were two `>`s:
       rewrite the token in place to TK_GT (consuming the first `>`)
       and leave the position alone so the outer matcher picks up the
       remaining `>`. */
    if (k == TK_GT && pp_check(p, TK_SHR)) {
        Token *t = pp_peek(p, 0);
        t->kind = TK_GT;
        return t;
    }
    return NULL;
}

/* Token.sval sits in a union with ival/fval, so dereferencing it on a
   numeric token (TK_INT/TK_FLOAT/TK_BOOL/TK_NULL) reads the number's
   bits as a pointer and crashes on any print path. Only the kinds that
   actually wrote sval can be trusted; everything else falls back to the
   token kind's static display name. Use this when surfacing a token's
   text to the user (diagnostics, suggestions, captured type text). */
static const char *safe_tok_text(const Token *t) {
    int sval_ok = (t->kind == TK_STRING || t->kind == TK_RAW_STRING ||
                   t->kind == TK_CHAR   || t->kind == TK_IDENT      ||
                   t->kind == TK_BIGINT || t->kind == TK_MACRO_BANG ||
                   t->kind == TK_UNKNOWN ||
                   ((int)t->kind >= TK_PLUGIN_BASE &&
                    (int)t->kind <  TK_PLUGIN_MAX));
    return (sval_ok && t->sval) ? t->sval : token_kind_name(t->kind);
}

/* contextual keyword: match 'where' as an identifier, not a reserved word */
static int pp_match_where(Parser *p) {
    Token *t = pp_peek(p, 0);
    if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "where") == 0) {
        pp_advance(p);
        return 1;
    }
    return 0;
}

static const char *token_display_name(TokenKind k) {
    switch (k) {
    case TK_SEMICOLON: return "';'";
    case TK_COLON:     return "':'";
    case TK_COMMA:     return "','";
    case TK_LPAREN:    return "'('";
    case TK_RPAREN:    return "')'";
    case TK_LBRACE:    return "'{'";
    case TK_RBRACE:    return "'}'";
    case TK_LBRACKET:  return "'['";
    case TK_RBRACKET:  return "']'";
    case TK_ASSIGN:    return "'='";
    case TK_ARROW:     return "'->'";
    case TK_FAT_ARROW: return "'=>'";
    case TK_IDENT:     return "an identifier";
    case TK_INT:       return "a number";
    case TK_STRING:    return "a string";
    case TK_EOF:       return "end of file";
    default:           return token_kind_name(k);
    }
}

static const char *pp_suggest(TokenKind expected, TokenKind got, const char *got_text) {
    if (expected == TK_SEMICOLON) {
        return " (hint: add a ';' at the end of the previous statement)";
    }
    if (expected == TK_RPAREN && got == TK_SEMICOLON) {
        return " (hint: you may have an unmatched '(')";
    }
    if (expected == TK_RBRACE && got == TK_EOF) {
        return " (hint: you may have an unmatched '{')";
    }
    if (expected == TK_RBRACKET && (got == TK_EOF || got == TK_SEMICOLON)) {
        return " (hint: you may have an unmatched '[')";
    }
    if (expected == TK_IDENT && got >= TK_IF && got <= TK_RESUME) {
        return " (hint: this is a reserved keyword and cannot be used as a name)";
    }
    if (expected == TK_ASSIGN && got == TK_EQ) {
        return " (did you mean '=' instead of '=='?)";
    }
    if (expected == TK_FAT_ARROW && got == TK_ASSIGN) {
        return " (did you mean '=>' instead of '='?)";
    }
    if (expected == TK_FAT_ARROW && got == TK_ARROW) {
        return " (did you mean '=>' instead of '->'?)";
    }
    if (expected == TK_IN && got == TK_COLON) {
        return " (did you mean 'in'? XS uses 'for x in collection')";
    }
    return "";
}

static Token *pp_expect_ex(Parser *p, TokenKind k, const char *msg, int open_line) {
    if (pp_check(p, k)) return pp_advance(p);
    if (p->panic_mode) return pp_peek(p, 0);

    Token *t = pp_peek(p, 0);
    const char *expected_name = token_display_name(k);
    const char *got_name      = safe_tok_text(t);
    const char *hint_str      = pp_suggest(k, t->kind,
        (t->kind == TK_IDENT) ? t->sval : NULL);

    p->panic_mode = 1;
    p->had_error  = 1;
    p->error_count++;

    if (p->error_count > p->max_errors) return t;

    snprintf(p->error.msg, sizeof(p->error.msg),
             "%s: expected %s, got %s%s",
             msg ? msg : "parse error",
             expected_name, got_name, hint_str);
    p->error.span = t->span;

    if (p->error_count == p->max_errors) {
        if (p->diag) {
            Diagnostic *d = diag_new(DIAG_ERROR, DIAG_PHASE_PARSER, "P0099",
                                     "too many errors, stopping");
            diag_annotate(d, t->span, 1, "error limit reached");
            diag_emit(p->diag, d);
        }
        return t;
    }

    if (p->diag) {
        const char *code = "P0001";
        if (k == TK_SEMICOLON)
            code = "P0010";
        else if ((k == TK_RBRACE && t->kind == TK_EOF) ||
                 (k == TK_RPAREN && (t->kind == TK_SEMICOLON || t->kind == TK_EOF)) ||
                 (k == TK_RBRACKET && (t->kind == TK_EOF || t->kind == TK_SEMICOLON)))
            code = "P0012";

        Diagnostic *d = diag_new(DIAG_ERROR, DIAG_PHASE_PARSER, code,
                                 "expected %s, found %s",
                                 expected_name, got_name);
        diag_annotate(d, t->span, 1, "unexpected %s here", got_name);

        if (k == TK_RPAREN && open_line > 0)
            diag_hint(d, "unmatched '(' opened on line %d", open_line);
        else if (k == TK_RBRACE && open_line > 0)
            diag_hint(d, "unmatched '{' opened on line %d", open_line);
        else if (k == TK_RBRACKET && open_line > 0)
            diag_hint(d, "unmatched '[' opened on line %d", open_line);
        else if (k == TK_SEMICOLON)
            diag_hint(d, "add a ';' at the end of the previous statement");
        else if (k == TK_RPAREN && t->kind == TK_SEMICOLON)
            diag_hint(d, "you may have an unmatched '('");
        else if (k == TK_RBRACE && t->kind == TK_EOF)
            diag_hint(d, "you may have an unmatched '{'");
        else if (k == TK_RBRACKET && (t->kind == TK_EOF || t->kind == TK_SEMICOLON))
            diag_hint(d, "you may have an unmatched '['");
        else if (k == TK_ASSIGN && t->kind == TK_EQ)
            diag_hint(d, "did you mean '=' instead of '=='?");
        else if (k == TK_FAT_ARROW && t->kind == TK_ASSIGN)
            diag_hint(d, "did you mean '=>' instead of '='?");
        else if (k == TK_FAT_ARROW && t->kind == TK_ARROW)
            diag_hint(d, "did you mean '=>' instead of '->'?");
        else if (k == TK_IN && t->kind == TK_COLON)
            diag_hint(d, "XS uses 'for x in collection', not ':'");
        else if (k == TK_IDENT && t->kind >= TK_IF && t->kind <= TK_RESUME)
            diag_hint(d, "this is a reserved keyword and cannot be used as a name");

        diag_emit(p->diag, d);
    }

    return t;
}

static Token *pp_expect(Parser *p, TokenKind k, const char *msg) {
    return pp_expect_ex(p, k, msg, 0);
}

static int pp_at_end(Parser *p) {
    return pp_peek(p, 0)->kind == TK_EOF;
}

static void skip_semis(Parser *p) {
    while (pp_match(p, TK_SEMICOLON));
}

static int at_sync_point(TokenKind k) {
    return k == TK_LET || k == TK_VAR || k == TK_CONST || k == TK_FN ||
           k == TK_STRUCT || k == TK_ENUM || k == TK_CLASS || k == TK_IF ||
           k == TK_FOR || k == TK_WHILE || k == TK_RETURN || k == TK_IMPORT ||
           k == TK_BIND ||
           k == TK_SEMICOLON || k == TK_NEWLINE || k == TK_RBRACE || k == TK_EOF;
}

static void synchronize(Parser *p) {
    p->panic_mode = 0;
    while (!pp_at_end(p)) {
        TokenKind k = pp_peek(p, 0)->kind;
        if (k == TK_SEMICOLON || k == TK_NEWLINE) {
            pp_advance(p);
            return;
        }
        if (at_sync_point(k)) return;
        pp_advance(p);
    }
}

static void parse_error_at(Parser *p, Span span, const char *code,
                           const char *fmt, ...) {
    if (p->panic_mode) return;
    p->panic_mode = 1;
    p->had_error  = 1;
    p->error_count++;

    if (p->error_count > p->max_errors) return;

    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    snprintf(p->error.msg, sizeof(p->error.msg), "%s", buf);
    p->error.span = span;

    if (p->error_count == p->max_errors) {
        if (p->diag) {
            Diagnostic *d = diag_new(DIAG_ERROR, DIAG_PHASE_PARSER, "P0099",
                                     "too many errors, stopping");
            diag_annotate(d, span, 1, "error limit reached");
            diag_emit(p->diag, d);
        }
        return;
    }

    if (p->diag) {
        Diagnostic *d = diag_new(DIAG_ERROR, DIAG_PHASE_PARSER, code, "%s", buf);
        diag_annotate(d, span, 1, "%s", buf);
        diag_emit(p->diag, d);
    }
}

static Node *parse_expr(Parser *p, int min_prec);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);

/* Decorators recognised on fn declarations. Anything else is a parse
   error. Names starting with '@' are stored without the leading '@';
   '#[name]' uses the same name space. */
static int is_known_decorator(const char *name) {
    if (!name) return 0;
    static const char *known[] = {
        "on_start", "on_exit", "on_signal", "on_panic",
        "every", "cron", "delayed",
        "watch",
        "bench", "example",
        "once",
        "test",
        /* wrapping decorators -- replace the bound fn with a wrapper
           that runs around it. */
        "memoize", "retry", "trace", "timed",
        NULL
    };
    for (int i = 0; known[i]; i++)
        if (strcmp(name, known[i]) == 0) return 1;
    return 0;
}

/* Wrapping decorators replace the function's binding with a callable
   wrapper map after the runtime constructs the original. They differ
   from triggers (which just record the fn into a registry) and from
   discovery markers (which only flag for the test runner). */
int parser_decorator_is_wrapping(const char *name) {
    if (!name) return 0;
    return strcmp(name, "memoize") == 0 ||
           strcmp(name, "retry") == 0 ||
           strcmp(name, "trace") == 0 ||
           strcmp(name, "timed") == 0;
}

/* @cron / @every / @delayed schedule the function with no caller, so
   they can't accept parameters. @once / @watch and the no-arg
   lifecycle hooks (start / exit / signal) all also fire without args,
   so we apply the same no-params rule to them. @on_panic receives the
   exception, and @bench / @example are caller-driven discovery markers
   (the runner passes a test harness), so they can declare params. */
static int decorator_forbids_params(const char *name) {
    if (!name) return 0;
    return strcmp(name, "every") == 0 ||
           strcmp(name, "cron") == 0 ||
           strcmp(name, "delayed") == 0 ||
           strcmp(name, "on_start") == 0 ||
           strcmp(name, "on_exit") == 0 ||
           strcmp(name, "on_signal") == 0 ||
           strcmp(name, "watch") == 0 ||
           strcmp(name, "once") == 0;
}

static void validate_decorators(Parser *p, int n_params,
                                Decorator *decs, int n_decs) {
    int once_count = 0;
    int trigger_count = 0;
    for (int i = 0; i < n_decs; i++) {
        const char *name = decs[i].name;
        if (name && strcmp(name, "export") == 0) {
            parse_error_at(p, decs[i].span, "P0054",
                "`@export` is gone. List the names to expose at the "
                "bottom of the file: `export { foo, bar }`");
            continue;
        }
        if (!is_known_decorator(name)) {
            parse_error_at(p, decs[i].span, "P0051",
                "unknown decorator '@%s'", name ? name : "?");
            continue;
        }
        if (strcmp(name, "once") == 0) once_count++;
        else trigger_count++;
        if (decorator_forbids_params(name) && n_params > 0) {
            parse_error_at(p, decs[i].span, "P0052",
                "@%s cannot decorate a fn with parameters", name);
        }
    }
    if (once_count > 0 && trigger_count == 0) {
        parse_error_at(p, decs[0].span, "P0053",
            "@once must compose with a trigger decorator");
    }
    if (once_count > 1) {
        parse_error_at(p, decs[0].span, "P0054",
            "@once can only appear once");
    }
}
static Node *parse_pattern(Parser *p);
static Node *parse_loop_pattern(Parser *p);
static Node *parse_prefix(Parser *p);
static Node *parse_postfix(Parser *p, Node *left);
static Node *parse_primary(Parser *p);
static void  skip_type(Parser *p);
static TypeExpr *parse_type_expr(Parser *p);
static ParamList parse_params(Parser *p);
static Node *parse_handle(Parser *p);
static Node *parse_effect_decl(Parser *p);
static Node *parse_fn_decl(Parser *p, int is_pub, int is_async, int is_pure);
static Node *parse_actor_decl(Parser *p);

static Node *make_block(NodeList stmts, Node *expr, Span span) {
    Node *n = node_new(NODE_BLOCK, span);
    n->block.stmts     = stmts;
    n->block.expr      = expr;
    n->block.has_decls  = -1;
    n->block.is_unsafe  = 0;
    return n;
}

/* Parses interpolated or plain string from a TK_STRING token. */
static Node *parse_string_literal(Parser *p, Token *tok) {
    Span span = tok->span;
    const char *raw = tok->sval;
    if (!raw) raw = "";

    int has_interp = 0;
    for (const char *c = raw; *c; c++) {
        if (*c == '{' && (c == raw || *(c-1) != '\x01')) {
            has_interp = 1; break;
        }
    }

    if (!has_interp) {
        int len = (int)strlen(raw);
        char *s = xs_malloc(len + 1);
        int si = 0;
        for (int i = 0; i < len; i++) {
            if (raw[i] == '\x01' && i+1 < len && (raw[i+1]=='{' || raw[i+1]=='}')) {
                s[si++] = raw[i+1]; i++;
            } else {
                s[si++] = raw[i];
            }
        }
        s[si] = '\0';
        Node *n = node_new(NODE_LIT_STRING, span);
        n->lit_string.sval = s;
        n->lit_string.interpolated = 0;
        n->lit_string.parts = nodelist_new();
        return n;
    }

    Node *n = node_new(NODE_INTERP_STRING, span);
    n->lit_string.sval = xs_strdup(raw);
    n->lit_string.interpolated = 1;
    n->lit_string.parts = nodelist_new();

    int len = (int)strlen(raw);
    char *piece = xs_malloc(len + 1);
    int pi = 0;

    for (int i = 0; i <= len; ) {
        if (i == len) {
            /* flush piece */
            piece[pi] = '\0';
            Node *sn = node_new(NODE_LIT_STRING, span);
            sn->lit_string.sval = xs_strdup(piece);
            sn->lit_string.interpolated = 0;
            sn->lit_string.parts = nodelist_new();
            nodelist_push(&n->lit_string.parts, sn);
            break;
        }
        char c = raw[i];
        if (c == '\x01' && i+1 < len && (raw[i+1]=='{' || raw[i+1]=='}')) {
            piece[pi++] = raw[i+1]; i += 2;
        } else if (c == '{') {
            /* flush current piece as string node */
            piece[pi] = '\0';
            Node *sn = node_new(NODE_LIT_STRING, span);
            sn->lit_string.sval = xs_strdup(piece);
            sn->lit_string.interpolated = 0;
            sn->lit_string.parts = nodelist_new();
            nodelist_push(&n->lit_string.parts, sn);
            pi = 0;

            /* find matching }: skip over quoted strings */
            int depth = 1, j = i+1;
            while (j < len && depth > 0) {
                char ch = raw[j];
                if (ch == '"' || ch == '\'') {
                    /* skip quoted string */
                    char q = ch;
                    j++;
                    while (j < len) {
                        if (raw[j] == '\\' && j+1 < len) { j += 2; continue; }
                        if (raw[j] == q) { j++; break; }
                        j++;
                    }
                    continue;
                }
                if (ch == '{') depth++;
                else if (ch == '}') depth--;
                if (depth > 0) j++;
                else break;
            }
            /* raw[i+1 .. j] is the expression source */
            int elen = j - i - 1;

            if (elen == 0) {
                /* Empty {}: treat as literal '{' '}' */
                piece[pi++] = '{'; piece[pi++] = '}';
                i = j + 1;
                continue;
            }

            /* Compute the (line, col) of the byte after the `{` in the
               original source. raw[0..i] is the prefix; walk it counting
               newlines so error spans inside the interpolation point at
               the actual identifier rather than 1:1. */
            int sub_line = span.line;
            int sub_col = span.col + 1; /* skip the opening quote */
            for (int rk = 0; rk <= i; rk++) {
                if (raw[rk] == '\n') {
                    sub_line++;
                    sub_col = 1;
                } else {
                    sub_col++;
                }
            }

            char *expr_src = xs_malloc(elen + 1);
            memcpy(expr_src, raw + i + 1, elen);
            expr_src[elen] = '\0';

            /* Pull off a `:spec` tail (Python-style format spec). The
               spec runs from the first top-level ':' to the end of the
               brace expression. We split at the unquoted top-level ':'
               only; nested type annotations / colons inside parens or
               quotes stay inside the expression. */
            char *fmt_spec = NULL;
            int paren_d = 0, brack_d = 0, brace_d = 0;
            for (int k = 0; k < elen; k++) {
                char ck = expr_src[k];
                if (ck == '"' || ck == '\'') {
                    char q = ck; k++;
                    while (k < elen && expr_src[k] != q) {
                        if (expr_src[k] == '\\' && k+1 < elen) k++;
                        k++;
                    }
                    continue;
                }
                if (ck == '(') paren_d++;
                else if (ck == ')') paren_d--;
                else if (ck == '[') brack_d++;
                else if (ck == ']') brack_d--;
                else if (ck == '{') brace_d++;
                else if (ck == '}') brace_d--;
                else if (ck == ':' && paren_d == 0 && brack_d == 0 && brace_d == 0) {
                    fmt_spec = xs_strdup(expr_src + k + 1);
                    expr_src[k] = '\0';
                    break;
                }
            }

            Lexer sub_lex;
            lexer_init(&sub_lex, expr_src, span.file);
            sub_lex.line = sub_line;
            sub_lex.col = sub_col;
            TokenArray sub_ta = lexer_tokenize(&sub_lex);
            Parser sub_p;
            parser_init(&sub_p, &sub_ta, span.file);
            Node *expr_node = parse_expr(&sub_p, 0);
            token_array_free(&sub_ta);
            comment_list_free(&sub_lex.comments);
            free(expr_src);

            if (expr_node && fmt_spec) {
                Node *callee = node_new(NODE_IDENT, span);
                callee->ident.name = xs_strdup("__xs_fmt");
                Node *spec_node = node_new(NODE_LIT_STRING, span);
                spec_node->lit_string.sval = fmt_spec;
                spec_node->lit_string.interpolated = 0;
                spec_node->lit_string.parts = nodelist_new();
                Node *call = node_new(NODE_CALL, span);
                call->call.callee = callee;
                call->call.args = nodelist_new();
                call->call.kwargs = nodepairlist_new();
                nodelist_push(&call->call.args, expr_node);
                nodelist_push(&call->call.args, spec_node);
                expr_node = call;
            } else if (fmt_spec) {
                free(fmt_spec);
            }

            if (expr_node) nodelist_push(&n->lit_string.parts, expr_node);
            i = j + 1;
        } else {
            piece[pi++] = c; i++;
        }
    }
    free(piece);
    return n;
}

/* primary */
static Node *parse_primary(Parser *p) {
    Token *tok = pp_peek(p, 0);
    Span span = tok->span;

    switch (tok->kind) {
    case TK_INT: {
        pp_advance(p);
        Token *suf = pp_peek(p, 0);
        /* Require the unit suffix to sit immediately after the number,
           with no intervening whitespace -- otherwise `count = 5\ns =
           ...` would silently turn the float on the previous line into
           a duration when the next line starts with `s`/`m`/`h`/etc. */
        int adjacent = (suf->span.offset == tok->span.offset + tok->span.length);
        if (adjacent && suf->kind == TK_IDENT && suf->sval) {
            int64_t ival = tok->ival;
            int64_t dur = 0;
            int matched = 0;
            if      (strcmp(suf->sval, "ns") == 0) { dur = ival; matched = 1; }
            else if (strcmp(suf->sval, "us") == 0) { dur = ival * 1000LL; matched = 1; }
            else if (strcmp(suf->sval, "ms") == 0) { dur = ival * 1000000LL; matched = 1; }
            else if (strcmp(suf->sval, "s") == 0)  { dur = ival * 1000000000LL; matched = 1; }
            else if (strcmp(suf->sval, "h") == 0)  { dur = ival * 3600LL * 1000000000LL; matched = 1; }
            else if (strcmp(suf->sval, "d") == 0)  { dur = ival * 86400LL * 1000000000LL; matched = 1; }
            else if (strcmp(suf->sval, "m") == 0) {
                dur = ival * 60LL * 1000000000LL;
                matched = 1;
                pp_advance(p);
                /* compound: 2m30s - peek for INT followed by 's' suffix */
                Token *nx2 = pp_peek(p, 0);
                if (nx2->kind == TK_INT) {
                    Token *suf2 = pp_peek(p, 1);
                    if (suf2->kind == TK_IDENT && suf2->sval && strcmp(suf2->sval, "s") == 0) {
                        dur += nx2->ival * 1000000000LL;
                        pp_advance(p); pp_advance(p);
                    }
                }
                Node *n = node_new(NODE_LIT_DURATION, span);
                n->lit_duration.ns = dur;
                return n;
            }
            else if (suf->sval[0] == 'm' && isdigit(suf->sval[1])) {
                /* compound: 2m30s lexed as INT + IDENT("m30s") */
                int64_t total = ival * 60LL * 1000000000LL;
                const char *rest = suf->sval + 1;
                char *endp = NULL;
                long long sub = strtoll(rest, &endp, 10);
                if (endp && (*endp == 's' || *endp == '\0')) {
                    total += (int64_t)sub * 1000000000LL;
                }
                pp_advance(p);
                Node *n = node_new(NODE_LIT_DURATION, span);
                n->lit_duration.ns = total;
                return n;
            }
            if (matched) {
                pp_advance(p);
                Node *n = node_new(NODE_LIT_DURATION, span);
                n->lit_duration.ns = dur;
                return n;
            }
        }
        Node *n = node_new(NODE_LIT_INT, span);
        n->lit_int.ival = tok->ival;
        return n;
    }
    case TK_BIGINT: {
        pp_advance(p);
        Node *n = node_new(NODE_LIT_BIGINT, span);
        n->lit_bigint.bigint_str = xs_strdup(tok->sval);
        return n;
    }
    case TK_FLOAT: {
        pp_advance(p);
        double numval = tok->fval;
        /* check for unit suffix literals */
        {
            Token *suf = pp_peek(p, 0);
            int adjacent = (suf->span.offset == tok->span.offset + tok->span.length);
            if (adjacent && suf->kind == TK_IDENT && suf->sval) {
                double scale = 0.0;
                int matched = 0;
                if      (strcmp(suf->sval, "ns") == 0) { scale = 1.0;          matched = 1; }
                else if (strcmp(suf->sval, "us") == 0) { scale = 1e3;          matched = 1; }
                else if (strcmp(suf->sval, "ms") == 0) { scale = 1e6;          matched = 1; }
                else if (strcmp(suf->sval, "s") == 0)  { scale = 1e9;          matched = 1; }
                else if (strcmp(suf->sval, "m") == 0)  { scale = 60e9;         matched = 1; }
                else if (strcmp(suf->sval, "h") == 0)  { scale = 3600e9;       matched = 1; }
                else if (strcmp(suf->sval, "d") == 0)  { scale = 86400e9;      matched = 1; }
                if (matched) {
                    pp_advance(p);
                    Node *n = node_new(NODE_LIT_DURATION, span);
                    n->lit_duration.ns = (int64_t)(numval * scale);
                    return n;
                }
            }
        }
        Node *n = node_new(NODE_LIT_FLOAT, span);
        n->lit_float.fval = tok->fval;
        return n;
    }
    case TK_BOOL: {
        pp_advance(p);
        Node *n = node_new(NODE_LIT_BOOL, span);
        n->lit_bool.bval = (int)tok->ival;
        return n;
    }
    case TK_NULL: {
        pp_advance(p);
        return node_new(NODE_LIT_NULL, span);
    }
    case TK_STRING: {
        pp_advance(p);
        return parse_string_literal(p, tok);
    }
    case TK_RAW_STRING: {
        pp_advance(p);
        Node *n = node_new(NODE_LIT_STRING, span);
        n->lit_string.sval = xs_strdup(tok->sval ? tok->sval : "");
        n->lit_string.interpolated = 0;
        n->lit_string.parts = nodelist_new();
        return n;
    }
    case TK_REGEX: {
        pp_advance(p);
        Node *n = node_new(NODE_LIT_REGEX, span);
        n->lit_regex.pattern = xs_strdup(tok->sval ? tok->sval : "");
        return n;
    }
    case TK_CHAR: {
        pp_advance(p);
        Node *n = node_new(NODE_LIT_CHAR, span);
        n->lit_char.cval = tok->sval ? tok->sval[0] : 0;
        return n;
    }
    case TK_SELF: {
        pp_advance(p);
        Node *n = node_new(NODE_IDENT, span);
        n->ident.name = xs_strdup("self");
        return n;
    }
    case TK_SUPER: {
        pp_advance(p);
        Node *n = node_new(NODE_IDENT, span);
        n->ident.name = xs_strdup("super");
        return n;
    }
    case TK_MACRO_BANG: {
        /* name!(args) */
        pp_advance(p);
        char *name = xs_strdup(tok->sval ? tok->sval : "");
        NodeList args = nodelist_new();
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *a = parse_expr(p, 0);
                if (a) nodelist_push(&args, a);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')' after macro args");
        }
        /* Store as a call to an ident node named the macro */
        Node *callee = node_new(NODE_IDENT, span);
        callee->ident.name = name;
        Node *n = node_new(NODE_CALL, span);
        n->call.callee = callee;
        n->call.args   = args;
        n->call.kwargs = nodepairlist_new();
        return n;
    }
    case TK_IDENT: {
        pp_advance(p);
        char *name = xs_strdup(tok->sval ? tok->sval : "");

        /* Unparenthesized single-arg arrow lambda: `x => body`. The
           parens form `(x) => body` is handled later in the LPAREN
           branch; this catches the bare-name shape that's common in
           `arr.map(x => x * 2)` / `xs |> filter(it => it > 0)`. The
           no_arrow_lambda guard mirrors the parens path so match arms
           (`pattern => result`) keep working. */
        if (!p->no_arrow_lambda && pp_check(p, TK_FAT_ARROW)) {
            pp_advance(p);
            ParamList pl = paramlist_new();
            Param pm = {0};
            pm.span = span;
            pm.name = xs_strdup(name);
            Node *id = node_new(NODE_IDENT, span);
            id->ident.name = xs_strdup(name);
            pm.pattern = id;
            paramlist_push(&pl, pm);
            free(name);
            Node *body;
            if (pp_check(p, TK_LBRACE)) body = parse_block(p);
            else                          body = parse_expr(p, 0);
            Node *n = node_new(NODE_LAMBDA, span);
            n->lambda.params = pl;
            n->lambda.body   = body;
            return n;
        }

        /* Scope resolution: Name::Item */
        if (pp_check(p, TK_COLON_COLON)) {
            /* build a chain */
            char **parts = xs_malloc(sizeof(char*));
            parts[0] = name;
            int nparts = 1;
            while (pp_match(p, TK_COLON_COLON)) {
                Token *part = pp_peek(p, 0);
                if (part->kind == TK_IDENT) {
                    pp_advance(p);
                    parts = xs_realloc(parts, (nparts+1)*sizeof(char*));
                    parts[nparts++] = xs_strdup(part->sval ? part->sval : "");
                } else break;
            }
            /* Scope resolution node */
            Node *n = node_new(NODE_SCOPE, span);
            n->scope.parts  = parts;
            n->scope.nparts = nparts;
            /* Check if followed by { for struct init (same line) */
            Token *next = pp_peek(p, 0);
            if (next->kind == TK_LBRACE && next->span.line == tok->span.line) {
                /* Build path string */
                int plen = 0;
                for (int i = 0; i < nparts; i++) plen += (int)strlen(parts[i]) + 2;
                char *path = xs_malloc(plen + 1);
                path[0] = '\0';
                for (int i = 0; i < nparts; i++) {
                    strcat(path, parts[i]);
                    if (i < nparts-1) strcat(path, "::");
                }
                /* Free scope node, parse struct init instead */
                node_free(n);
                pp_advance(p); /* consume { */
                Node *si = node_new(NODE_STRUCT_INIT, span);
                si->struct_init.path   = path;
                si->struct_init.fields = nodepairlist_new();
                si->struct_init.rest   = NULL;
                while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                    if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                        pp_advance(p);
                        si->struct_init.rest = parse_expr(p, 0);
                        if (pp_check(p, TK_COMMA)) pp_advance(p);
                        continue;
                    }
                    Token *fn_tok = pp_peek(p, 0);
                    char *fname = NULL;
                    /* Accept any keyword as a field name; in this slot
                       (after `Name {`) the syntax already disambiguates,
                       so the keyword meaning never applies. */
                    if (is_ident_or_kw(fn_tok->kind)) {
                        pp_advance(p);
                        fname = xs_strdup(fn_tok->sval ? fn_tok->sval :
                                          token_kind_name(fn_tok->kind));
                    } else break;
                    pp_expect(p, TK_COLON, "expected ':' in struct init");
                    Node *fval = parse_expr(p, 0);
                    nodepairlist_push(&si->struct_init.fields, fname, fval);
                    free(fname);
                    pp_match(p, TK_COMMA);
                }
                pp_expect(p, TK_RBRACE, "expected '}' in struct init");
                return si;
            }
            return n;
        }

        /* Struct init: Name { ... }: only on same line */
        Token *next = pp_peek(p, 0);
        if (next->kind == TK_LBRACE && next->span.line == tok->span.line) {
            /* Heuristic: check if next content looks like field: val */
            /* Peek ahead: if IDENT COLON or } or .. → struct init */
            Token *t1 = pp_peek(p, 1);
            Token *t2 = pp_peek(p, 2);
            int is_si = 0;
            int t1_field_ident = is_ident_or_kw(t1->kind);
            if (next->kind == TK_LBRACE) {
                if (t1->kind == TK_RBRACE) is_si = 1; /* empty */
                else if (t1_field_ident && t2->kind == TK_COLON) is_si = 1;
                else if (t1->kind == TK_DOTDOT || t1->kind == TK_DOTDOTDOT) is_si = 1;
            }
            if (is_si) {
                pp_advance(p); /* consume { */
                Node *si = node_new(NODE_STRUCT_INIT, span);
                si->struct_init.path   = name;
                si->struct_init.fields = nodepairlist_new();
                si->struct_init.rest   = NULL;
                while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                    if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                        pp_advance(p);
                        si->struct_init.rest = parse_expr(p, 0);
                        if (pp_check(p, TK_COMMA)) pp_advance(p);
                        continue;
                    }
                    Token *fn_tok = pp_peek(p, 0);
                    char *fname = NULL;
                    if (is_ident_or_kw(fn_tok->kind)) {
                        pp_advance(p);
                        fname = xs_strdup(fn_tok->sval ? fn_tok->sval :
                                          token_kind_name(fn_tok->kind));
                    } else break;
                    pp_expect(p, TK_COLON, "expected ':' in struct init");
                    Node *fval = parse_expr(p, 0);
                    nodepairlist_push(&si->struct_init.fields, fname, fval);
                    free(fname);
                    pp_match(p, TK_COMMA);
                }
                pp_expect(p, TK_RBRACE, "expected '}' in struct init");
                return si;
            }
        }

        Node *n = node_new(NODE_IDENT, span);
        n->ident.name = name;
        return n;
    }

    /* spawn Name → Name() */
    case TK_SPAWN: {
        pp_advance(p);
        Token *name_tok = pp_peek(p, 0);
        if (name_tok->kind == TK_IDENT) {
            pp_advance(p);
            Node *callee = node_new(NODE_IDENT, name_tok->span);
            callee->ident.name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            Node *call = node_new(NODE_CALL, span);
            call->call.callee = callee;
            call->call.args = nodelist_new();
            call->call.kwargs = nodepairlist_new();
            return call;
        }
        /* fallthrough: treat spawn as identifier */
        Node *n = node_new(NODE_IDENT, span);
        n->ident.name = xs_strdup("spawn");
        return n;
    }

    /* contextual keywords usable as identifiers */
    case TK_TYPE:
    case TK_FROM:
    case TK_AS:
    case TK_EXPORT:
    case TK_ASYNC:
    case TK_AWAIT:
    case TK_PUB:
    case TK_STATIC: {
        pp_advance(p);
        Node *n = node_new(NODE_IDENT, span);
        /* Keyword tokens drop sval; without the spelling fallback the
           IDENT carries an empty name and sema/runtime can't resolve
           it. Hits the `import async; async.channel()` shape. */
        n->ident.name = xs_strdup(tok->sval ? tok->sval :
                                  token_kind_name(tok->kind));
        return n;
    }

    case TK_YIELD: {
        pp_advance(p);
        /* yield expr: parse expression if not at statement boundary */
        Node *val = NULL;
        Token *nt = pp_peek(p, 0);
        if (nt->kind != TK_NEWLINE && nt->kind != TK_SEMICOLON &&
            nt->kind != TK_RBRACE && nt->kind != TK_EOF &&
            nt->kind != TK_RETURN && nt->kind != TK_LET &&
            nt->kind != TK_VAR && nt->kind != TK_CONST &&
            nt->kind != TK_FN && nt->kind != TK_FOR &&
            nt->kind != TK_WHILE && nt->kind != TK_IF &&
            nt->kind != TK_BREAK && nt->kind != TK_CONTINUE &&
            nt->kind != TK_THROW &&
            !(nt->span.line > span.line)) {
            val = parse_expr(p, 0);
        }
        Node *n = node_new(NODE_YIELD, span);
        n->yield_.value = val;
        return n;
    }

    /* Parenthesized, tuple, or lambda */
    case TK_LPAREN: {
        int paren_line = span.line;
        pp_advance(p);
        /* empty () = empty tuple */
        if (pp_match(p, TK_RPAREN)) {
            Node *n = node_new(NODE_LIT_TUPLE, span);
            n->lit_array.elems = nodelist_new();
            return n;
        }
        Node *first = parse_expr(p, 0);
        /* Check for lambda: (params) => body */
        /* Heuristic: if we see , or ) then => it's a lambda */
        /* For now just collect exprs and if => appears, it's lambda */
        if (pp_match(p, TK_COMMA)) {
            /* tuple or lambda params */
            NodeList elems = nodelist_new();
            if (first) nodelist_push(&elems, first);
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *e = parse_expr(p, 0);
                if (e) nodelist_push(&elems, e);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')'");
            if (!p->no_arrow_lambda && pp_check(p, TK_FAT_ARROW)) {
                /* Lambda: (params) => body */
                pp_advance(p);
                ParamList pl = paramlist_new();
                for (int i = 0; i < elems.len; i++) {
                    Node *pn = elems.items[i];
                    Param pm = {0};
                    pm.span = pn->span;
                    if (VAL_TAG(pn) == NODE_IDENT) {
                        pm.name    = xs_strdup(pn->ident.name);
                        pm.pattern = pn; /* owns it */
                        elems.items[i] = NULL;
                    } else {
                        pm.pattern = pn;
                        elems.items[i] = NULL;
                    }
                    paramlist_push(&pl, pm);
                }
                free(elems.items);
                Node *body;
                if (pp_check(p, TK_LBRACE)) body = parse_block(p);
                else                          body = parse_expr(p, 0);
                Node *n = node_new(NODE_LAMBDA, span);
                n->lambda.params = pl;
                n->lambda.body   = body;
                return n;
            }
            Node *n = node_new(NODE_LIT_TUPLE, span);
            n->lit_array.elems = elems;
            return n;
        }
        pp_expect_ex(p, TK_RPAREN, "expected ')'", paren_line);
        /* Check for single-param lambda: (x) => body: suppressed in pattern expr context */
        if (!p->no_arrow_lambda && pp_check(p, TK_FAT_ARROW)) {
            pp_advance(p);
            ParamList pl = paramlist_new();
            if (first) {
                Param pm = {0};
                pm.span = first->span;
                if (VAL_TAG(first) == NODE_IDENT) {
                    pm.name = xs_strdup(first->ident.name);
                    pm.pattern = first; first = NULL;
                } else {
                    pm.pattern = first; first = NULL;
                }
                paramlist_push(&pl, pm);
            }
            Node *body;
            if (pp_check(p, TK_LBRACE)) body = parse_block(p);
            else                          body = parse_expr(p, 0);
            Node *n = node_new(NODE_LAMBDA, span);
            n->lambda.params = pl;
            n->lambda.body   = body;
            node_free(first);
            return n;
        }
        return first ? first : node_new(NODE_LIT_NULL, span);
    }

    /* Array literal */
    case TK_LBRACKET: {
        int bracket_line = span.line;
        pp_advance(p);
        (void)bracket_line;
        NodeList elems = nodelist_new();
        Node *first = NULL;
        if (!pp_check2(p, TK_RBRACKET, TK_EOF)) {
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                pp_advance(p);
                Node *sp = node_new(NODE_SPREAD, pp_peek(p,0)->span);
                sp->spread.expr = parse_expr(p, 0);
                nodelist_push(&elems, sp);
            } else {
                first = parse_expr(p, 0);
            }
        }
        /* [expr; N] repeat syntax */
        if (first && pp_check(p, TK_SEMICOLON)) {
            pp_advance(p);
            Node *cnt_expr = parse_expr(p, 0);
            pp_expect(p, TK_RBRACKET, "expected ']'");
            Node *n = node_new(NODE_LIT_ARRAY, span);
            n->lit_array.elems = nodelist_new();
            n->lit_array.repeat_val = first;
            n->lit_array.repeat_cnt = (cnt_expr && VAL_TAG(cnt_expr) == NODE_LIT_INT)
                                       ? cnt_expr->lit_int.ival : 0;
            if (cnt_expr) node_free(cnt_expr);
            return n;
        }
        /* [expr for ident in iter (for ident in iter)* (if cond)?] list comprehension */
        if (first && pp_check(p, TK_FOR)) {
            pp_advance(p); /* consume 'for' */
            Node *n = node_new(NODE_LIST_COMP, span);
            n->list_comp.element = first;
            n->list_comp.clause_pats  = nodelist_new();
            n->list_comp.clause_iters = nodelist_new();
            n->list_comp.clause_conds = nodelist_new();
            /* Parse first for clause */
            Node *pat = parse_loop_pattern(p);
            pp_expect(p, TK_IN, "expected 'in' in list comprehension");
            Node *iter = parse_expr(p, 0);
            nodelist_push(&n->list_comp.clause_pats, pat);
            nodelist_push(&n->list_comp.clause_iters, iter);
            nodelist_push(&n->list_comp.clause_conds, NULL);
            /* Parse additional for clauses */
            while (pp_check(p, TK_FOR)) {
                pp_advance(p);
                Node *pat2 = parse_loop_pattern(p);
                pp_expect(p, TK_IN, "expected 'in' in list comprehension");
                Node *iter2 = parse_expr(p, 0);
                nodelist_push(&n->list_comp.clause_pats, pat2);
                nodelist_push(&n->list_comp.clause_iters, iter2);
                nodelist_push(&n->list_comp.clause_conds, NULL);
            }
            /* Optional if guard (applies to innermost clause) */
            if (pp_match(p, TK_IF)) {
                Node *cond = parse_expr(p, 0);
                /* Replace last NULL cond with the actual condition */
                int last = n->list_comp.clause_conds.len - 1;
                n->list_comp.clause_conds.items[last] = cond;
            }
            pp_expect(p, TK_RBRACKET, "expected ']' after list comprehension");
            return n;
        }
        if (first) nodelist_push(&elems, first);
        while (!pp_check2(p, TK_RBRACKET, TK_EOF)) {
            if (!pp_match(p, TK_COMMA)) break;
            if (pp_check(p, TK_RBRACKET)) break;
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                pp_advance(p);
                Node *sp = node_new(NODE_SPREAD, pp_peek(p,0)->span);
                sp->spread.expr = parse_expr(p, 0);
                nodelist_push(&elems, sp);
            } else {
                Node *e = parse_expr(p, 0);
                if (e) nodelist_push(&elems, e);
            }
        }
        pp_expect(p, TK_RBRACKET, "expected ']'");
        Node *n = node_new(NODE_LIT_ARRAY, span);
        n->lit_array.elems = elems;
        return n;
    }

    /* Block or map */
    case TK_HASH_LBRACE:
    case TK_LBRACE: {
        /* `#{...}` is an unambiguous map literal -- skip the heuristic.
           Plain `{...}` uses lookahead: IDENT/STRING followed by `:`,
           `...spread`, or `}` (empty map). */
        int is_map = (tok->kind == TK_HASH_LBRACE);
        Token *t1 = pp_peek(p, 1);
        Token *t2 = pp_peek(p, 2);
        if (!is_map && t1->kind == TK_RBRACE) { is_map = 1; /* empty {} → empty map */
        } else if (!is_map && t1->kind == TK_IDENT && t2->kind == TK_COLON) {
            is_map = 1;
        } else if (!is_map && t2->kind == TK_COLON &&
                   t1->kind >= TK_IF && t1->kind <= TK_LOAD) {
            /* keyword as bareword map key; the parser body lifts these
               into string keys. lets you put `handle:` / `fn:` /
               `type:` on a config map without quoting. */
            is_map = 1;
        } else if (!is_map && t1->kind == TK_STRING && t2->kind == TK_COLON) {
            is_map = 1;
        } else if (!is_map && (t1->kind == TK_DOTDOT || t1->kind == TK_DOTDOTDOT)) {
            is_map = 1; /* spread in map: {...base, ...} */
        }
        if (is_map) {
            pp_advance(p); /* consume { */
            NodeList keys = nodelist_new();
            NodeList vals = nodelist_new();
            /* Parse first key:value pair (or spread) */
            if (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                    /* Spread in map: ...expr */
                    pp_advance(p);
                    Node *sp = node_new(NODE_SPREAD, pp_peek(p,0)->span);
                    sp->spread.expr = parse_expr(p, 0);
                    nodelist_push(&keys, sp);
                    nodelist_push(&vals, NULL);
                } else {
                    Token *kt = pp_peek(p, 0);
                    Node *k = NULL;
                    /* Any keyword followed by `:` is a bareword key.
                       Names like `handle`, `fn`, `effect`, `type`, etc.
                       are common enough on framework-style maps that
                       requiring quotes for them is just friction.
                       The lexer keeps the source text on tok->sval, so
                       we use that rather than the printable form (which
                       isn't filled in for every keyword). */
                    if (pp_peek(p, 1)->kind == TK_COLON &&
                        kt->kind >= TK_IF && kt->kind <= TK_LOAD) {
                        const char *kw = kt->sval ? kt->sval
                                                  : token_kind_name(kt->kind);
                        pp_advance(p);
                        k = node_new(NODE_LIT_STRING, kt->span);
                        k->lit_string.sval = xs_strdup(kw);
                        k->lit_string.parts = nodelist_new();
                        k->lit_string.interpolated = 0;
                    } else {
                        k = parse_expr(p, 0);
                    }
                    pp_expect(p, TK_COLON, "expected ':' in map");
                    Node *v = parse_expr(p, 0);
                    /* Check for map comprehension: {k: v for pat in iter} */
                    if (pp_check(p, TK_FOR)) {
                        Node *n = node_new(NODE_MAP_COMP, span);
                        n->map_comp.key   = k;
                        n->map_comp.value = v;
                        n->map_comp.clause_pats  = nodelist_new();
                        n->map_comp.clause_iters = nodelist_new();
                        n->map_comp.clause_conds = nodelist_new();
                        while (pp_check(p, TK_FOR)) {
                            pp_advance(p); /* consume 'for' */
                            Node *pat = parse_loop_pattern(p);
                            pp_expect(p, TK_IN, "expected 'in' in map comprehension");
                            Node *iter = parse_expr(p, 0);
                            nodelist_push(&n->map_comp.clause_pats, pat);
                            nodelist_push(&n->map_comp.clause_iters, iter);
                            if (pp_match(p, TK_IF)) {
                                Node *cond = parse_expr(p, 0);
                                nodelist_push(&n->map_comp.clause_conds, cond);
                            } else {
                                nodelist_push(&n->map_comp.clause_conds, NULL);
                            }
                        }
                        pp_expect(p, TK_RBRACE, "expected '}' after map comprehension");
                        return n;
                    }
                    if (k) nodelist_push(&keys, k);
                    if (v) nodelist_push(&vals, v);
                }
                pp_match(p, TK_COMMA);
            }
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                    /* Spread in map: ...expr */
                    pp_advance(p);
                    Node *sp = node_new(NODE_SPREAD, pp_peek(p,0)->span);
                    sp->spread.expr = parse_expr(p, 0);
                    nodelist_push(&keys, sp);
                    nodelist_push(&vals, NULL);
                } else {
                    Token *kt2 = pp_peek(p, 0);
                    Node *k = NULL;
                    if (pp_peek(p, 1)->kind == TK_COLON &&
                        kt2->kind >= TK_IF && kt2->kind <= TK_LOAD) {
                        const char *kw = kt2->sval ? kt2->sval
                                                   : token_kind_name(kt2->kind);
                        pp_advance(p);
                        k = node_new(NODE_LIT_STRING, kt2->span);
                        k->lit_string.sval = xs_strdup(kw);
                        k->lit_string.parts = nodelist_new();
                        k->lit_string.interpolated = 0;
                    } else {
                        k = parse_expr(p, 0);
                    }
                    pp_expect(p, TK_COLON, "expected ':' in map");
                    Node *v = parse_expr(p, 0);
                    if (k) nodelist_push(&keys, k);
                    if (v) nodelist_push(&vals, v);
                }
                pp_match(p, TK_COMMA);
            }
            pp_expect(p, TK_RBRACE, "expected '}' in map");
            Node *n = node_new(NODE_LIT_MAP, span);
            n->lit_map.keys = keys;
            n->lit_map.vals = vals;
            return n;
        }
        return parse_block(p);
    }

    /* fn(params) { body } anonymous function */
    case TK_FN: {
        pp_advance(p);
        /* fn* generator marker */
        int fn_is_gen = 0;
        if (pp_check(p, TK_STAR)) { fn_is_gen = 1; pp_advance(p); }
        /* optional name (ignored for anonymous) */
        if (pp_peek(p,0)->kind == TK_IDENT) pp_advance(p);
        /* optional generics */
        if (pp_check(p, TK_LT)) {
            int d2=1; pp_advance(p);
            while (!pp_at_end(p)&&d2>0){
                if(pp_peek(p,0)->kind==TK_LT) d2++;
                else if(pp_peek(p,0)->kind==TK_GT) d2--;
                pp_advance(p);
            }
        }
        ParamList pl = paramlist_new();
        if (pp_match(p, TK_LPAREN)) {
            pl = parse_params(p);
            pp_expect(p, TK_RPAREN, "expected ')'");
        }
        /* skip return type */
        if (pp_check(p, TK_ARROW)) {
            pp_advance(p);
            skip_type(p);
        }
        Node *body;
        if (pp_check(p, TK_LBRACE)) body = parse_block(p);
        else body = parse_expr(p, 0);
        Node *n = node_new(NODE_LAMBDA, span);
        n->lambda.params       = pl;
        n->lambda.body         = body;
        n->lambda.is_generator = fn_is_gen;
        return n;
    }

    /* Pipe lambda: |x, y| body */
    case TK_PIPE: {
        pp_advance(p);
        ParamList pl = paramlist_new();
        while (!pp_check2(p, TK_PIPE, TK_EOF)) {
            Token *pt = pp_peek(p, 0);
            Param pm = {0};
            pm.span = pt->span;
            if (pt->kind == TK_IDENT) {
                pp_advance(p);
                pm.name = xs_strdup(pt->sval ? pt->sval : "");
                Node *pn = node_new(NODE_PAT_IDENT, pt->span);
                pn->pat_ident.name = xs_strdup(pm.name);
                pn->pat_ident.mutable = 0;
                pm.pattern = pn;
            } else if (pt->kind == TK_MUT) {
                pp_advance(p);
                Token *nt = pp_peek(p, 0);
                pp_advance(p);
                pm.name = xs_strdup(nt->sval ? nt->sval : "");
                Node *pn = node_new(NODE_PAT_IDENT, nt->span);
                pn->pat_ident.name = xs_strdup(pm.name);
                pn->pat_ident.mutable = 1;
                pm.pattern = pn;
            }
            /* optional type annotation: |x: int| — skip the type expr */
            if (pp_match(p, TK_COLON)) {
                int depth = 0;
                while (!pp_at_end(p)) {
                    Token *tt = pp_peek(p, 0);
                    if (depth == 0 && (tt->kind == TK_COMMA || tt->kind == TK_PIPE)) break;
                    if (tt->kind == TK_LT || tt->kind == TK_LBRACKET ||
                        tt->kind == TK_LPAREN) depth++;
                    else if (tt->kind == TK_GT || tt->kind == TK_RBRACKET ||
                             tt->kind == TK_RPAREN) depth--;
                    pp_advance(p);
                }
            }
            paramlist_push(&pl, pm);
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_PIPE, "expected '|' after lambda params");
        /* optional return type: |x| -> int { ... } — skip it */
        if (pp_match(p, TK_ARROW)) {
            int depth = 0;
            while (!pp_at_end(p)) {
                Token *tt = pp_peek(p, 0);
                if (depth == 0 && (tt->kind == TK_LBRACE)) break;
                if (depth == 0 && tt->kind == TK_NEWLINE) break;
                if (tt->kind == TK_LT || tt->kind == TK_LBRACKET ||
                    tt->kind == TK_LPAREN) depth++;
                else if (tt->kind == TK_GT || tt->kind == TK_RBRACKET ||
                         tt->kind == TK_RPAREN) depth--;
                pp_advance(p);
            }
        }
        Node *body;
        if (pp_check(p, TK_LBRACE)) body = parse_block(p);
        else                          body = parse_expr(p, 0);
        Node *n = node_new(NODE_LAMBDA, span);
        n->lambda.params = pl;
        n->lambda.body   = body;
        return n;
    }

    /* || empty lambda */
    case TK_OP_OR: {
        pp_advance(p);
        Node *body;
        if (pp_check(p, TK_LBRACE)) body = parse_block(p);
        else                          body = parse_expr(p, 0);
        Node *n = node_new(NODE_LAMBDA, span);
        n->lambda.params = paramlist_new();
        n->lambda.body   = body;
        return n;
    }

    default: {
        /* phase 2: try plugin expression handlers before error */
        Token *t = pp_peek(p, 0);
        if (g_plugin_is_keyword && t->kind == TK_IDENT && t->sval &&
            g_plugin_is_keyword(t->sval)) {
            int saved_pos = p->pos;
            pp_advance(p);
            /* disambiguation: check for !<plugin-id> after keyword */
            const char *saved_forced = g_plugin_forced_id;
            if (pp_peek(p, 0)->kind == TK_OP_NOT) {
                Token *lt = pp_peek(p, 1);
                if (lt->kind == TK_LT) {
                    Token *id_tok = pp_peek(p, 2);
                    Token *gt_tok = pp_peek(p, 3);
                    if (id_tok->kind == TK_IDENT && id_tok->sval && gt_tok->kind == TK_GT) {
                        g_plugin_forced_id = id_tok->sval;
                        pp_advance(p); /* skip ! */
                        pp_advance(p); /* skip < */
                        pp_advance(p); /* skip ident */
                        pp_advance(p); /* skip > */
                    }
                }
            }
            Node *plugin_node = g_plugin_try_syntax_expr_handler ?
                g_plugin_try_syntax_expr_handler(p, t) : NULL;
            g_plugin_forced_id = saved_forced;
            if (plugin_node) return plugin_node;
            p->pos = saved_pos;
            t = pp_peek(p, 0);
        }
        const char *tname = safe_tok_text(t);
        const char *suggestion = (t->kind == TK_IDENT && t->sval)
                               ? suggest_keyword(t->sval) : NULL;
        if (suggestion)
            parse_error_at(p, t->span, "P0020",
                           "unexpected token '%s' (did you mean '%s'?)", tname, suggestion);
        else
            parse_error_at(p, t->span, "P0001",
                           "unexpected token '%s'", tname);
        pp_advance(p);
        return node_new(NODE_LIT_NULL, span);
    }
    } /* switch */
} /* parse_primary */

static void parse_call_args(Parser *p, NodeList *args, NodePairList *kwargs) {
    while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
        /* Spread: ...expr or ..expr */
        if (pp_check(p, TK_DOTDOTDOT) || pp_check(p, TK_DOTDOT)) {
            pp_advance(p);
            Node *sp = node_new(NODE_SPREAD, pp_peek(p,0)->span);
            sp->spread.expr = parse_expr(p, 0);
            nodelist_push(args, sp);
        }
        /* Kwarg: name: val */
        else if (pp_peek(p,0)->kind == TK_IDENT && pp_peek(p,1)->kind == TK_COLON) {
            Token *kt = pp_advance(p);
            pp_advance(p); /* : */
            char *kn = xs_strdup(kt->sval ? kt->sval : "");
            Node *kv = parse_expr(p, 0);
            nodepairlist_push(kwargs, kn, kv);
            free(kn);
        } else {
            Node *a = parse_expr(p, 0);
            if (a) nodelist_push(args, a);
        }
        if (!pp_match(p, TK_COMMA)) break;
    }
}

/* postfix */
static Node *parse_postfix(Parser *p, Node *left) {
    while (1) {
        Token *tok = pp_peek(p, 0);

        /* Field access / method call: expr.name */
        if (tok->kind == TK_DOT) {
            pp_advance(p);
            Token *name_tok = pp_peek(p, 0);
            if (is_ident_or_kw(name_tok->kind)) {
                pp_advance(p);
                char *fname = xs_strdup(name_tok->sval ? name_tok->sval :
                                        token_kind_name(name_tok->kind));
                Token *paren = pp_peek(p, 0);
                if (paren->kind == TK_LPAREN && paren->span.line == name_tok->span.line) {
                    pp_advance(p);
                    NodeList args = nodelist_new();
                    NodePairList kwargs = nodepairlist_new();
                    parse_call_args(p, &args, &kwargs);
                    pp_expect(p, TK_RPAREN, "expected ')'");
                    Node *n = node_new(NODE_METHOD_CALL, tok->span);
                    n->method_call.obj      = left;
                    n->method_call.method   = fname;
                    n->method_call.args     = args;
                    n->method_call.kwargs   = kwargs;
                    n->method_call.optional = 0;
                    left = n;
                } else {
                    Node *n = node_new(NODE_FIELD, tok->span);
                    n->field.obj      = left;
                    n->field.name     = fname;
                    n->field.optional = 0;
                    left = n;
                }
                continue;
            }
            /* dot followed by int: tuple indexing (0.0 etc): treat as field */
            if (name_tok->kind == TK_INT) {
                pp_advance(p);
                char buf[32]; snprintf(buf,32,"%lld",(long long)name_tok->ival);
                Node *n = node_new(NODE_FIELD, tok->span);
                n->field.obj  = left;
                n->field.name = xs_strdup(buf);
                n->field.optional = 0;
                left = n;
                continue;
            }
            break;
        }

        /* Optional chaining ?.field or ?.method() */
        if (tok->kind == TK_QUESTION_DOT) {
            pp_advance(p);
            Token *name_tok = pp_peek(p, 0);
            if (is_ident_or_kw(name_tok->kind)) {
                pp_advance(p);
                char *fname = xs_strdup(name_tok->sval ? name_tok->sval :
                                        token_kind_name(name_tok->kind));
                if (pp_check(p, TK_LPAREN)) {
                    pp_advance(p);
                    NodeList args = nodelist_new();
                    NodePairList kwargs = nodepairlist_new();
                    parse_call_args(p, &args, &kwargs);
                    pp_expect(p, TK_RPAREN, "expected ')'");
                    Node *n = node_new(NODE_METHOD_CALL, tok->span);
                    n->method_call.obj      = left;
                    n->method_call.method   = fname;
                    n->method_call.args     = args;
                    n->method_call.kwargs   = kwargs;
                    n->method_call.optional = 1;
                    left = n;
                } else {
                    Node *n = node_new(NODE_FIELD, tok->span);
                    n->field.obj      = left;
                    n->field.name     = fname;
                    n->field.optional = 1;
                    left = n;
                }
                continue;
            }
            break;
        }

        /* Index: expr[i] */
        if (tok->kind == TK_LBRACKET) {
            if (tok->span.line > left->span.end_line) break;
            pp_advance(p);
            Node *idx;
            /* Open-start slice `arr[..end]` or `arr[..]`: synthesize a
               range with a NULL start so the runtime/compiler treats
               it as "from 0". Without this the bare `..` token fails
               to parse as an expression. */
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTEQ)) {
                Token *rt = pp_peek(p, 0);
                int incl = (rt->kind == TK_DOTDOTEQ);
                Span rs = rt->span;
                pp_advance(p);
                Node *end = NULL;
                if (!pp_check(p, TK_RBRACKET))
                    end = parse_expr(p, 3);
                idx = node_new(NODE_RANGE, rs);
                idx->range.start = NULL;
                idx->range.end = end;
                idx->range.inclusive = incl;
            } else {
                idx = parse_expr(p, 0);
            }
            pp_expect(p, TK_RBRACKET, "expected ']'");
            Node *n = node_new(NODE_INDEX, tok->span);
            n->index.obj   = left;
            n->index.index = idx;
            left = n;
            continue;
        }

        /* Call: expr(args) with optional trailing block */
        if (tok->kind == TK_LPAREN) {
            if (tok->span.line > left->span.end_line) break;
            pp_advance(p);
            NodeList args = nodelist_new();
            NodePairList kwargs = nodepairlist_new();
            parse_call_args(p, &args, &kwargs);
            pp_expect(p, TK_RPAREN, "expected ')'");
            /* Note: trailing block syntax is handled at the statement level, not here,
               to avoid conflicts with for/if/while bodies */
            Node *n = node_new(NODE_CALL, tok->span);
            n->call.callee = left;
            n->call.args   = args;
            n->call.kwargs = kwargs;
            left = n;
            continue;
        }

        /* Try operator: expr?: propagate Err early return */
        if (tok->kind == TK_QUESTION &&
            pp_peek(p, 1)->kind != TK_QUESTION &&
            pp_peek(p, 1)->kind != TK_DOT) {
            Span qspan = tok->span;
            pp_advance(p);
            Node *prop = node_new(NODE_UNARY, qspan);
            prop->unary.op[0] = '?';
            prop->unary.op[1] = '\0';
            prop->unary.expr = left;
            prop->unary.prefix = 0;
            left = prop;
            continue;
        }

        break;
    }
    return left;
}

static Node *parse_if(Parser *p);
static Node *parse_while(Parser *p);
static Node *parse_for(Parser *p);
static Node *parse_loop_expr(Parser *p);
static Node *parse_match(Parser *p);
static Node *parse_try(Parser *p);

static Node *parse_prefix(Parser *p) {
    Token *tok = pp_peek(p, 0);
    Span span = tok->span;

    if (tok->kind == TK_MINUS || tok->kind == TK_OP_NOT ||
        tok->kind == TK_TILDE || tok->kind == TK_NOT) {
        pp_advance(p);
        Node *expr = parse_prefix(p);
        const char *op = (tok->kind == TK_NOT) ? "!" :
                         (tok->kind == TK_OP_NOT) ? "!" :
                         (tok->kind == TK_TILDE)  ? "~" : "-";
        Node *n = node_new(NODE_UNARY, span);
        strncpy(n->unary.op, op, sizeof(n->unary.op)-1);
        n->unary.expr   = expr;
        n->unary.prefix = 1;
        return n;
    }

    /* phase 3: check override hooks before built-in keyword parsing */
    if (g_plugin_try_parser_override) {
        const char *kw = NULL;
        if (tok->kind == TK_IF) kw = "if";
        else if (tok->kind == TK_MATCH) kw = "match";
        else if (tok->kind == TK_WHILE) kw = "while";
        else if (tok->kind == TK_FOR) kw = "for";
        if (kw) {
            Node *override_result = g_plugin_try_parser_override(p, kw);
            if (override_result) return override_result;
        }
    }
    if (tok->kind == TK_IF)     return parse_if(p);
    if (tok->kind == TK_MATCH)  return parse_match(p);
    if (tok->kind == TK_WHILE)  return parse_while(p);
    if (tok->kind == TK_FOR)    return parse_for(p);
    if (tok->kind == TK_LOOP)   return parse_loop_expr(p);
    if (tok->kind == TK_UNSAFE) {
        pp_advance(p);  /* consume 'unsafe' */
        Node *blk = parse_block(p);
        if (blk) blk->block.is_unsafe = 1;
        return blk;
    }
    if (tok->kind == TK_TRY)    return parse_try(p);

    if (tok->kind == TK_RETURN) {
        pp_advance(p);
        Node *val = NULL;
        Token *next = pp_peek(p, 0);
        if (next->kind != TK_SEMICOLON && next->kind != TK_RBRACE &&
            next->kind != TK_EOF) {
            val = parse_expr(p, 0);
        }
        Node *n = node_new(NODE_RETURN, span);
        n->ret.value = val;
        return n;
    }
    if (tok->kind == TK_BREAK) {
        pp_advance(p);
        char *lbl = NULL;
        Node *val = NULL;
        Token *next = pp_peek(p, 0);
        /* 'break label': if next is an ident followed by statement boundary */
        if (next->kind == TK_IDENT) {
            Token *after = pp_peek(p, 1);
            if (after->kind == TK_SEMICOLON || after->kind == TK_RBRACE ||
                after->kind == TK_EOF || after->span.line > next->span.line) {
                lbl = xs_strdup(next->sval ? next->sval : "");
                pp_advance(p);
                next = pp_peek(p, 0);
            }
        }
        if (!lbl && next->kind != TK_SEMICOLON && next->kind != TK_RBRACE &&
            next->kind != TK_EOF) {
            /* only parse value if not immediately followed by terminator */
            val = parse_expr(p, 0);
        }
        Node *n = node_new(NODE_BREAK, span);
        n->brk.label = lbl;
        n->brk.value = val;
        return n;
    }
    if (tok->kind == TK_CONTINUE) {
        pp_advance(p);
        char *lbl = NULL;
        Token *next = pp_peek(p, 0);
        /* 'continue label' */
        if (next->kind == TK_IDENT) {
            Token *after = pp_peek(p, 1);
            if (after->kind == TK_SEMICOLON || after->kind == TK_RBRACE ||
                after->kind == TK_EOF || after->span.line > next->span.line) {
                lbl = xs_strdup(next->sval ? next->sval : "");
                pp_advance(p);
            }
        }
        Node *n = node_new(NODE_CONTINUE, span);
        n->cont.label = lbl;
        return n;
    }
    if (tok->kind == TK_THROW) {
        pp_advance(p);
        Node *val = parse_expr(p, 0);
        Node *n = node_new(NODE_THROW, span);
        n->throw_.value = val;
        return n;
    }

    /* do { block } expression */
    if (tok->kind == TK_DO) {
        pp_advance(p);
        Node *body = parse_block(p);
        Node *n = node_new(NODE_DO_EXPR, span);
        n->do_expr.body = body;
        return n;
    }

    /* with expr as name { block } */
    if (tok->kind == TK_WITH) {
        pp_advance(p);
        Node *expr = parse_prefix(p);
        char *name = NULL;
        if (pp_check(p, TK_AS)) {
            pp_advance(p);
            Token *nt = pp_expect(p, TK_IDENT, "expected name after 'as'");
            name = xs_strdup(nt->sval ? nt->sval : "_");
        }
        Node *body = parse_block(p);
        Node *n = node_new(NODE_WITH, span);
        n->with_.expr = expr;
        n->with_.name = name;
        n->with_.body = body;
        return n;
    }

    if (tok->kind == TK_HANDLE) {
        return parse_handle(p);
    }

    /* await expr  (unary prefix; binds tighter than binary operators
       so `await a + 1` reads as `(await a) + 1` like every other
       async language). */
    if (tok->kind == TK_AWAIT) {
        pp_advance(p);
        Node *expr = parse_expr(p, 14);
        Node *n = node_new(NODE_AWAIT, span);
        n->await_.expr = expr;
        return n;
    }

    /* nursery { body } */
    if (tok->kind == TK_NURSERY) {
        pp_advance(p);
        Node *body = parse_block(p);
        Node *n = node_new(NODE_NURSERY, span);
        n->nursery_.body = body;
        return n;
    }

    /* spawn expr */
    if (tok->kind == TK_SPAWN) {
        pp_advance(p);
        Node *expr = parse_expr(p, 0);
        Node *n = node_new(NODE_SPAWN, span);
        n->spawn_.expr = expr;
        return n;
    }

    /* perform Effect.op(args) */
    if (tok->kind == TK_PERFORM) {
        pp_advance(p);
        /* Expect: EffectName.opName(args) */
        char *effect_name = NULL;
        char *op_name     = NULL;
        NodeList args     = nodelist_new();

        Token *eff_tok = pp_peek(p, 0);
        if (eff_tok->kind == TK_IDENT) {
            effect_name = xs_strdup(eff_tok->sval ? eff_tok->sval : "");
            pp_advance(p);
        } else {
            effect_name = xs_strdup("__unknown__");
        }
        if (pp_match(p, TK_DOT)) {
            Token *op_tok = pp_peek(p, 0);
            if (op_tok->kind == TK_IDENT) {
                op_name = xs_strdup(op_tok->sval ? op_tok->sval : "");
                pp_advance(p);
            } else {
                op_name = xs_strdup("__op__");
            }
        } else {
            op_name = xs_strdup(effect_name);
            free(effect_name);
            effect_name = xs_strdup("__effect__");
        }
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *a = parse_expr(p, 0);
                if (a) nodelist_push(&args, a);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')'");
        }
        Node *n = node_new(NODE_PERFORM, span);
        n->perform.effect_name = effect_name;
        n->perform.op_name     = op_name;
        n->perform.args        = args;
        return n;
    }

    /* resume val */
    if (tok->kind == TK_RESUME) {
        pp_advance(p);
        Node *val = NULL;
        Token *next = pp_peek(p, 0);
        if (next->kind != TK_SEMICOLON && next->kind != TK_RBRACE &&
            next->kind != TK_EOF && next->kind != TK_NEWLINE) {
            val = parse_expr(p, 0);
        }
        Node *n = node_new(NODE_RESUME, span);
        n->resume_.value = val;
        return n;
    }

    return parse_postfix(p, parse_primary(p));
}

/* Pratt expression parser */
static Node *parse_expr(Parser *p, int min_prec) {
    Node *left = parse_prefix(p);

    while (1) {
        Token *tok = pp_peek(p, 0);
        int prec = prec_of(tok->kind);

        /* | on a new line = lambda delimiter */
        if ((tok->kind == TK_PIPE || tok->kind == TK_OP_OR) &&
            tok->span.line > left->span.end_line)
            break;

        /* - on a new line after call/block → new statement */
        if (tok->kind == TK_MINUS && tok->span.line > left->span.end_line &&
            (VAL_TAG(left) == NODE_CALL || VAL_TAG(left) == NODE_METHOD_CALL ||
             VAL_TAG(left) == NODE_BLOCK || VAL_TAG(left) == NODE_IF ||
             VAL_TAG(left) == NODE_MATCH || VAL_TAG(left) == NODE_WHILE))
            break;

        /* Special lookahead: 'not in' is a compound infix operator */
        if (prec == 0 && tok->kind == TK_NOT && pp_peek(p, 1)->kind == TK_IN)
            prec = prec_of(TK_IN);

        if (prec <= min_prec) break;

        pp_advance(p);
        Span opspan = tok->span;

        /* Assignment (right-assoc) */
        if (is_assign_op(tok->kind)) {
            Node *right = parse_expr(p, 0);
            Node *n = node_new(NODE_ASSIGN, opspan);
            strncpy(n->assign.op, assign_op_str(tok->kind), sizeof(n->assign.op));
            n->assign.target = left;
            n->assign.value  = right;
            left = n; continue;
        }

        /* Pipe: a |> f(b) → f(a, b) */
        if (tok->kind == TK_PIPE_ARROW) {
            Node *right = parse_expr(p, prec);
            if (right && VAL_TAG(right) == NODE_CALL) {
                NodeList new_args = nodelist_new();
                nodelist_push(&new_args, left);
                for (int i = 0; i < right->call.args.len; i++)
                    nodelist_push(&new_args, right->call.args.items[i]);
                free(right->call.args.items);
                right->call.args = new_args;
                left = right;
            } else {
                Node *n = node_new(NODE_CALL, opspan);
                n->call.callee = right;
                n->call.args   = nodelist_new();
                nodelist_push(&n->call.args, left);
                n->call.kwargs = nodepairlist_new();
                left = n;
            }
            continue;
        }

        /* Range */
        if (tok->kind == TK_DOTDOT || tok->kind == TK_DOTDOTEQ) {
            int incl = (tok->kind == TK_DOTDOTEQ);
            Node *right = NULL;
            Token *next = pp_peek(p, 0);
            if (next->kind != TK_SEMICOLON && next->kind != TK_RBRACKET &&
                next->kind != TK_RPAREN && next->kind != TK_EOF)
                right = parse_expr(p, prec);
            Node *n = node_new(NODE_RANGE, opspan);
            n->range.start     = left;
            n->range.end       = right;
            n->range.inclusive = incl;
            left = n; continue;
        }

        /* Power (right-assoc) */
        if (tok->kind == TK_POWER) {
            Node *right = parse_expr(p, prec - 1);
            Node *n = node_new(NODE_BINOP, opspan);
            strncpy(n->binop.op, "**", sizeof(n->binop.op)-1);
            n->binop.left  = left;
            n->binop.right = right;
            left = n; continue;
        }

        /* actor ! msg → NODE_SEND_EXPR */
        if (tok->kind == TK_OP_NOT) {
            Node *msg = parse_expr(p, prec);
            Node *n = node_new(NODE_SEND_EXPR, opspan);
            n->send_expr.target  = left;
            n->send_expr.message = msg;
            left = n; continue;
        }

        /* as / is */
        if (tok->kind == TK_AS) {
            Node *n = node_new(NODE_CAST, opspan);
            n->cast.expr = left;
            /* Just consume the type name */
            Token *tn = pp_peek(p, 0);
            if (tn->kind == TK_IDENT || (tn->kind >= TK_IF && tn->kind <= TK_PANIC))
                pp_advance(p);
            n->cast.type_name = xs_strdup(tn->sval ? tn->sval : "?");
            left = n; continue;
        }

        /* is (type check) */
        if (tok->kind == TK_IS) {
            Token *tn = pp_peek(p, 0);
            const char *type_name = "?";
            if (tn->kind == TK_IDENT || tn->kind == TK_NULL ||
                tn->kind == TK_FN ||
                (tn->kind >= TK_IF && tn->kind <= TK_PANIC)) {
                type_name = tn->sval ? tn->sval : (tn->kind == TK_NULL ? "null" : "?");
                pp_advance(p);
            }
            Node *n = node_new(NODE_BINOP, opspan);
            strncpy(n->binop.op, "is", sizeof(n->binop.op)-1);
            n->binop.left = left;
            Node *tnode = node_new(NODE_LIT_STRING, opspan);
            tnode->lit_string.sval = xs_strdup(type_name);
            tnode->lit_string.interpolated = 0;
            tnode->lit_string.parts = nodelist_new();
            n->binop.right = tnode;
            left = n; continue;
        }

        /* in / not in */
        if (tok->kind == TK_IN || (tok->kind == TK_NOT && pp_peek(p, 0)->kind == TK_IN)) {
            int negated = 0;
            if (tok->kind == TK_NOT) {
                pp_advance(p); /* consume 'in' */
                negated = 1;
            }
            Node *right = parse_expr(p, prec);
            Node *n = node_new(NODE_BINOP, opspan);
            strncpy(n->binop.op, negated ? "not in" : "in", sizeof(n->binop.op)-1);
            n->binop.left  = left; n->binop.right = right;
            left = n; continue;
        }

        /* Regular binary op */
        Node *right = parse_expr(p, prec);
        Node *n = node_new(NODE_BINOP, opspan);
        const char *op_str;
        switch (tok->kind) {
        case TK_PLUS:    op_str="+";  break;
        case TK_MINUS:   op_str="-";  break;
        case TK_STAR:    op_str="*";  break;
        case TK_SLASH:   op_str="/";  break;
        case TK_PERCENT: op_str="%";  break;
        case TK_FLOORDIV:op_str="//"; break;
        case TK_CONCAT:  op_str="++"; break;
        case TK_EQ:      op_str="=="; break;
        case TK_NEQ:     op_str="!="; break;
        case TK_LT:      op_str="<";  break;
        case TK_GT:      op_str=">";  break;
        case TK_LE:      op_str="<="; break;
        case TK_GE:      op_str=">="; break;
        case TK_SPACESHIP:op_str="<=>"; break;
        case TK_OP_AND: case TK_AND: op_str="&&"; break;
        case TK_OP_OR:  case TK_OR:  op_str="||"; break;
        case TK_NULL_COALESCE: op_str="??"; break;
        case TK_PIPE:    op_str="|";  break;
        case TK_AMP:     op_str="&";  break;
        case TK_CARET:   op_str="^";  break;
        case TK_SHL:     op_str="<<"; break;
        case TK_SHR:     op_str=">>"; break;
        case TK_PIPE_ARROW: op_str="|>"; break;
        default:         op_str="?";  break;
        }
        strncpy(n->binop.op, op_str, sizeof(n->binop.op)-1);
        n->binop.left  = left;
        n->binop.right = right;
        left = n;
    }
    return left;
}

static Node *parse_handle(Parser *p) {
    Token *kw = pp_expect(p, TK_HANDLE, "expected 'handle'");
    Span span = kw->span;

    /* handle expr { arms }                         -- old-style arms
       handle expr with Effect { fn op(args){body} } -- effect-scoped */
    Node *expr = parse_expr(p, 0);
    char *with_effect = NULL;
    if (pp_match(p, TK_WITH)) {
        Token *et = pp_peek(p, 0);
        if (et->kind == TK_IDENT) {
            with_effect = xs_strdup(et->sval ? et->sval : "");
            pp_advance(p);
        }
    }
    pp_expect(p, TK_LBRACE, "expected '{' after handle expression");

    EffectArmList arms = effectarmlist_new();

    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;

        /* In `with Effect { ... }` form each arm is written as
           `fn op(params) { body }` and inherits the effect name from
           the outer `with`. Otherwise the arms use the older
           `Effect.op(params) => body` shape. */
        if (with_effect && pp_check(p, TK_FN)) {
            pp_advance(p); /* consume 'fn' */
            Token *on_tok = pp_peek(p, 0);
            char *op_name = on_tok->kind == TK_IDENT
                ? xs_strdup(on_tok->sval ? on_tok->sval : "")
                : xs_strdup("__op__");
            if (on_tok->kind == TK_IDENT) pp_advance(p);

            ParamList params = paramlist_new();
            if (pp_match(p, TK_LPAREN)) {
                while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                    Token *pt = pp_peek(p, 0);
                    Param pm = {0};
                    pm.span = pt->span;
                    if (pt->kind == TK_IDENT) {
                        pp_advance(p);
                        pm.name = xs_strdup(pt->sval ? pt->sval : "");
                        Node *pn = node_new(NODE_PAT_IDENT, pt->span);
                        pn->pat_ident.name = xs_strdup(pm.name);
                        pn->pat_ident.mutable = 0;
                        pm.pattern = pn;
                    } else {
                        pp_advance(p);
                    }
                    /* Optional ': type' annotation on the parameter. */
                    if (pp_match(p, TK_COLON)) typeexpr_free(parse_type_expr(p));
                    paramlist_push(&params, pm);
                    if (!pp_match(p, TK_COMMA)) break;
                }
                pp_expect(p, TK_RPAREN, "expected ')' after handler params");
            }
            /* Optional '-> type' return annotation, just consume. */
            if (pp_match(p, TK_ARROW)) typeexpr_free(parse_type_expr(p));
            Node *body = pp_check(p, TK_LBRACE) ? parse_block(p) : parse_expr(p, 0);

            EffectArm arm;
            arm.effect_name = xs_strdup(with_effect);
            arm.op_name     = op_name;
            arm.params      = params;
            arm.body        = body;
            arm.span        = span;
            effectarmlist_push(&arms, arm);

            pp_match(p, TK_COMMA);
            skip_semis(p);
            continue;
        }

        /* Effect.op(params) => { body } */
        Token *eff_tok = pp_peek(p, 0);
        if (eff_tok->kind != TK_IDENT) {
            pp_advance(p); /* skip unknown token */
            continue;
        }
        char *effect_name = xs_strdup(eff_tok->sval ? eff_tok->sval : "");
        pp_advance(p);

        char *op_name = NULL;
        if (pp_match(p, TK_DOT)) {
            Token *op_tok = pp_peek(p, 0);
            if (op_tok->kind == TK_IDENT) {
                op_name = xs_strdup(op_tok->sval ? op_tok->sval : "");
                pp_advance(p);
            } else {
                op_name = xs_strdup("__op__");
            }
        } else {
            /* No dot: treat effect_name as "Effect" and op_name as ""  */
            op_name = xs_strdup(effect_name);
            free(effect_name);
            effect_name = xs_strdup("__effect__");
        }

        /* Parse params (handler bindings) */
        ParamList params = paramlist_new();
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Token *pt = pp_peek(p, 0);
                Param pm = {0};
                pm.span = pt->span;
                if (pt->kind == TK_IDENT) {
                    pp_advance(p);
                    pm.name = xs_strdup(pt->sval ? pt->sval : "");
                    Node *pn = node_new(NODE_PAT_IDENT, pt->span);
                    pn->pat_ident.name = xs_strdup(pm.name);
                    pn->pat_ident.mutable = 0;
                    pm.pattern = pn;
                } else {
                    parse_error_at(p, pt->span, "P0001",
                                   "expected identifier for handler parameter, got '%s'",
                                   pt->sval ? pt->sval : token_kind_name(pt->kind));
                    pp_advance(p);
                }
                paramlist_push(&params, pm);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')' after handler params");
        }

        /* => body */
        pp_expect(p, TK_FAT_ARROW, "expected '=>' in effect handler");
        Node *body;
        if (pp_check(p, TK_LBRACE)) body = parse_block(p);
        else                          body = parse_expr(p, 0);

        EffectArm arm;
        arm.effect_name = effect_name;
        arm.op_name     = op_name;
        arm.params      = params;
        arm.body        = body;
        arm.span        = span;
        effectarmlist_push(&arms, arm);

        pp_match(p, TK_COMMA);
        skip_semis(p);
    }
    pp_expect(p, TK_RBRACE, "expected '}' after handle arms");

    Node *n = node_new(NODE_HANDLE, span);
    n->handle.expr = expr;
    n->handle.arms = arms;
    free(with_effect);
    return n;
}

static Node *parse_effect_decl(Parser *p) {
    Token *kw = pp_expect(p, TK_EFFECT, "expected 'effect'");
    Span span = kw->span;
    Token *name_tok = pp_expect(p, TK_IDENT, "expected effect name");
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    pp_expect(p, TK_LBRACE, "expected '{'");
    NodeList ops = nodelist_new();
    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        if (pp_check(p, TK_FN)) {
            Node *fn = parse_fn_decl(p, 0, 0, 0);
            if (fn) nodelist_push(&ops, fn);
        } else {
            pp_advance(p);
        }
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    Node *n = node_new(NODE_EFFECT_DECL, span);
    n->effect_decl.name = name;
    n->effect_decl.ops  = ops;
    return n;
}

static Node *parse_block(Parser *p) {
    Token *ob = pp_expect(p, TK_LBRACE, "expected '{'");
    Span span = ob->span;
    int open_line = ob->span.line;
    NodeList stmts = nodelist_new();
    Node *trailing = NULL;

    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        if (p->error_count >= p->max_errors) break;
        if (p->panic_mode) {
            synchronize(p);
            if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        }
        Node *stmt = parse_stmt(p);
        if (!stmt) continue;
            if (VAL_TAG(stmt) == NODE_EXPR_STMT && !stmt->expr_stmt.has_semicolon &&
            pp_check(p, TK_RBRACE)) {
            trailing = stmt->expr_stmt.expr;
            stmt->expr_stmt.expr = NULL;
            node_free(stmt);
            break;
        }
        nodelist_push(&stmts, stmt);
    }
    pp_expect_ex(p, TK_RBRACE, "expected '}'", open_line);
    return make_block(stmts, trailing, span);
}

/* pattern */
static Node *parse_pattern(Parser *p) {
    Token *tok = pp_peek(p, 0);
    Span span = tok->span;

    /* Map pattern: `{ "key": sub, ident: sub, short_ident, .. }` or
       the explicit `#{...}` form. Both lex paths land here -- the
       parser treats TK_HASH_LBRACE as unconditionally a map pattern
       (the user said so), while bare TK_LBRACE still uses lookahead
       to distinguish from blocks. */
    if (tok->kind == TK_LBRACE || tok->kind == TK_HASH_LBRACE) {
        Token *a = pp_peek(p, 1);
        Token *b = pp_peek(p, 2);
        int looks_like_map =
            tok->kind == TK_HASH_LBRACE ||
            a->kind == TK_RBRACE ||
            a->kind == TK_DOTDOT ||
            a->kind == TK_DOTDOTDOT ||
            a->kind == TK_STRING ||
            (a->kind == TK_IDENT && (b->kind == TK_COLON || b->kind == TK_COMMA || b->kind == TK_RBRACE));
        if (looks_like_map) {
            pp_advance(p); /* consume '{' */
            char **keys = NULL; Node **subs = NULL;
            int nfields = 0, cap = 0; int rest = 0;
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                    pp_advance(p); rest = 1;
                    /* `..rest` / `...rest` -- skip the bound name; we
                       don't currently expose it as a value, but
                       accepting it keeps the syntax forgiving. */
                    if (pp_peek(p, 0)->kind == TK_IDENT) pp_advance(p);
                    break;
                }
                Token *kt = pp_peek(p, 0);
                char *key = NULL;
                if (kt->kind == TK_STRING) {
                    pp_advance(p);
                    key = xs_strdup(kt->sval ? kt->sval : "");
                } else if (kt->kind == TK_IDENT) {
                    pp_advance(p);
                    key = xs_strdup(kt->sval ? kt->sval : "");
                } else {
                    break;
                }
                Node *sub = NULL;
                if (pp_match(p, TK_COLON)) {
                    sub = parse_pattern(p);
                } else {
                    /* shorthand: `name` matches key "name" and binds to local name */
                    Node *id = node_new(NODE_PAT_IDENT, kt->span);
                    id->pat_ident.name    = xs_strdup(key);
                    id->pat_ident.mutable = 0;
                    sub = id;
                }
                if (nfields == cap) {
                    cap = cap ? cap * 2 : 4;
                    keys = xs_realloc(keys, sizeof(char*) * cap);
                    subs = xs_realloc(subs, sizeof(Node*) * cap);
                }
                keys[nfields] = key;
                subs[nfields] = sub;
                nfields++;
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RBRACE, "expected '}' in map pattern");
            Node *n = node_new(NODE_PAT_MAP, span);
            n->pat_map.keys = keys;
            n->pat_map.sub = subs;
            n->pat_map.nfields = nfields;
            n->pat_map.rest = rest;
            return n;
        }
    }

    /* Wildcard */
    if (tok->kind == TK_IDENT && strcmp(tok->sval ? tok->sval : "", "_") == 0) {
        pp_advance(p);
        return node_new(NODE_PAT_WILD, span);
    }

    /* Mutable ident */
    int is_mut = 0;
    if (tok->kind == TK_MUT) {
        pp_advance(p); is_mut = 1;
        tok = pp_peek(p, 0); span = tok->span;
    }

    /* Allow contextual keywords as identifier patterns */
    if (tok->kind == TK_FROM || tok->kind == TK_AS || tok->kind == TK_TYPE ||
        tok->kind == TK_EXPORT || tok->kind == TK_ASYNC ||
        tok->kind == TK_AWAIT || tok->kind == TK_PUB || tok->kind == TK_STATIC ||
        tok->kind == TK_YIELD || tok->kind == TK_IN) {
        pp_advance(p);
        Node *n = node_new(NODE_PAT_IDENT, span);
        n->pat_ident.name    = xs_strdup(tok->sval ? tok->sval : "");
        n->pat_ident.mutable = is_mut;
        return n;
    }

    /* Enum pattern with dot syntax: Shape.Circle(r) or Shape.Circle */
    if (tok->kind == TK_IDENT && pp_peek(p, 1)->kind == TK_DOT &&
        pp_peek(p, 2)->kind == TK_IDENT) {
        /* check if this is Name.Variant or Name.Variant(args) */
        Token *t2 = pp_peek(p, 2);
        int is_enum_pat = (t2->sval && t2->sval[0] >= 'A' && t2->sval[0] <= 'Z') ||
                          pp_peek(p, 3)->kind == TK_LPAREN ||
                          pp_peek(p, 3)->kind == TK_FAT_ARROW ||
                          pp_peek(p, 3)->kind == TK_COMMA ||
                          pp_peek(p, 3)->kind == TK_RPAREN ||
                          pp_peek(p, 3)->kind == TK_PIPE;
        if (is_enum_pat) {
            pp_advance(p); /* consume Name */
            char *ename = xs_strdup(tok->sval ? tok->sval : "");
            pp_advance(p); /* consume . */
            Token *vt = pp_advance(p); /* consume Variant */
            char path_buf[512];
            snprintf(path_buf, sizeof path_buf, "%s::%s", ename,
                     vt->sval ? vt->sval : "");
            free(ename);
            if (pp_match(p, TK_LPAREN)) {
                NodeList args = nodelist_new();
                while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                    Node *sub = parse_pattern(p);
                    if (sub) nodelist_push(&args, sub);
                    if (!pp_match(p, TK_COMMA)) break;
                }
                pp_expect(p, TK_RPAREN, "expected ')'");
                Node *n = node_new(NODE_PAT_ENUM, span);
                n->pat_enum.path = xs_strdup(path_buf);
                n->pat_enum.args = args;
                return n;
            }
            /* no parens: simple enum variant pattern */
            Node *n = node_new(NODE_PAT_ENUM, span);
            n->pat_enum.path = xs_strdup(path_buf);
            n->pat_enum.args = nodelist_new();
            return n;
        }
        /* fallback: expression pattern */
        Node *expr = parse_expr(p, 8);
        Node *n = node_new(NODE_PAT_EXPR, span);
        n->pat_expr.expr = expr;
        return n;
    }

    /* Identifier pattern (or enum pattern with ::) */
    if (tok->kind == TK_IDENT) {
        pp_advance(p);
        char *name = xs_strdup(tok->sval ? tok->sval : "");

        /* Qualified path: Name::Variant */
        if (pp_check(p, TK_COLON_COLON)) {
            size_t path_cap = strlen(name) + 64;
            size_t path_len = strlen(name);
            char *path = xs_malloc(path_cap);
            memcpy(path, name, path_len + 1); free(name);
            while (pp_match(p, TK_COLON_COLON)) {
                Token *part = pp_peek(p, 0);
                if (part->kind == TK_IDENT) {
                    pp_advance(p);
                    const char *ps = part->sval ? part->sval : "";
                    size_t ps_len = strlen(ps);
                    while (path_len + 2 + ps_len + 1 > path_cap) {
                        path_cap *= 2;
                        path = realloc(path, path_cap);
                    }
                    memcpy(path + path_len, "::", 2); path_len += 2;
                    memcpy(path + path_len, ps, ps_len + 1); path_len += ps_len;
                } else break;
            }
            /* PatEnum with args */
            if (pp_match(p, TK_LPAREN)) {
                NodeList args = nodelist_new();
                while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                    Node *sub = parse_pattern(p);
                    if (sub) nodelist_push(&args, sub);
                    if (!pp_match(p, TK_COMMA)) break;
                }
                pp_expect(p, TK_RPAREN, "expected ')'");
                Node *n = node_new(NODE_PAT_ENUM, span);
                n->pat_enum.path = path;
                n->pat_enum.args = args;
                return n;
            }
            /* PatStruct */
            if (pp_check(p, TK_LBRACE)) {
                pp_advance(p);
                NodePairList fields = nodepairlist_new();
                NodeList defaults = nodelist_new();
                int rest = 0;
                while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                    if (pp_check(p, TK_DOTDOT)) { pp_advance(p); rest=1; break; }
                    Token *fn = pp_peek(p, 0);
                    if (fn->kind != TK_IDENT) break;
                    pp_advance(p);
                    char *fn_name = xs_strdup(fn->sval ? fn->sval : "");
                    Node *sub = NULL;
                    Node *def = NULL;
                    if (pp_match(p, TK_COLON)) sub = parse_pattern(p);
                    if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
                    nodepairlist_push(&fields, fn_name, sub);
                    nodelist_push(&defaults, def);
                    free(fn_name);
                    pp_match(p, TK_COMMA);
                }
                pp_expect(p, TK_RBRACE, "expected '}'");
                Node *n = node_new(NODE_PAT_STRUCT, span);
                n->pat_struct.path     = path;
                n->pat_struct.fields   = fields;
                n->pat_struct.defaults = defaults;
                n->pat_struct.rest     = rest;
                return n;
            }
            /* Unit enum variant */
            Node *n = node_new(NODE_PAT_ENUM, span);
            n->pat_enum.path = path;
            n->pat_enum.args = nodelist_new();
            return n;
        }

        /* Enum-style: Name(args) */
        if (pp_check(p, TK_LPAREN)) {
            pp_advance(p);
            NodeList args = nodelist_new();
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *sub = parse_pattern(p);
                if (sub) nodelist_push(&args, sub);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')'");
            Node *n = node_new(NODE_PAT_ENUM, span);
            n->pat_enum.path = name;
            n->pat_enum.args = args;
            return n;
        }

        /* Struct pattern: Name { fields } */
        if (pp_check(p, TK_LBRACE)) {
            pp_advance(p);
            NodePairList fields = nodepairlist_new();
            NodeList defaults = nodelist_new();
            int rest = 0;
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                if (pp_check(p, TK_DOTDOT)) { pp_advance(p); rest=1; break; }
                Token *fn = pp_peek(p, 0);
                if (fn->kind != TK_IDENT) break;
                pp_advance(p);
                char *fn_name = xs_strdup(fn->sval ? fn->sval : "");
                Node *sub = NULL;
                Node *def = NULL;
                if (pp_match(p, TK_COLON)) sub = parse_pattern(p);
                if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
                nodepairlist_push(&fields, fn_name, sub);
                nodelist_push(&defaults, def);
                free(fn_name);
                pp_match(p, TK_COMMA);
            }
            pp_expect(p, TK_RBRACE, "expected '}'");
            Node *n = node_new(NODE_PAT_STRUCT, span);
            n->pat_struct.path     = name;
            n->pat_struct.fields   = fields;
            n->pat_struct.defaults = defaults;
            n->pat_struct.rest     = rest;
            return n;
        }

        /* Capture pattern: x @ subpat */
        if (pp_check(p, TK_AT)) {
            pp_advance(p);
            Node *sub = parse_pattern(p);
            Node *n = node_new(NODE_PAT_CAPTURE, span);
            n->pat_capture.name = name;
            n->pat_capture.pattern = sub;
            return n;
        }

        Node *n = node_new(NODE_PAT_IDENT, span);
        n->pat_ident.name    = name;
        n->pat_ident.mutable = is_mut;
        return n;
    }

    /* Negative number literal patterns: -1, -3.14 */
    if (tok->kind == TK_MINUS) {
        Token *next = pp_peek(p, 1);
        if (next->kind == TK_INT) {
            pp_advance(p); /* consume '-' */
            pp_advance(p); /* consume int */
            /* Check for range pattern: -5..0 or -5..=0 */
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTEQ)) {
                int inclusive = pp_check(p, TK_DOTDOTEQ);
                pp_advance(p);
                Node *start_expr = node_new(NODE_LIT_INT, span);
                start_expr->lit_int.ival = -(next->ival);
                /* Parse end (may also be negative) */
                int neg_end = 0;
                if (pp_check(p, TK_MINUS)) { pp_advance(p); neg_end = 1; }
                Token *end_tok = pp_peek(p, 0);
                pp_advance(p);
                Node *end_expr = node_new(NODE_LIT_INT, end_tok->span);
                end_expr->lit_int.ival = neg_end ? -(end_tok->ival) : end_tok->ival;
                Node *range = node_new(NODE_PAT_RANGE, span);
                range->pat_range.start = start_expr;
                range->pat_range.end = end_expr;
                range->pat_range.inclusive = inclusive;
                return range;
            }
            Node *n = node_new(NODE_PAT_LIT, span);
            n->pat_lit.ival = -(next->ival); n->pat_lit.tag = 0;
            return n;
        }
        if (next->kind == TK_FLOAT) {
            pp_advance(p); /* consume '-' */
            pp_advance(p); /* consume float */
            Node *n = node_new(NODE_PAT_LIT, span);
            n->pat_lit.fval = -(next->fval); n->pat_lit.tag = 1;
            return n;
        }
    }

    /* Literal patterns */
    if (tok->kind == TK_INT) {
        pp_advance(p);
        /* Check for range pattern: 0..18 or 0..=18 */
        if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTEQ)) {
            int inclusive = pp_check(p, TK_DOTDOTEQ);
            pp_advance(p);
            Node *start_expr = node_new(NODE_LIT_INT, span);
            start_expr->lit_int.ival = tok->ival;
            /* Parse end literal */
            Token *end_tok = pp_peek(p, 0);
            pp_advance(p);
            Node *end_expr = node_new(NODE_LIT_INT, end_tok->span);
            end_expr->lit_int.ival = end_tok->ival;
            Node *range = node_new(NODE_PAT_RANGE, span);
            range->pat_range.start = start_expr;
            range->pat_range.end = end_expr;
            range->pat_range.inclusive = inclusive;
            return range;
        }
        Node *n = node_new(NODE_PAT_LIT, span);
        n->pat_lit.ival = tok->ival; n->pat_lit.tag = 0;
        return n;
    }
    if (tok->kind == TK_FLOAT) {
        pp_advance(p);
        /* Check for range pattern: 0.0..1.0 or 0.0..=1.0 */
        if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTEQ)) {
            int inclusive = pp_check(p, TK_DOTDOTEQ);
            pp_advance(p);
            Node *start_expr = node_new(NODE_LIT_FLOAT, span);
            start_expr->lit_float.fval = tok->fval;
            Token *end_tok = pp_peek(p, 0);
            pp_advance(p);
            Node *end_expr = node_new(NODE_LIT_FLOAT, end_tok->span);
            end_expr->lit_float.fval = end_tok->fval;
            Node *range = node_new(NODE_PAT_RANGE, span);
            range->pat_range.start = start_expr;
            range->pat_range.end = end_expr;
            range->pat_range.inclusive = inclusive;
            return range;
        }
        Node *n = node_new(NODE_PAT_LIT, span);
        n->pat_lit.fval = tok->fval; n->pat_lit.tag = 1;
        return n;
    }
    if (tok->kind == TK_RAW_STRING || tok->kind == TK_REGEX) {
        pp_advance(p);
        Node *n = node_new(NODE_PAT_REGEX, span);
        n->pat_regex.pattern = xs_strdup(tok->sval ? tok->sval : "");
        return n;
    }
    if (tok->kind == TK_STRING) {
        pp_advance(p);
        /* Check for string concat pattern: "prefix" ++ rest */
        if (pp_check(p, TK_CONCAT)) {
            pp_advance(p); /* consume ++ */
            Node *rest = parse_pattern(p);
            Node *n = node_new(NODE_PAT_STRING_CONCAT, span);
            n->pat_str_concat.prefix = xs_strdup(tok->sval ? tok->sval : "");
            n->pat_str_concat.rest = rest;
            return n;
        }
        Node *n = node_new(NODE_PAT_LIT, span);
        n->pat_lit.sval = xs_strdup(tok->sval ? tok->sval : "");
        n->pat_lit.tag = 2;
        return n;
    }
    if (tok->kind == TK_BOOL) {
        pp_advance(p);
        Node *n = node_new(NODE_PAT_LIT, span);
        n->pat_lit.bval = (int)tok->ival; n->pat_lit.tag = 3;
        return n;
    }
    if (tok->kind == TK_NULL) {
        pp_advance(p);
        Node *n = node_new(NODE_PAT_LIT, span);
        n->pat_lit.tag = 4;
        return n;
    }

    /* Tuple pattern: (a, b): but ( followed by lambda is expression pattern */
    if (tok->kind == TK_LPAREN) {
        Token *t1 = pp_peek(p, 1);
        /* ( followed by | or (| means lambda → expression pattern */
        if (t1->kind == TK_PIPE || t1->kind == TK_OP_OR ||
            (t1->kind == TK_LPAREN && pp_peek(p, 2)->kind == TK_PIPE)) {
            p->no_arrow_lambda = 1;
            Node *expr = parse_expr(p, 8); /* stop before | (prec 8) */
            p->no_arrow_lambda = 0;
            Node *n = node_new(NODE_PAT_EXPR, span);
            n->pat_expr.expr = expr;
            return n;
        }
        pp_advance(p);
        NodeList elems = nodelist_new();
        while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
            Node *sub = parse_pattern(p);
            if (sub) nodelist_push(&elems, sub);
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_RPAREN, "expected ')'");
        Node *n = node_new(NODE_PAT_TUPLE, span);
        n->pat_tuple.elems = elems;
        return n;
    }

    /* Slice pattern: [a, b, ..rest] */
    if (tok->kind == TK_LBRACKET) {
        pp_advance(p);
        NodeList elems = nodelist_new();
        char *rest = NULL;
        while (!pp_check2(p, TK_RBRACKET, TK_EOF)) {
            /* Accept both `..rest` and `...rest`: the latter mirrors the
               spread call/array literal syntax users already write. */
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) {
                pp_advance(p);
                if (pp_peek(p, 0)->kind == TK_IDENT) {
                    Token *rn = pp_advance(p);
                    rest = xs_strdup(rn->sval ? rn->sval : "");
                }
                break;
            }
            Node *sub = parse_pattern(p);
            if (sub) nodelist_push(&elems, sub);
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_RBRACKET, "expected ']'");
        Node *n = node_new(NODE_PAT_SLICE, span);
        n->pat_slice.elems = elems;
        n->pat_slice.rest  = rest;
        return n;
    }

    /* Map/struct destructuring: { field, field: pat, field = default, ... } */
    if (tok->kind == TK_LBRACE) {
        pp_advance(p);
        NodePairList fields = nodepairlist_new();
        NodeList defaults = nodelist_new();
        int rest = 0;
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            if (pp_check(p, TK_DOTDOT) || pp_check(p, TK_DOTDOTDOT)) { pp_advance(p); rest=1; break; }
            Token *fn = pp_peek(p, 0);
            if (fn->kind != TK_IDENT) break;
            pp_advance(p);
            char *fn_name = xs_strdup(fn->sval ? fn->sval : "");
            Node *sub = NULL;
            Node *def = NULL;
            if (pp_match(p, TK_COLON)) sub = parse_pattern(p);
            if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
            nodepairlist_push(&fields, fn_name, sub);
            nodelist_push(&defaults, def); /* NULL if no default */
            free(fn_name);
            pp_match(p, TK_COMMA);
        }
        pp_expect(p, TK_RBRACE, "expected '}'");
        Node *n = node_new(NODE_PAT_STRUCT, span);
        n->pat_struct.path     = NULL; /* no type name: map destructuring */
        n->pat_struct.fields   = fields;
        n->pat_struct.defaults = defaults;
        n->pat_struct.rest     = rest;
        return n;
    }

    /* Wildcard _ already handled; fallback */
    Node *n = node_new(NODE_PAT_WILD, span);
    return n;
}

static Node *parse_if(Parser *p) {
    Token *kw = pp_expect(p, TK_IF, "expected 'if'");
    Span span = kw->span;
    Node *cond = parse_expr(p, 0);
    Node *then = parse_block(p);

    NodeList elif_conds = nodelist_new();
    NodeList elif_thens = nodelist_new();
    Node *else_branch = NULL;

    while (pp_check(p, TK_ELIF)) {
        pp_advance(p);
        Node *ec = parse_expr(p, 0);
        Node *et = parse_block(p);
        nodelist_push(&elif_conds, ec);
        nodelist_push(&elif_thens, et);
    }

    while (pp_match(p, TK_ELSE)) {
        if (pp_check(p, TK_IF)) {
            /* else if → flatten into elif */
            pp_advance(p); /* consume 'if' */
            Node *ec = parse_expr(p, 0);
            Node *et = parse_block(p);
            nodelist_push(&elif_conds, ec);
            nodelist_push(&elif_thens, et);
            /* continue loop for more else if / else */
        } else {
            else_branch = parse_block(p);
            break; /* else is always terminal */
        }
    }

    Node *n = node_new(NODE_IF, span);
    n->if_expr.cond        = cond;
    n->if_expr.then        = then;
    n->if_expr.elif_conds  = elif_conds;
    n->if_expr.elif_thens  = elif_thens;
    n->if_expr.else_branch = else_branch;
    return n;
}

static Node *parse_while(Parser *p) {
    Token *kw = pp_expect(p, TK_WHILE, "expected 'while'");
    Span span = kw->span;
    Node *cond = parse_expr(p, 0);
    Node *body = parse_block(p);
    Node *n = node_new(NODE_WHILE, span);
    n->while_loop.cond  = cond;
    n->while_loop.body  = body;
    n->while_loop.label = NULL;
    return n;
}

/* Pattern with the bare comma-tuple sugar: `k, v` parses as `(k, v)`.
 * Used by `for k, v in ...` (statement and comprehension forms). */
static Node *parse_loop_pattern(Parser *p) {
    Node *pat = parse_pattern(p);
    if (pp_check(p, TK_COMMA)) {
        NodeList elems = nodelist_new();
        nodelist_push(&elems, pat);
        while (pp_match(p, TK_COMMA)) {
            Node *more = parse_pattern(p);
            nodelist_push(&elems, more);
        }
        Node *tup = node_new(NODE_PAT_TUPLE, pat->span);
        tup->pat_tuple.elems = elems;
        pat = tup;
    }
    return pat;
}

static Node *parse_for(Parser *p) {
    Token *kw = pp_expect(p, TK_FOR, "expected 'for'");
    Span span = kw->span;
    Node *pat = parse_loop_pattern(p);
    pp_expect(p, TK_IN, "expected 'in'");
    Node *iter = parse_expr(p, 0);
    Node *body = parse_block(p);
    Node *n = node_new(NODE_FOR, span);
    n->for_loop.pattern = pat;
    n->for_loop.iter    = iter;
    n->for_loop.body    = body;
    n->for_loop.label   = NULL;
    return n;
}

static Node *parse_loop_expr(Parser *p) {
    Token *kw = pp_expect(p, TK_LOOP, "expected 'loop'");
    Span span = kw->span;
    Node *body = parse_block(p);
    Node *n = node_new(NODE_LOOP, span);
    n->loop.body  = body;
    n->loop.label = NULL;
    return n;
}

static Node *parse_match(Parser *p) {
    Token *kw = pp_expect(p, TK_MATCH, "expected 'match'");
    Span span = kw->span;
    Node *subject = parse_expr(p, 0);
    pp_expect(p, TK_LBRACE, "expected '{' after match");
    MatchArmList arms = matcharmlist_new();

    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;

        Node *pat = parse_pattern(p);

        /* Or-pattern: pat | pat */
        while (pp_check(p, TK_PIPE)) {
            pp_advance(p);
            Node *right = parse_pattern(p);
            Node *or_n = node_new(NODE_PAT_OR, pat->span);
            or_n->pat_or.left  = pat;
            or_n->pat_or.right = right;
            pat = or_n;
        }

        Node *guard = NULL;
        if (pp_check(p, TK_IF)) {
            pp_advance(p);
            /* Suppress arrow-lambda parsing inside the guard so the
               match arm's trailing `=> body` isn't eaten as a lambda
               body. e.g. `(a, b) if a == b => "eq"` previously parsed
               as `(a == lambda(b) => "eq")` because the guard's b =>
               grabbed the arrow. */
            int saved_nal = p->no_arrow_lambda;
            p->no_arrow_lambda = 1;
            guard = parse_expr(p, 0);
            p->no_arrow_lambda = saved_nal;
        }

        /* => or { */
        Node *body = NULL;
        if (pp_match(p, TK_FAT_ARROW)) {
            if (pp_check(p, TK_LBRACE)) body = parse_block(p);
            else                          body = parse_expr(p, 0);
        } else if (pp_check(p, TK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_expr(p, 0);
        }

        MatchArm arm = {pat, guard, body, span};
        matcharmlist_push(&arms, arm);
        /* Accept both , and ; as arm separators */
        if (!pp_match(p, TK_COMMA))
            pp_match(p, TK_SEMICOLON);
        skip_semis(p);
    }
    pp_expect(p, TK_RBRACE, "expected '}' after match");

    Node *n = node_new(NODE_MATCH, span);
    n->match.subject = subject;
    n->match.arms    = arms;
    return n;
}

/* phase 3: public wrappers for override chaining */
Node *parser_parse_if(Parser *p)    { return parse_if(p); }
Node *parser_parse_for(Parser *p)   { return parse_for(p); }
Node *parser_parse_while(Parser *p) { return parse_while(p); }
Node *parser_parse_match(Parser *p) { return parse_match(p); }
Node *parser_parse_fn_decl(Parser *p, int is_pub, int is_async, int is_pure) {
    return parse_fn_decl(p, is_pub, is_async, is_pure);
}

static Node *parse_try(Parser *p) {
    Token *kw = pp_expect(p, TK_TRY, "expected 'try'");
    Span span = kw->span;
    Node *body = parse_block(p);
    MatchArmList catch_arms = matcharmlist_new();
    Node *finally_block = NULL;

    while (pp_check(p, TK_CATCH)) {
        pp_advance(p);
        Node *pat = NULL;
        if (pp_check(p, TK_LPAREN)) {
            /* catch (pattern) { ... } */
            pp_advance(p);
            pat = parse_pattern(p);
            pp_expect(p, TK_RPAREN, "expected ')'");
        } else if (pp_peek(p,0)->kind == TK_IDENT && pp_peek(p,1)->kind == TK_LBRACE) {
            /* catch e { ... }: bare identifier before block */
            Token *et = pp_advance(p);
            pat = node_new(NODE_PAT_IDENT, et->span);
            pat->pat_ident.name    = xs_strdup(et->sval ? et->sval : "_");
            pat->pat_ident.mutable = 0;
        } else if (!pp_check(p, TK_LBRACE)) {
            /* some other non-block pattern */
            pat = parse_pattern(p);
        }
        Node *cb = parse_block(p);
        MatchArm arm = {pat, NULL, cb, span};
        matcharmlist_push(&catch_arms, arm);
    }
    if (pp_check(p, TK_FINALLY)) {
        pp_advance(p);
        finally_block = parse_block(p);
    }

    Node *n = node_new(NODE_TRY, span);
    n->try_.body          = body;
    n->try_.catch_arms    = catch_arms;
    n->try_.finally_block = finally_block;
    return n;
}

static ParamList parse_params(Parser *p) {
    ParamList pl = paramlist_new();
    while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
        if (pp_check(p, TK_SELF)) {
            Token *st = pp_advance(p);
            Param pm = {0}; pm.span = st->span;
            pm.name = xs_strdup("self");
            Node *pn = node_new(NODE_PAT_IDENT, st->span);
            pn->pat_ident.name    = xs_strdup("self");
            pn->pat_ident.mutable = 0;
            pm.pattern = pn;
            paramlist_push(&pl, pm);
        } else {
            int variadic = pp_match(p, TK_DOTDOTDOT) != NULL;
            Node *pat = parse_pattern(p);
            Param pm = {0};
            pm.span     = pat->span;
            pm.pattern  = pat;
            pm.variadic = variadic;
            /* Extract name from PatIdent */
            if (VAL_TAG(pat) == NODE_PAT_IDENT)
                pm.name = xs_strdup(pat->pat_ident.name);
            /* Optional type annotation */
            if (pp_match(p, TK_COLON)) {
                pm.type_ann = parse_type_expr(p);
            }
            /* Optional where clause */
            if (pp_match_where(p)) {
                pm.contract = parse_expr(p, 2);
            }
            /* Default value */
            if (pp_match(p, TK_ASSIGN)) {
                pm.default_val = parse_expr(p, 0);
            }
            paramlist_push(&pl, pm);
        }
        if (!pp_match(p, TK_COMMA)) break;
    }
    return pl;
}

static void skip_type(Parser *p) {
    int depth = 0;
    while (!pp_at_end(p)) {
        Token *t = pp_peek(p, 0);
        if (t->kind == TK_LT || t->kind == TK_LPAREN || t->kind == TK_LBRACKET) { depth++; pp_advance(p); }
        else if ((t->kind == TK_GT || t->kind == TK_RPAREN || t->kind == TK_RBRACKET) && depth > 0) { depth--; pp_advance(p); }
        else if (depth == 0 && (t->kind == TK_COMMA || t->kind == TK_RPAREN ||
                 t->kind == TK_LBRACE || t->kind == TK_SEMICOLON ||
                 t->kind == TK_ASSIGN || t->kind == TK_RBRACE ||
                 t->kind == TK_EOF)) break;
        else pp_advance(p);
    }
}

/* like skip_type but captures text */
static char *capture_type_text(Parser *p) {
    int start = p->pos;
    skip_type(p);
    int end = p->pos;
    if (end <= start) return xs_strdup("?");
    /* Build string from tokens between start..end */
    int buflen = 0;
    for (int i = start; i < end; i++) {
        const char *s = safe_tok_text(&p->tokens[i]);
        buflen += (int)strlen(s) + 1; /* +1 for possible separator */
    }
    char *buf = xs_malloc(buflen + 1);
    buf[0] = '\0';
    for (int i = start; i < end; i++) {
        strcat(buf, safe_tok_text(&p->tokens[i]));
    }
    if (buf[0] == '\0') { free(buf); return xs_strdup("?"); }
    return buf;
}

static TypeExpr *parse_type_expr(Parser *p) {
    TypeExpr *te = xs_malloc(sizeof *te);
    memset(te, 0, sizeof *te);
    te->span = pp_peek(p, 0)->span;
    Token *t = pp_peek(p, 0);

    if (t->kind == TK_LBRACKET) {                   /* [T] */
        pp_advance(p);
        te->kind = TEXPR_ARRAY;
        te->inner = parse_type_expr(p);
        pp_match(p, TK_RBRACKET);
    } else if (t->kind == TK_LPAREN) {              /* (A, B) */
        pp_advance(p);
        te->kind = TEXPR_TUPLE;
        TypeExpr **elems = NULL; int n = 0;
        while (!pp_at_end(p) && !pp_check(p, TK_RPAREN)) {
            elems = xs_realloc(elems, sizeof(TypeExpr*) * (n+1));
            elems[n++] = parse_type_expr(p);
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_match(p, TK_RPAREN);
        te->elems = elems; te->nelems = n;
    } else if (t->kind == TK_FN) {                  /* fn(A,B)->R */
        pp_advance(p);
        te->kind = TEXPR_FN;
        TypeExpr **params = NULL; int n = 0;
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_at_end(p) && !pp_check(p, TK_RPAREN)) {
                params = xs_realloc(params, sizeof(TypeExpr*) * (n+1));
                params[n++] = parse_type_expr(p);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_match(p, TK_RPAREN);
        }
        te->params = params; te->nparams = n;
        if (pp_match(p, TK_ARROW)) te->ret = parse_type_expr(p);
    } else if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "_") == 0) {
        pp_advance(p); te->kind = TEXPR_INFER;      /* _ */
    } else if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "forall") == 0) {
        /* forall<T, U> body -- higher-rank polymorphic type. */
        pp_advance(p);                              /* consume 'forall' */
        te->kind = TEXPR_FORALL;
        if (pp_match(p, TK_LT)) {
            char **qn = NULL; int nq = 0;
            while (!pp_at_end(p) && !pp_check(p, TK_GT)) {
                Token *q = pp_peek(p, 0);
                if (q->kind != TK_IDENT) { pp_advance(p); break; }
                qn = xs_realloc(qn, sizeof(char*) * (nq + 1));
                qn[nq++] = xs_strdup(q->sval ? q->sval : "");
                pp_advance(p);
                /* swallow optional `T: Bound` */
                if (pp_match(p, TK_COLON)) typeexpr_free(parse_type_expr(p));
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_match(p, TK_GT);
            te->quant_names = qn;
            te->nquant      = nq;
        }
        te->inner = parse_type_expr(p);
    } else if (t->kind == TK_IDENT) {               /* Foo or Foo<T,U> */
        te->kind = TEXPR_NAMED;
        te->name = xs_strdup(t->sval ? t->sval : "");
        pp_advance(p);
        if (pp_check(p, TK_LT)) {
            pp_advance(p); /* consume < */
            TypeExpr **args = NULL; int n = 0;
            while (!pp_at_end(p) && !pp_check(p, TK_GT)) {
                args = xs_realloc(args, sizeof(TypeExpr*) * (n+1));
                args[n++] = parse_type_expr(p);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_match(p, TK_GT);
            te->args = args; te->nargs = n;
        }
    } else {
        te->kind = TEXPR_INFER; pp_advance(p); /* fallback */
    }

    /* Postfix ? → wrap in TEXPR_OPTION */
    while (pp_check(p, TK_QUESTION)) {
        pp_advance(p);
        TypeExpr *opt = xs_malloc(sizeof *opt);
        memset(opt, 0, sizeof *opt);
        opt->kind = TEXPR_OPTION;
        opt->inner = te;
        te = opt;
    }
    return te;
}

/* fn declaration */
static Node *parse_fn_decl(Parser *p, int is_pub, int is_async, int is_pure) {
    Token *kw = pp_expect(p, TK_FN, "expected 'fn'");
    Span span = kw->span;

    /* fn* generator marker: but fn *(params) is operator overload, not a generator */
    int is_generator = 0;
    if (pp_check(p, TK_STAR) && pp_peek(p, 1)->kind != TK_LPAREN) {
        is_generator = 1;
        pp_advance(p);
    }

    /* function name (allow keywords and operators as fn names) */
    Token *name_tok = pp_peek(p, 0);
    char *fname = NULL;
    if (name_tok->kind == TK_IDENT) {
        pp_advance(p);
        fname = xs_strdup(name_tok->sval ? name_tok->sval : "");
    } else if (is_ident_or_kw(name_tok->kind)) {
        /* keyword used as fn name -- a name-only slot, no ambiguity here */
        pp_advance(p);
        fname = xs_strdup(name_tok->sval ? name_tok->sval :
                          token_kind_name(name_tok->kind));
    } else if (name_tok->kind == TK_PLUS || name_tok->kind == TK_MINUS ||
               name_tok->kind == TK_STAR || name_tok->kind == TK_SLASH ||
               name_tok->kind == TK_PERCENT || name_tok->kind == TK_POWER ||
               name_tok->kind == TK_EQ || name_tok->kind == TK_NEQ ||
               name_tok->kind == TK_LT || name_tok->kind == TK_GT ||
               name_tok->kind == TK_LE || name_tok->kind == TK_GE ||
               name_tok->kind == TK_OP_AND || name_tok->kind == TK_OP_OR ||
               name_tok->kind == TK_CONCAT) {
        /* operator used as fn name (for operator overloading in impl blocks) */
        pp_advance(p);
        fname = xs_strdup(token_kind_name(name_tok->kind));
    } else {
        fname = xs_strdup("<anonymous>");
    }

    /* parse generic type parameters <T>, <+T>, <-T>, <T: Bound>, <T: Bound + Bound2> */
    char **tparams = NULL; TypeExpr **tbounds = NULL; int *tvariance = NULL; int ntp = 0;
    if (pp_check(p, TK_LT)) {
        pp_advance(p); /* consume < */
        while (!pp_at_end(p) && !pp_check(p, TK_GT)) {
            int variance = 0;
            if (pp_match(p, TK_PLUS))       variance = 1;
            else if (pp_match(p, TK_MINUS)) variance = -1;
            Token *tp = pp_peek(p, 0);
            if (tp->kind != TK_IDENT) { pp_advance(p); break; }
            tparams   = xs_realloc(tparams, sizeof(char*) * (ntp+1));
            tbounds   = xs_realloc(tbounds, sizeof(TypeExpr*) * (ntp+1));
            tvariance = xs_realloc(tvariance, sizeof(int) * (ntp+1));
            tparams[ntp]   = xs_strdup(tp->sval ? tp->sval : "");
            tbounds[ntp]   = NULL;
            tvariance[ntp] = variance;
            pp_advance(p); /* consume type param name */
            if (pp_match(p, TK_COLON)) {
                tbounds[ntp] = parse_type_expr(p);
                while (pp_match(p, TK_PLUS)) typeexpr_free(parse_type_expr(p));
            }
            ntp++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_match(p, TK_GT);
    }

    Token *fn_open = pp_expect(p, TK_LPAREN, "expected '('");
    int fn_paren_line = fn_open->span.line;
    ParamList params = parse_params(p);
    pp_expect_ex(p, TK_RPAREN, "expected ')'", fn_paren_line);

    /* optional return type */
    TypeExpr *ret_type = NULL;
    if (pp_match(p, TK_ARROW)) ret_type = parse_type_expr(p);

    /* optional where clause */
    if (pp_check(p, TK_IDENT) && pp_peek(p,0)->sval &&
        strcmp(pp_peek(p,0)->sval, "where") == 0) {
        pp_advance(p); /* consume 'where' */
        while (!pp_at_end(p) && !pp_check(p, TK_LBRACE) && !pp_check(p, TK_SEMICOLON)) {
            if (pp_peek(p,0)->kind == TK_IDENT) pp_advance(p);
            if (pp_match(p, TK_COLON)) {
                typeexpr_free(parse_type_expr(p));
                while (pp_match(p, TK_PLUS)) typeexpr_free(parse_type_expr(p));
            }
            if (!pp_match(p, TK_COMMA)) break;
        }
    }

    /* body */
    Node *body = NULL;
    if (pp_check(p, TK_LBRACE)) {
        body = parse_block(p);
    } else if (pp_match(p, TK_ASSIGN)) {
        Node *expr = parse_expr(p, 0);
        pp_match(p, TK_SEMICOLON);
        /* wrap in a block */
        NodeList stmts = nodelist_new();
        Node *ret = node_new(NODE_RETURN, expr->span);
        ret->ret.value = expr;
        Node *rs = node_new(NODE_EXPR_STMT, expr->span);
        rs->expr_stmt.expr = ret;
        rs->expr_stmt.has_semicolon = 0;
        nodelist_push(&stmts, rs);
        body = make_block(stmts, NULL, expr->span);
    } else {
        pp_match(p, TK_SEMICOLON);
    }

    Node *n = node_new(NODE_FN_DECL, span);
    n->fn_decl.name          = fname;
    n->fn_decl.params        = params;
    n->fn_decl.body          = body;
    n->fn_decl.is_pub        = is_pub;
    n->fn_decl.is_async      = is_async;
    n->fn_decl.is_generator  = is_generator;
    n->fn_decl.is_pure       = is_pure;
    n->fn_decl.ret_type      = ret_type;
    n->fn_decl.type_params         = tparams;
    n->fn_decl.type_bounds         = tbounds;
    n->fn_decl.type_param_variance = tvariance;
    n->fn_decl.n_type_params       = ntp;
    return n;
}

static Node *parse_actor_decl(Parser *p) {
    pp_expect(p, TK_ACTOR, "expected 'actor'");
    Token *name_tok = pp_expect(p, TK_IDENT, "expected actor name");
    Span span = name_tok->span;
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    pp_expect(p, TK_LBRACE, "expected '{'");
    NodePairList state_fields = nodepairlist_new();
    NodeList methods = nodelist_new();

    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        /* skip modifiers */
        while (pp_check(p, TK_PUB) || pp_check(p, TK_MUT) || pp_check(p, TK_STATIC))
            pp_advance(p);
        if (pp_check(p, TK_FN)) {
            Node *fn = parse_fn_decl(p, 0, 0, 0);
            if (fn) nodelist_push(&methods, fn);
        } else if (pp_check(p, TK_VAR) || pp_check(p, TK_LET) || pp_check(p, TK_CONST)) {
            /* state field: let count = 0 / var name = "default" */
            pp_advance(p); /* consume var/let/const */
            Token *fn_tok = pp_peek(p, 0);
            if (fn_tok->kind != TK_IDENT) { skip_semis(p); continue; }
            pp_advance(p);
            char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : "");
            if (pp_match(p, TK_COLON)) { TypeExpr *te = parse_type_expr(p); typeexpr_free(te); }
            Node *def = NULL;
            if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
            nodepairlist_push(&state_fields, fn_name, def);
            free(fn_name);
            pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
        } else {
            /* bare field: name = default or name: type = default */
            Token *fn_tok = pp_peek(p, 0);
            if (!is_ident_or_kw(fn_tok->kind))
                break;
            pp_advance(p);
            char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : token_kind_name(fn_tok->kind));
            if (pp_match(p, TK_COLON)) { TypeExpr *te = parse_type_expr(p); typeexpr_free(te); }
            Node *def = NULL;
            if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
            nodepairlist_push(&state_fields, fn_name, def);
            free(fn_name);
            pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
        }
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    Node *n = node_new(NODE_ACTOR_DECL, span);
    n->actor_decl.name         = name;
    n->actor_decl.state_fields = state_fields;
    n->actor_decl.methods      = methods;
    return n;
}

static Node *parse_class_decl(Parser *p) {
    pp_expect(p, TK_CLASS, "expected 'class'");
    Token *name_tok = pp_expect(p, TK_IDENT, "expected class name");
    Span span = name_tok->span;
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    /* skip generics */
    if (pp_check(p, TK_LT)) {
        int d=1; pp_advance(p);
        while (!pp_at_end(p)&&d>0) {
            if(pp_peek(p,0)->kind==TK_LT) d++;
            else if(pp_peek(p,0)->kind==TK_GT) d--;
            pp_advance(p);
        }
    }
    /* parse base classes: class Derived : Base1, Base2 { ... } */
    char **bases = NULL;
    int nbases = 0;
    int bases_cap = 0;
    if (pp_match(p, TK_COLON)) {
        do {
            Token *base_tok = pp_expect(p, TK_IDENT, "expected base class name");
            if (base_tok && base_tok->sval) {
                if (nbases >= bases_cap) {
                    bases_cap = bases_cap ? bases_cap * 2 : 4;
                    bases = xs_realloc(bases, bases_cap * sizeof(char*));
                }
                bases[nbases++] = xs_strdup(base_tok->sval);
            }
        } while (pp_match(p, TK_COMMA));
    }

    pp_expect(p, TK_LBRACE, "expected '{'");
    NodeList members = nodelist_new();
    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        /* parse modifiers */
        int is_static = 0;
        while (pp_check(p, TK_PUB) || pp_check(p, TK_MUT) || pp_check(p, TK_STATIC)) {
            if (pp_check(p, TK_STATIC)) is_static = 1;
            pp_advance(p);
        }
        if (pp_check(p, TK_FN)) {
            Node *fn = parse_fn_decl(p, 0, 0, 0);
            if (fn) {
                fn->fn_decl.is_static = is_static;
                nodelist_push(&members, fn);
            }
        } else if (pp_check(p, TK_VAR) || pp_check(p, TK_LET) || pp_check(p, TK_CONST)) {
            /* var/let field: var count: i64 = 0 */
            int is_mut = pp_peek(p,0)->kind == TK_VAR;
            pp_advance(p); /* consume var/let/const */
            Token *fn_tok = pp_peek(p, 0);
            if (fn_tok->kind != TK_IDENT) { skip_semis(p); continue; }
            pp_advance(p);
            char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : "");
            TypeExpr *ann = NULL;
            if (pp_match(p, TK_COLON)) ann = parse_type_expr(p);
            Node *def = NULL;
            if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
            pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
            Node *fn_node = node_new(NODE_LET, fn_tok->span);
            fn_node->let.name     = fn_name;
            fn_node->let.value    = def;
            fn_node->let.mutable  = is_mut;
            fn_node->let.pattern  = NULL;
            fn_node->let.type_ann = ann;
            nodelist_push(&members, fn_node);
        } else {
            /* field decl: name: type [= default] */
            Token *fn_tok = pp_peek(p, 0);
            if (!is_ident_or_kw(fn_tok->kind))
                break;
            pp_advance(p);
            char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : token_kind_name(fn_tok->kind));
            TypeExpr *ann = NULL;
            if (pp_match(p, TK_COLON)) ann = parse_type_expr(p);
            Node *def = NULL;
            if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
            pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
            /* Store field as a let node */
            Node *fn_node = node_new(NODE_LET, fn_tok->span);
            fn_node->let.name     = fn_name;
            fn_node->let.value    = def;
            fn_node->let.mutable  = 1;
            fn_node->let.pattern  = NULL;
            fn_node->let.type_ann = ann;
            nodelist_push(&members, fn_node);
        }
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    Node *n = node_new(NODE_CLASS_DECL, span);
    n->class_decl.name    = name;
    n->class_decl.bases   = bases;
    n->class_decl.nbases  = nbases;
    n->class_decl.members = members;
    return n;
}

static Node *parse_struct_decl(Parser *p) {
    pp_expect(p, TK_STRUCT, "expected 'struct'");
    Token *name_tok = pp_expect(p, TK_IDENT, "expected struct name");
    Span span = name_tok->span;
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    /* generic params with optional variance markers: <+T, -U, V> */
    char **st_tparams = NULL; int *st_tvariance = NULL; int st_ntp = 0;
    if (pp_check(p, TK_LT)) {
        pp_advance(p);
        while (!pp_at_end(p) && !pp_check(p, TK_GT)) {
            int variance = 0;
            if (pp_match(p, TK_PLUS))       variance = 1;
            else if (pp_match(p, TK_MINUS)) variance = -1;
            Token *tp = pp_peek(p, 0);
            if (tp->kind != TK_IDENT) { pp_advance(p); break; }
            st_tparams   = xs_realloc(st_tparams,   sizeof(char*) * (st_ntp+1));
            st_tvariance = xs_realloc(st_tvariance, sizeof(int)   * (st_ntp+1));
            st_tparams[st_ntp]   = xs_strdup(tp->sval ? tp->sval : "");
            st_tvariance[st_ntp] = variance;
            pp_advance(p);
            /* allow `T: Bound` style without binding (we drop it for now) */
            if (pp_match(p, TK_COLON)) typeexpr_free(parse_type_expr(p));
            st_ntp++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_match(p, TK_GT);
    }

    pp_expect(p, TK_LBRACE, "expected '{'");
    NodePairList fields = nodepairlist_new();
    TypeExpr **field_types = NULL;
    int n_field_types = 0;
    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        /* skip modifiers */
        while (pp_check(p, TK_PUB) || pp_check(p, TK_MUT)) pp_advance(p);
        Token *fn_tok = pp_peek(p, 0);
        if (!is_ident_or_kw(fn_tok->kind))
            break;
        pp_advance(p);
        char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : token_kind_name(fn_tok->kind));
        TypeExpr *ft = NULL;
        if (pp_match(p, TK_COLON)) { ft = parse_type_expr(p); }
        Node *def = NULL;
        if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
        nodepairlist_push(&fields, fn_name, def);
        field_types = xs_realloc(field_types, sizeof(TypeExpr*) * (n_field_types + 1));
        field_types[n_field_types++] = ft;
        free(fn_name);
        pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    /* Parse optional 'derives Trait1, Trait2' after struct body */
    char **derives = NULL;
    int n_derives = 0;
    if (pp_check(p, TK_IDENT) && pp_peek(p,0)->sval &&
        strcmp(pp_peek(p,0)->sval, "derives") == 0) {
        pp_advance(p); /* consume 'derives' */
        while (pp_check(p, TK_IDENT)) {
            Token *dt = pp_peek(p, 0);
            derives = xs_realloc(derives, sizeof(char*) * (n_derives + 1));
            derives[n_derives++] = xs_strdup(dt->sval ? dt->sval : "");
            pp_advance(p);
            if (!pp_match(p, TK_COMMA)) break;
        }
    }

    Node *n = node_new(NODE_STRUCT_DECL, span);
    n->struct_decl.name        = name;
    n->struct_decl.fields      = fields;
    n->struct_decl.field_types = field_types;
    n->struct_decl.n_field_types = n_field_types;
    n->struct_decl.type_params         = st_tparams;
    n->struct_decl.type_param_variance = st_tvariance;
    n->struct_decl.n_type_params       = st_ntp;
    n->struct_decl.derives     = derives;
    n->struct_decl.n_derives = n_derives;
    return n;
}

static Node *parse_enum_decl(Parser *p) {
    pp_expect(p, TK_ENUM, "expected 'enum'");
    Token *name_tok = pp_expect(p, TK_IDENT, "expected enum name");
    Span span = name_tok->span;
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    /* generic params with optional variance markers */
    char **en_tparams = NULL; int *en_tvariance = NULL; int en_ntp = 0;
    if (pp_check(p, TK_LT)) {
        pp_advance(p);
        while (!pp_at_end(p) && !pp_check(p, TK_GT)) {
            int variance = 0;
            if (pp_match(p, TK_PLUS))       variance = 1;
            else if (pp_match(p, TK_MINUS)) variance = -1;
            Token *tp = pp_peek(p, 0);
            if (tp->kind != TK_IDENT) { pp_advance(p); break; }
            en_tparams   = xs_realloc(en_tparams,   sizeof(char*) * (en_ntp+1));
            en_tvariance = xs_realloc(en_tvariance, sizeof(int)   * (en_ntp+1));
            en_tparams[en_ntp]   = xs_strdup(tp->sval ? tp->sval : "");
            en_tvariance[en_ntp] = variance;
            pp_advance(p);
            if (pp_match(p, TK_COLON)) typeexpr_free(parse_type_expr(p));
            en_ntp++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_match(p, TK_GT);
    }

    pp_expect(p, TK_LBRACE, "expected '{'");
    EnumVariantList variants = enumvariantlist_new();

    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        /* When the body token isn't an identifier, pp_expect returns the
           current token without advancing and sets panic_mode. Without
           forcing progress here the fuzzer can loop on the same token
           and grow the variant list until we OOM. */
        int pos_before = p->pos;
        Token *vn = pp_expect(p, TK_IDENT, "expected variant name");
        if (vn->kind != TK_IDENT) {
            if (p->pos == pos_before) pp_advance(p);
            break;
        }
        EnumVariant v = {0};
        v.name = xs_strdup(vn->sval ? vn->sval : "");
        v.span = vn->span;
        v.fields      = nodelist_new();
        v.field_names = nodelist_new();

        if (pp_match(p, TK_LPAREN)) {
            /* tuple variant: parse each field type */
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Token *ft = pp_peek(p, 0);
                char *type_text = capture_type_text(p);
                /* Store field type as an identifier node with the type name */
                Node *field_node = node_new(NODE_IDENT, ft->span);
                field_node->ident.name = type_text;
                nodelist_push(&v.fields, field_node);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')'");
        } else if (pp_check(p, TK_LBRACE)) {
            pp_advance(p);
            v.is_struct = 1;
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                skip_semis(p);
                while (pp_check(p, TK_PUB) || pp_check(p, TK_MUT)) pp_advance(p);
                Token *fn_tok = pp_peek(p, 0);
                if (fn_tok->kind != TK_IDENT) break;
                pp_advance(p);
                char *fn_name = xs_strdup(fn_tok->sval ? fn_tok->sval : "");
                /* store field name as string literal node */
                Node *fn_node = node_new(NODE_LIT_STRING, fn_tok->span);
                fn_node->lit_string.sval = fn_name;
                fn_node->lit_string.interpolated = 0;
                fn_node->lit_string.parts = nodelist_new();
                nodelist_push(&v.field_names, fn_node);
                if (pp_match(p, TK_COLON)) { TypeExpr *te = parse_type_expr(p); typeexpr_free(te); }
                Node *def = NULL;
                if (pp_match(p, TK_ASSIGN)) def = parse_expr(p, 0);
                nodelist_push(&v.fields, def ? def : node_new(NODE_LIT_NULL, fn_tok->span));
                pp_match(p, TK_COMMA); pp_match(p, TK_SEMICOLON);
            }
            pp_expect(p, TK_RBRACE, "expected '}'");
        }
        if (pp_match(p, TK_ASSIGN)) {
            Node *disc = parse_expr(p, 0); /* skip discriminant */
            if (disc) node_free(disc);
        }
        pp_match(p, TK_COMMA);
        enumvariantlist_push(&variants, v);
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    Node *n = node_new(NODE_ENUM_DECL, span);
    n->enum_decl.name                = name;
    n->enum_decl.variants            = variants;
    n->enum_decl.type_params         = en_tparams;
    n->enum_decl.type_param_variance = en_tvariance;
    n->enum_decl.n_type_params       = en_ntp;
    return n;
}

static Node *parse_impl_decl(Parser *p) {
    pp_expect(p, TK_IMPL, "expected 'impl'");
    /* skip generics */
    if (pp_check(p, TK_LT)) {
        int d=1; pp_advance(p);
        while (!pp_at_end(p)&&d>0){
            if(pp_peek(p,0)->kind==TK_LT) d++;
            else if(pp_peek(p,0)->kind==TK_GT) d--;
            pp_advance(p);
        }
    }
    /* type name */
    Token *type_tok = pp_expect(p, TK_IDENT, "expected type name");
    char *type_name = xs_strdup(type_tok->sval ? type_tok->sval : "");
    char *trait_name = NULL;
    Span span = type_tok->span;

    /* skip generic args on the type name itself: impl<T> Box<T> { ... } */
    if (pp_check(p, TK_LT)) {
        int d=1; pp_advance(p);
        while (!pp_at_end(p)&&d>0){
            if(pp_peek(p,0)->kind==TK_LT) d++;
            else if(pp_peek(p,0)->kind==TK_GT) d--;
            pp_advance(p);
        }
    }

    /* optional 'for Type' (impl Trait for Type) */
    if (pp_check(p, TK_FOR) || pp_check(p, TK_FROM) ||
        (pp_peek(p,0)->kind == TK_IDENT &&
         pp_peek(p,0)->sval && strcmp(pp_peek(p,0)->sval,"for")==0)) {
        pp_advance(p);
        trait_name = type_name;
        Token *tn = pp_expect(p, TK_IDENT, "expected type name after 'for'");
        type_name = xs_strdup(tn->sval ? tn->sval : "");
        /* generic args on the for-type: impl Show for Box<T> */
        if (pp_check(p, TK_LT)) {
            int d=1; pp_advance(p);
            while (!pp_at_end(p)&&d>0){
                if(pp_peek(p,0)->kind==TK_LT) d++;
                else if(pp_peek(p,0)->kind==TK_GT) d--;
                pp_advance(p);
            }
        }
    }

    pp_expect(p, TK_LBRACE, "expected '{'");
    NodeList members = nodelist_new();
    while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
        skip_semis(p);
        if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
        while (pp_check(p, TK_PUB) || pp_check(p, TK_MUT) || pp_check(p, TK_STATIC))
            pp_advance(p);
        if (pp_check(p, TK_FN)) {
            Node *fn = parse_fn_decl(p, 0, 0, 0);
            if (fn) nodelist_push(&members, fn);
        } else {
            /* skip unknown member */
            pp_advance(p);
        }
    }
    pp_expect(p, TK_RBRACE, "expected '}'");

    Node *n = node_new(NODE_IMPL_DECL, span);
    n->impl_decl.type_name  = type_name;
    n->impl_decl.trait_name = trait_name;
    n->impl_decl.members    = members;
    return n;
}

static Node *parse_use(Parser *p) {
    Token *kw = pp_expect(p, TK_USE, "expected 'use'");
    Span span = kw->span;

    /* check for `use plugin "path"` */
    int is_plugin = 0;
    Token *next = pp_peek(p, 0);
    if (next->kind == TK_IDENT && next->sval && strcmp(next->sval, "plugin") == 0) {
        is_plugin = 1;
        pp_advance(p);
    }

    Token *path_tok = pp_expect(p, TK_STRING, "expected file path string after 'use'");
    char *path = xs_strdup(path_tok->sval ? path_tok->sval : "");

    char  *alias = NULL;
    char **names = NULL;
    char **name_aliases = NULL;
    int    nnames = 0;
    int    import_all = 1;

    if (is_plugin) {
        /* parse optional sandbox { flags } */
        int sandbox_flags = 0;
        if (pp_peek(p, 0)->kind == TK_IDENT && pp_peek(p, 0)->sval &&
            strcmp(pp_peek(p, 0)->sval, "sandbox") == 0) {
            pp_advance(p); /* consume 'sandbox' */
            pp_expect(p, TK_LBRACE, "expected '{' after sandbox");
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                Token *flag = pp_peek(p, 0);
                if (flag->kind == TK_IDENT && flag->sval) {
                    if (strcmp(flag->sval, "inject_only") == 0) sandbox_flags |= 1;
                    else if (strcmp(flag->sval, "no_override") == 0) sandbox_flags |= 2;
                    else if (strcmp(flag->sval, "no_eval_hook") == 0) sandbox_flags |= 4;
                }
                pp_advance(p);
                pp_match(p, TK_COMMA);
            }
            pp_expect(p, TK_RBRACE, "expected '}' after sandbox flags");
        }
        pp_match(p, TK_SEMICOLON);
        Node *n = node_new(NODE_USE, span);
        n->use_.path = path;
        n->use_.alias = NULL;
        n->use_.names = NULL;
        n->use_.name_aliases = NULL;
        n->use_.nnames = 0;
        n->use_.import_all = 0;
        n->use_.is_plugin = 1;
        n->use_.sandbox_flags = sandbox_flags;
        return n;
    }

    if (pp_check(p, TK_AS)) {
        pp_advance(p);
        Token *al = pp_expect(p, TK_IDENT, "expected alias name");
        alias = xs_strdup(al->sval ? al->sval : "");
    } else if (pp_check(p, TK_LBRACE)) {
        pp_advance(p);
        import_all = 0;
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            Token *it = pp_expect(p, TK_IDENT, "expected name");
            char *orig = xs_strdup(it->sval ? it->sval : "");
            char *renamed = NULL;
            if (pp_match(p, TK_AS)) {
                Token *al = pp_expect(p, TK_IDENT, "expected alias name");
                renamed = xs_strdup(al->sval ? al->sval : "");
            } else {
                renamed = xs_strdup(orig);
            }
            names = xs_realloc(names, (nnames + 1) * sizeof(char *));
            name_aliases = xs_realloc(name_aliases, (nnames + 1) * sizeof(char *));
            names[nnames] = orig;
            name_aliases[nnames] = renamed;
            nnames++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_RBRACE, "expected '}'");
    }

    /* derive namespace name from filename if no alias and namespace import */
    if (import_all && !alias) {
        /* work on a copy so we can strip trailing slash */
        char *tmp = xs_strdup(path);
        size_t tlen = strlen(tmp);
        while (tlen > 0 && tmp[tlen - 1] == '/') tmp[--tlen] = '\0';
        const char *slash = strrchr(tmp, '/');
        const char *base = slash ? slash + 1 : tmp;
        /* strip .xs extension */
        const char *dot = strrchr(base, '.');
        if (dot && strcmp(dot, ".xs") == 0) {
            alias = xs_malloc((size_t)(dot - base) + 1);
            memcpy(alias, base, (size_t)(dot - base));
            alias[dot - base] = '\0';
        } else {
            alias = xs_strdup(base);
        }
        free(tmp);
    }

    pp_match(p, TK_SEMICOLON);

    Node *n = node_new(NODE_USE, span);
    n->use_.path = path;
    n->use_.alias = alias;
    n->use_.names = names;
    n->use_.name_aliases = name_aliases;
    n->use_.nnames = nnames;
    n->use_.import_all = import_all;
    n->use_.is_plugin = is_plugin;
    return n;
}

static Node *parse_import(Parser *p) {
    Token *kw = pp_expect(p, TK_IMPORT, "expected 'import'");
    Span span = kw->span;

    /* import path as alias  OR  from path import items */
    int from_style = 0;
    if (pp_check(p, TK_FROM)) { pp_advance(p); from_style = 1; }

    /* Collect path parts (allow hyphens in names: math-utils -> "math-utils").
       Stdlib modules whose name collides with a keyword (`async`, `type`,
       `from`, `as`, ...) lex as the keyword token; we accept those here
       so `import async` actually binds the module. */
    char **path = NULL; int nparts = 0;
    Token *pt = pp_peek(p, 0);
    int pt_is_kw_ident = (pt->kind == TK_TYPE || pt->kind == TK_FROM ||
                           pt->kind == TK_AS || pt->kind == TK_EXPORT ||
                           pt->kind == TK_ASYNC || pt->kind == TK_AWAIT ||
                           pt->kind == TK_PUB || pt->kind == TK_STATIC);
    if (pt->kind == TK_IDENT || pt->kind == TK_STRING || pt_is_kw_ident) {
        if (pt->kind == TK_STRING) {
            /* import "math-utils" */
            path = xs_malloc(sizeof(char*));
            path[0] = xs_strdup(pt->sval ? pt->sval : "");
            nparts = 1; pp_advance(p);
        } else {
            /* import math or import math-utils */
            char namebuf[512];
            const char *spelling = pt->sval ? pt->sval :
                                   (pt_is_kw_ident ? token_kind_name(pt->kind) : "");
            snprintf(namebuf, sizeof(namebuf), "%s", spelling);
            pp_advance(p);
            /* absorb hyphen-ident sequences: math-utils, my-cool-lib */
            while (pp_peek(p, 0)->kind == TK_MINUS &&
                   pp_peek(p, 1)->kind == TK_IDENT &&
                   pp_peek(p, 0)->span.line == pt->span.line) {
                pp_advance(p); /* consume - */
                Token *next = pp_advance(p); /* consume ident */
                size_t cur = strlen(namebuf);
                snprintf(namebuf + cur, sizeof(namebuf) - cur, "-%s",
                         next->sval ? next->sval : "");
            }
            path = xs_malloc(sizeof(char*));
            path[0] = xs_strdup(namebuf);
            nparts = 1;
        }
        while (pp_check2(p, TK_COLON_COLON, TK_DOT)) {
            pp_advance(p);
            Token *part = pp_peek(p, 0);
            if (part->kind == TK_IDENT) {
                pp_advance(p);
                path = xs_realloc(path, (nparts+1)*sizeof(char*));
                path[nparts++] = xs_strdup(part->sval ? part->sval : "");
            } else break;
        }
    }

    char  *alias = NULL;
    char **items = NULL;
    int    nitems = 0;

    /* `from x import { ... }` is one form; the other historical form
       was `import x import { y, z }` (two-keyword), which we still
       accept but only when an `{` actually follows. Without the
       LBRACE check we'd swallow the next statement's `import`
       keyword (e.g. `import math\nimport time` was being parsed as
       one import with `time` left as a stray expression statement). */
    if (from_style ||
        (pp_check(p, TK_IMPORT) && pp_peek(p, 1)->kind == TK_LBRACE)) {
        if (from_style) pp_match(p, TK_IMPORT);
        else            pp_advance(p); /* consume 'import' */
        if (pp_check(p, TK_LBRACE)) {
            pp_advance(p);
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                Token *it = pp_expect(p, TK_IDENT, "expected name");
                items = xs_realloc(items, (nitems+1)*sizeof(char*));
                items[nitems++] = xs_strdup(it->sval ? it->sval : "");
                pp_match(p, TK_AS); /* optional alias: skip for now */
                if (pp_peek(p,0)->kind == TK_IDENT && pp_peek(p,-1)->kind == TK_AS)
                    pp_advance(p);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RBRACE, "expected '}'");
        }
    } else if (pp_check(p, TK_AS)) {
        pp_advance(p);
        Token *al = pp_expect(p, TK_IDENT, "expected alias name");
        alias = xs_strdup(al->sval ? al->sval : "");
    }
    pp_match(p, TK_SEMICOLON);

    /* auto-alias hyphenated names: math-utils -> math_utils */
    if (!alias && nitems == 0 && nparts > 0 && strchr(path[0], '-')) {
        alias = xs_strdup(path[0]);
        for (char *c = alias; *c; c++) {
            if (*c == '-') *c = '_';
        }
    }

    Node *n = node_new(NODE_IMPORT, span);
    n->import.path   = path;
    n->import.nparts = nparts;
    n->import.alias  = alias;
    n->import.items  = items;
    n->import.nitems = nitems;
    return n;
}

/* load "path" or load "path" with { KEY: "val", ... } */
static Node *parse_load_stmt(Parser *p) {
    Token *kw = pp_expect(p, TK_LOAD, "expected 'load'");
    Span span = kw->span;

    Token *path_tok = pp_expect(p, TK_STRING, "expected file path string after 'load'");
    char *path = xs_strdup(path_tok->sval ? path_tok->sval : "");

    char **rename_keys = NULL;
    char **rename_vals = NULL;
    int nrenames = 0;

    /* optional: with { KEY: "val", ... } */
    if (pp_check(p, TK_WITH)) {
        pp_advance(p); /* consume 'with' */
        pp_expect(p, TK_LBRACE, "expected '{' after 'with'");
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            /* key can be IDENT or STRING */
            Token *key_tok = pp_peek(p, 0);
            char *key = NULL;
            if (key_tok->kind == TK_IDENT) {
                key = xs_strdup(key_tok->sval ? key_tok->sval : "");
                pp_advance(p);
            } else if (key_tok->kind == TK_STRING) {
                key = xs_strdup(key_tok->sval ? key_tok->sval : "");
                pp_advance(p);
            } else {
                parse_error_at(p, key_tok->span, "P0001", "expected key name");
                pp_advance(p);
                continue;
            }
            pp_expect(p, TK_COLON, "expected ':' after key");
            Token *val_tok = pp_expect(p, TK_STRING, "expected string value");
            char *val = xs_strdup(val_tok->sval ? val_tok->sval : "");

            rename_keys = xs_realloc(rename_keys, (nrenames + 1) * sizeof(char *));
            rename_vals = xs_realloc(rename_vals, (nrenames + 1) * sizeof(char *));
            rename_keys[nrenames] = key;
            rename_vals[nrenames] = val;
            nrenames++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_RBRACE, "expected '}'");
    }

    /* optional: sandbox { flags } -- same set as `use plugin`. */
    int sandbox_flags = 0;
    if (pp_peek(p, 0)->kind == TK_IDENT && pp_peek(p, 0)->sval &&
        strcmp(pp_peek(p, 0)->sval, "sandbox") == 0) {
        pp_advance(p); /* consume 'sandbox' */
        pp_expect(p, TK_LBRACE, "expected '{' after sandbox");
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            Token *flag = pp_peek(p, 0);
            if (flag->kind == TK_IDENT && flag->sval) {
                if      (strcmp(flag->sval, "inject_only")  == 0) sandbox_flags |= 1;
                else if (strcmp(flag->sval, "no_override")  == 0) sandbox_flags |= 2;
                else if (strcmp(flag->sval, "no_eval_hook") == 0) sandbox_flags |= 4;
            }
            pp_advance(p);
            pp_match(p, TK_COMMA);
        }
        pp_expect(p, TK_RBRACE, "expected '}' after sandbox flags");
    }

    if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);

    Node *n = node_new(NODE_LOAD, span);
    n->load_.path = path;
    n->load_.rename_keys = rename_keys;
    n->load_.rename_vals = rename_vals;
    n->load_.nrenames = nrenames;
    n->load_.sandbox_flags = sandbox_flags;
    n->load_.preloaded = 0;

    /* Plugins routinely call plugin.lexer.add_keyword / plugin.parser.on_unknown
       at top level, so anything they register has to be visible by the time we
       parse the next statement. Hand the path off to the runtime callback
       (interp.c sets it on startup) and mark the node so the runtime evaluator
       does not re-execute the file a second time. Falls through harmlessly when
       no callback is wired (e.g. tooling that lexes/parses without an Interp). */
    if (g_parser_eager_load && path) {
        if (g_parser_eager_load(path, p->filename) == 0)
            n->load_.preloaded = 1;
    }
    return n;
}

/* statement */
static Node *parse_stmt(Parser *p) {
    skip_semis(p);
    if (pp_at_end(p)) return NULL;
    Token *tok = pp_peek(p, 0);
    Span span = tok->span;

    /* skip modifiers / attributes. `pub` is no longer a modifier --
       it's a parse error pointing the author at the `export { ... }`
       list, which is now the only way to mark a public surface. */
    int is_pub = 0, is_async = 0;
    if (pp_check(p, TK_PUB)) {
        Token *pt = pp_advance(p);
        parse_error_at(p, pt->span, "P0054",
            "`pub` is no longer a modifier. List the names you want to "
            "expose at the bottom of the file: `export { foo, bar }`");
        synchronize(p);
        return NULL;
    }
    while (pp_check(p, TK_ASYNC)) { pp_advance(p); is_async=1; }
    while (pp_check(p, TK_STATIC) || pp_check(p, TK_MUT)) pp_advance(p);
    /* skip #[...] attributes, detect #[test], #[deprecated("msg")], #[derive(...)] */
    int fn_is_test = 0;
    char *fn_deprecated_msg = NULL;
    char **attr_derives = NULL;
    int attr_n_derives = 0;
    while (pp_check(p, TK_HASH_BRACKET)) {
        pp_advance(p); /* consume #[ */
        Token *attr = pp_peek(p, 0);
        int is_hash_deprecated = 0;
        int is_derive = 0;
        if (attr->kind == TK_IDENT && attr->sval && strcmp(attr->sval, "test") == 0)
            fn_is_test = 1;
        if (attr->kind == TK_IDENT && attr->sval && strcmp(attr->sval, "deprecated") == 0)
            is_hash_deprecated = 1;
        if (attr->kind == TK_IDENT && attr->sval && strcmp(attr->sval, "derive") == 0)
            is_derive = 1;
        pp_advance(p); /* consume attribute name */
        /* skip optional (...) arguments */
        if (pp_check(p, TK_LPAREN)) {
            int d=1; pp_advance(p);
            if (is_hash_deprecated && !fn_deprecated_msg && pp_peek(p,0)->kind == TK_STRING) {
                fn_deprecated_msg = xs_strdup(pp_peek(p,0)->sval ? pp_peek(p,0)->sval : "deprecated");
            }
            if (is_hash_deprecated && !fn_deprecated_msg) {
                fn_deprecated_msg = xs_strdup("deprecated");
            }
            if (is_derive) {
                /* Parse derive trait names: #[derive(Debug, Clone, ...)] */
                while (!pp_at_end(p) && d > 0) {
                    Token *dt = pp_peek(p, 0);
                    if (dt->kind == TK_IDENT && dt->sval) {
                        attr_derives = xs_realloc(attr_derives, sizeof(char*) * (attr_n_derives + 1));
                        attr_derives[attr_n_derives++] = xs_strdup(dt->sval);
                    }
                    if (dt->kind == TK_RPAREN) { d--; pp_advance(p); break; }
                    pp_advance(p);
                    pp_match(p, TK_COMMA);
                }
            } else {
                while (!pp_at_end(p) && d > 0) {
                    if (pp_peek(p,0)->kind == TK_LPAREN) d++;
                    else if (pp_peek(p,0)->kind == TK_RPAREN) d--;
                    pp_advance(p);
                }
            }
        } else if (is_hash_deprecated && !fn_deprecated_msg) {
            fn_deprecated_msg = xs_strdup("deprecated");
        }
        pp_expect(p, TK_RBRACKET, "expected ']' after #[attribute]");
    }

    /* skip @ attributes, detect @pure, @deprecated, @scoped, @[macro] and
     * collect the trigger / discovery / api / modifier decorators into a
     * unified list that gets attached to the fn_decl below. */
    int fn_is_pure = 0;
    int decl_is_scoped = 0;
    int fn_is_macro = 0;
    Decorator *decorators = NULL;
    int n_decorators = 0;
    while (pp_check(p, TK_AT)) {
        Span at_span = pp_peek(p, 0)->span;
        pp_advance(p); /* consume @ */
        /* @[name] form: bracketed attribute, currently used for @[macro]. */
        if (pp_check(p, TK_LBRACKET)) {
            pp_advance(p);
            Token *bn = pp_peek(p, 0);
            if (bn->kind == TK_IDENT && bn->sval &&
                strcmp(bn->sval, "macro") == 0) {
                fn_is_macro = 1;
            }
            pp_advance(p);
            pp_match(p, TK_RBRACKET);
            continue;
        }
        Token *attr = pp_peek(p, 0);
        const char *attr_name = attr->sval;
        int is_deprecated = 0;
        int legacy = 0;
        if (attr_name) {
            if      (strcmp(attr_name, "pure") == 0)       { fn_is_pure = 1;       legacy = 1; }
            else if (strcmp(attr_name, "deprecated") == 0) { is_deprecated = 1;    legacy = 1; }
            else if (strcmp(attr_name, "scoped") == 0)     { decl_is_scoped = 1;   legacy = 1; }
            else if (strcmp(attr_name, "test") == 0)       { fn_is_test = 1;       legacy = 1; }
        }
        pp_advance(p); /* consume attribute name */
        Node **dec_args = NULL;
        int n_dec_args = 0;
        if (pp_check(p, TK_LPAREN)) {
            pp_advance(p);
            /* For @deprecated, capture the first string argument
               directly so the legacy field stays populated. */
            if (is_deprecated && !fn_deprecated_msg && pp_peek(p,0)->kind == TK_STRING) {
                fn_deprecated_msg = xs_strdup(pp_peek(p,0)->sval ? pp_peek(p,0)->sval : "deprecated");
            }
            if (legacy) {
                /* Legacy attributes pre-date expression-args; keep the
                   skip-until-matching-paren behaviour so quirky inputs
                   like @deprecated("foo (bar)") don't suddenly fail to
                   parse. */
                int d = 1;
                while (!pp_at_end(p) && d > 0) {
                    if (pp_peek(p,0)->kind == TK_LPAREN) d++;
                    else if (pp_peek(p,0)->kind == TK_RPAREN) d--;
                    if (d > 0) pp_advance(p); else pp_advance(p);
                }
            } else {
                /* Trigger / api / discovery decorators: parse args as
                   expressions so values like 1s, "INT", "0 * * * *"
                   become real nodes the runtime can inspect. */
                if (!pp_check(p, TK_RPAREN)) {
                    while (1) {
                        Node *arg = parse_expr(p, 0);
                        if (!arg) break;
                        dec_args = xs_realloc(dec_args, sizeof(Node*) * (n_dec_args + 1));
                        dec_args[n_dec_args++] = arg;
                        if (!pp_match(p, TK_COMMA)) break;
                    }
                }
                pp_expect(p, TK_RPAREN, "expected ')' after decorator arguments");
            }
            if (is_deprecated && !fn_deprecated_msg) {
                fn_deprecated_msg = xs_strdup("deprecated");
            }
        } else if (is_deprecated && !fn_deprecated_msg) {
            fn_deprecated_msg = xs_strdup("deprecated");
        }
        if (!legacy && attr_name) {
            decorators = xs_realloc(decorators, sizeof(Decorator) * (n_decorators + 1));
            Decorator *d = &decorators[n_decorators++];
            d->name = xs_strdup(attr_name);
            d->args = dec_args;
            d->n_args = n_dec_args;
            d->span = at_span;
        } else if (dec_args) {
            for (int j = 0; j < n_dec_args; j++) node_free(dec_args[j]);
            free(dec_args);
        }
    }

    tok = pp_peek(p, 0);

    /* phase 3: check override hooks for fn at statement level */
    if (tok->kind == TK_FN && g_plugin_try_parser_override) {
        Node *override_result = g_plugin_try_parser_override(p, "fn");
        if (override_result) {
            free(fn_deprecated_msg);
            if (attr_derives) { for (int di=0; di<attr_n_derives; di++) free(attr_derives[di]); free(attr_derives); }
            return override_result;
        }
    }

    /* Declarations */
    if (tok->kind == TK_FN) {
        /* A bare `fn(...) { ... }` in statement position is actually an
           anonymous function *expression* -- the natural case being
           `fn(n) { fn(x) { x + n } }` where the inner fn is the outer
           block's trailing value. Distinguish by the token right after
           `fn`: if it's not an identifier / operator name, nothing
           binds this function, so it has to be an expression. Route
           those through the normal expression path so parse_block's
           trailing-expression logic picks them up. */
        Token *nt1 = pp_peek(p, 1);
        int looks_anon = 0;
        TokenKind after = nt1->kind;
        if (after == TK_STAR) {
            /* fn*(...) generator or fn* name(...) */
            after = pp_peek(p, 2)->kind;
        }
        if (after == TK_LPAREN || after == TK_LT) looks_anon = 1;
        if (looks_anon && !is_pub && !fn_is_test && !fn_deprecated_msg) {
            free(fn_deprecated_msg);
            if (attr_derives) {
                for (int di = 0; di < attr_n_derives; di++) free(attr_derives[di]);
                free(attr_derives);
            }
            Node *expr = parse_expr(p, 0);
            int has_semi = (pp_match(p, TK_SEMICOLON) != NULL);
            Node *n = node_new(NODE_EXPR_STMT, span);
            n->expr_stmt.expr = expr;
            n->expr_stmt.has_semicolon = has_semi;
            return n;
        }
        Node *fn_node = parse_fn_decl(p, is_pub, is_async, fn_is_pure);
        if (fn_node) {
            fn_node->fn_decl.is_test  = fn_is_test;
            fn_node->fn_decl.is_macro = fn_is_macro;
            if (fn_deprecated_msg) {
                fn_node->fn_decl.deprecated_msg = fn_deprecated_msg;
            } else {
                free(fn_deprecated_msg);
            }
            if (n_decorators > 0) {
                fn_node->fn_decl.decorators = decorators;
                fn_node->fn_decl.n_decorators = n_decorators;
                validate_decorators(p, fn_node->fn_decl.params.len, decorators, n_decorators);
                decorators = NULL;
                n_decorators = 0;
            }
        } else {
            free(fn_deprecated_msg);
        }
        if (decorators) {
            for (int j = 0; j < n_decorators; j++) {
                free(decorators[j].name);
                for (int k = 0; k < decorators[j].n_args; k++) node_free(decorators[j].args[k]);
                free(decorators[j].args);
            }
            free(decorators);
        }
        return fn_node;
    }
    if (n_decorators > 0) {
        parse_error_at(p, decorators[0].span, "P0050",
            "decorators only apply to fn declarations");
        for (int j = 0; j < n_decorators; j++) {
            free(decorators[j].name);
            for (int k = 0; k < decorators[j].n_args; k++) node_free(decorators[j].args[k]);
            free(decorators[j].args);
        }
        free(decorators);
    }
    if (tok->kind == TK_MACRO) {
        /* macro name!(params) { body } → fn name(params) { body } */
        pp_advance(p);
        Token *name_tok = pp_peek(p, 0);
        char *macro_name = NULL;
        if (name_tok->kind == TK_MACRO_BANG) {
            /* name! is single token */
            macro_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            pp_advance(p);
        } else if (name_tok->kind == TK_IDENT) {
            macro_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            pp_advance(p);
            pp_match(p, TK_OP_NOT); /* consume trailing ! if present */
        }
        /* parse params */
        ParamList params;
        if (pp_match(p, TK_LPAREN)) {
            params = parse_params(p);
            pp_expect(p, TK_RPAREN, "expected ')'");
        } else {
            params = (ParamList){NULL, 0, 0};
        }
        Node *body = NULL;
        if (pp_check(p, TK_LBRACE)) body = parse_block(p);
        Node *n = node_new(NODE_FN_DECL, span);
        n->fn_decl.name     = macro_name ? macro_name : xs_strdup("__macro__");
        n->fn_decl.params   = params;
        n->fn_decl.body     = body;
        n->fn_decl.is_pub   = 0;
        n->fn_decl.is_async = 0;
        return n;
    }
    if (tok->kind == TK_CLASS)  return parse_class_decl(p);
    if (tok->kind == TK_ACTOR) return parse_actor_decl(p);
    if (tok->kind == TK_STRUCT) {
        Node *sd = parse_struct_decl(p);
        if (sd) sd->struct_decl.is_pub = is_pub;
        /* Merge #[derive(...)] attributes into struct's derives list */
        if (sd && attr_n_derives > 0) {
            for (int di = 0; di < attr_n_derives; di++) {
                sd->struct_decl.derives = xs_realloc(sd->struct_decl.derives,
                    sizeof(char*) * (sd->struct_decl.n_derives + 1));
                sd->struct_decl.derives[sd->struct_decl.n_derives++] = attr_derives[di];
            }
            free(attr_derives);
            attr_derives = NULL;
            attr_n_derives = 0;
        }
        if (attr_derives) { for (int di=0; di<attr_n_derives; di++) free(attr_derives[di]); free(attr_derives); }
        return sd;
    }
    if (tok->kind == TK_ENUM) {
        Node *ed = parse_enum_decl(p);
        if (ed) ed->enum_decl.is_pub = is_pub;
        return ed;
    }
    if (tok->kind == TK_IMPL)   return parse_impl_decl(p);
    if (tok->kind == TK_TRAIT) {
        pp_advance(p); /* consume 'trait' */
        Token *name_tok = pp_expect(p, TK_IDENT, "expected trait name");
        Node *n = node_new(NODE_TRAIT_DECL, span);
        n->trait_decl.name         = xs_strdup(name_tok->sval ? name_tok->sval : "");
        n->trait_decl.assoc_types  = NULL; n->trait_decl.n_assoc_types = 0;
        n->trait_decl.method_names = NULL; n->trait_decl.n_methods     = 0;
        n->trait_decl.super_trait  = NULL;
        n->trait_decl.methods      = nodelist_new();

        /* optional generics <T>: skip */
        if (pp_check(p, TK_LT)) {
            int d=1; pp_advance(p);
            while (!pp_at_end(p) && d>0) {
                if (pp_peek(p,0)->kind==TK_LT) d++;
                else if (pp_peek(p,0)->kind==TK_GT) d--;
                pp_advance(p);
            }
        }
        /* optional super trait: trait Foo: Bar */
        if (pp_match(p, TK_COLON)) {
            Token *st = pp_peek(p, 0);
            if (st->kind == TK_IDENT)
                { n->trait_decl.super_trait = xs_strdup(st->sval ? st->sval : ""); pp_advance(p); }
        }
        /* optional where clause */
        if (pp_check(p, TK_IDENT) && pp_peek(p,0)->sval &&
            strcmp(pp_peek(p,0)->sval, "where") == 0)
            while (!pp_at_end(p) && !pp_check(p, TK_LBRACE)) pp_advance(p);

        /* parse body collecting type and fn names */
        if (pp_match(p, TK_LBRACE)) {
            while (!pp_at_end(p) && !pp_check(p, TK_RBRACE)) {
                Token *bt = pp_peek(p, 0);
                if (bt->kind == TK_TYPE) {
                    pp_advance(p);
                    Token *atn = pp_expect(p, TK_IDENT, "expected assoc type name");
                    int idx = n->trait_decl.n_assoc_types++;
                    n->trait_decl.assoc_types = xs_realloc(n->trait_decl.assoc_types,
                        sizeof(char*) * n->trait_decl.n_assoc_types);
                    n->trait_decl.assoc_types[idx] = xs_strdup(atn->sval ? atn->sval : "");
                    if (pp_match(p, TK_ASSIGN)) typeexpr_free(parse_type_expr(p));
                    pp_match(p, TK_NEWLINE); pp_match(p, TK_SEMICOLON);
                } else if (bt->kind == TK_FN || bt->kind == TK_PUB) {
                    int is_pub_method = 0;
                    if (bt->kind == TK_PUB) { pp_advance(p); is_pub_method = 1; }
                    /* Use parse_fn_decl to properly parse the method signature */
                    Node *fn = parse_fn_decl(p, is_pub_method, 0, 0);
                    if (fn) {
                        /* Record method name for backward compatibility */
                        int idx = n->trait_decl.n_methods++;
                        n->trait_decl.method_names = xs_realloc(n->trait_decl.method_names,
                            sizeof(char*) * n->trait_decl.n_methods);
                        n->trait_decl.method_names[idx] = xs_strdup(fn->fn_decl.name ? fn->fn_decl.name : "");
                        /* Also store the full fn decl node */
                        nodelist_push(&n->trait_decl.methods, fn);
                    }
                } else {
                    pp_advance(p);
                }
            }
            pp_match(p, TK_RBRACE);
        }
        pp_match(p, TK_SEMICOLON);
        return n;
    }
    if (tok->kind == TK_IMPORT) return parse_import(p);
    if (tok->kind == TK_USE) return parse_use(p);
    if (tok->kind == TK_LOAD) return parse_load_stmt(p);
    /* `export { name, name as alias, ... }` declares a file's public
       surface. Followed by an LBRACE so it doesn't conflict with
       contextual-identifier uses of the word `export`. */
    if (tok->kind == TK_EXPORT && pp_peek(p, 1)->kind == TK_LBRACE) {
        Span span = tok->span;
        pp_advance(p); /* consume 'export' */
        pp_advance(p); /* consume '{' */
        char **names = NULL, **aliases = NULL;
        int nnames = 0;
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            Token *it = pp_expect(p, TK_IDENT, "expected name in export list");
            char *n_ = xs_strdup(it->sval ? it->sval : "");
            char *a_ = NULL;
            if (pp_match(p, TK_AS)) {
                Token *al = pp_expect(p, TK_IDENT, "expected alias name");
                a_ = xs_strdup(al->sval ? al->sval : "");
            } else {
                a_ = xs_strdup(n_);
            }
            names   = xs_realloc(names,   (nnames + 1) * sizeof(char*));
            aliases = xs_realloc(aliases, (nnames + 1) * sizeof(char*));
            names[nnames] = n_;
            aliases[nnames] = a_;
            nnames++;
            if (!pp_match(p, TK_COMMA)) break;
        }
        pp_expect(p, TK_RBRACE, "expected '}' to close export list");
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_EXPORT, span);
        n->export_.names   = names;
        n->export_.aliases = aliases;
        n->export_.nnames  = nnames;
        return n;
    }
    if (tok->kind == TK_PAUSE) {
        pp_advance(p);
        Node *dur = parse_expr(p, 0);
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_PAUSE, tok->span);
        n->pause_.duration = dur;
        return n;
    }
    if (tok->kind == TK_DEL) {
        pp_advance(p);
        Token *name = pp_expect(p, TK_IDENT, "expected variable name after 'del'");
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_DEL, tok->span);
        n->del_.name = xs_strdup(name->sval ? name->sval : "");
        return n;
    }
    if (tok->kind == TK_EFFECT) return parse_effect_decl(p);

    if (tok->kind == TK_MODULE) {
        /* module Name { stmts } */
        pp_advance(p); /* consume 'module' */
        Token *name_tok = pp_expect(p, TK_IDENT, "expected module name");
        char *mod_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
        Token *ob = pp_expect(p, TK_LBRACE, "expected '{'");
        int mod_open_line = ob->span.line;
        NodeList body = nodelist_new();
        while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
            if (p->error_count >= p->max_errors) break;
            if (p->panic_mode) {
                synchronize(p);
                if (pp_check2(p, TK_RBRACE, TK_EOF)) break;
            }
            Node *s = parse_stmt(p);
            if (s) nodelist_push(&body, s);
        }
        pp_expect_ex(p, TK_RBRACE, "expected '}'", mod_open_line);
        Node *n = node_new(NODE_MODULE_DECL, span);
        n->module_decl.name = mod_name;
        n->module_decl.body = body;
        n->module_decl.is_pub = is_pub;
        return n;
    }

    if (tok->kind == TK_FROM) {
        /* 'from path import {items}' or 'from path import name' */
        pp_advance(p); /* consume 'from' */

        /* Collect module path (e.g. "math", "std.io", "std::io") */
        char **path = NULL; int nparts = 0;
        Token *pt2 = pp_peek(p, 0);
        if (pt2->kind == TK_IDENT || pt2->kind == TK_STRING) {
            if (pt2->kind == TK_STRING) {
                path = xs_malloc(sizeof(char*));
                path[0] = xs_strdup(pt2->sval ? pt2->sval : "");
                nparts = 1; pp_advance(p);
            } else {
                char namebuf2[512];
                snprintf(namebuf2, sizeof(namebuf2), "%s", pt2->sval ? pt2->sval : "");
                pp_advance(p);
                while (pp_peek(p, 0)->kind == TK_MINUS &&
                       pp_peek(p, 1)->kind == TK_IDENT &&
                       pp_peek(p, 0)->span.line == pt2->span.line) {
                    pp_advance(p);
                    Token *nx = pp_advance(p);
                    size_t cur = strlen(namebuf2);
                    snprintf(namebuf2 + cur, sizeof(namebuf2) - cur, "-%s",
                             nx->sval ? nx->sval : "");
                }
                path = xs_malloc(sizeof(char*));
                path[0] = xs_strdup(namebuf2);
                nparts = 1;
            }
            while (pp_check2(p, TK_COLON_COLON, TK_DOT)) {
                pp_advance(p);
                Token *part = pp_peek(p, 0);
                if (part->kind == TK_IDENT) {
                    pp_advance(p);
                    path = xs_realloc(path, (nparts+1)*sizeof(char*));
                    path[nparts++] = xs_strdup(part->sval ? part->sval : "");
                } else break;
            }
        }
        /* consume 'import' keyword */
        pp_expect(p, TK_IMPORT, "expected 'import' after module path");
        /* parse import items: either {a, b, c} or a single name */
        char **items = NULL; int nitems = 0;
        if (pp_check(p, TK_LBRACE)) {
            pp_advance(p);
            while (!pp_check2(p, TK_RBRACE, TK_EOF)) {
                Token *it = pp_expect(p, TK_IDENT, "expected name");
                items = xs_realloc(items, (nitems+1)*sizeof(char*));
                items[nitems++] = xs_strdup(it->sval ? it->sval : "");
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RBRACE, "expected '}'");
        } else {
            Token *it = pp_expect(p, TK_IDENT, "expected name");
            items = xs_malloc(sizeof(char*));
            items[0] = xs_strdup(it->sval ? it->sval : "");
            nitems = 1;
        }
        pp_match(p, TK_SEMICOLON);
        Node *n = node_new(NODE_IMPORT, span);
        n->import.path = path; n->import.nparts = nparts;
        n->import.alias = NULL;
        n->import.items = items; n->import.nitems = nitems;
        return n;
    }

    if (tok->kind == TK_TYPE) {
        /* type alias */
        pp_advance(p);
        Token *name_tok = pp_expect(p, TK_IDENT, "expected type name");
        /* skip generics */
        if (pp_check(p, TK_LT)) {
            int d=1; pp_advance(p);
            while (!pp_at_end(p)&&d>0){
                if(pp_peek(p,0)->kind==TK_LT) d++;
                else if(pp_peek(p,0)->kind==TK_GT) d--;
                pp_advance(p);
            }
        }
        pp_expect(p, TK_ASSIGN, "expected '='");
        char *target = capture_type_text(p);
        pp_match(p, TK_SEMICOLON);
        Node *n = node_new(NODE_TYPE_ALIAS, name_tok->span);
        n->type_alias.name   = xs_strdup(name_tok->sval ? name_tok->sval : "");
        n->type_alias.target = target;
        n->type_alias.is_pub = is_pub;
        return n;
    }

    /* Variable declarations */
    if (tok->kind == TK_LET) {
        pp_advance(p);
        int mutable = (pp_match(p, TK_MUT) != NULL);
        Node *pat = parse_pattern(p);
        TypeExpr *ann = NULL;
        if (pp_match(p, TK_COLON)) ann = parse_type_expr(p);
        Node *contract = NULL;
        if (pp_match_where(p)) {
            contract = parse_expr(p, 2);
        }
        Node *val = NULL;
        if (pp_match(p, TK_ASSIGN)) {
            Token *next = pp_peek(p, 0);
            if (next->kind == TK_SEMICOLON || next->kind == TK_NEWLINE || next->kind == TK_EOF) {
                parse_error_at(p, next->span, "P0001", "expected expression after '='");
                synchronize(p);
            } else {
                val = parse_expr(p, 0);
                /* trailing block for let x = call(args) { block } */
                if (val && VAL_TAG(val) == NODE_CALL && pp_check(p, TK_LBRACE)) {
                    Node *block = parse_block(p);
                    Node *lambda = node_new(NODE_LAMBDA, block->span);
                    lambda->lambda.params = paramlist_new();
                    lambda->lambda.body = block;
                    lambda->lambda.is_generator = 0;
                    nodelist_push(&val->call.args, lambda);
                }
            }
        }
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_LET, span);
        n->let.pattern  = pat;
        n->let.name     = (VAL_TAG(pat) == NODE_PAT_IDENT) ?
                           xs_strdup(pat->pat_ident.name) : NULL;
        n->let.value    = val;
        n->let.mutable  = mutable;
        n->let.is_scoped = decl_is_scoped;
        n->let.is_pub   = is_pub;
        n->let.type_ann = ann;
        n->let.contract = contract;
        return n;
    }
    if (tok->kind == TK_VAR) {
        pp_advance(p);
        Node *pat = parse_pattern(p);
        TypeExpr *ann = NULL;
        if (pp_match(p, TK_COLON)) ann = parse_type_expr(p);
        Node *contract = NULL;
        if (pp_match_where(p)) {
            contract = parse_expr(p, 2);
        }
        Node *val = NULL;
        if (pp_match(p, TK_ASSIGN)) {
            Token *next = pp_peek(p, 0);
            if (next->kind == TK_SEMICOLON || next->kind == TK_NEWLINE || next->kind == TK_EOF) {
                parse_error_at(p, next->span, "P0001", "expected expression after '='");
                synchronize(p);
            } else {
                val = parse_expr(p, 0);
            }
        }
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_VAR, span);
        n->let.pattern  = pat;
        n->let.name     = (VAL_TAG(pat) == NODE_PAT_IDENT) ?
                           xs_strdup(pat->pat_ident.name) : NULL;
        n->let.value    = val;
        n->let.mutable  = 1;
        n->let.is_scoped = decl_is_scoped;
        n->let.is_pub   = is_pub;
        n->let.type_ann = ann;
        n->let.contract = contract;
        return n;
    }
    if (tok->kind == TK_CONST) {
        pp_advance(p);
        Token *name_tok3 = pp_expect(p, TK_IDENT, "expected name");
        TypeExpr *ann = NULL;
        if (pp_match(p, TK_COLON)) ann = parse_type_expr(p);
        Node *contract = NULL;
        if (pp_match_where(p)) {
            contract = parse_expr(p, 2);
        }
        pp_expect(p, TK_ASSIGN, "const requires value");
        Node *val = NULL;
        Token *cnext = pp_peek(p, 0);
        if (cnext->kind == TK_SEMICOLON || cnext->kind == TK_NEWLINE || cnext->kind == TK_EOF) {
            parse_error_at(p, cnext->span, "P0001", "expected expression after '='");
            synchronize(p);
        } else if (!p->panic_mode) {
            val = parse_expr(p, 0);
        }
        if (!pp_match(p, TK_SEMICOLON)) pp_match(p, TK_NEWLINE);
        Node *n = node_new(NODE_CONST, span);
        n->const_.name     = xs_strdup(name_tok3->sval ? name_tok3->sval : "");
        n->const_.value    = val;
        n->const_.type_ann = ann;
        n->const_.contract = contract;
        n->const_.is_pub   = is_pub;
        return n;
    }

    /* assert(cond, msg) or assert expr statement */
    if (tok->kind == TK_ASSERT) {
        pp_advance(p);
        Node *callee = node_new(NODE_IDENT, span);
        callee->ident.name = xs_strdup("assert");
        NodeList args = nodelist_new();
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *a = parse_expr(p, 0);
                if (a) nodelist_push(&args, a);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')' after assert");
        } else {
            Node *cond = parse_expr(p, 0);
            if (cond) nodelist_push(&args, cond);
            if (pp_match(p, TK_COMMA)) {
                Node *msg = parse_expr(p, 0);
                if (msg) nodelist_push(&args, msg);
            }
        }
        pp_match(p, TK_SEMICOLON);
        Node *call = node_new(NODE_CALL, span);
        call->call.callee = callee;
        call->call.args   = args;
        call->call.kwargs = nodepairlist_new();
        Node *n = node_new(NODE_EXPR_STMT, span);
        n->expr_stmt.expr = call;
        n->expr_stmt.has_semicolon = 1;
        return n;
    }

    /* panic(msg) or panic msg statement */
    if (tok->kind == TK_PANIC) {
        pp_advance(p);
        Node *callee = node_new(NODE_IDENT, span);
        callee->ident.name = xs_strdup("panic");
        NodeList args = nodelist_new();
        if (pp_match(p, TK_LPAREN)) {
            while (!pp_check2(p, TK_RPAREN, TK_EOF)) {
                Node *a = parse_expr(p, 0);
                if (a) nodelist_push(&args, a);
                if (!pp_match(p, TK_COMMA)) break;
            }
            pp_expect(p, TK_RPAREN, "expected ')' after panic");
        } else if (!pp_check(p, TK_SEMICOLON) && !pp_check(p, TK_RBRACE) && !pp_check(p, TK_EOF)) {
            Node *msg = parse_expr(p, 0);
            if (msg) nodelist_push(&args, msg);
        }
        pp_match(p, TK_SEMICOLON);
        Node *call = node_new(NODE_CALL, span);
        call->call.callee = callee;
        call->call.args   = args;
        call->call.kwargs = nodepairlist_new();
        Node *n = node_new(NODE_EXPR_STMT, span);
        n->expr_stmt.expr = call;
        n->expr_stmt.has_semicolon = 1;
        return n;
    }

    /* defer { block } or defer <stmt> (single statement form) */
    if (tok->kind == TK_DEFER) {
        pp_advance(p);
        Node *body;
        if (pp_peek(p, 0)->kind == TK_LBRACE) {
            body = parse_block(p);
        } else {
            Node *s = parse_stmt(p);
            body = node_new(NODE_BLOCK, span);
            nodelist_push(&body->block.stmts, s);
        }
        Node *n = node_new(NODE_DEFER, span);
        n->defer_.body = body;
        return n;
    }

    /* bind name = expr - reactive binding */
    if (tok->kind == TK_BIND) {
        pp_advance(p); /* consume 'bind' */
        Token *name_tok2 = pp_expect(p, TK_IDENT, "expected binding name after 'bind'");
        char *bname = xs_strdup(name_tok2->sval ? name_tok2->sval : "");
        pp_expect(p, TK_ASSIGN, "expected '=' after bind name");
        Node *expr = parse_expr(p, 0);
        Node *n = node_new(NODE_BIND, span);
        n->bind_decl.name = bname;
        n->bind_decl.expr = expr;
        return n;
    }

    /* tag name(params) { body } - user-defined control structures */
    if (tok->kind == TK_TAG) {
        pp_advance(p); /* consume 'tag' */
        Token *name_tok2 = pp_expect(p, TK_IDENT, "expected tag name");
        char *tname = xs_strdup(name_tok2->sval ? name_tok2->sval : "");
        ParamList params = {NULL, 0, 0};
        if (pp_match(p, TK_LPAREN)) {
            params = parse_params(p);
            pp_expect(p, TK_RPAREN, "expected ')'");
        }
        Node *body = NULL;
        if (pp_check(p, TK_LBRACE)) {
            body = parse_block(p);
        }
        Node *n = node_new(NODE_TAG_DECL, span);
        n->tag_decl.name   = tname;
        n->tag_decl.params = params;
        n->tag_decl.body   = body;
        n->tag_decl.is_pub = is_pub;
        return n;
    }

    /* plugin "name" { ... } block declaration */
    if (tok->kind == TK_IDENT && tok->sval && strcmp(tok->sval, "plugin") == 0 &&
        pp_peek(p, 1)->kind == TK_STRING) {
        pp_advance(p); /* consume 'plugin' */
        return parse_plugin_decl(p);
    }

    /* pipeline { name -> name -> name } constraint block */
    if (tok->kind == TK_IDENT && tok->sval && strcmp(tok->sval, "pipeline") == 0 &&
        pp_peek(p, 1)->kind == TK_LBRACE) {
        pp_advance(p); /* consume 'pipeline' */
        pp_advance(p); /* consume '{' */
        /* parse constraint chains: name -> name -> name
           produces a NODE_BLOCK with string-pair lets encoding constraints */
        NodeList stmts = nodelist_new();
        int pair_id = 0;
        while (!pp_check(p, TK_RBRACE) && pp_peek(p, 0)->kind != TK_EOF) {
            /* skip newlines/semis */
            while (pp_check(p, TK_NEWLINE) || pp_check(p, TK_SEMICOLON))
                pp_advance(p);
            if (pp_check(p, TK_RBRACE)) break;
            /* parse chain: ident -> ident -> ident ... */
            if (pp_peek(p, 0)->kind != TK_IDENT) { pp_advance(p); continue; }
            Token *first = pp_peek(p, 0);
            char *prev_name = xs_strdup(first->sval ? first->sval : "");
            pp_advance(p);
            while (pp_check(p, TK_ARROW)) {
                pp_advance(p); /* consume -> */
                Token *next_tok = pp_peek(p, 0);
                char *next_name = xs_strdup(next_tok->sval ? next_tok->sval : "");
                pp_advance(p);
                /* encode constraint: let __pipeline_from_N = "prev" */
                char from_buf[64], to_buf[64];
                snprintf(from_buf, sizeof(from_buf), "__pipeline_from_%d", pair_id);
                snprintf(to_buf, sizeof(to_buf), "__pipeline_to_%d", pair_id);
                pair_id++;

                Node *let_from = node_new(NODE_LET, span);
                Node *pat_from = node_new(NODE_PAT_IDENT, span);
                pat_from->pat_ident.name = xs_strdup(from_buf);
                pat_from->pat_ident.mutable = 0;
                let_from->let.pattern = pat_from;
                let_from->let.name = xs_strdup(from_buf);
                Node *val_from = node_new(NODE_LIT_STRING, span);
                val_from->lit_string.sval = xs_strdup(prev_name);
                val_from->lit_string.interpolated = 0;
                val_from->lit_string.parts = nodelist_new();
                let_from->let.value = val_from;
                let_from->let.mutable = 0;
                let_from->let.type_ann = NULL;
                let_from->let.contract = NULL;
                nodelist_push(&stmts, let_from);

                Node *let_to = node_new(NODE_LET, span);
                Node *pat_to = node_new(NODE_PAT_IDENT, span);
                pat_to->pat_ident.name = xs_strdup(to_buf);
                pat_to->pat_ident.mutable = 0;
                let_to->let.pattern = pat_to;
                let_to->let.name = xs_strdup(to_buf);
                Node *val_to = node_new(NODE_LIT_STRING, span);
                val_to->lit_string.sval = xs_strdup(next_name);
                val_to->lit_string.interpolated = 0;
                val_to->lit_string.parts = nodelist_new();
                let_to->let.value = val_to;
                let_to->let.mutable = 0;
                let_to->let.type_ann = NULL;
                let_to->let.contract = NULL;
                nodelist_push(&stmts, let_to);

                free(prev_name);
                prev_name = xs_strdup(next_name);
                free(next_name);
            }
            free(prev_name);
            /* skip newlines/semis between chains */
            while (pp_check(p, TK_NEWLINE) || pp_check(p, TK_SEMICOLON))
                pp_advance(p);
        }
        if (pp_check(p, TK_RBRACE)) pp_advance(p);
        if (stmts.len == 0) {
            return node_new(NODE_LIT_NULL, span);
        }
        Node *blk = node_new(NODE_BLOCK, span);
        blk->block.stmts = stmts;
        blk->block.expr = NULL;
        blk->block.has_decls = 1;
        blk->block.is_unsafe = 0;
        return blk;
    }

    /* Labeled loop: ident: for/while/loop */
    if (tok->kind == TK_IDENT && pp_peek(p, 1)->kind == TK_COLON) {
        Token *next2 = pp_peek(p, 2);
        if (next2->kind == TK_FOR || next2->kind == TK_WHILE || next2->kind == TK_LOOP) {
            char *label = xs_strdup(tok->sval ? tok->sval : "");
            pp_advance(p); /* consume ident */
            pp_advance(p); /* consume : */
            Node *loop_node = NULL;
            if (pp_peek(p,0)->kind == TK_FOR)   loop_node = parse_for(p);
            else if (pp_peek(p,0)->kind == TK_WHILE) loop_node = parse_while(p);
            else                                  loop_node = parse_loop_expr(p);
            if (loop_node) {
                if (VAL_TAG(loop_node) == NODE_FOR)   loop_node->for_loop.label = label;
                else if (VAL_TAG(loop_node) == NODE_WHILE) loop_node->while_loop.label = label;
                else if (VAL_TAG(loop_node) == NODE_LOOP)  loop_node->loop.label = label;
                else free(label);
            } else free(label);
            return loop_node;
        }
    }

    /* phase 2: check plugin syntax handlers for unknown tokens at statement level */
    if (g_plugin_is_keyword && tok->kind == TK_IDENT && tok->sval &&
        g_plugin_is_keyword(tok->sval)) {
        int saved_pos = p->pos;
        pp_advance(p);
        /* disambiguation: check for !<plugin-id> after keyword */
        const char *saved_forced = g_plugin_forced_id;
        Token *peek0 = pp_peek(p, 0);
        if (peek0->kind == TK_OP_NOT) {
            Token *lt = pp_peek(p, 1);
            if (lt->kind == TK_LT) {
                Token *id_tok = pp_peek(p, 2);
                Token *gt_tok = pp_peek(p, 3);
                if (id_tok->kind == TK_IDENT && id_tok->sval && gt_tok->kind == TK_GT) {
                    g_plugin_forced_id = id_tok->sval;
                    pp_advance(p); /* skip ! */
                    pp_advance(p); /* skip < */
                    pp_advance(p); /* skip ident */
                    pp_advance(p); /* skip > */
                }
            }
        }
        Node *plugin_node = g_plugin_try_syntax_handler ?
            g_plugin_try_syntax_handler(p, tok) : NULL;
        g_plugin_forced_id = saved_forced;
        if (plugin_node) return plugin_node;
        p->pos = saved_pos;
        tok = pp_peek(p, 0);
    }

    /* Check for common declaration-like keyword typos from other languages.
       Only trigger when: (1) exact match in typo table, (2) followed by identifier or '('.
       This catches patterns like "function foo()" or "def bar()" early with a helpful message.
       We skip the entire pseudo-declaration to avoid cascading errors. */
    if (tok->kind == TK_IDENT && tok->sval) {
        const char *suggestion = suggest_keyword_exact(tok->sval);
        if (suggestion) {
            Token *next = pp_peek(p, 1);
            if (next->kind == TK_IDENT || next->kind == TK_LPAREN ||
                next->kind == TK_STAR) {
                parse_error_at(p, tok->span, "P0020",
                               "'%s' is not a keyword in XS (did you mean '%s'?)",
                               tok->sval, suggestion);
                /* Skip the pseudo-declaration including { ... } body */
                int brace_depth = 0;
                while (!pp_at_end(p)) {
                    TokenKind k = pp_peek(p, 0)->kind;
                    if (k == TK_LBRACE) { brace_depth++; pp_advance(p); }
                    else if (k == TK_RBRACE) {
                        if (brace_depth > 0) { brace_depth--; pp_advance(p); }
                        if (brace_depth == 0) break;
                    }
                    else if (brace_depth == 0 && (k == TK_SEMICOLON || k == TK_NEWLINE))
                        break;
                    else pp_advance(p);
                }
                pp_match(p, TK_SEMICOLON);
                p->panic_mode = 0;
                return node_new(NODE_LIT_NULL, span);
            }
        }
    }

    /* Expression statement */
    Node *expr = parse_expr(p, 0);
    /* Trailing block: call(args) { block } -> call(args, || { block })
       Only at statement level to avoid conflicts with for/if/while */
    if (VAL_TAG(expr) == NODE_CALL && pp_check(p, TK_LBRACE)) {
        Node *block = parse_block(p);
        Node *lambda = node_new(NODE_LAMBDA, block->span);
        lambda->lambda.params = paramlist_new();
        lambda->lambda.body = block;
        lambda->lambda.is_generator = 0;
        nodelist_push(&expr->call.args, lambda);
    }
    int has_semi = (pp_match(p, TK_SEMICOLON) != NULL);
    /* Return/Break/Continue/Throw are their own stmt types */
    if (VAL_TAG(expr) == NODE_RETURN || VAL_TAG(expr) == NODE_BREAK ||
        VAL_TAG(expr) == NODE_CONTINUE || VAL_TAG(expr) == NODE_THROW)
        return expr;
    Node *n = node_new(NODE_EXPR_STMT, span);
    n->expr_stmt.expr = expr;
    n->expr_stmt.has_semicolon = has_semi;
    return n;
}

Node *parser_parse(Parser *p) {
    Span span = pp_peek(p, 0)->span;
    NodeList stmts = nodelist_new();

    while (!pp_at_end(p)) {
        if (p->error_count >= p->max_errors) break;
        if (p->panic_mode) synchronize(p);
        skip_semis(p);
        if (pp_at_end(p)) break;
        Node *stmt = parse_stmt(p);
        if (stmt) nodelist_push(&stmts, stmt);
    }

    return program_new(stmts, span);
}

/* phase 2: exported parser accessors for plugin system */

Node *parser_parse_expr(Parser *p, int min_prec) {
    return parse_expr(p, min_prec);
}

Node *parser_parse_block(Parser *p) {
    return parse_block(p);
}

Token *parser_peek(Parser *p, int offset) {
    return pp_peek(p, offset);
}

Token *parser_advance(Parser *p) {
    return pp_advance(p);
}

int parser_check(Parser *p, TokenKind kind) {
    return pp_check(p, kind);
}

Token *parser_expect(Parser *p, TokenKind kind, const char *msg) {
    return pp_expect(p, kind, msg);
}
