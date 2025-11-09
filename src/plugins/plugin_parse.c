#include "plugins/plugin_parse.h"
#include "core/xs.h"
#include <string.h>
#include <stdlib.h>

/* helper: skip newlines/semicolons */
static void pp_skip_semis(Parser *p) {
    while (parser_check(p, TK_NEWLINE) || parser_check(p, TK_SEMICOLON))
        parser_advance(p);
}

/* helper: skip tokens until closing brace at depth 0 */
static void skip_brace_body(Parser *p) {
    int depth = 1;
    while (depth > 0) {
        Token *t = parser_peek(p, 0);
        if (t->kind == TK_EOF) break;
        if (t->kind == TK_LBRACE) depth++;
        else if (t->kind == TK_RBRACE) { depth--; if (depth == 0) break; }
        parser_advance(p);
    }
}

/* parse meta { key: value, ... } - returns a NODE_LIT_MAP */
static Node *parse_meta_section(Parser *p) {
    parser_expect(p, TK_LBRACE, "expected '{' after 'meta'");
    Span span = parser_peek(p, 0)->span;

    NodeList keys = nodelist_new();
    NodeList vals = nodelist_new();

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *key_tok = parser_peek(p, 0);
        char *key = NULL;
        if (key_tok->kind == TK_IDENT) {
            key = xs_strdup(key_tok->sval ? key_tok->sval : "");
            parser_advance(p);
        } else if (key_tok->kind == TK_STRING) {
            key = xs_strdup(key_tok->sval ? key_tok->sval : "");
            parser_advance(p);
        } else {
            parser_advance(p);
            continue;
        }
        parser_expect(p, TK_COLON, "expected ':' after meta key");

        /* parse value: string, int, or array */
        Token *val_tok = parser_peek(p, 0);
        Node *val_node = NULL;
        if (val_tok->kind == TK_STRING) {
            val_node = node_new(NODE_LIT_STRING, val_tok->span);
            val_node->lit_string.sval = xs_strdup(val_tok->sval ? val_tok->sval : "");
            val_node->lit_string.interpolated = 0;
            val_node->lit_string.parts = nodelist_new();
            parser_advance(p);
        } else if (val_tok->kind == TK_INT) {
            val_node = node_new(NODE_LIT_INT, val_tok->span);
            val_node->lit_int.ival = val_tok->ival;
            parser_advance(p);
        } else if (val_tok->kind == TK_LBRACKET) {
            /* array value: [item, item, ...] */
            parser_advance(p);
            NodeList elems = nodelist_new();
            while (!parser_check(p, TK_RBRACKET) && parser_peek(p, 0)->kind != TK_EOF) {
                Token *et = parser_peek(p, 0);
                if (et->kind == TK_STRING) {
                    Node *s = node_new(NODE_LIT_STRING, et->span);
                    s->lit_string.sval = xs_strdup(et->sval ? et->sval : "");
                    s->lit_string.interpolated = 0;
                    s->lit_string.parts = nodelist_new();
                    nodelist_push(&elems, s);
                    parser_advance(p);
                } else {
                    parser_advance(p);
                }
                Token *comma = parser_peek(p, 0);
                if (comma->kind == TK_COMMA) parser_advance(p);
                else break;
            }
            parser_expect(p, TK_RBRACKET, "expected ']'");
            val_node = node_new(NODE_LIT_ARRAY, val_tok->span);
            val_node->lit_array.elems = elems;
            val_node->lit_array.repeat_val = NULL;
            val_node->lit_array.repeat_cnt = 0;
        } else {
            /* fallback: just treat as identifier string */
            val_node = node_new(NODE_LIT_STRING, val_tok->span);
            val_node->lit_string.sval = xs_strdup(val_tok->sval ? val_tok->sval : "");
            val_node->lit_string.interpolated = 0;
            val_node->lit_string.parts = nodelist_new();
            parser_advance(p);
        }

        Node *key_node = node_new(NODE_LIT_STRING, key_tok->span);
        key_node->lit_string.sval = key;
        key_node->lit_string.interpolated = 0;
        key_node->lit_string.parts = nodelist_new();
        nodelist_push(&keys, key_node);
        nodelist_push(&vals, val_node);

        /* optional comma or newline separator */
        if (parser_check(p, TK_COMMA)) parser_advance(p);
        pp_skip_semis(p);
    }
    parser_expect(p, TK_RBRACE, "expected '}' after meta section");

    Node *map = node_new(NODE_LIT_MAP, span);
    map->lit_map.keys = keys;
    map->lit_map.vals = vals;
    return map;
}

/* parse runtime { exec Type(node, env) { ... } before exec type(node, env) { ... } }
   Returns a NODE_BLOCK with encoded statements */
static Node *parse_runtime_section(Parser *p) {
    parser_expect(p, TK_LBRACE, "expected '{' after 'runtime'");
    Span span = parser_peek(p, 0)->span;

    NodeList stmts = nodelist_new();

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *t = parser_peek(p, 0);
        /* before/after/exec */
        char prefix[16] = "exec";
        if (t->kind == TK_IDENT && t->sval) {
            if (strcmp(t->sval, "before") == 0) {
                strncpy(prefix, "before", sizeof(prefix));
                parser_advance(p);
                t = parser_peek(p, 0);
            } else if (strcmp(t->sval, "after") == 0) {
                strncpy(prefix, "after", sizeof(prefix));
                parser_advance(p);
                t = parser_peek(p, 0);
            }
        }

        /* expect "exec" keyword (as ident) */
        if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "exec") == 0) {
            parser_advance(p);
        } else {
            /* skip unknown token */
            parser_advance(p);
            if (parser_check(p, TK_LBRACE)) {
                parser_advance(p);
                skip_brace_body(p);
                if (parser_check(p, TK_RBRACE)) parser_advance(p);
            }
            continue;
        }

        /* node type name, e.g. fn_call */
        Token *type_tok = parser_peek(p, 0);
        char *type_name = xs_strdup(type_tok->sval ? type_tok->sval : "");
        parser_advance(p);

        /* collect param names from (node, env) */
        ParamList rt_params;
        rt_params.items = NULL; rt_params.len = 0; rt_params.cap = 0;
        if (parser_check(p, TK_LPAREN)) {
            parser_advance(p);
            while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                Token *ptk = parser_peek(p, 0);
                if (ptk->kind == TK_IDENT && ptk->sval) {
                    if (rt_params.len >= rt_params.cap) {
                        rt_params.cap = rt_params.cap ? rt_params.cap * 2 : 4;
                        rt_params.items = realloc(rt_params.items, rt_params.cap * sizeof(Param));
                    }
                    Param *pm = &rt_params.items[rt_params.len++];
                    memset(pm, 0, sizeof(Param));
                    pm->name = xs_strdup(ptk->sval);
                    pm->span = ptk->span;
                    Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                    ppat->pat_ident.name = xs_strdup(ptk->sval);
                    ppat->pat_ident.mutable = 0;
                    pm->pattern = ppat;
                }
                parser_advance(p);
                if (parser_check(p, TK_COMMA)) parser_advance(p);
            }
            if (parser_check(p, TK_RPAREN)) parser_advance(p);
        }

        /* parse body block and wrap as lambda */
        Node *body = NULL;
        if (parser_check(p, TK_LBRACE)) {
            Node *raw_body = parser_parse_block(p);
            Node *lam = node_new(NODE_LAMBDA, t->span);
            lam->lambda.params = rt_params;
            lam->lambda.body = raw_body;
            lam->lambda.is_generator = 0;
            body = lam;
        } else {
            free(rt_params.items);
        }

        /* encode as __rt_exec_Type or __rt_before_Type etc */
        char namebuf[256];
        snprintf(namebuf, sizeof(namebuf), "__rt_%s_%s", prefix, type_name);
        free(type_name);

        Node *name_node = node_new(NODE_LIT_STRING, t->span);
        name_node->lit_string.sval = xs_strdup(namebuf);
        name_node->lit_string.interpolated = 0;
        name_node->lit_string.parts = nodelist_new();

        /* store as a let: let __rt_exec_fn_call = body */
        Node *let = node_new(NODE_LET, t->span);
        Node *pat = node_new(NODE_PAT_IDENT, t->span);
        pat->pat_ident.name = xs_strdup(namebuf);
        pat->pat_ident.mutable = 0;
        let->let.pattern = pat;
        let->let.name = xs_strdup(namebuf);
        let->let.value = body;
        let->let.mutable = 0;
        let->let.type_ann = NULL;
        let->let.contract = NULL;

        nodelist_push(&stmts, let);
        pp_skip_semis(p);
    }
    parser_expect(p, TK_RBRACE, "expected '}' after runtime section");

    Node *blk = node_new(NODE_BLOCK, span);
    blk->block.stmts = stmts;
    blk->block.expr = NULL;
    blk->block.has_decls = 1;
    blk->block.is_unsafe = 0;
    return blk;
}

/* parse sema { rule Name(node) { ... } override rule Name(node) { ... } } */
static Node *parse_sema_section(Parser *p) {
    parser_expect(p, TK_LBRACE, "expected '{' after 'sema'");
    Span span = parser_peek(p, 0)->span;
    NodeList stmts = nodelist_new();

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *t = parser_peek(p, 0);
        /* optional "override" and/or "exclusive" prefix */
        char kind_prefix[16] = "new";
        if (t->kind == TK_IDENT && t->sval) {
            if (strcmp(t->sval, "override") == 0) {
                strncpy(kind_prefix, "override", sizeof(kind_prefix));
                parser_advance(p);
                t = parser_peek(p, 0);
                /* check for "override exclusive" combo */
                if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "exclusive") == 0) {
                    strncpy(kind_prefix, "exclusive", sizeof(kind_prefix));
                    parser_advance(p);
                    t = parser_peek(p, 0);
                }
            } else if (strcmp(t->sval, "exclusive") == 0) {
                strncpy(kind_prefix, "exclusive", sizeof(kind_prefix));
                parser_advance(p);
                t = parser_peek(p, 0);
            }
        }

        /* expect "rule" keyword */
        if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "rule") == 0) {
            parser_advance(p);
            Token *name_tok = parser_peek(p, 0);
            char *rule_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            parser_advance(p);

            /* parse (node) params into a proper parameter list */
            ParamList sema_params;
            memset(&sema_params, 0, sizeof(sema_params));
            if (parser_check(p, TK_LPAREN)) {
                parser_advance(p);
                while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                    Token *ptk = parser_peek(p, 0);
                    if (ptk->kind == TK_IDENT && ptk->sval) {
                        parser_advance(p);
                        if (sema_params.len >= sema_params.cap) {
                            sema_params.cap = sema_params.cap ? sema_params.cap * 2 : 4;
                            sema_params.items = realloc(sema_params.items, sema_params.cap * sizeof(Param));
                        }
                        Param *pm = &sema_params.items[sema_params.len++];
                        memset(pm, 0, sizeof(Param));
                        pm->name = xs_strdup(ptk->sval);
                        pm->span = ptk->span;
                        Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                        ppat->pat_ident.name = xs_strdup(ptk->sval);
                        ppat->pat_ident.mutable = 0;
                        pm->pattern = ppat;
                    } else {
                        parser_advance(p);
                    }
                    if (parser_check(p, TK_COMMA)) parser_advance(p);
                }
                if (parser_check(p, TK_RPAREN)) parser_advance(p);
            }

            /* parse body and wrap in lambda with params */
            Node *body = NULL;
            if (parser_check(p, TK_LBRACE)) {
                Node *raw_body = parser_parse_block(p);
                Node *lam = node_new(NODE_LAMBDA, t->span);
                lam->lambda.params = sema_params;
                lam->lambda.body = raw_body;
                lam->lambda.is_generator = 0;
                body = lam;
            } else {
                free(sema_params.items);
            }

            /* encode as: let __sema_KIND_Name = body */
            char namebuf[256];
            snprintf(namebuf, sizeof(namebuf), "__sema_%s_%s", kind_prefix, rule_name);
            free(rule_name);

            Node *let = node_new(NODE_LET, t->span);
            Node *pat = node_new(NODE_PAT_IDENT, t->span);
            pat->pat_ident.name = xs_strdup(namebuf);
            pat->pat_ident.mutable = 0;
            let->let.pattern = pat;
            let->let.name = xs_strdup(namebuf);
            let->let.value = body;
            let->let.mutable = 0;
            let->let.type_ann = NULL;
            let->let.contract = NULL;

            nodelist_push(&stmts, let);
        } else {
            /* skip unknown */
            if (t->kind == TK_LBRACE) {
                parser_advance(p);
                skip_brace_body(p);
                if (parser_check(p, TK_RBRACE)) parser_advance(p);
            } else {
                parser_advance(p);
            }
        }
        pp_skip_semis(p);
    }
    parser_expect(p, TK_RBRACE, "expected '}' after sema section");

    Node *blk = node_new(NODE_BLOCK, span);
    blk->block.stmts = stmts;
    blk->block.expr = NULL;
    blk->block.has_decls = 1;
    blk->block.is_unsafe = 0;
    return blk;
}

/* parse lexer { token NAME = "pattern" ... } */
static Node *parse_lexer_section(Parser *p) {
    parser_expect(p, TK_LBRACE, "expected '{' after 'lexer'");
    Span span = parser_peek(p, 0)->span;
    NodeList stmts = nodelist_new();

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *t = parser_peek(p, 0);
        /* token NAME = "pattern" */
        if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "token") == 0) {
            parser_advance(p);
            Token *name_tok = parser_peek(p, 0);
            char *tok_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            parser_advance(p);

            /* expect '=' */
            if (parser_check(p, TK_ASSIGN)) parser_advance(p);

            /* expect pattern string */
            Token *pat_tok = parser_peek(p, 0);
            char *tok_pattern = xs_strdup(pat_tok->sval ? pat_tok->sval : "");
            parser_advance(p);

            /* encode as: let __lx_NAME = "pattern" */
            char namebuf[256];
            snprintf(namebuf, sizeof(namebuf), "__lx_%s", tok_name);
            free(tok_name);

            Node *let = node_new(NODE_LET, t->span);
            Node *pat = node_new(NODE_PAT_IDENT, t->span);
            pat->pat_ident.name = xs_strdup(namebuf);
            pat->pat_ident.mutable = 0;
            let->let.pattern = pat;
            let->let.name = xs_strdup(namebuf);

            Node *val = node_new(NODE_LIT_STRING, t->span);
            val->lit_string.sval = tok_pattern;
            val->lit_string.interpolated = 0;
            val->lit_string.parts = nodelist_new();
            let->let.value = val;
            let->let.mutable = 0;
            let->let.type_ann = NULL;
            let->let.contract = NULL;

            nodelist_push(&stmts, let);
        } else if (t->kind == TK_IDENT && t->sval && strcmp(t->sval, "rule") == 0) {
            /* rule after(TOKEN, TOKEN) { body } */
            static int lx_rule_idx = 0;
            parser_advance(p);
            Token *event_tok = parser_peek(p, 0);
            char *event_name = xs_strdup(event_tok->sval ? event_tok->sval : "after");
            parser_advance(p);

            /* parse (TOKEN, TOKEN) params */
            ParamList rule_params;
            rule_params.items = NULL; rule_params.len = 0; rule_params.cap = 0;
            if (parser_check(p, TK_LPAREN)) {
                parser_advance(p);
                while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                    Token *ptk = parser_peek(p, 0);
                    if (ptk->kind == TK_IDENT && ptk->sval) {
                        if (rule_params.len >= rule_params.cap) {
                            rule_params.cap = rule_params.cap ? rule_params.cap * 2 : 4;
                            rule_params.items = realloc(rule_params.items, rule_params.cap * sizeof(Param));
                        }
                        Param *pm = &rule_params.items[rule_params.len++];
                        memset(pm, 0, sizeof(Param));
                        pm->name = xs_strdup(ptk->sval);
                        pm->span = ptk->span;
                        Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                        ppat->pat_ident.name = xs_strdup(ptk->sval);
                        ppat->pat_ident.mutable = 0;
                        pm->pattern = ppat;
                    }
                    parser_advance(p);
                    if (parser_check(p, TK_COMMA)) parser_advance(p);
                }
                if (parser_check(p, TK_RPAREN)) parser_advance(p);
            }

            /* parse body block and wrap as lambda */
            Node *body = NULL;
            if (parser_check(p, TK_LBRACE)) {
                Node *raw_body = parser_parse_block(p);
                Node *lam = node_new(NODE_LAMBDA, t->span);
                lam->lambda.params = rule_params;
                lam->lambda.body = raw_body;
                lam->lambda.is_generator = 0;
                body = lam;
            } else {
                free(rule_params.items);
            }

            /* encode as: let __lx_rule_N_EVENT = body */
            char namebuf[256];
            snprintf(namebuf, sizeof(namebuf), "__lx_rule_%d_%s", lx_rule_idx++, event_name);
            free(event_name);

            Node *let = node_new(NODE_LET, t->span);
            Node *pat = node_new(NODE_PAT_IDENT, t->span);
            pat->pat_ident.name = xs_strdup(namebuf);
            pat->pat_ident.mutable = 0;
            let->let.pattern = pat;
            let->let.name = xs_strdup(namebuf);
            let->let.value = body;
            let->let.mutable = 0;
            let->let.type_ann = NULL;
            let->let.contract = NULL;

            nodelist_push(&stmts, let);
        } else {
            /* skip unknown content */
            if (t->kind == TK_LBRACE) {
                parser_advance(p);
                skip_brace_body(p);
                if (parser_check(p, TK_RBRACE)) parser_advance(p);
            } else {
                parser_advance(p);
            }
        }
        pp_skip_semis(p);
    }
    parser_expect(p, TK_RBRACE, "expected '}' after lexer section");

    Node *blk = node_new(NODE_BLOCK, span);
    blk->block.stmts = stmts;
    blk->block.expr = NULL;
    blk->block.has_decls = 1;
    blk->block.is_unsafe = 0;
    return blk;
}

/* parse parser { extend IDENT (params) { body } | production IDENT (params) { body } } */
static Node *parse_parser_section(Parser *p) {
    parser_expect(p, TK_LBRACE, "expected '{' after 'parser'");
    Span span = parser_peek(p, 0)->span;
    NodeList stmts = nodelist_new();

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *t = parser_peek(p, 0);
        /* extend NAME or production NAME */
        char kind_prefix[16] = "";
        if (t->kind == TK_IDENT && t->sval) {
            if (strcmp(t->sval, "extend") == 0) {
                strncpy(kind_prefix, "extend", sizeof(kind_prefix));
                parser_advance(p);
            } else if (strcmp(t->sval, "production") == 0) {
                strncpy(kind_prefix, "production", sizeof(kind_prefix));
                parser_advance(p);
            }
        }

        if (kind_prefix[0]) {
            Token *name_tok = parser_peek(p, 0);
            char *rule_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
            parser_advance(p);

            /* collect params from (parser, token) */
            ParamList pr_params;
            pr_params.items = NULL; pr_params.len = 0; pr_params.cap = 0;
            if (parser_check(p, TK_LPAREN)) {
                parser_advance(p);
                while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                    Token *ptk = parser_peek(p, 0);
                    if (ptk->kind == TK_IDENT && ptk->sval) {
                        if (pr_params.len >= pr_params.cap) {
                            pr_params.cap = pr_params.cap ? pr_params.cap * 2 : 4;
                            pr_params.items = realloc(pr_params.items, pr_params.cap * sizeof(Param));
                        }
                        Param *pm = &pr_params.items[pr_params.len++];
                        memset(pm, 0, sizeof(Param));
                        pm->name = xs_strdup(ptk->sval);
                        pm->span = ptk->span;
                        Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                        ppat->pat_ident.name = xs_strdup(ptk->sval);
                        ppat->pat_ident.mutable = 0;
                        pm->pattern = ppat;
                    }
                    parser_advance(p);
                    if (parser_check(p, TK_COMMA)) parser_advance(p);
                }
                if (parser_check(p, TK_RPAREN)) parser_advance(p);
            }

            /* parse body block and wrap as lambda */
            Node *body = NULL;
            if (parser_check(p, TK_LBRACE)) {
                Node *raw_body = parser_parse_block(p);
                Node *lam = node_new(NODE_LAMBDA, t->span);
                lam->lambda.params = pr_params;
                lam->lambda.body = raw_body;
                lam->lambda.is_generator = 0;
                body = lam;
            } else {
                free(pr_params.items);
            }

            /* encode as let __parser_extend_NAME = body or __parser_production_NAME = body */
            char namebuf[256];
            snprintf(namebuf, sizeof(namebuf), "__parser_%s_%s", kind_prefix, rule_name);
            free(rule_name);

            Node *let = node_new(NODE_LET, t->span);
            Node *pat = node_new(NODE_PAT_IDENT, t->span);
            pat->pat_ident.name = xs_strdup(namebuf);
            pat->pat_ident.mutable = 0;
            let->let.pattern = pat;
            let->let.name = xs_strdup(namebuf);
            let->let.value = body;
            let->let.mutable = 0;
            let->let.type_ann = NULL;
            let->let.contract = NULL;

            nodelist_push(&stmts, let);
        } else {
            /* skip unknown content */
            if (t->kind == TK_LBRACE) {
                parser_advance(p);
                skip_brace_body(p);
                if (parser_check(p, TK_RBRACE)) parser_advance(p);
            } else {
                parser_advance(p);
            }
        }
        pp_skip_semis(p);
    }
    parser_expect(p, TK_RBRACE, "expected '}' after parser section");

    Node *blk = node_new(NODE_BLOCK, span);
    blk->block.stmts = stmts;
    blk->block.expr = NULL;
    blk->block.has_decls = 1;
    blk->block.is_unsafe = 0;
    return blk;
}

/* parse pass "name" { phase: after(parser), kind: analyze, visit Type(node) { ... }, ... } */
static Node *parse_pass_section(Parser *p) {
    /* "pass" has already been consumed, now get the name string */
    Token *name_tok = parser_expect(p, TK_STRING, "expected pass name string");
    char *pass_name = xs_strdup(name_tok->sval ? name_tok->sval : "");
    parser_expect(p, TK_LBRACE, "expected '{' after pass name");
    Span span = parser_peek(p, 0)->span;

    NodeList stmts = nodelist_new();

    /* store the pass name as __pass_name */
    {
        Node *let = node_new(NODE_LET, span);
        Node *pat = node_new(NODE_PAT_IDENT, span);
        pat->pat_ident.name = xs_strdup("__pass_name");
        pat->pat_ident.mutable = 0;
        let->let.pattern = pat;
        let->let.name = xs_strdup("__pass_name");
        Node *val = node_new(NODE_LIT_STRING, span);
        val->lit_string.sval = pass_name;
        val->lit_string.interpolated = 0;
        val->lit_string.parts = nodelist_new();
        let->let.value = val;
        let->let.mutable = 0;
        let->let.type_ann = NULL;
        let->let.contract = NULL;
        nodelist_push(&stmts, let);
    }

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *t = parser_peek(p, 0);
        if (t->kind == TK_IDENT && t->sval) {
            /* phase: after(parser) or phase: before(sema) */
            if (strcmp(t->sval, "phase") == 0) {
                parser_advance(p);
                if (parser_check(p, TK_COLON)) parser_advance(p);
                Token *dir_tok = parser_peek(p, 0);
                char *direction = xs_strdup(dir_tok->sval ? dir_tok->sval : "after");
                parser_advance(p);
                char *ref = NULL;
                if (parser_check(p, TK_LPAREN)) {
                    parser_advance(p);
                    Token *ref_tok = parser_peek(p, 0);
                    ref = xs_strdup(ref_tok->sval ? ref_tok->sval : "parser");
                    parser_advance(p);
                    if (parser_check(p, TK_RPAREN)) parser_advance(p);
                } else {
                    ref = xs_strdup("parser");
                }
                /* encode as: let __pass_phase = "after" or "before" */
                Node *let = node_new(NODE_LET, t->span);
                Node *pat = node_new(NODE_PAT_IDENT, t->span);
                pat->pat_ident.name = xs_strdup("__pass_phase");
                pat->pat_ident.mutable = 0;
                let->let.pattern = pat;
                let->let.name = xs_strdup("__pass_phase");
                Node *val = node_new(NODE_LIT_STRING, t->span);
                val->lit_string.sval = direction;
                val->lit_string.interpolated = 0;
                val->lit_string.parts = nodelist_new();
                let->let.value = val;
                let->let.mutable = 0;
                let->let.type_ann = NULL;
                let->let.contract = NULL;
                nodelist_push(&stmts, let);
                /* encode phase_ref */
                Node *let2 = node_new(NODE_LET, t->span);
                Node *pat2 = node_new(NODE_PAT_IDENT, t->span);
                pat2->pat_ident.name = xs_strdup("__pass_phase_ref");
                pat2->pat_ident.mutable = 0;
                let2->let.pattern = pat2;
                let2->let.name = xs_strdup("__pass_phase_ref");
                Node *val2 = node_new(NODE_LIT_STRING, t->span);
                val2->lit_string.sval = ref;
                val2->lit_string.interpolated = 0;
                val2->lit_string.parts = nodelist_new();
                let2->let.value = val2;
                let2->let.mutable = 0;
                let2->let.type_ann = NULL;
                let2->let.contract = NULL;
                nodelist_push(&stmts, let2);
            }
            /* kind: analyze/annotate/transform */
            else if (strcmp(t->sval, "kind") == 0) {
                parser_advance(p);
                if (parser_check(p, TK_COLON)) parser_advance(p);
                Token *kind_tok = parser_peek(p, 0);
                char *kind_str = xs_strdup(kind_tok->sval ? kind_tok->sval : "analyze");
                parser_advance(p);
                Node *let = node_new(NODE_LET, t->span);
                Node *pat = node_new(NODE_PAT_IDENT, t->span);
                pat->pat_ident.name = xs_strdup("__pass_kind");
                pat->pat_ident.mutable = 0;
                let->let.pattern = pat;
                let->let.name = xs_strdup("__pass_kind");
                Node *val = node_new(NODE_LIT_STRING, t->span);
                val->lit_string.sval = kind_str;
                val->lit_string.interpolated = 0;
                val->lit_string.parts = nodelist_new();
                let->let.value = val;
                let->let.mutable = 0;
                let->let.type_ann = NULL;
                let->let.contract = NULL;
                nodelist_push(&stmts, let);
            }
            /* state { key: val, ... } - parse as map for visitor state */
            else if (strcmp(t->sval, "state") == 0) {
                parser_advance(p);
                if (parser_check(p, TK_LBRACE)) {
                    /* reuse meta section parser for key: value pairs */
                    Node *state_map = parse_meta_section(p);
                    /* store as __pass_state = #{...} */
                    Node *let = node_new(NODE_LET, t->span);
                    Node *pat = node_new(NODE_PAT_IDENT, t->span);
                    pat->pat_ident.name = xs_strdup("__pass_state");
                    pat->pat_ident.mutable = 0;
                    let->let.pattern = pat;
                    let->let.name = xs_strdup("__pass_state");
                    let->let.value = state_map;
                    let->let.mutable = 0;
                    let->let.type_ann = NULL;
                    let->let.contract = NULL;
                    nodelist_push(&stmts, let);
                }
            }
            /* visit NodeType(param) { body } */
            else if (strcmp(t->sval, "visit") == 0) {
                parser_advance(p);
                Token *type_tok = parser_peek(p, 0);
                char *type_name = xs_strdup(type_tok->sval ? type_tok->sval : "");
                parser_advance(p);

                /* collect params from (node) */
                ParamList visit_params;
                visit_params.items = NULL; visit_params.len = 0; visit_params.cap = 0;
                if (parser_check(p, TK_LPAREN)) {
                    parser_advance(p);
                    while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                        Token *ptk = parser_peek(p, 0);
                        if (ptk->kind == TK_IDENT && ptk->sval) {
                            if (visit_params.len >= visit_params.cap) {
                                visit_params.cap = visit_params.cap ? visit_params.cap * 2 : 4;
                                visit_params.items = realloc(visit_params.items, visit_params.cap * sizeof(Param));
                            }
                            Param *pm = &visit_params.items[visit_params.len++];
                            memset(pm, 0, sizeof(Param));
                            pm->name = xs_strdup(ptk->sval);
                            pm->span = ptk->span;
                            Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                            ppat->pat_ident.name = xs_strdup(ptk->sval);
                            ppat->pat_ident.mutable = 0;
                            pm->pattern = ppat;
                        }
                        parser_advance(p);
                        if (parser_check(p, TK_COMMA)) parser_advance(p);
                    }
                    if (parser_check(p, TK_RPAREN)) parser_advance(p);
                }

                /* parse body as lambda */
                Node *body = NULL;
                if (parser_check(p, TK_LBRACE)) {
                    Node *raw_body = parser_parse_block(p);
                    Node *lam = node_new(NODE_LAMBDA, t->span);
                    lam->lambda.params = visit_params;
                    lam->lambda.body = raw_body;
                    lam->lambda.is_generator = 0;
                    body = lam;
                } else {
                    free(visit_params.items);
                }

                /* encode as: let __pass_visit_TYPE = lambda */
                char namebuf[256];
                snprintf(namebuf, sizeof(namebuf), "__pass_visit_%s", type_name);
                free(type_name);

                Node *let = node_new(NODE_LET, t->span);
                Node *pat = node_new(NODE_PAT_IDENT, t->span);
                pat->pat_ident.name = xs_strdup(namebuf);
                pat->pat_ident.mutable = 0;
                let->let.pattern = pat;
                let->let.name = xs_strdup(namebuf);
                let->let.value = body;
                let->let.mutable = 0;
                let->let.type_ann = NULL;
                let->let.contract = NULL;
                nodelist_push(&stmts, let);
            }
            /* on scope_exit(param) { body } */
            else if (strcmp(t->sval, "on") == 0) {
                parser_advance(p);
                Token *event_tok = parser_peek(p, 0);
                (void)event_tok; /* currently only scope_exit */
                parser_advance(p);
                /* params */
                ParamList on_params;
                on_params.items = NULL; on_params.len = 0; on_params.cap = 0;
                if (parser_check(p, TK_LPAREN)) {
                    parser_advance(p);
                    while (!parser_check(p, TK_RPAREN) && parser_peek(p, 0)->kind != TK_EOF) {
                        Token *ptk = parser_peek(p, 0);
                        if (ptk->kind == TK_IDENT && ptk->sval) {
                            if (on_params.len >= on_params.cap) {
                                on_params.cap = on_params.cap ? on_params.cap * 2 : 4;
                                on_params.items = realloc(on_params.items, on_params.cap * sizeof(Param));
                            }
                            Param *pm = &on_params.items[on_params.len++];
                            memset(pm, 0, sizeof(Param));
                            pm->name = xs_strdup(ptk->sval);
                            pm->span = ptk->span;
                            Node *ppat = node_new(NODE_PAT_IDENT, ptk->span);
                            ppat->pat_ident.name = xs_strdup(ptk->sval);
                            ppat->pat_ident.mutable = 0;
                            pm->pattern = ppat;
                        }
                        parser_advance(p);
                        if (parser_check(p, TK_COMMA)) parser_advance(p);
                    }
                    if (parser_check(p, TK_RPAREN)) parser_advance(p);
                }
                Node *body = NULL;
                if (parser_check(p, TK_LBRACE)) {
                    Node *raw_body = parser_parse_block(p);
                    Node *lam = node_new(NODE_LAMBDA, t->span);
                    lam->lambda.params = on_params;
                    lam->lambda.body = raw_body;
                    lam->lambda.is_generator = 0;
                    body = lam;
                } else {
                    free(on_params.items);
                }
                Node *let = node_new(NODE_LET, t->span);
                Node *pat = node_new(NODE_PAT_IDENT, t->span);
                pat->pat_ident.name = xs_strdup("__pass_on_scope_exit");
                pat->pat_ident.mutable = 0;
                let->let.pattern = pat;
                let->let.name = xs_strdup("__pass_on_scope_exit");
                let->let.value = body;
                let->let.mutable = 0;
                let->let.type_ann = NULL;
                let->let.contract = NULL;
                nodelist_push(&stmts, let);
            }
            else {
                /* skip unknown key: value or block */
                parser_advance(p);
                if (parser_check(p, TK_COLON)) {
                    parser_advance(p);
                    parser_advance(p); /* skip value */
                } else if (parser_check(p, TK_LBRACE)) {
                    parser_advance(p);
                    skip_brace_body(p);
                    if (parser_check(p, TK_RBRACE)) parser_advance(p);
                }
            }
        } else {
            /* skip non-ident tokens */
            if (t->kind == TK_LBRACE) {
                parser_advance(p);
                skip_brace_body(p);
                if (parser_check(p, TK_RBRACE)) parser_advance(p);
            } else {
                parser_advance(p);
            }
        }
        pp_skip_semis(p);
    }
    if (parser_check(p, TK_RBRACE)) parser_advance(p);

    Node *blk = node_new(NODE_BLOCK, span);
    blk->block.stmts = stmts;
    blk->block.expr = NULL;
    blk->block.has_decls = 1;
    blk->block.is_unsafe = 0;
    return blk;
}

Node *parse_plugin_decl(Parser *p) {
    /* "plugin" ident has already been consumed by caller */
    Span span = parser_peek(p, -1)->span;

    Token *name_tok = parser_expect(p, TK_STRING, "expected plugin name string");
    char *name = xs_strdup(name_tok->sval ? name_tok->sval : "");

    parser_expect(p, TK_LBRACE, "expected '{' after plugin name");

    Node *meta = NULL;
    Node *lexer_sec = NULL;
    Node *parser_sec = NULL;
    NodeList passes = nodelist_new();
    Node *sema_sec = NULL;
    Node *runtime_sec = NULL;

    while (!parser_check(p, TK_RBRACE) && parser_peek(p, 0)->kind != TK_EOF) {
        pp_skip_semis(p);
        if (parser_check(p, TK_RBRACE)) break;

        Token *sec = parser_peek(p, 0);
        if (sec->kind == TK_IDENT && sec->sval) {
            if (strcmp(sec->sval, "meta") == 0) {
                parser_advance(p);
                meta = parse_meta_section(p);
            } else if (strcmp(sec->sval, "lexer") == 0) {
                parser_advance(p);
                lexer_sec = parse_lexer_section(p);
            } else if (strcmp(sec->sval, "parser") == 0) {
                parser_advance(p);
                parser_sec = parse_parser_section(p);
            } else if (strcmp(sec->sval, "pass") == 0) {
                parser_advance(p);
                Node *pass = parse_pass_section(p);
                nodelist_push(&passes, pass);
            } else if (strcmp(sec->sval, "sema") == 0) {
                parser_advance(p);
                sema_sec = parse_sema_section(p);
            } else if (strcmp(sec->sval, "runtime") == 0) {
                parser_advance(p);
                runtime_sec = parse_runtime_section(p);
            } else {
                /* skip unknown section */
                parser_advance(p);
                if (parser_check(p, TK_LBRACE)) {
                    parser_advance(p);
                    skip_brace_body(p);
                    if (parser_check(p, TK_RBRACE)) parser_advance(p);
                }
            }
        } else {
            parser_advance(p);
        }
    }
    parser_expect(p, TK_RBRACE, "expected '}' after plugin block");

    Node *n = node_new(NODE_PLUGIN_DECL, span);
    n->plugin_decl.name = name;
    n->plugin_decl.meta = meta;
    n->plugin_decl.lexer_sec = lexer_sec;
    n->plugin_decl.parser_sec = parser_sec;
    n->plugin_decl.passes = passes;
    n->plugin_decl.sema_sec = sema_sec;
    n->plugin_decl.runtime_sec = runtime_sec;
    return n;
}
