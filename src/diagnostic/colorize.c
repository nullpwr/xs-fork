#include "diagnostic/colorize.h"
#include "core/xs.h"

#include <string.h>
#include <ctype.h>

#define ANSI_RESET   "\033[0m"
#define COL_KEYWORD  "\033[34m"
#define COL_STRING   "\033[32m"
#define COL_NUMBER   "\033[36m"
#define COL_DURATION "\033[36;1m"  /* bold cyan for ns/us/ms/s/m/h/d-suffixed numbers */
#define COL_COMMENT  "\033[90m"
#define COL_OPERATOR "\033[33m"
#define COL_BUILTIN  "\033[35m"
#define COL_TYPE     "\033[33;1m"
#define COL_DECORATOR "\033[35;1m" /* bold magenta for @decorators */

static const char *diag_keywords[] = {
    "fn", "let", "var", "const", "if", "else", "elif",
    "while", "for", "in", "loop", "match", "when",
    "struct", "enum", "trait", "impl", "import", "export",
    "return", "break", "continue", "true", "false", "null",
    "and", "or", "not", "is", "try", "catch", "finally",
    "throw", "defer", "yield", "async", "await",
    "pub", "mut", "static", "self", "super",
    "module", "class", "type", "as", "from",
    "effect", "perform", "handle", "resume",
    "spawn", "nursery", "actor", "macro",
    "pure", "inline", "unsafe", "tag",
    "do", "with", "use", "plugin",
    "pause", "del",
    "every", "after", "timeout", "debounce",
    NULL
};

static const char *diag_builtins[] = {
    "print", "println", "input", "len", "type", "range",
    "keys", "values", "entries", "map", "filter", "reduce",
    "zip", "any", "all", "min", "max", "sum", "sort",
    "reverse", "push", "pop", "slice", "join", "split",
    "contains", "starts_with", "ends_with", "trim",
    "upper", "lower", "replace", "int", "float", "str",
    "bool", "char", "typeof", "assert", "assert_eq",
    "panic", "exit", "sorted", "reversed", "enumerate",
    "chars", "flatten",
    NULL
};

/* True if the trailing characters of the just-emitted number are one
   of the duration suffixes (`ns`, `us`, `ms`, `s`, `m`, `h`, `d`).
   The lexer checks adjacency, so does this. */
static int diag_is_duration_suffix(const char *p, size_t avail) {
    if (avail >= 2) {
        if ((p[0] == 'n' || p[0] == 'u' || p[0] == 'm') && p[1] == 's')
            return 2;
    }
    if (avail >= 1) {
        if (p[0] == 's' || p[0] == 'm' || p[0] == 'h' || p[0] == 'd')
            return 1;
    }
    return 0;
}

static int diag_is_keyword(const char *word) {
    for (int i = 0; diag_keywords[i]; i++)
        if (strcmp(diag_keywords[i], word) == 0) return 1;
    return 0;
}

static int diag_is_builtin(const char *word) {
    for (int i = 0; diag_builtins[i]; i++)
        if (strcmp(diag_builtins[i], word) == 0) return 1;
    return 0;
}

void diag_colorize_line(const char *line, char *out, size_t outsz) {
    if (g_no_color) {
        size_t len = strlen(line);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, line, len);
        out[len] = '\0';
        return;
    }

    size_t j = 0;
    size_t len = strlen(line);

    #define EMIT(s) do { \
        const char *_s = (s); \
        while (*_s && j + 1 < outsz) out[j++] = *_s++; \
    } while(0)

    #define EMIT_CHAR(c) do { if (j + 1 < outsz) out[j++] = (c); } while(0)

    for (size_t i = 0; i < len; ) {
        if (line[i] == '-' && i + 1 < len && line[i+1] == '-') {
            EMIT(COL_COMMENT);
            while (i < len) EMIT_CHAR(line[i++]);
            EMIT(ANSI_RESET);
            break;
        }

        /* {- ... -} nestable block comments */
        if (line[i] == '{' && i + 1 < len && line[i+1] == '-') {
            EMIT(COL_COMMENT);
            EMIT_CHAR(line[i++]); EMIT_CHAR(line[i++]);
            int depth = 1;
            while (i < len && depth > 0) {
                if (line[i] == '{' && i + 1 < len && line[i+1] == '-') {
                    EMIT_CHAR(line[i++]); EMIT_CHAR(line[i++]); depth++;
                } else if (line[i] == '-' && i + 1 < len && line[i+1] == '}') {
                    EMIT_CHAR(line[i++]); EMIT_CHAR(line[i++]); depth--;
                } else {
                    EMIT_CHAR(line[i++]);
                }
            }
            EMIT(ANSI_RESET);
            continue;
        }

        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            EMIT(COL_STRING);
            EMIT_CHAR(line[i++]);
            while (i < len && line[i] != quote) {
                if (line[i] == '\\' && i + 1 < len) {
                    EMIT_CHAR(line[i++]);
                }
                EMIT_CHAR(line[i++]);
            }
            if (i < len) EMIT_CHAR(line[i++]);
            EMIT(ANSI_RESET);
            continue;
        }

        if (isdigit((unsigned char)line[i]) ||
            (line[i] == '.' && i + 1 < len && isdigit((unsigned char)line[i+1]))) {
            if (i == 0 || !isalnum((unsigned char)line[i-1])) {
                /* Scan the number first without emitting, so we can
                   pick the colour based on whether a duration suffix
                   follows. */
                size_t num_end = i;
                int is_hex = 0;
                if (line[num_end] == '0' && num_end + 1 < len &&
                    (line[num_end+1] == 'x' || line[num_end+1] == 'X')) {
                    is_hex = 1;
                    num_end += 2;
                    while (num_end < len &&
                           (isxdigit((unsigned char)line[num_end]) || line[num_end] == '_'))
                        num_end++;
                } else {
                    while (num_end < len &&
                           (isdigit((unsigned char)line[num_end]) || line[num_end] == '.' ||
                            line[num_end] == '_' ||
                            line[num_end] == 'e' || line[num_end] == 'E'))
                        num_end++;
                }
                int unit_len = is_hex ? 0 : diag_is_duration_suffix(line + num_end, len - num_end);
                const char *col = unit_len > 0 ? COL_DURATION : COL_NUMBER;
                EMIT(col);
                while (i < num_end) EMIT_CHAR(line[i++]);
                /* Compound forms (1m30s) and lone suffixes both come as
                   one lexer token; consume any chain of unit then digits
                   then unit. */
                while (unit_len > 0 && i < len) {
                    while (unit_len-- > 0 && i < len) EMIT_CHAR(line[i++]);
                    while (i < len && isdigit((unsigned char)line[i]))
                        EMIT_CHAR(line[i++]);
                    unit_len = diag_is_duration_suffix(line + i, len - i);
                }
                EMIT(ANSI_RESET);
                continue;
            }
        }

        /* @decorator -- @-prefixed identifier highlighted as a
           decorator. Does not consume the parens or args, those fall
           through normal tokenisation. */
        if (line[i] == '@' && i + 1 < len &&
            (isalpha((unsigned char)line[i+1]) || line[i+1] == '_')) {
            EMIT(COL_DECORATOR);
            EMIT_CHAR(line[i++]);
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_'))
                EMIT_CHAR(line[i++]);
            EMIT(ANSI_RESET);
            continue;
        }

        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_'))
                i++;
            size_t wlen = i - start;
            char word[128];
            if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
            memcpy(word, line + start, wlen);
            word[wlen] = '\0';

            if (diag_is_keyword(word)) {
                EMIT(COL_KEYWORD);
                for (size_t k = start; k < start + wlen; k++) EMIT_CHAR(line[k]);
                EMIT(ANSI_RESET);
            } else if (diag_is_builtin(word)) {
                EMIT(COL_BUILTIN);
                for (size_t k = start; k < start + wlen; k++) EMIT_CHAR(line[k]);
                EMIT(ANSI_RESET);
            } else if (wlen > 0 && isupper((unsigned char)word[0])) {
                EMIT(COL_TYPE);
                for (size_t k = start; k < start + wlen; k++) EMIT_CHAR(line[k]);
                EMIT(ANSI_RESET);
            } else {
                for (size_t k = start; k < start + wlen; k++) EMIT_CHAR(line[k]);
            }
            continue;
        }

        if (strchr("+-*/%=<>!&|^~?:@", line[i])) {
            EMIT(COL_OPERATOR);
            EMIT_CHAR(line[i++]);
            if (i < len && strchr("=>&|+-<>.*?", line[i])) {
                EMIT_CHAR(line[i++]);
                if (i < len && (line[i] == '=' || line[i] == '>'))
                    EMIT_CHAR(line[i++]);
            }
            EMIT(ANSI_RESET);
            continue;
        }

        EMIT_CHAR(line[i++]);
    }

    out[j] = '\0';
    #undef EMIT
    #undef EMIT_CHAR
}
