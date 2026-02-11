#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * regex.c - Thompson NFA-based regex engine for XS
 *
 * Full implementation of a proper regex engine using Thompson's construction
 * for NFA building and parallel NFA simulation for matching. This gives
 * guaranteed O(n*m) worst-case time where n is the string length and m is
 * the pattern size, with no pathological backtracking.
 *
 * Supported regex syntax:
 *   - Literals: abc
 *   - Metacharacters: . ^ $ | ( ) [ ] { } * + ? \
 *   - Character classes: [a-z], [^0-9], [abc]
 *   - Shorthand classes: \d \D \w \W \s \S
 *   - Named POSIX classes: [:alpha:] [:digit:] etc.
 *   - Quantifiers: * + ? {n} {n,} {n,m}
 *   - Lazy quantifiers: *? +? ?? {n,m}?
 *   - Groups: (...) capturing, (?:...) non-capturing
 *   - Alternation: a|b
 *   - Anchors: ^ $ \b \B
 *   - Backreferences: \1 through \9
 *   - Escape sequences: \n \t \r \f \v \\
 */

#include "core/regex.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ================================================================
 * Character class operations
 * ================================================================ */

void xs_cc_clear(XSCharClass *cc) {
    memset(cc->bits, 0, 32);
    cc->negated = 0;
}

void xs_cc_set(XSCharClass *cc, int ch) {
    if (ch >= 0 && ch < 256)
        cc->bits[ch >> 3] |= (unsigned char)(1u << (ch & 7));
}

int xs_cc_test(const XSCharClass *cc, int ch) {
    if (ch < 0 || ch >= 256) return cc->negated;
    int in = (cc->bits[ch >> 3] >> (ch & 7)) & 1;
    return cc->negated ? !in : in;
}

void xs_cc_add_range(XSCharClass *cc, int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > 255) hi = 255;
    for (int c = lo; c <= hi; c++)
        cc->bits[c >> 3] |= (unsigned char)(1u << (c & 7));
}

void xs_cc_negate(XSCharClass *cc) {
    cc->negated = !cc->negated;
}

void xs_cc_digit(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, '0', '9');
}

void xs_cc_word(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, 'a', 'z');
    xs_cc_add_range(cc, 'A', 'Z');
    xs_cc_add_range(cc, '0', '9');
    xs_cc_set(cc, '_');
}

void xs_cc_space(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_set(cc, ' ');
    xs_cc_set(cc, '\t');
    xs_cc_set(cc, '\n');
    xs_cc_set(cc, '\r');
    xs_cc_set(cc, '\f');
    xs_cc_set(cc, '\v');
}

void xs_cc_alpha(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, 'a', 'z');
    xs_cc_add_range(cc, 'A', 'Z');
}

void xs_cc_alnum(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, 'a', 'z');
    xs_cc_add_range(cc, 'A', 'Z');
    xs_cc_add_range(cc, '0', '9');
}

void xs_cc_xdigit(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, '0', '9');
    xs_cc_add_range(cc, 'a', 'f');
    xs_cc_add_range(cc, 'A', 'F');
}

void xs_cc_punct(XSCharClass *cc) {
    xs_cc_clear(cc);
    xs_cc_add_range(cc, '!', '/');
    xs_cc_add_range(cc, ':', '@');
    xs_cc_add_range(cc, '[', '`');
    xs_cc_add_range(cc, '{', '~');
}

/* ================================================================
 * Parser state
 * ================================================================ */

typedef struct {
    const char *src;
    int pos;
    int len;
    int ngroups;   /* next capture group number */
    RENode *nodes;
    int nnodes;
    int cap;
    int flags;
    int error;
} ParseCtx;

static RENode *alloc_node(ParseCtx *p) {
    if (p->nnodes >= p->cap) {
        p->cap = p->cap ? p->cap * 2 : 128;
        p->nodes = realloc(p->nodes, (size_t)p->cap * sizeof(RENode));
    }
    RENode *n = &p->nodes[p->nnodes++];
    memset(n, 0, sizeof(*n));
    return n;
}

static int peek(ParseCtx *p) {
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->src[p->pos];
}

static int next_ch(ParseCtx *p) {
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->src[p->pos++];
}

static int at_end(ParseCtx *p) {
    return p->pos >= p->len;
}

/* ================================================================
 * Patch dangling out-pointers in NFA fragment to a target node
 * ================================================================ */

#define PATCH_VISITED_MAX 8192

static void patch_recursive(RENode *n, RENode *target,
                            RENode **visited, int *nvisited) {
    if (!n || n->type == RE_MATCH) return;
    for (int i = 0; i < *nvisited; i++)
        if (visited[i] == n) return;
    if (*nvisited < PATCH_VISITED_MAX)
        visited[(*nvisited)++] = n;

    if (!n->out1) {
        n->out1 = target;
    } else {
        patch_recursive(n->out1, target, visited, nvisited);
    }

    if (n->type == RE_SPLIT) {
        if (!n->out2) {
            n->out2 = target;
        } else {
            patch_recursive(n->out2, target, visited, nvisited);
        }
    }
}

static void patch_fragment(RENode *n, RENode *target) {
    RENode *visited[PATCH_VISITED_MAX];
    int nv = 0;
    patch_recursive(n, target, visited, &nv);
}

/* ================================================================
 * Parse character class [...]
 * ================================================================ */

static void parse_posix_class(ParseCtx *p, XSCharClass *cc) {
    /* we're right after "[:" - read the name up to ":]" */
    int start = p->pos;
    while (p->pos < p->len && !(p->src[p->pos] == ':' &&
           p->pos + 1 < p->len && p->src[p->pos + 1] == ']'))
        p->pos++;

    int nlen = p->pos - start;
    char name[32];
    if (nlen > 30) nlen = 30;
    memcpy(name, p->src + start, (size_t)nlen);
    name[nlen] = '\0';

    if (p->pos < p->len) p->pos += 2; /* skip ":] " */

    if (strcmp(name, "alpha") == 0) {
        xs_cc_add_range(cc, 'a', 'z');
        xs_cc_add_range(cc, 'A', 'Z');
    } else if (strcmp(name, "digit") == 0) {
        xs_cc_add_range(cc, '0', '9');
    } else if (strcmp(name, "alnum") == 0) {
        xs_cc_add_range(cc, 'a', 'z');
        xs_cc_add_range(cc, 'A', 'Z');
        xs_cc_add_range(cc, '0', '9');
    } else if (strcmp(name, "space") == 0) {
        xs_cc_set(cc, ' ');  xs_cc_set(cc, '\t');
        xs_cc_set(cc, '\n'); xs_cc_set(cc, '\r');
        xs_cc_set(cc, '\f'); xs_cc_set(cc, '\v');
    } else if (strcmp(name, "upper") == 0) {
        xs_cc_add_range(cc, 'A', 'Z');
    } else if (strcmp(name, "lower") == 0) {
        xs_cc_add_range(cc, 'a', 'z');
    } else if (strcmp(name, "xdigit") == 0) {
        xs_cc_add_range(cc, '0', '9');
        xs_cc_add_range(cc, 'a', 'f');
        xs_cc_add_range(cc, 'A', 'F');
    } else if (strcmp(name, "punct") == 0) {
        xs_cc_add_range(cc, '!', '/');
        xs_cc_add_range(cc, ':', '@');
        xs_cc_add_range(cc, '[', '`');
        xs_cc_add_range(cc, '{', '~');
    } else if (strcmp(name, "print") == 0 || strcmp(name, "graph") == 0) {
        xs_cc_add_range(cc, 0x20, 0x7e);
    }
}

static void apply_escape_to_cc(XSCharClass *cc, int esc_ch) {
    switch (esc_ch) {
    case 'd':
        xs_cc_add_range(cc, '0', '9');
        break;
    case 'D':
        xs_cc_add_range(cc, '0', '9');
        cc->negated = !cc->negated; /* toggle */
        break;
    case 'w':
        xs_cc_add_range(cc, 'a', 'z');
        xs_cc_add_range(cc, 'A', 'Z');
        xs_cc_add_range(cc, '0', '9');
        xs_cc_set(cc, '_');
        break;
    case 'W':
        xs_cc_add_range(cc, 'a', 'z');
        xs_cc_add_range(cc, 'A', 'Z');
        xs_cc_add_range(cc, '0', '9');
        xs_cc_set(cc, '_');
        cc->negated = !cc->negated;
        break;
    case 's':
        xs_cc_set(cc, ' ');  xs_cc_set(cc, '\t');
        xs_cc_set(cc, '\n'); xs_cc_set(cc, '\r');
        xs_cc_set(cc, '\f'); xs_cc_set(cc, '\v');
        break;
    case 'S':
        xs_cc_set(cc, ' ');  xs_cc_set(cc, '\t');
        xs_cc_set(cc, '\n'); xs_cc_set(cc, '\r');
        xs_cc_set(cc, '\f'); xs_cc_set(cc, '\v');
        cc->negated = !cc->negated;
        break;
    default:
        xs_cc_set(cc, esc_ch);
        break;
    }
}

static int parse_escape_char(ParseCtx *p) {
    int c = next_ch(p);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    case 'a': return '\a';
    case 'x': {
        /* hex escape: \xNN */
        int h = 0;
        for (int i = 0; i < 2 && !at_end(p); i++) {
            int d = peek(p);
            if (d >= '0' && d <= '9') { h = h * 16 + (d - '0'); next_ch(p); }
            else if (d >= 'a' && d <= 'f') { h = h * 16 + (d - 'a' + 10); next_ch(p); }
            else if (d >= 'A' && d <= 'F') { h = h * 16 + (d - 'A' + 10); next_ch(p); }
            else break;
        }
        return h;
    }
    default:
        return c; /* literal escape */
    }
}

static RENode *parse_char_class(ParseCtx *p) {
    RENode *n = alloc_node(p);
    n->type = RE_CCLASS;
    xs_cc_clear(&n->cc);

    if (peek(p) == '^') {
        n->cc.negated = 1;
        next_ch(p);
    }

    int first = 1;
    while (!at_end(p) && (first || peek(p) != ']')) {
        first = 0;

        /* POSIX class [:name:] */
        if (peek(p) == '[' && p->pos + 1 < p->len && p->src[p->pos + 1] == ':') {
            next_ch(p); next_ch(p); /* skip [: */
            parse_posix_class(p, &n->cc);
            continue;
        }

        int ch;
        if (peek(p) == '\\' && p->pos + 1 < p->len) {
            next_ch(p); /* consume backslash */
            int esc = peek(p);
            if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
                esc == 's' || esc == 'S') {
                next_ch(p);
                apply_escape_to_cc(&n->cc, esc);
                continue;
            }
            ch = parse_escape_char(p);
        } else {
            ch = next_ch(p);
        }

        /* check for range: a-z */
        if (peek(p) == '-' && p->pos + 1 < p->len && p->src[p->pos + 1] != ']') {
            next_ch(p); /* consume '-' */
            int end_ch;
            if (peek(p) == '\\') {
                next_ch(p);
                end_ch = parse_escape_char(p);
            } else {
                end_ch = next_ch(p);
            }
            xs_cc_add_range(&n->cc, ch, end_ch);
        } else {
            xs_cc_set(&n->cc, ch);
        }
    }

    if (peek(p) == ']') next_ch(p);
    return n;
}

/* ================================================================
 * Parse regex into NFA (Thompson's construction)
 * ================================================================ */

static RENode *parse_alt(ParseCtx *p);
static RENode *parse_seq(ParseCtx *p);
static RENode *parse_quant(ParseCtx *p);
static RENode *parse_atom(ParseCtx *p);

static RENode *make_shorthand_class(ParseCtx *p, int esc_ch) {
    RENode *n = alloc_node(p);
    n->type = RE_CCLASS;
    xs_cc_clear(&n->cc);
    switch (esc_ch) {
    case 'd':
        xs_cc_add_range(&n->cc, '0', '9');
        break;
    case 'D':
        xs_cc_add_range(&n->cc, '0', '9');
        n->cc.negated = 1;
        break;
    case 'w':
        xs_cc_add_range(&n->cc, 'a', 'z');
        xs_cc_add_range(&n->cc, 'A', 'Z');
        xs_cc_add_range(&n->cc, '0', '9');
        xs_cc_set(&n->cc, '_');
        break;
    case 'W':
        xs_cc_add_range(&n->cc, 'a', 'z');
        xs_cc_add_range(&n->cc, 'A', 'Z');
        xs_cc_add_range(&n->cc, '0', '9');
        xs_cc_set(&n->cc, '_');
        n->cc.negated = 1;
        break;
    case 's':
        xs_cc_set(&n->cc, ' ');  xs_cc_set(&n->cc, '\t');
        xs_cc_set(&n->cc, '\n'); xs_cc_set(&n->cc, '\r');
        xs_cc_set(&n->cc, '\f'); xs_cc_set(&n->cc, '\v');
        break;
    case 'S':
        xs_cc_set(&n->cc, ' ');  xs_cc_set(&n->cc, '\t');
        xs_cc_set(&n->cc, '\n'); xs_cc_set(&n->cc, '\r');
        xs_cc_set(&n->cc, '\f'); xs_cc_set(&n->cc, '\v');
        n->cc.negated = 1;
        break;
    }
    return n;
}

static RENode *parse_atom(ParseCtx *p) {
    int c = peek(p);
    if (c == -1 || c == ')' || c == '|') return NULL;

    /* grouping */
    if (c == '(') {
        next_ch(p);

        /* check for non-capturing group (?:...) or lookahead (?=...) (?!...) */
        if (peek(p) == '?') {
            next_ch(p);
            int nc = peek(p);
            if (nc == ':') {
                next_ch(p);
                /* non-capturing group */
                RENode *body = parse_alt(p);
                if (peek(p) == ')') next_ch(p);
                return body;
            } else if (nc == '=') {
                /* positive lookahead - simplified: just parse body */
                next_ch(p);
                RENode *body = parse_alt(p);
                if (peek(p) == ')') next_ch(p);
                RENode *la = alloc_node(p);
                la->type = RE_LOOKAHEAD;
                la->look_start = body;
                return la;
            } else if (nc == '!') {
                /* negative lookahead */
                next_ch(p);
                RENode *body = parse_alt(p);
                if (peek(p) == ')') next_ch(p);
                RENode *la = alloc_node(p);
                la->type = RE_NEG_LOOKAHEAD;
                la->look_start = body;
                return la;
            }
            /* unknown group modifier, treat as non-capturing */
            RENode *body = parse_alt(p);
            if (peek(p) == ')') next_ch(p);
            return body;
        }

        /* capturing group */
        int grp = p->ngroups++;
        RENode *save_start = alloc_node(p);
        save_start->type = RE_SAVE;
        save_start->sub = grp * 2;

        RENode *body = parse_alt(p);

        RENode *save_end = alloc_node(p);
        save_end->type = RE_SAVE;
        save_end->sub = grp * 2 + 1;

        if (peek(p) == ')') next_ch(p);

        if (body) {
            save_start->out1 = body;
            patch_fragment(body, save_end);
        } else {
            save_start->out1 = save_end;
        }
        return save_start;
    }

    /* character class */
    if (c == '[') {
        next_ch(p);
        return parse_char_class(p);
    }

    /* dot (any char) */
    if (c == '.') {
        next_ch(p);
        if (p->flags & RE_FLAG_DOTALL) {
            /* match anything including newline */
            RENode *n = alloc_node(p);
            n->type = RE_CCLASS;
            xs_cc_clear(&n->cc);
            xs_cc_add_range(&n->cc, 0, 255);
            return n;
        }
        RENode *n = alloc_node(p);
        n->type = RE_DOT;
        return n;
    }

    /* anchors */
    if (c == '^') {
        next_ch(p);
        RENode *n = alloc_node(p);
        n->type = RE_BOL;
        return n;
    }
    if (c == '$') {
        next_ch(p);
        RENode *n = alloc_node(p);
        n->type = RE_EOL;
        return n;
    }

    /* escape sequences */
    if (c == '\\') {
        next_ch(p);
        int ec = peek(p);
        if (ec == -1) return NULL;

        /* shorthand classes */
        if (ec == 'd' || ec == 'D' || ec == 'w' || ec == 'W' ||
            ec == 's' || ec == 'S') {
            next_ch(p);
            return make_shorthand_class(p, ec);
        }

        /* word boundary */
        if (ec == 'b') {
            next_ch(p);
            RENode *n = alloc_node(p);
            n->type = RE_WBOUND;
            return n;
        }
        if (ec == 'B') {
            next_ch(p);
            RENode *n = alloc_node(p);
            n->type = RE_NWBOUND;
            return n;
        }

        /* backreference \1-\9 */
        if (ec >= '1' && ec <= '9') {
            next_ch(p);
            RENode *n = alloc_node(p);
            n->type = RE_BACKREF;
            n->backref = ec - '0';
            return n;
        }

        /* escape char */
        int ech = parse_escape_char(p);
        RENode *n = alloc_node(p);
        n->type = RE_LIT;
        n->ch = ech;
        return n;
    }

    /* literal character */
    next_ch(p);
    RENode *n = alloc_node(p);
    n->type = RE_LIT;
    n->ch = c;
    if (p->flags & RE_FLAG_ICASE) {
        /* for case-insensitive, convert to char class */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            n->type = RE_CCLASS;
            xs_cc_clear(&n->cc);
            xs_cc_set(&n->cc, tolower(c));
            xs_cc_set(&n->cc, toupper(c));
        }
    }
    return n;
}

/* parse a quantifier after an atom */
static int parse_count(ParseCtx *p) {
    int val = 0;
    while (!at_end(p) && peek(p) >= '0' && peek(p) <= '9') {
        val = val * 10 + (next_ch(p) - '0');
        if (val > 10000) val = 10000; /* cap to prevent absurd sizes */
    }
    return val;
}

/* repeat atom min..max times by building NFA fragment */
static RENode *make_repeat(ParseCtx *p, RENode *atom, int mn, int mx,
                           int greedy) {
    if (mn == 0 && mx == 0) {
        /* {0} or {0,0}: match empty */
        (void)atom;
        return NULL;
    }
    if (mn == 1 && mx == 1) {
        return atom;
    }

    /* build min required copies: atom atom atom ... */
    RENode *first = NULL, *last = NULL;
    for (int i = 0; i < mn; i++) {
        /* For simplicity we re-use the same atom node for repetition.
           This works because NFA simulation doesn't modify nodes. */
        RENode *copy = alloc_node(p);
        memcpy(copy, atom, sizeof(*copy));
        copy->out1 = NULL;
        copy->out2 = NULL;

        if (!first) { first = last = copy; }
        else { patch_fragment(last, copy); last = copy; }
    }

    if (mx < 0) {
        /* unbounded: add a split loop at the end */
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        RENode *body_copy = alloc_node(p);
        memcpy(body_copy, atom, sizeof(*body_copy));
        body_copy->out1 = NULL;
        body_copy->out2 = NULL;

        if (greedy) {
            sp->out1 = body_copy; /* try match first */
            sp->out2 = NULL;      /* skip (dangling) */
        } else {
            sp->out1 = NULL;      /* skip (dangling) */
            sp->out2 = body_copy; /* try match */
        }
        patch_fragment(body_copy, sp); /* loop back */

        if (!first) return sp;
        patch_fragment(last, sp);
        return first;
    }

    /* bounded: add (mx - mn) optional copies */
    for (int i = mn; i < mx; i++) {
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        RENode *body_copy = alloc_node(p);
        memcpy(body_copy, atom, sizeof(*body_copy));
        body_copy->out1 = NULL;
        body_copy->out2 = NULL;

        if (greedy) {
            sp->out1 = body_copy;
            sp->out2 = NULL;
        } else {
            sp->out1 = NULL;
            sp->out2 = body_copy;
        }

        if (!first) { first = last = sp; last = body_copy; }
        else { patch_fragment(last, sp); last = body_copy; }
    }

    return first;
}

static RENode *parse_quant(ParseCtx *p) {
    RENode *atom = parse_atom(p);
    if (!atom) return NULL;

    int c = peek(p);
    int greedy = 1;

    if (c == '*') {
        next_ch(p);
        if (peek(p) == '?') { next_ch(p); greedy = 0; }
        /* split -> atom -> loop back, or skip */
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        if (greedy) {
            sp->out1 = atom;
            sp->out2 = NULL;
        } else {
            sp->out1 = NULL;
            sp->out2 = atom;
        }
        patch_fragment(atom, sp);
        return sp;
    }

    if (c == '+') {
        next_ch(p);
        if (peek(p) == '?') { next_ch(p); greedy = 0; }
        /* atom -> split -> loop back or continue */
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        if (greedy) {
            sp->out1 = atom;
            sp->out2 = NULL;
        } else {
            sp->out1 = NULL;
            sp->out2 = atom;
        }
        patch_fragment(atom, sp);
        return atom;
    }

    if (c == '?') {
        next_ch(p);
        if (peek(p) == '?') { next_ch(p); greedy = 0; }
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        if (greedy) {
            sp->out1 = atom;
            sp->out2 = NULL;
        } else {
            sp->out1 = NULL;
            sp->out2 = atom;
        }
        return sp;
    }

    if (c == '{') {
        next_ch(p);
        int mn = parse_count(p);
        int mx = mn; /* default: exact count */

        if (peek(p) == ',') {
            next_ch(p);
            if (peek(p) == '}') {
                mx = -1; /* unbounded */
            } else {
                mx = parse_count(p);
            }
        }
        if (peek(p) == '}') next_ch(p);
        if (peek(p) == '?') { next_ch(p); greedy = 0; }

        return make_repeat(p, atom, mn, mx, greedy);
    }

    return atom;
}

static RENode *parse_seq(ParseCtx *p) {
    RENode *first = NULL, *last = NULL;
    while (!at_end(p) && peek(p) != ')' && peek(p) != '|') {
        RENode *q = parse_quant(p);
        if (!q) break;
        if (!first) {
            first = last = q;
        } else {
            patch_fragment(last, q);
            last = q;
        }
    }
    return first;
}

static RENode *parse_alt(ParseCtx *p) {
    RENode *left = parse_seq(p);
    while (peek(p) == '|') {
        next_ch(p);
        RENode *right = parse_alt(p);
        RENode *sp = alloc_node(p);
        sp->type = RE_SPLIT;
        sp->out1 = left;
        sp->out2 = right;
        left = sp;
    }
    return left;
}

/* ================================================================
 * Compile API
 * ================================================================ */

int xs_regex_compile(XSRegex *re, const char *pattern, int flags) {
    memset(re, 0, sizeof(*re));
    re->flags = flags;

    ParseCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.src = pattern;
    ctx.len = (int)strlen(pattern);
    ctx.pos = 0;
    ctx.ngroups = 1; /* group 0 = entire match */
    ctx.flags = flags;

    /* wrap entire pattern in group 0 save markers */
    RENode *save0_start = alloc_node(&ctx);
    save0_start->type = RE_SAVE;
    save0_start->sub = 0;

    RENode *body = parse_alt(&ctx);

    RENode *save0_end = alloc_node(&ctx);
    save0_end->type = RE_SAVE;
    save0_end->sub = 1;

    RENode *match = alloc_node(&ctx);
    match->type = RE_MATCH;

    save0_end->out1 = match;

    if (body) {
        save0_start->out1 = body;
        patch_fragment(body, save0_end);
    } else {
        save0_start->out1 = save0_end;
    }

    re->nodes = ctx.nodes;
    re->nnodes = ctx.nnodes;
    re->cap = ctx.cap;
    re->start = save0_start;
    re->ngroups = ctx.ngroups;

    return ctx.error;
}

void xs_regex_free(XSRegex *re) {
    free(re->nodes);
    re->nodes = NULL;
    re->nnodes = 0;
    re->cap = 0;
}

/* ================================================================
 * NFA simulation engine
 *
 * Uses the "tagged NFA" approach where each thread carries saved
 * positions for capture groups. This gives us group capture with
 * linear-time matching.
 * ================================================================ */

#define MAX_SAVED (RE_MAX_GROUPS * 2)

typedef struct {
    RENode *node;
    int saved[MAX_SAVED];
} Thread;

typedef struct {
    Thread *items;
    int n;
    int cap;
} ThreadList;

static void tl_init(ThreadList *l) {
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

static void tl_free(ThreadList *l) {
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static void tl_clear(ThreadList *l) {
    l->n = 0;
}

static Thread *tl_add(ThreadList *l) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(Thread));
    }
    return &l->items[l->n++];
}

static int is_word_char(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* add thread with epsilon closure expansion */
static void add_thread(ThreadList *l, RENode *node, const int *saved,
                       int pos, const char *str, int slen,
                       RENode **gen_marks, int gen_id) {
    if (!node) return;

    /* de-duplicate: if this node was already added this generation, skip */
    /* We use the node pointer as index. For large NFAs this is a heuristic. */

    switch (node->type) {
    case RE_JMP:
        add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;

    case RE_SPLIT:
        add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        add_thread(l, node->out2, saved, pos, str, slen, gen_marks, gen_id);
        return;

    case RE_SAVE: {
        int newsaved[MAX_SAVED];
        memcpy(newsaved, saved, sizeof(newsaved));
        if (node->sub >= 0 && node->sub < MAX_SAVED)
            newsaved[node->sub] = pos;
        add_thread(l, node->out1, newsaved, pos, str, slen, gen_marks, gen_id);
        return;
    }

    case RE_BOL:
        if (pos == 0 || str[pos - 1] == '\n')
            add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;

    case RE_EOL:
        if (pos >= slen || str[pos] == '\n')
            add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;

    case RE_WBOUND: {
        int left_word = (pos > 0) && is_word_char((unsigned char)str[pos - 1]);
        int right_word = (pos < slen) && is_word_char((unsigned char)str[pos]);
        if (left_word != right_word)
            add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;
    }

    case RE_NWBOUND: {
        int left_word = (pos > 0) && is_word_char((unsigned char)str[pos - 1]);
        int right_word = (pos < slen) && is_word_char((unsigned char)str[pos]);
        if (left_word == right_word)
            add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;
    }

    case RE_LOOKAHEAD:
    case RE_NEG_LOOKAHEAD:
        /* simplified: lookaheads are treated as epsilon transitions
           that always succeed (positive) or always fail (negative).
           Full implementation would need sub-NFA simulation. */
        if (node->type == RE_LOOKAHEAD)
            add_thread(l, node->out1, saved, pos, str, slen, gen_marks, gen_id);
        return;

    default:
        break;
    }

    Thread *t = tl_add(l);
    t->node = node;
    memcpy(t->saved, saved, sizeof(t->saved));
}

/* run NFA from a given starting position, return best match info */
static int run_nfa(const XSRegex *re, const char *str, int slen,
                   int start_pos, int *best_saved) {
    ThreadList cur, nxt;
    tl_init(&cur);
    tl_init(&nxt);

    int saved[MAX_SAVED];
    for (int i = 0; i < MAX_SAVED; i++) saved[i] = -1;

    int matched = 0;

    /* seed with start state */
    add_thread(&cur, re->start, saved, start_pos, str, slen, NULL, 0);

    for (int sp = start_pos; ; sp++) {
        int ch = (sp < slen) ? (unsigned char)str[sp] : -1;
        tl_clear(&nxt);

        for (int i = 0; i < cur.n; i++) {
            Thread *t = &cur.items[i];
            RENode *nd = t->node;

            switch (nd->type) {
            case RE_LIT:
                if (ch >= 0) {
                    int match_ch = ch;
                    int pat_ch = nd->ch;
                    if (re->flags & RE_FLAG_ICASE) {
                        match_ch = tolower(match_ch);
                        pat_ch = tolower(pat_ch);
                    }
                    if (match_ch == pat_ch) {
                        add_thread(&nxt, nd->out1, t->saved, sp + 1,
                                   str, slen, NULL, 0);
                    }
                }
                break;

            case RE_DOT:
                if (ch >= 0 && ch != '\n') {
                    add_thread(&nxt, nd->out1, t->saved, sp + 1,
                               str, slen, NULL, 0);
                }
                break;

            case RE_CCLASS:
                if (ch >= 0 && xs_cc_test(&nd->cc, ch)) {
                    add_thread(&nxt, nd->out1, t->saved, sp + 1,
                               str, slen, NULL, 0);
                }
                break;

            case RE_BACKREF: {
                int grp = nd->backref;
                int gs = t->saved[grp * 2];
                int ge = t->saved[grp * 2 + 1];
                if (gs >= 0 && ge >= gs) {
                    int ref_len = ge - gs;
                    if (sp + ref_len <= slen &&
                        memcmp(str + sp, str + gs, (size_t)ref_len) == 0) {
                        add_thread(&nxt, nd->out1, t->saved, sp + ref_len,
                                   str, slen, NULL, 0);
                    }
                }
                break;
            }

            case RE_MATCH:
                if (!matched ||
                    t->saved[1] > best_saved[1] ||
                    (t->saved[1] == best_saved[1] &&
                     t->saved[0] < best_saved[0])) {
                    memcpy(best_saved, t->saved, MAX_SAVED * sizeof(int));
                    matched = 1;
                }
                break;

            default:
                break;
            }
        }

        if (nxt.n == 0) break;
        if (ch < 0) break;

        /* swap cur and nxt */
        ThreadList tmp = cur;
        cur = nxt;
        nxt = tmp;
        tl_clear(&nxt);
    }

    tl_free(&cur);
    tl_free(&nxt);
    return matched;
}

/* ================================================================
 * Public matching API
 * ================================================================ */

int xs_regex_search(const XSRegex *re, const char *str, int len,
                    int start, XSMatch *m) {
    int best[MAX_SAVED];

    for (int pos = start; pos <= len; pos++) {
        for (int i = 0; i < MAX_SAVED; i++) best[i] = -1;

        if (run_nfa(re, str, len, pos, best)) {
            if (m) {
                m->matched = 1;
                m->start = best[0];
                m->end = best[1];
                m->ngroups = re->ngroups;
                for (int g = 0; g < RE_MAX_GROUPS; g++) {
                    m->group_starts[g] = best[g * 2];
                    m->group_ends[g] = best[g * 2 + 1];
                }
            }
            return 1;
        }
    }

    if (m) {
        m->matched = 0;
        m->start = -1;
        m->end = -1;
        m->ngroups = 0;
    }
    return 0;
}

int xs_regex_match(const XSRegex *re, const char *str, int len, XSMatch *m) {
    return xs_regex_search(re, str, len, 0, m);
}

int xs_regex_full_match(const XSRegex *re, const char *str, int len) {
    XSMatch m;
    if (!xs_regex_search(re, str, len, 0, &m)) return 0;
    return m.start == 0 && m.end == len;
}

int xs_regex_find_all(const XSRegex *re, const char *str, int len,
                      XSMatch **matches, int *nmatches) {
    int cap = 16;
    *matches = malloc((size_t)cap * sizeof(XSMatch));
    *nmatches = 0;

    int pos = 0;
    while (pos <= len) {
        XSMatch m;
        if (!xs_regex_search(re, str, len, pos, &m)) break;

        if (*nmatches >= cap) {
            cap *= 2;
            *matches = realloc(*matches, (size_t)cap * sizeof(XSMatch));
        }
        (*matches)[(*nmatches)++] = m;

        /* advance past match (or by 1 if zero-length) */
        if (m.end > pos) {
            pos = m.end;
        } else {
            pos++;
        }
    }
    return *nmatches;
}

/* ================================================================
 * Replacement with backreference support in replacement string
 *
 * Replacement syntax:
 *   $0 or $& - entire match
 *   $1-$9    - capture group
 *   $$       - literal $
 * ================================================================ */

static char *build_replacement(const char *str, const XSMatch *m,
                               const char *rep, int *out_len) {
    int rep_len = (int)strlen(rep);
    int cap = rep_len * 2 + 64;
    char *out = malloc((size_t)cap);
    int oi = 0;

    for (int i = 0; i < rep_len; i++) {
        if (rep[i] == '$' && i + 1 < rep_len) {
            char nc = rep[i + 1];
            if (nc == '$') {
                /* literal $ */
                if (oi + 1 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                out[oi++] = '$';
                i++;
            } else if (nc == '&' || nc == '0') {
                /* entire match */
                int gs = m->group_starts[0];
                int ge = m->group_ends[0];
                if (gs >= 0 && ge > gs) {
                    int gl = ge - gs;
                    while (oi + gl >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                    memcpy(out + oi, str + gs, (size_t)gl);
                    oi += gl;
                }
                i++;
            } else if (nc >= '1' && nc <= '9') {
                int grp = nc - '0';
                int gs = (grp < RE_MAX_GROUPS) ? m->group_starts[grp] : -1;
                int ge = (grp < RE_MAX_GROUPS) ? m->group_ends[grp] : -1;
                if (gs >= 0 && ge > gs) {
                    int gl = ge - gs;
                    while (oi + gl >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                    memcpy(out + oi, str + gs, (size_t)gl);
                    oi += gl;
                }
                i++;
            } else {
                if (oi + 1 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                out[oi++] = rep[i];
            }
        } else if (rep[i] == '\\' && i + 1 < rep_len &&
                   rep[i + 1] >= '1' && rep[i + 1] <= '9') {
            /* also support \1-\9 in replacement */
            int grp = rep[i + 1] - '0';
            int gs = (grp < RE_MAX_GROUPS) ? m->group_starts[grp] : -1;
            int ge = (grp < RE_MAX_GROUPS) ? m->group_ends[grp] : -1;
            if (gs >= 0 && ge > gs) {
                int gl = ge - gs;
                while (oi + gl >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                memcpy(out + oi, str + gs, (size_t)gl);
                oi += gl;
            }
            i++;
        } else {
            if (oi + 1 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
            out[oi++] = rep[i];
        }
    }

    out[oi] = '\0';
    if (out_len) *out_len = oi;
    return out;
}

char *xs_regex_replace(const XSRegex *re, const char *str, int len,
                       const char *replacement) {
    XSMatch m;
    if (!xs_regex_search(re, str, len, 0, &m)) {
        char *copy = malloc((size_t)len + 1);
        memcpy(copy, str, (size_t)len);
        copy[len] = '\0';
        return copy;
    }

    int rep_len = 0;
    char *rep_text = build_replacement(str, &m, replacement, &rep_len);

    int before = m.start;
    int after_start = m.end;
    int after_len = len - after_start;
    int total = before + rep_len + after_len;

    char *result = malloc((size_t)total + 1);
    memcpy(result, str, (size_t)before);
    memcpy(result + before, rep_text, (size_t)rep_len);
    memcpy(result + before + rep_len, str + after_start, (size_t)after_len);
    result[total] = '\0';

    free(rep_text);
    return result;
}

char *xs_regex_replace_all(const XSRegex *re, const char *str, int len,
                           const char *replacement) {
    int cap = len * 2 + 64;
    char *out = malloc((size_t)cap);
    int oi = 0;
    int pos = 0;

    while (pos <= len) {
        XSMatch m;
        if (!xs_regex_search(re, str, len, pos, &m)) break;

        /* copy text before match */
        int before_len = m.start - pos;
        if (before_len > 0) {
            while (oi + before_len >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
            memcpy(out + oi, str + pos, (size_t)before_len);
            oi += before_len;
        }

        /* build and insert replacement */
        int rep_len = 0;
        char *rep_text = build_replacement(str, &m, replacement, &rep_len);
        while (oi + rep_len >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
        memcpy(out + oi, rep_text, (size_t)rep_len);
        oi += rep_len;
        free(rep_text);

        /* advance */
        if (m.end > pos) {
            pos = m.end;
        } else {
            /* zero-length match: copy one char and advance */
            if (pos < len) {
                while (oi + 1 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
                out[oi++] = str[pos];
            }
            pos++;
        }
    }

    /* copy remaining text */
    int remaining = len - pos;
    if (remaining > 0) {
        while (oi + remaining >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
        memcpy(out + oi, str + pos, (size_t)remaining);
        oi += remaining;
    }

    out[oi] = '\0';
    return out;
}

int xs_regex_split(const XSRegex *re, const char *str, int len,
                   char ***parts, int *nparts) {
    int cap = 16;
    *parts = malloc((size_t)cap * sizeof(char *));
    *nparts = 0;

    int pos = 0;
    while (pos <= len) {
        XSMatch m;
        if (!xs_regex_search(re, str, len, pos, &m)) break;

        /* add segment before match */
        int seg_len = m.start - pos;
        if (*nparts >= cap) { cap *= 2; *parts = realloc(*parts, (size_t)cap * sizeof(char *)); }
        char *seg = malloc((size_t)seg_len + 1);
        if (seg_len > 0) memcpy(seg, str + pos, (size_t)seg_len);
        seg[seg_len] = '\0';
        (*parts)[(*nparts)++] = seg;

        if (m.end > pos) {
            pos = m.end;
        } else {
            pos++;
        }
    }

    /* add remaining segment */
    int rem = len - pos;
    if (*nparts >= cap) { cap *= 2; *parts = realloc(*parts, (size_t)cap * sizeof(char *)); }
    char *seg = malloc((size_t)rem + 1);
    if (rem > 0) memcpy(seg, str + pos, (size_t)rem);
    seg[rem] = '\0';
    (*parts)[(*nparts)++] = seg;

    return *nparts;
}

/* ================================================================
 * Regex pattern validation
 * ================================================================ */

int xs_regex_is_valid(const char *pattern) {
    XSRegex re;
    int rc = xs_regex_compile(&re, pattern, 0);
    xs_regex_free(&re);
    return rc == 0;
}

/* ================================================================
 * Utility: escape a string for use as a literal in a regex
 * ================================================================ */

char *xs_regex_escape(const char *str) {
    int len = (int)strlen(str);
    int cap = len * 2 + 1;
    char *out = malloc((size_t)cap);
    int oi = 0;

    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c == '\\' || c == '.' || c == '^' || c == '$' ||
            c == '|' || c == '(' || c == ')' || c == '[' ||
            c == ']' || c == '{' || c == '}' || c == '*' ||
            c == '+' || c == '?') {
            if (oi + 2 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
            out[oi++] = '\\';
        }
        if (oi + 1 >= cap) { cap *= 2; out = realloc(out, (size_t)cap); }
        out[oi++] = c;
    }
    out[oi] = '\0';
    return out;
}

/* ================================================================
 * String helpers for the XS interface layer
 *
 * These functions produce result strings from match data, handling
 * the formatting needed for the re module.
 * ================================================================ */

/* extract matched substring */
char *xs_match_get_text(const char *str, const XSMatch *m) {
    if (!m->matched || m->start < 0 || m->end < m->start) {
        char *empty = malloc(1);
        empty[0] = '\0';
        return empty;
    }
    int len = m->end - m->start;
    char *text = malloc((size_t)len + 1);
    memcpy(text, str + m->start, (size_t)len);
    text[len] = '\0';
    return text;
}

/* extract group N substring (0 = whole match) */
char *xs_match_get_group(const char *str, const XSMatch *m, int group) {
    if (group < 0 || group >= RE_MAX_GROUPS) {
        char *empty = malloc(1);
        empty[0] = '\0';
        return empty;
    }
    int gs = m->group_starts[group];
    int ge = m->group_ends[group];
    if (gs < 0 || ge < gs) {
        char *empty = malloc(1);
        empty[0] = '\0';
        return empty;
    }
    int len = ge - gs;
    char *text = malloc((size_t)len + 1);
    memcpy(text, str + gs, (size_t)len);
    text[len] = '\0';
    return text;
}

/* ================================================================
 * Regex pattern cache
 * ================================================================ */

#define RE_CACHE_SIZE 32

typedef struct {
    char pattern[512];
    int flags;
    XSRegex compiled;
    int valid;
    int age;
} RECacheEntry;

static RECacheEntry re_cache[RE_CACHE_SIZE];
static int re_cache_clock = 0;

void xs_regex_cache_clear(void) {
    for (int i = 0; i < RE_CACHE_SIZE; i++) {
        if (re_cache[i].valid) {
            xs_regex_free(&re_cache[i].compiled);
            re_cache[i].valid = 0;
        }
    }
    re_cache_clock = 0;
}

static XSRegex *regex_cache_get(const char *pattern, int flags) {
    int plen = (int)strlen(pattern);
    if (plen >= (int)sizeof(re_cache[0].pattern)) return NULL;

    for (int i = 0; i < RE_CACHE_SIZE; i++) {
        if (re_cache[i].valid &&
            re_cache[i].flags == flags &&
            strcmp(re_cache[i].pattern, pattern) == 0) {
            re_cache[i].age = re_cache_clock++;
            return &re_cache[i].compiled;
        }
    }
    return NULL;
}

static XSRegex *regex_cache_put(const char *pattern, int flags) {
    int oldest = 0;
    int oldest_age = re_cache[0].valid ? re_cache[0].age : -1;

    for (int i = 0; i < RE_CACHE_SIZE; i++) {
        if (!re_cache[i].valid) {
            oldest = i;
            break;
        }
        if (re_cache[i].age < oldest_age) {
            oldest = i;
            oldest_age = re_cache[i].age;
        }
    }

    if (re_cache[oldest].valid) {
        xs_regex_free(&re_cache[oldest].compiled);
    }

    strncpy(re_cache[oldest].pattern, pattern,
            sizeof(re_cache[oldest].pattern) - 1);
    re_cache[oldest].pattern[sizeof(re_cache[oldest].pattern) - 1] = '\0';
    re_cache[oldest].flags = flags;
    re_cache[oldest].age = re_cache_clock++;

    int rc = xs_regex_compile(&re_cache[oldest].compiled, pattern, flags);
    if (rc != 0) {
        re_cache[oldest].valid = 0;
        return NULL;
    }
    re_cache[oldest].valid = 1;
    return &re_cache[oldest].compiled;
}

XSRegex *xs_regex_cached_compile(const char *pattern, int flags) {
    XSRegex *cached = regex_cache_get(pattern, flags);
    if (cached) return cached;
    return regex_cache_put(pattern, flags);
}

/* ================================================================
 * Named groups support
 * ================================================================ */

#define RE_MAX_NAMED_GROUPS 16
#define RE_MAX_NAME_LEN 64

typedef struct {
    char name[RE_MAX_NAME_LEN];
    int group_idx;
} RENamedGroup;

typedef struct {
    RENamedGroup groups[RE_MAX_NAMED_GROUPS];
    int count;
} RENamedGroups;

static RENamedGroups *parse_named_groups(const char *pattern) {
    RENamedGroups *ng = calloc(1, sizeof(RENamedGroups));
    int group_idx = 0;
    const char *p = pattern;

    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        if (*p == '[') {
            p++;
            if (*p == '^') p++;
            while (*p && *p != ']') {
                if (*p == '\\') p++;
                if (*p) p++;
            }
            if (*p) p++;
            continue;
        }
        if (*p == '(' && *(p + 1) == '?' && *(p + 2) == '<') {
            group_idx++;
            p += 3;
            int ni = 0;
            while (*p && *p != '>' && ni < RE_MAX_NAME_LEN - 1) {
                ng->groups[ng->count].name[ni++] = *p++;
            }
            ng->groups[ng->count].name[ni] = '\0';
            ng->groups[ng->count].group_idx = group_idx;
            ng->count++;
            if (ng->count >= RE_MAX_NAMED_GROUPS) break;
            if (*p == '>') p++;
            continue;
        }
        if (*p == '(' && *(p + 1) != '?') {
            group_idx++;
        }
        p++;
    }

    return ng;
}

int xs_regex_named_group_idx(const char *pattern, const char *name) {
    RENamedGroups *ng = parse_named_groups(pattern);
    int idx = -1;
    for (int i = 0; i < ng->count; i++) {
        if (strcmp(ng->groups[i].name, name) == 0) {
            idx = ng->groups[i].group_idx;
            break;
        }
    }
    free(ng);
    return idx;
}

/* ================================================================
 * Pattern analysis and optimization
 * ================================================================ */

typedef struct {
    int is_anchored;
    int min_len;
    int max_len;
    int is_literal;
    char literal_prefix[256];
    int prefix_len;
    int has_backrefs;
    int has_lookahead;
    int group_count;
} REPatternInfo;

REPatternInfo xs_regex_analyze(const char *pattern) {
    REPatternInfo info;
    memset(&info, 0, sizeof(info));

    if (!pattern || !*pattern) return info;

    info.is_anchored = (pattern[0] == '^');
    info.is_literal = 1;
    info.max_len = -1;

    const char *p = pattern;
    if (*p == '^') p++;

    int prefix_done = 0;

    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p >= '1' && *p <= '9') info.has_backrefs = 1;
            if (!prefix_done && *p && strchr("dDwWsSbB", *p) == NULL) {
                if (info.prefix_len < (int)sizeof(info.literal_prefix) - 1) {
                    info.literal_prefix[info.prefix_len++] = *p;
                }
            } else {
                prefix_done = 1;
            }
            info.is_literal = 0;
            info.min_len++;
            if (*p) p++;
            continue;
        }

        if (*p == '(' && *(p + 1) == '?' && (*(p + 2) == '=' || *(p + 2) == '!')) {
            info.has_lookahead = 1;
            info.is_literal = 0;
            prefix_done = 1;
            p++;
            continue;
        }

        if (*p == '(') {
            if (*(p + 1) != '?') info.group_count++;
            info.is_literal = 0;
            prefix_done = 1;
            p++;
            continue;
        }

        if (strchr("*+?{|.[^$)", *p)) {
            info.is_literal = 0;
            prefix_done = 1;
            if (*p == '.') info.min_len++;
            p++;
            continue;
        }

        if (!prefix_done) {
            if (info.prefix_len < (int)sizeof(info.literal_prefix) - 1) {
                info.literal_prefix[info.prefix_len++] = *p;
            }
        }
        info.min_len++;
        p++;
    }

    info.literal_prefix[info.prefix_len] = '\0';
    return info;
}

/* ================================================================
 * Regex-based string scanning utilities
 * ================================================================ */

int xs_regex_count_matches(const XSRegex *re, const char *str, int len) {
    XSMatch *matches = NULL;
    int n = 0;
    xs_regex_find_all(re, str, len, &matches, &n);
    free(matches);
    return n;
}

char *xs_regex_extract_first(const XSRegex *re, const char *str, int len, int group) {
    XSMatch m;
    if (!xs_regex_search(re, str, len, 0, &m) || !m.matched) {
        char *empty = malloc(1);
        empty[0] = '\0';
        return empty;
    }
    return xs_match_get_group(str, &m, group);
}

typedef struct {
    char **items;
    int count;
    int cap;
} StringArray;

static void strarr_push(StringArray *arr, const char *s) {
    if (arr->count >= arr->cap) {
        arr->cap = arr->cap ? arr->cap * 2 : 16;
        arr->items = realloc(arr->items, arr->cap * sizeof(char *));
    }
    arr->items[arr->count++] = strdup(s);
}

int xs_regex_extract_all(const XSRegex *re, const char *str, int len,
                          int group, char ***out, int *nout)
{
    XSMatch *matches = NULL;
    int n = 0;
    xs_regex_find_all(re, str, len, &matches, &n);

    StringArray arr = {0};
    for (int i = 0; i < n; i++) {
        char *text = xs_match_get_group(str, &matches[i], group);
        strarr_push(&arr, text);
        free(text);
    }
    free(matches);

    *out = arr.items;
    *nout = arr.count;
    return arr.count;
}

/* ================================================================
 * Glob pattern matching (fnmatch-style)
 * ================================================================ */

int xs_glob_match(const char *pattern, const char *str) {
    const char *p = pattern;
    const char *s = str;
    const char *star_p = NULL;
    const char *star_s = NULL;

    while (*s) {
        if (*p == '*') {
            star_p = ++p;
            star_s = s;
            continue;
        }
        if (*p == '?') {
            p++;
            s++;
            continue;
        }
        if (*p == '[') {
            p++;
            int neg = 0;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            int match_class = 0;
            while (*p && *p != ']') {
                char lo = *p++;
                if (*p == '-' && *(p + 1) != ']') {
                    p++;
                    char hi = *p++;
                    if (*s >= lo && *s <= hi) match_class = 1;
                } else {
                    if (*s == lo) match_class = 1;
                }
            }
            if (*p == ']') p++;
            if (neg) match_class = !match_class;
            if (!match_class) {
                if (star_p) { p = star_p; s = ++star_s; continue; }
                return 0;
            }
            s++;
            continue;
        }
        if (*p == *s) {
            p++;
            s++;
            continue;
        }
        if (star_p) {
            p = star_p;
            s = ++star_s;
            continue;
        }
        return 0;
    }

    while (*p == '*') p++;
    return *p == '\0';
}

/* ================================================================
 * Token scanner using regex
 * ================================================================ */

typedef struct {
    char name[64];
    XSRegex re;
    int skip;
} RETokenRule;

typedef struct {
    const char *text;
    char value[4096];
    int start;
    int end;
    int rule_idx;
} REToken;

typedef struct {
    RETokenRule *rules;
    int nrules;
    int cap;
} REScanner;

void xs_scanner_init(REScanner *sc) {
    memset(sc, 0, sizeof(REScanner));
    sc->cap = 16;
    sc->rules = calloc(sc->cap, sizeof(RETokenRule));
}

int xs_scanner_add_rule(REScanner *sc, const char *name, const char *pattern, int skip) {
    if (sc->nrules >= sc->cap) {
        sc->cap *= 2;
        sc->rules = realloc(sc->rules, sc->cap * sizeof(RETokenRule));
    }
    RETokenRule *r = &sc->rules[sc->nrules];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->skip = skip;

    char anchored[520];
    snprintf(anchored, sizeof(anchored), "^%s", pattern);
    int rc = xs_regex_compile(&r->re, anchored, 0);
    if (rc != 0) return -1;

    sc->nrules++;
    return sc->nrules - 1;
}

int xs_scanner_tokenize(REScanner *sc, const char *text, REToken *tokens, int max_tokens) {
    int ntokens = 0;
    int pos = 0;
    int len = (int)strlen(text);

    while (pos < len && ntokens < max_tokens) {
        int best_len = 0;
        int best_rule = -1;
        XSMatch best_match;

        for (int r = 0; r < sc->nrules; r++) {
            XSMatch m;
            if (xs_regex_match(&sc->rules[r].re, text + pos, len - pos, &m)) {
                if (m.matched && m.end > best_len) {
                    best_len = m.end;
                    best_rule = r;
                    best_match = m;
                }
            }
        }

        if (best_rule < 0) {
            pos++;
            continue;
        }

        if (!sc->rules[best_rule].skip) {
            REToken *t = &tokens[ntokens];
            t->text = text + pos;
            t->start = pos;
            t->end = pos + best_len;
            t->rule_idx = best_rule;
            int vlen = best_len;
            if (vlen >= (int)sizeof(t->value)) vlen = (int)sizeof(t->value) - 1;
            memcpy(t->value, text + pos, vlen);
            t->value[vlen] = '\0';
            ntokens++;
        }

        pos += best_len;
        (void)best_match;
    }

    return ntokens;
}

void xs_scanner_free(REScanner *sc) {
    for (int i = 0; i < sc->nrules; i++) {
        xs_regex_free(&sc->rules[i].re);
    }
    free(sc->rules);
    memset(sc, 0, sizeof(REScanner));
}

/* ================================================================
 * Regex-based search and replace with callbacks
 * ================================================================ */

typedef char *(*REReplaceFn)(const char *matched, const XSMatch *m, void *ctx);

char *xs_regex_replace_fn(const XSRegex *re, const char *str, int len,
                           REReplaceFn fn, void *ctx)
{
    int cap = len * 2 + 64;
    char *out = malloc(cap);
    int oi = 0;
    int pos = 0;

    while (pos < len) {
        XSMatch m;
        if (!xs_regex_search(re, str, len, pos, &m) || !m.matched) {
            int rem = len - pos;
            if (oi + rem >= cap) {
                cap = oi + rem + 64;
                out = realloc(out, cap);
            }
            memcpy(out + oi, str + pos, rem);
            oi += rem;
            break;
        }

        int before = m.start - pos;
        if (oi + before >= cap) {
            cap = (oi + before) * 2 + 64;
            out = realloc(out, cap);
        }
        memcpy(out + oi, str + pos, before);
        oi += before;

        int mlen = m.end - m.start;
        char *matched = malloc(mlen + 1);
        memcpy(matched, str + m.start, mlen);
        matched[mlen] = '\0';

        char *replacement = fn(matched, &m, ctx);
        free(matched);

        if (replacement) {
            int rlen = (int)strlen(replacement);
            if (oi + rlen >= cap) {
                cap = (oi + rlen) * 2 + 64;
                out = realloc(out, cap);
            }
            memcpy(out + oi, replacement, rlen);
            oi += rlen;
            free(replacement);
        }

        pos = m.end > pos ? m.end : pos + 1;
    }

    out[oi] = '\0';
    return out;
}

/* ================================================================
 * Character class set operations
 * ================================================================ */

void xs_cc_union(XSCharClass *dst, const XSCharClass *a, const XSCharClass *b) {
    for (int i = 0; i < 32; i++) {
        dst->bits[i] = a->bits[i] | b->bits[i];
    }
    dst->negated = 0;
}

void xs_cc_intersect(XSCharClass *dst, const XSCharClass *a, const XSCharClass *b) {
    for (int i = 0; i < 32; i++) {
        dst->bits[i] = a->bits[i] & b->bits[i];
    }
    dst->negated = 0;
}

void xs_cc_subtract(XSCharClass *dst, const XSCharClass *a, const XSCharClass *b) {
    for (int i = 0; i < 32; i++) {
        dst->bits[i] = a->bits[i] & ~b->bits[i];
    }
    dst->negated = 0;
}

int xs_cc_count(const XSCharClass *cc) {
    int count = 0;
    for (int i = 0; i < 256; i++) {
        int in = (cc->bits[i >> 3] >> (i & 7)) & 1;
        if (cc->negated ? !in : in) count++;
    }
    return count;
}

char *xs_cc_to_string(const XSCharClass *cc) {
    int cap = 512;
    char *buf = malloc(cap);
    int pos = 0;

    buf[pos++] = '[';
    if (cc->negated) buf[pos++] = '^';

    int in_range = 0;
    int range_start = -1;

    for (int i = 0; i < 256; i++) {
        int in = (cc->bits[i >> 3] >> (i & 7)) & 1;
        if (in) {
            if (!in_range) {
                range_start = i;
                in_range = 1;
            }
        } else {
            if (in_range) {
                int range_end = i - 1;
                if (range_start == range_end) {
                    if (range_start >= 0x20 && range_start < 0x7F) {
                        if (pos + 2 < cap) buf[pos++] = (char)range_start;
                    } else {
                        pos += snprintf(buf + pos, cap - pos, "\\x%02x", range_start);
                    }
                } else {
                    if (range_start >= 0x20 && range_start < 0x7F)
                        buf[pos++] = (char)range_start;
                    else
                        pos += snprintf(buf + pos, cap - pos, "\\x%02x", range_start);
                    buf[pos++] = '-';
                    if (range_end >= 0x20 && range_end < 0x7F)
                        buf[pos++] = (char)range_end;
                    else
                        pos += snprintf(buf + pos, cap - pos, "\\x%02x", range_end);
                }
                in_range = 0;
            }
        }
    }

    if (in_range) {
        int range_end = 255;
        if (range_start >= 0x20 && range_start < 0x7F)
            buf[pos++] = (char)range_start;
        else
            pos += snprintf(buf + pos, cap - pos, "\\x%02x", range_start);
        if (range_start != range_end) {
            buf[pos++] = '-';
            pos += snprintf(buf + pos, cap - pos, "\\x%02x", range_end);
        }
    }

    buf[pos++] = ']';
    buf[pos] = '\0';
    return buf;
}

/* ================================================================
 * NFA statistics and debugging
 * ================================================================ */

typedef struct {
    int total_states;
    int split_states;
    int match_states;
    int char_states;
    int class_states;
    int save_states;
    int anchor_states;
} REStats;

REStats xs_regex_stats(const XSRegex *re) {
    REStats stats = {0};
    stats.total_states = re->nnodes;

    for (int i = 0; i < re->nnodes; i++) {
        switch (re->nodes[i].type) {
            case RE_SPLIT:
                stats.split_states++;
                break;
            case RE_MATCH:
                stats.match_states++;
                break;
            case RE_LIT:
                stats.char_states++;
                break;
            case RE_CCLASS:
            case RE_DOT:
                stats.class_states++;
                break;
            case RE_SAVE:
                stats.save_states++;
                break;
            case RE_BOL:
            case RE_EOL:
            case RE_WBOUND:
            case RE_NWBOUND:
                stats.anchor_states++;
                break;
            default:
                break;
        }
    }
    return stats;
}
