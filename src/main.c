#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* The Makefile injects XS_VERSION via -D from the contents of
 * ./VERSION. The fallback below keeps the TU buildable in isolation
 * (IDE indexers, ad-hoc compiles); release builds always carry the
 * real version. */
#ifndef XS_VERSION
#define XS_VERSION "dev"
#endif
#define XS_VERSION_TAG "xs " XS_VERSION

#include "core/xs_compat.h"
#include "core/xs.h"
#include "core/value.h"
#include "core/ast.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/limits.h"
#include "core/gc.h"
#include "diagnostic/diagnostic.h"
#include "runtime/interp.h"
#include "runtime/error.h"
#include "runtime/concurrent.h"
#include "runtime/triggers.h"
#include "semantic/sema.h"
#include "semantic/cache.h"
#include "types/inference.h"
#include "lint/lint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <time.h>
#include <signal.h>
#ifdef XSC_ENABLE_VM
#include "vm/bytecode.h"
#include "vm/compiler.h"
#include "vm/vm.h"
#endif
#ifdef XSC_ENABLE_LSP
#include "lsp/lsp.h"
#endif
#ifdef XSC_ENABLE_DAP
#include "dap/dap.h"
#endif
#ifdef XSC_ENABLE_TRACER
#include "tracer/tracer.h"
#include "tracer/replay.h"
#endif
#ifdef XSC_ENABLE_FMT
#include "fmt/fmt.h"
#endif
#ifdef XSC_ENABLE_PKG
#include "pkg/pkg.h"
#endif
#ifdef XSC_ENABLE_PROFILER
#include "profiler/profiler.h"
#endif
#ifdef XSC_ENABLE_COVERAGE
#include "coverage/coverage.h"
#endif
#ifdef XSC_ENABLE_DOC
#include "doc/docgen.h"
#endif
#ifdef XSC_ENABLE_TRANSPILER
#include "transpiler/js.h"
#include "transpiler/c_gen.h"
#include "transpiler/wasm.h"
#endif
#ifdef XSC_ENABLE_PLUGINS
#include "plugins/plugin_api.h"
#include "plugins/pipeline.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef XSC_ENABLE_JIT
#include "jit/jit.h"
#endif

#include "optimizer/optimizer.h"
#include "ir/ir.h"

int g_no_color = 0;

static int program_has_plugin_use(Node *program) {
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return 0;
    for (int j = 0; j < program->program.stmts.len; j++) {
        Node *s = program->program.stmts.items[j];
        if (s && VAL_TAG(s) == NODE_USE && s->use_.is_plugin) return 1;
        if (s && VAL_TAG(s) == NODE_LOAD) return 1;
    }
    return 0;
}

/* Detect whether a program registers AST-level runtime hooks
   (plugin.runtime.after_eval, before_eval, ast_hook, etc.). These require
   the tree-walk interpreter because the VM compiles AST nodes away before
   execution. Returns 1 if any such call is present. */
static int node_uses_plugin_runtime_hook(Node *n) {
    if (!n) return 0;
    /* bind requires interp-side reactive tracking; VM compiles it as a
       plain let so subsequent mutations of dependencies do not propagate. */
    if (VAL_TAG(n) == NODE_BIND) return 1;
    if (VAL_TAG(n) == NODE_METHOD_CALL) {
        const char *m = n->method_call.method;
        if (m && (strcmp(m, "after_eval") == 0 ||
                  strcmp(m, "before_eval") == 0 ||
                  strcmp(m, "ast_hook") == 0)) {
            Node *obj = n->method_call.obj;
            if (obj && VAL_TAG(obj) == NODE_FIELD && obj->field.name &&
                strcmp(obj->field.name, "runtime") == 0) {
                Node *inner = obj->field.obj;
                if (inner && VAL_TAG(inner) == NODE_IDENT && inner->ident.name &&
                    strcmp(inner->ident.name, "plugin") == 0)
                    return 1;
            }
        }
    }
    /* recurse into common children */
    switch (VAL_TAG(n)) {
    case NODE_PROGRAM:
        for (int j = 0; j < n->program.stmts.len; j++)
            if (node_uses_plugin_runtime_hook(n->program.stmts.items[j])) return 1;
        break;
    case NODE_BLOCK:
        for (int j = 0; j < n->block.stmts.len; j++)
            if (node_uses_plugin_runtime_hook(n->block.stmts.items[j])) return 1;
        break;
    case NODE_EXPR_STMT:
        return node_uses_plugin_runtime_hook(n->expr_stmt.expr);
    case NODE_IF:
        if (node_uses_plugin_runtime_hook(n->if_expr.cond)) return 1;
        if (node_uses_plugin_runtime_hook(n->if_expr.then)) return 1;
        for (int j = 0; j < n->if_expr.elif_conds.len; j++)
            if (node_uses_plugin_runtime_hook(n->if_expr.elif_conds.items[j])) return 1;
        for (int j = 0; j < n->if_expr.elif_thens.len; j++)
            if (node_uses_plugin_runtime_hook(n->if_expr.elif_thens.items[j])) return 1;
        if (node_uses_plugin_runtime_hook(n->if_expr.else_branch)) return 1;
        break;
    case NODE_LET: case NODE_VAR:
        return node_uses_plugin_runtime_hook(n->let.value);
    case NODE_CONST:
        return node_uses_plugin_runtime_hook(n->const_.value);
    case NODE_METHOD_CALL:
        if (node_uses_plugin_runtime_hook(n->method_call.obj)) return 1;
        for (int j = 0; j < n->method_call.args.len; j++)
            if (node_uses_plugin_runtime_hook(n->method_call.args.items[j])) return 1;
        break;
    case NODE_CALL:
        if (node_uses_plugin_runtime_hook(n->call.callee)) return 1;
        for (int j = 0; j < n->call.args.len; j++)
            if (node_uses_plugin_runtime_hook(n->call.args.items[j])) return 1;
        break;
    case NODE_FN_DECL:
        return node_uses_plugin_runtime_hook(n->fn_decl.body);
    default: break;
    }
    return 0;
}

/* Look for a plugin file on disk and grep it for AST-level runtime hook
   registrations. Returns 1 if any hook API call is present. */
static int file_uses_runtime_hooks(const char *path) {
    if (!path) return 0;
    const char *candidates[6] = { path, NULL, NULL, NULL, NULL, NULL };
    char buf_a[1024], buf_b[1024], buf_c[1024], buf_d[1024], buf_e[1024];
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash) base = slash + 1;
    snprintf(buf_a, sizeof buf_a, "tests/%s", path);
    snprintf(buf_b, sizeof buf_b, "examples/%s", path);
    snprintf(buf_c, sizeof buf_c, "plugins/%s", base);
    snprintf(buf_d, sizeof buf_d, "tests/plugins/%s", base);
    snprintf(buf_e, sizeof buf_e, "%s.xs", path);
    candidates[1] = buf_a;
    candidates[2] = buf_b;
    candidates[3] = buf_c;
    candidates[4] = buf_d;
    candidates[5] = buf_e;
    for (int k = 0; k < 6; k++) {
        FILE *f = fopen(candidates[k], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 20 * 1024 * 1024) { fclose(f); continue; }
        char *data = malloc((size_t)sz + 1);
        if (!data) { fclose(f); continue; }
        size_t got = fread(data, 1, (size_t)sz, f);
        data[got] = '\0';
        fclose(f);
        int hit = strstr(data, "plugin.runtime.after_eval")  != NULL ||
                  strstr(data, "plugin.runtime.before_eval") != NULL ||
                  strstr(data, "plugin.runtime.ast_hook")    != NULL;
        free(data);
        if (hit) return 1;
    }
    return 0;
}

static int program_uses_plugin_runtime_hook(Node *program) {
    if (node_uses_plugin_runtime_hook(program)) return 1;
    if (!program || VAL_TAG(program) != NODE_PROGRAM) return 0;
    for (int j = 0; j < program->program.stmts.len; j++) {
        Node *s = program->program.stmts.items[j];
        if (s && VAL_TAG(s) == NODE_LOAD && s->load_.path &&
            file_uses_runtime_hooks(s->load_.path))
            return 1;
    }
    return 0;
}

static SemaCache *g_sema_cache = NULL;

#ifdef XSC_ENABLE_COVERAGE
static void cov_register_nodelist(XSCoverage *cov, NodeList *nl);
static void cov_register_node(XSCoverage *cov, Node *n) {
    if (!n || !cov) return;
    if (n->span.line > 0) coverage_register_line(cov, n->span.line);
    switch (VAL_TAG(n)) {
    case NODE_LIT_INT: case NODE_LIT_BIGINT: case NODE_LIT_FLOAT: case NODE_LIT_BOOL:
    case NODE_LIT_NULL: case NODE_LIT_CHAR:
    case NODE_IDENT: case NODE_SCOPE:
    case NODE_PAT_WILD: case NODE_PAT_IDENT: case NODE_PAT_LIT:
    case NODE_CONTINUE: case NODE_TYPE_ALIAS: case NODE_IMPORT: case NODE_USE:
    case NODE_TRAIT_DECL:
        break;
    case NODE_LIT_STRING: case NODE_INTERP_STRING:
        cov_register_nodelist(cov, &n->lit_string.parts);
        break;
    case NODE_LIT_ARRAY: case NODE_LIT_TUPLE:
        cov_register_nodelist(cov, &n->lit_array.elems);
        cov_register_node(cov, n->lit_array.repeat_val);
        break;
    case NODE_LIT_MAP:
        cov_register_nodelist(cov, &n->lit_map.keys);
        cov_register_nodelist(cov, &n->lit_map.vals);
        break;
    case NODE_BINOP:
        cov_register_node(cov, n->binop.left);
        cov_register_node(cov, n->binop.right);
        break;
    case NODE_UNARY:
        cov_register_node(cov, n->unary.expr);
        break;
    case NODE_ASSIGN:
        cov_register_node(cov, n->assign.target);
        cov_register_node(cov, n->assign.value);
        break;
    case NODE_CALL:
        cov_register_node(cov, n->call.callee);
        cov_register_nodelist(cov, &n->call.args);
        for (int j = 0; j < n->call.kwargs.len; j++)
            cov_register_node(cov, n->call.kwargs.items[j].val);
        break;
    case NODE_METHOD_CALL:
        cov_register_node(cov, n->method_call.obj);
        cov_register_nodelist(cov, &n->method_call.args);
        for (int j = 0; j < n->method_call.kwargs.len; j++)
            cov_register_node(cov, n->method_call.kwargs.items[j].val);
        break;
    case NODE_INDEX:
        cov_register_node(cov, n->index.obj);
        cov_register_node(cov, n->index.index);
        break;
    case NODE_FIELD:
        cov_register_node(cov, n->field.obj);
        break;
    case NODE_IF:
        cov_register_node(cov, n->if_expr.cond);
        cov_register_node(cov, n->if_expr.then);
        cov_register_nodelist(cov, &n->if_expr.elif_conds);
        cov_register_nodelist(cov, &n->if_expr.elif_thens);
        cov_register_node(cov, n->if_expr.else_branch);
        break;
    case NODE_MATCH:
        cov_register_node(cov, n->match.subject);
        for (int j = 0; j < n->match.arms.len; j++) {
            cov_register_node(cov, n->match.arms.items[j].pattern);
            cov_register_node(cov, n->match.arms.items[j].guard);
            cov_register_node(cov, n->match.arms.items[j].body);
        }
        break;
    case NODE_WHILE:
        cov_register_node(cov, n->while_loop.cond);
        cov_register_node(cov, n->while_loop.body);
        break;
    case NODE_FOR:
        cov_register_node(cov, n->for_loop.pattern);
        cov_register_node(cov, n->for_loop.iter);
        cov_register_node(cov, n->for_loop.body);
        break;
    case NODE_LOOP:
        cov_register_node(cov, n->loop.body);
        break;
    case NODE_BREAK:
        cov_register_node(cov, n->brk.value);
        break;
    case NODE_RETURN:
        cov_register_node(cov, n->ret.value);
        break;
    case NODE_YIELD:
        cov_register_node(cov, n->yield_.value);
        break;
    case NODE_THROW:
        cov_register_node(cov, n->throw_.value);
        break;
    case NODE_TRY:
        cov_register_node(cov, n->try_.body);
        for (int j = 0; j < n->try_.catch_arms.len; j++) {
            cov_register_node(cov, n->try_.catch_arms.items[j].pattern);
            cov_register_node(cov, n->try_.catch_arms.items[j].body);
        }
        cov_register_node(cov, n->try_.finally_block);
        break;
    case NODE_DEFER:
        cov_register_node(cov, n->defer_.body);
        break;
    case NODE_LAMBDA:
        cov_register_node(cov, n->lambda.body);
        break;
    case NODE_BLOCK:
        cov_register_nodelist(cov, &n->block.stmts);
        cov_register_node(cov, n->block.expr);
        break;
    case NODE_CAST:
        cov_register_node(cov, n->cast.expr);
        break;
    case NODE_RANGE:
        cov_register_node(cov, n->range.start);
        cov_register_node(cov, n->range.end);
        break;
    case NODE_STRUCT_INIT:
        for (int j = 0; j < n->struct_init.fields.len; j++)
            cov_register_node(cov, n->struct_init.fields.items[j].val);
        cov_register_node(cov, n->struct_init.rest);
        break;
    case NODE_SPREAD:
        cov_register_node(cov, n->spread.expr);
        break;
    case NODE_LIST_COMP:
        cov_register_node(cov, n->list_comp.element);
        cov_register_nodelist(cov, &n->list_comp.clause_pats);
        cov_register_nodelist(cov, &n->list_comp.clause_iters);
        cov_register_nodelist(cov, &n->list_comp.clause_conds);
        break;
    case NODE_MAP_COMP:
        cov_register_node(cov, n->map_comp.key);
        cov_register_node(cov, n->map_comp.value);
        cov_register_nodelist(cov, &n->map_comp.clause_pats);
        cov_register_nodelist(cov, &n->map_comp.clause_iters);
        cov_register_nodelist(cov, &n->map_comp.clause_conds);
        break;
    case NODE_LET: case NODE_VAR:
        cov_register_node(cov, n->let.pattern);
        cov_register_node(cov, n->let.value);
        break;
    case NODE_CONST:
        cov_register_node(cov, n->const_.value);
        break;
    case NODE_EXPR_STMT:
        cov_register_node(cov, n->expr_stmt.expr);
        break;
    case NODE_FN_DECL:
        cov_register_node(cov, n->fn_decl.body);
        break;
    case NODE_CLASS_DECL:
        cov_register_nodelist(cov, &n->class_decl.members);
        break;
    case NODE_STRUCT_DECL:
        for (int j = 0; j < n->struct_decl.fields.len; j++)
            cov_register_node(cov, n->struct_decl.fields.items[j].val);
        break;
    case NODE_ENUM_DECL:
        break;
    case NODE_IMPL_DECL:
        cov_register_nodelist(cov, &n->impl_decl.members);
        break;
    case NODE_MODULE_DECL:
        cov_register_nodelist(cov, &n->module_decl.body);
        break;
    case NODE_EFFECT_DECL:
        cov_register_nodelist(cov, &n->effect_decl.ops);
        break;
    case NODE_PERFORM:
        cov_register_nodelist(cov, &n->perform.args);
        break;
    case NODE_HANDLE:
        cov_register_node(cov, n->handle.expr);
        for (int j = 0; j < n->handle.arms.len; j++)
            cov_register_node(cov, n->handle.arms.items[j].body);
        break;
    case NODE_RESUME:
        cov_register_node(cov, n->resume_.value);
        break;
    case NODE_AWAIT:
        cov_register_node(cov, n->await_.expr);
        break;
    case NODE_NURSERY:
        cov_register_node(cov, n->nursery_.body);
        break;
    case NODE_SPAWN:
        cov_register_node(cov, n->spawn_.expr);
        break;
    case NODE_ACTOR_DECL:
        for (int j = 0; j < n->actor_decl.state_fields.len; j++)
            cov_register_node(cov, n->actor_decl.state_fields.items[j].val);
        cov_register_nodelist(cov, &n->actor_decl.methods);
        break;
    case NODE_SEND_EXPR:
        cov_register_node(cov, n->send_expr.target);
        cov_register_node(cov, n->send_expr.message);
        break;
    case NODE_PAT_TUPLE:
        cov_register_nodelist(cov, &n->pat_tuple.elems);
        break;
    case NODE_PAT_STRUCT:
        for (int j = 0; j < n->pat_struct.fields.len; j++)
            cov_register_node(cov, n->pat_struct.fields.items[j].val);
        break;
    case NODE_PAT_ENUM:
        cov_register_nodelist(cov, &n->pat_enum.args);
        break;
    case NODE_PAT_OR:
        cov_register_node(cov, n->pat_or.left);
        cov_register_node(cov, n->pat_or.right);
        break;
    case NODE_PAT_RANGE:
        cov_register_node(cov, n->pat_range.start);
        cov_register_node(cov, n->pat_range.end);
        break;
    case NODE_PAT_SLICE:
        cov_register_nodelist(cov, &n->pat_slice.elems);
        break;
    case NODE_PAT_GUARD:
        cov_register_node(cov, n->pat_guard.pattern);
        cov_register_node(cov, n->pat_guard.guard);
        break;
    case NODE_PAT_EXPR:
        cov_register_node(cov, n->pat_expr.expr);
        break;
    case NODE_PAT_CAPTURE:
        cov_register_node(cov, n->pat_capture.pattern);
        break;
    case NODE_PAT_STRING_CONCAT:
        cov_register_node(cov, n->pat_str_concat.rest);
        break;
    case NODE_PAT_REGEX:
        break;
    case NODE_LOAD:
    case NODE_PLUGIN_DECL:
        break;
    case NODE_PROGRAM:
        cov_register_nodelist(cov, &n->program.stmts);
        break;
    }
}
static void cov_register_nodelist(XSCoverage *cov, NodeList *nl) {
    if (!nl) return;
    for (int j = 0; j < nl->len; j++)
        cov_register_node(cov, nl->items[j]);
}
#endif /* XSC_ENABLE_COVERAGE */

static char *read_file(const char *path) {
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            fprintf(stderr, "xs: '%s' is a directory\n", path);
            return NULL;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "xs: cannot open '%s'\n", path);
        return NULL;
    }

    /* Try the fast path first: seek to end, ftell, read once.
       Pipes and character devices (like /dev/stdin under a shell pipe)
       report -1 from ftell, so fall back to a chunked read in that case. */
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
        if (fseek(f, 0, SEEK_SET) != 0) sz = -1;
    }

    if (sz >= 0) {
        char *buf = (char *)xs_malloc((size_t)sz + 1);
        size_t got = fread(buf, 1, (size_t)sz, f);
        if (got != (size_t)sz) {
            /* Short read on a seekable file is still a real error. */
            fprintf(stderr, "xs: read error on '%s'\n", path);
            fclose(f);
            free(buf);
            return NULL;
        }
        buf[sz] = '\0';
        fclose(f);
        return buf;
    }

    /* Non-seekable stream: grow buffer as we go. */
    size_t cap = 4096, len = 0;
    char *buf = (char *)xs_malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            buf = (char *)xs_realloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) {
            if (ferror(f)) {
                fprintf(stderr, "xs: read error on '%s'\n", path);
                fclose(f);
                free(buf);
                return NULL;
            }
            break; /* EOF */
        }
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static Node *parse_file(const char *path, DiagContext *dctx) {
    char *src = read_file(path);
    if (!src) return NULL;

    int own_dctx = 0;
    if (!dctx) {
        dctx = diag_context_new();
        own_dctx = 1;
    }
    diag_context_add_source(dctx, path, src);

    Lexer lex; lexer_init(&lex, src, path);
    TokenArray ta = lexer_tokenize(&lex);
    Parser p; parser_init(&p, &ta, path);
    p.source = src;
    p.diag = dctx;
    Node *prog = parser_parse(&p);
    token_array_free(&ta);
    /* A lexer error reports directly and may still leave enough tokens for
       the parser to succeed; treat that as a fatal parse failure too. */
    if (!prog || p.had_error ||
        diag_context_error_count(dctx) > 0 || lex.n_errors > 0) {
        diag_render_all(dctx);
        free(src);
        if (prog) node_free(prog);
        if (own_dctx) diag_context_free(dctx);
        return NULL;
    }
    free(src);
    if (own_dctx) diag_context_free(dctx);
    return prog;
}

static void usage(void) {
    printf(
        "Usage:\n"
        "  xs <file.xs> [args...]        Run a script\n"
        "  xs -e '<code>'                Evaluate inline code\n"
        "\n"
        "Commands:\n"
        "  run <file.xs|file.xsc>        Run source or compiled bytecode\n"
        "  check <file.xs>               Type-check without running\n"
        "  build <file.xs> [-o out.xsc]  Compile to bytecode\n"
        "  test [pattern]                Run test files\n"
        "  bench [pattern]               Run benchmarks\n"
        "  lint [file|dir] [--fix]       Lint source files\n"
        "  fmt [file|dir] [--check]      Format source files\n"
        "  doc [dir]                     Generate documentation\n"
        "  new <name>                    Scaffold a new project\n"
        "  lsp                           Start LSP server\n"
        "  dap                           Start DAP debug server\n"
        "  install [pkg]                 Install dependencies\n"
        "  remove <pkg>                  Remove a dependency\n"
        "  explain <code>                Explain an error code\n"
        "\n"
        "Flags (can appear before or after the filename):\n"
        "  --record <file.xst>  Record execution trace\n"
        "  --vm                 Use bytecode VM backend\n"
        "  --jit                Tier-2 native JIT (x86-64, arm64; bails to VM)\n"
        "  --check              Type-check only, don't run\n"
        "  --strict             Require all type annotations\n"
        "  --lenient            Downgrade errors to warnings\n"
        "  --optimize           Run AST optimizer\n"
        "  --emit <target>      Dump IR: ast, bytecode, ir, jit-ir, js, c, wasm\n"
        "  --trace              Auto-load provenance tracking\n"
        "  --watch              Re-run on file change\n"
        "  --profile            Enable sampling profiler\n"
        "  --coverage           Enable line/branch coverage\n"
        "  --plugin <path>      Load native plugin (.so/.dll)\n"
        "  --no-color           Disable colored output\n"
        "  -e, --eval <code>    Evaluate inline code\n"
        "  -V, --version        Show version\n"
        "  -h, --help           Show this help\n"
    );
}

static int watch_and_run(const char *filename, int file_arg, int argc, char **argv) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        fprintf(stderr, "xs: cannot stat '%s'\n", filename);
        return 1;
    }
    time_t last_mtime = st.st_mtime;
    int first = 1;

    for (;;) {
        if (!first) fprintf(stderr, "\033[2J\033[H"); /* clear screen on reload */
        first = 0;

        Node *program = parse_file(filename, NULL);
        if (!program) {
            fprintf(stderr, "xs: parse error in '%s'\n", filename);
        } else {
            DiagContext *dctx = diag_context_new();
            char *src = read_file(filename);
            if (src) {
                diag_context_add_source(dctx, filename, src);
                xs_error_set_source(src);
            }
            diag_context_set_lenient(dctx, 1);
            SemaCtx sema;
            sema_init(&sema, 1, 0);
            sema.diag = dctx;
            int sema_errors = sema_analyze(&sema, program, filename);
            diag_render_all(dctx);
            sema_free(&sema);
            diag_context_free(dctx);

            if (sema_errors > 0) {
                fprintf(stderr, "%d semantic warning%s\n", sema_errors, sema_errors > 1 ? "s" : "");
            }

            Interp *interp = interp_new(filename);
            interp->diag = diag_context_new();
            if (src) diag_context_add_source(interp->diag, filename, src);
            {
                int nargs = argc - file_arg - 1;
                Value *argv_val = xs_array_new();
                for (int k = 0; k < nargs; k++) {
                    Value *sv = xs_str(argv[file_arg + 1 + k]);
                    array_push(argv_val->arr, sv);
                    value_decref(sv);
                }
                env_define(interp->globals, "argv", argv_val, 0);
                value_decref(argv_val);
            }
            interp_run(interp, program);
            fflush(stdout);
            int had_error = (interp->cf.signal == CF_ERROR || interp->cf.signal == CF_PANIC);
            DiagContext *idctx = interp->diag;
            interp->diag = NULL;
            interp_free(interp);
            if (idctx) diag_context_free(idctx);
            node_free(program);
            free(src);
            if (had_error) {
                fprintf(stderr, "xs: execution error (watching for changes...)\n");
            }
        }

        fprintf(stderr, "\nxs: watching '%s' for changes... (Ctrl-C to stop)\n", filename);
        fflush(stderr);

        for (;;) {
            usleep(300000);

            if (stat(filename, &st) != 0) {
                fprintf(stderr, "xs: file '%s' disappeared, stopping watch\n", filename);
                return 1;
            }
            if (st.st_mtime != last_mtime) {
                last_mtime = st.st_mtime;
                fprintf(stderr, "xs: file changed, reloading...\n\n");
                break;
            }
        }
    }
    return 0;
}

static void ast_dump(Node *n, int depth) {
    if (!n) return;
    for (int i = 0; i < depth; i++) fprintf(stdout, "  ");
    switch (VAL_TAG(n)) {
        case NODE_PROGRAM: fprintf(stdout, "PROGRAM\n"); for (int i = 0; i < n->program.stmts.len; i++) ast_dump(n->program.stmts.items[i], depth+1); break;
        case NODE_FN_DECL: fprintf(stdout, "FN_DECL name=%s\n", n->fn_decl.name); ast_dump(n->fn_decl.body, depth+1); break;
        case NODE_LET: case NODE_VAR: fprintf(stdout, "%s name=%s\n", n->let.mutable ? "VAR" : "LET", n->let.name ? n->let.name : "_"); if (n->let.value) ast_dump(n->let.value, depth+1); break;
        case NODE_CONST: fprintf(stdout, "CONST name=%s\n", n->const_.name); if (n->const_.value) ast_dump(n->const_.value, depth+1); break;
        case NODE_BINOP: fprintf(stdout, "BINOP op=%s\n", n->binop.op); ast_dump(n->binop.left, depth+1); ast_dump(n->binop.right, depth+1); break;
        case NODE_UNARY: fprintf(stdout, "UNARY op=%s\n", n->unary.op); ast_dump(n->unary.expr, depth+1); break;
        case NODE_CALL: fprintf(stdout, "CALL\n"); ast_dump(n->call.callee, depth+1); for (int i = 0; i < n->call.args.len; i++) ast_dump(n->call.args.items[i], depth+1); break;
        case NODE_METHOD_CALL: fprintf(stdout, "METHOD_CALL method=%s\n", n->method_call.method); ast_dump(n->method_call.obj, depth+1); for (int i = 0; i < n->method_call.args.len; i++) ast_dump(n->method_call.args.items[i], depth+1); break;
        case NODE_IF: fprintf(stdout, "IF\n"); ast_dump(n->if_expr.cond, depth+1); ast_dump(n->if_expr.then, depth+1); if (n->if_expr.else_branch) ast_dump(n->if_expr.else_branch, depth+1); break;
        case NODE_WHILE: fprintf(stdout, "WHILE\n"); ast_dump(n->while_loop.cond, depth+1); ast_dump(n->while_loop.body, depth+1); break;
        case NODE_FOR: fprintf(stdout, "FOR\n"); ast_dump(n->for_loop.pattern, depth+1); ast_dump(n->for_loop.iter, depth+1); ast_dump(n->for_loop.body, depth+1); break;
        case NODE_BLOCK: fprintf(stdout, "BLOCK\n"); for (int i = 0; i < n->block.stmts.len; i++) ast_dump(n->block.stmts.items[i], depth+1); if (n->block.expr) ast_dump(n->block.expr, depth+1); break;
        case NODE_RETURN: fprintf(stdout, "RETURN\n"); if (n->ret.value) ast_dump(n->ret.value, depth+1); break;
        case NODE_MATCH: fprintf(stdout, "MATCH\n"); ast_dump(n->match.subject, depth+1); for (int i = 0; i < n->match.arms.len; i++) { ast_dump(n->match.arms.items[i].pattern, depth+1); ast_dump(n->match.arms.items[i].body, depth+1); } break;
        case NODE_IDENT: fprintf(stdout, "IDENT name=%s\n", n->ident.name); break;
        case NODE_LIT_INT: fprintf(stdout, "INT %lld\n", (long long)n->lit_int.ival); break;
        case NODE_LIT_BIGINT: fprintf(stdout, "BIGINT %s\n", n->lit_bigint.bigint_str); break;
        case NODE_LIT_FLOAT: fprintf(stdout, "FLOAT %g\n", n->lit_float.fval); break;
        case NODE_LIT_STRING: fprintf(stdout, "STRING \"%s\"\n", n->lit_string.sval ? n->lit_string.sval : ""); break;
        case NODE_LIT_BOOL: fprintf(stdout, "BOOL %s\n", n->lit_bool.bval ? "true" : "false"); break;
        case NODE_LIT_NULL: fprintf(stdout, "NULL\n"); break;
        case NODE_LIT_ARRAY: fprintf(stdout, "ARRAY len=%d\n", n->lit_array.elems.len); for (int i = 0; i < n->lit_array.elems.len; i++) ast_dump(n->lit_array.elems.items[i], depth+1); break;
        case NODE_LIT_TUPLE: fprintf(stdout, "TUPLE len=%d\n", n->lit_array.elems.len); for (int i = 0; i < n->lit_array.elems.len; i++) ast_dump(n->lit_array.elems.items[i], depth+1); break;
        case NODE_LIT_MAP: fprintf(stdout, "MAP\n"); break;
        case NODE_ASSIGN: fprintf(stdout, "ASSIGN op=%s\n", n->assign.op); ast_dump(n->assign.target, depth+1); ast_dump(n->assign.value, depth+1); break;
        case NODE_INDEX: fprintf(stdout, "INDEX\n"); ast_dump(n->index.obj, depth+1); ast_dump(n->index.index, depth+1); break;
        case NODE_FIELD: fprintf(stdout, "FIELD name=%s\n", n->field.name); ast_dump(n->field.obj, depth+1); break;
        case NODE_RANGE: fprintf(stdout, "RANGE inclusive=%d\n", n->range.inclusive); ast_dump(n->range.start, depth+1); ast_dump(n->range.end, depth+1); break;
        case NODE_STRUCT_DECL: fprintf(stdout, "STRUCT name=%s\n", n->struct_decl.name); break;
        case NODE_ENUM_DECL: fprintf(stdout, "ENUM name=%s\n", n->enum_decl.name); break;
        case NODE_TRAIT_DECL: fprintf(stdout, "TRAIT name=%s\n", n->trait_decl.name); break;
        case NODE_IMPL_DECL: fprintf(stdout, "IMPL type=%s trait=%s\n", n->impl_decl.type_name, n->impl_decl.trait_name ? n->impl_decl.trait_name : "(none)"); break;
        case NODE_IMPORT: case NODE_USE: fprintf(stdout, "IMPORT\n"); break;
        case NODE_LAMBDA: fprintf(stdout, "LAMBDA\n"); ast_dump(n->lambda.body, depth+1); break;
        case NODE_EXPR_STMT: fprintf(stdout, "EXPR_STMT\n"); ast_dump(n->expr_stmt.expr, depth+1); break;
        case NODE_BREAK: fprintf(stdout, "BREAK\n"); break;
        case NODE_CONTINUE: fprintf(stdout, "CONTINUE\n"); break;
        case NODE_THROW: fprintf(stdout, "THROW\n"); if (n->throw_.value) ast_dump(n->throw_.value, depth+1); break;
        case NODE_TRY: fprintf(stdout, "TRY\n"); ast_dump(n->try_.body, depth+1); break;
        case NODE_DEFER: fprintf(stdout, "DEFER\n"); ast_dump(n->defer_.body, depth+1); break;
        case NODE_STRUCT_INIT: fprintf(stdout, "STRUCT_INIT path=%s\n", n->struct_init.path); break;
        case NODE_CAST: fprintf(stdout, "CAST type=%s\n", n->cast.type_name); ast_dump(n->cast.expr, depth+1); break;
        case NODE_EFFECT_DECL: fprintf(stdout, "EFFECT name=%s\n", n->effect_decl.name); break;
        case NODE_PERFORM: fprintf(stdout, "PERFORM %s.%s\n", n->perform.effect_name, n->perform.op_name); break;
        case NODE_HANDLE: fprintf(stdout, "HANDLE\n"); ast_dump(n->handle.expr, depth+1); break;
        case NODE_RESUME: fprintf(stdout, "RESUME\n"); break;
        case NODE_AWAIT: fprintf(stdout, "AWAIT\n"); ast_dump(n->await_.expr, depth+1); break;
        case NODE_SPAWN: fprintf(stdout, "SPAWN\n"); ast_dump(n->spawn_.expr, depth+1); break;
        case NODE_NURSERY: fprintf(stdout, "NURSERY\n"); ast_dump(n->nursery_.body, depth+1); break;
        case NODE_LOOP: fprintf(stdout, "LOOP\n"); ast_dump(n->loop.body, depth+1); break;
        case NODE_YIELD: fprintf(stdout, "YIELD\n"); if (n->yield_.value) ast_dump(n->yield_.value, depth+1); break;
        case NODE_TYPE_ALIAS: fprintf(stdout, "TYPE_ALIAS name=%s target=%s\n", n->type_alias.name, n->type_alias.target); break;
        case NODE_MODULE_DECL: fprintf(stdout, "MODULE name=%s\n", n->module_decl.name); break;
        case NODE_SCOPE: fprintf(stdout, "SCOPE\n"); break;
        case NODE_SPREAD: fprintf(stdout, "SPREAD\n"); ast_dump(n->spread.expr, depth+1); break;
        case NODE_LIST_COMP: fprintf(stdout, "LIST_COMP\n"); break;
        case NODE_MAP_COMP: fprintf(stdout, "MAP_COMP\n"); break;
        case NODE_INTERP_STRING: fprintf(stdout, "INTERP_STRING\n"); break;
        case NODE_CLASS_DECL: fprintf(stdout, "CLASS name=%s\n", n->class_decl.name); break;
        case NODE_LIT_CHAR: fprintf(stdout, "CHAR '%c'\n", n->lit_char.cval); break;
        case NODE_PAT_WILD: fprintf(stdout, "PAT_WILD\n"); break;
        case NODE_PAT_IDENT: fprintf(stdout, "PAT_IDENT name=%s\n", n->pat_ident.name); break;
        case NODE_PAT_LIT: fprintf(stdout, "PAT_LIT\n"); break;
        case NODE_PAT_TUPLE: fprintf(stdout, "PAT_TUPLE\n"); break;
        case NODE_PAT_STRUCT: fprintf(stdout, "PAT_STRUCT\n"); break;
        case NODE_PAT_ENUM: fprintf(stdout, "PAT_ENUM\n"); break;
        case NODE_PAT_OR: fprintf(stdout, "PAT_OR\n"); break;
        case NODE_PAT_RANGE: fprintf(stdout, "PAT_RANGE\n"); break;
        case NODE_PAT_SLICE: fprintf(stdout, "PAT_SLICE\n"); break;
        case NODE_PAT_GUARD: fprintf(stdout, "PAT_GUARD\n"); break;
        case NODE_PAT_EXPR: fprintf(stdout, "PAT_EXPR\n"); break;
        case NODE_PAT_CAPTURE: fprintf(stdout, "PAT_CAPTURE name=%s\n", n->pat_capture.name); break;
        case NODE_PAT_STRING_CONCAT: fprintf(stdout, "PAT_STRING_CONCAT prefix=\"%s\"\n", n->pat_str_concat.prefix); break;
        case NODE_PAT_REGEX: fprintf(stdout, "PAT_REGEX pattern=\"%s\"\n", n->pat_regex.pattern); break;
        default: fprintf(stdout, "NODE_%d\n", VAL_TAG(n)); break;
    }
}

extern void xs_set_argv(int argc, char **argv);
extern void xs_set_user_args(int argc, char **argv);

static void sigint_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nInterrupted\n");
    exit(130);
}

extern void vm_drain_tasks(void);

static void xs_shutdown(void) {
    /* Wait for any unjoined spawn workers so a fire-and-forget
       spawn { ... } actually completes before the process exits.
       (Only VM-backend tasks are drained here. The interp backend
       shares its Interp with worker threads, so by the time atexit
       runs the Interp may be partially torn down; joining there
       would dereference freed state.) */
    vm_drain_tasks();
    /* Release process-lifetime allocations so leak checkers don't
       flag the pinned value singletons, the GC nodemap, and the int
       recycler freelist. Order matters: drop GC side-tables before
       freeing the singletons they may point at. */
    gc_shutdown();
    value_free_singletons();
}

int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    xs_set_argv(argc, argv);
    /* Honour the standard NO_COLOR env var (https://no-color.org)
       and the conventional dumb-terminal opt-out so the CLI quiets
       its diagnostic colour without --no-color in CI / piped runs.
       --no-color on the command line still wins later. */
    {
        const char *nc = getenv("NO_COLOR");
        if (nc && nc[0]) g_no_color = 1;
        const char *term = getenv("TERM");
        if (term && strcmp(term, "dumb") == 0) g_no_color = 1;
    }
    if (!g_sema_cache) g_sema_cache = cache_new();
    /* Initialize the global interpreter lock; main thread holds it for
       the duration. spawned threads acquire on entry. */
    xs_gil_init();
    atexit(xs_shutdown);
    int do_check    = 0;
    int lenient     = 0;
    int strict      = 0;
    int do_watch    = 0;
    int do_optimize = 0;
    int emit_ir_ssa = 0;
    int do_trace    = 0;
#ifdef XSC_ENABLE_VM
    /* Default backend is the bytecode VM. Programs that register AST-level
       runtime hooks (plugin.runtime.after_eval etc.) are detected and
       silently fall back to the tree-walk interpreter. --interp forces
       the tree-walk path explicitly via do_vm=0. */
    int do_vm         = 1;
    int emit_bytecode = 0;
    int emit_jit_ir = 0;
#endif
#ifdef XSC_ENABLE_JIT
    int do_jit = 0;
#endif
#ifdef XSC_ENABLE_TRANSPILER
    int emit_js   = 0;
    int emit_c    = 0;
    int emit_wasm = 0;
#endif
#ifdef XSC_ENABLE_TRACER
    const char *record_path      = NULL;
    double      trace_sample     = 0.0;
    int         trace_deep       = 0;
#endif
#ifdef XSC_ENABLE_PROFILER
    int do_profile = 0;
#endif
#ifdef XSC_ENABLE_COVERAGE
    int do_coverage = 0;
#endif
#ifdef XSC_ENABLE_PLUGINS
    const char *plugin_path = NULL;
#endif
    int emit_ast = 0;
    const char *filename = NULL;
    int file_arg = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("%s\n", XS_VERSION_TAG);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            const char *topic = NULL;
            for (int j = 1; j < i; j++) {
                if (argv[j][0] != '-') { topic = argv[j]; break; }
                else topic = argv[j]; /* flags like --vm are topics too */
            }
            if (topic) {
                #define H(cmd, text) if (strcmp(topic, cmd) == 0) { printf("%s", text); fflush(stdout); return 0; }
                /* Subcommands */
                H("run",       "Usage: xs run <file.xs> [args...]\n\n"
                               "Run an XS script. The full pipeline runs:\n"
                               "  lexer -> parser -> semantic analysis -> interpreter\n\n"
                               "Flags:\n"
                               "  --vm             Use bytecode VM backend\n"
                               "  --jit            Use JIT compilation backend\n"
                               "  --optimize       Run AST optimizer before execution\n"
                               "  --watch          Watch file and re-run on changes\n"
                               "  --record <file>  Record execution trace\n"
                               "  --profile        Enable sampling profiler\n"
                               "  --coverage       Enable line coverage tracking\n")
                H("test",      "Usage: xs test [file.xs | pattern]\n\n"
                               "Run tests. Two modes:\n"
                               "  xs test file.xs    Run #[test] functions in a specific file\n"
                               "  xs test [pattern]  Find and run all test_*.xs / *_test.xs files\n"
                               "                     Optional pattern filters by filename\n\n"
                               "Test functions are marked with #[test]:\n"
                               "  #[test]\n"
                               "  fn test_addition() {\n"
                               "      assert(1 + 1 == 2)\n"
                               "  }\n")
                H("bench",     "Usage: xs bench [pattern]\n\n"
                               "Run benchmark files matching bench_*.xs or *_bench.xs.\n"
                               "Each benchmark runs 10 times; min/max/avg times reported.\n")
                H("check",     "Usage: xs check <file.xs>\n\n"
                               "Type-check and analyze a file without running it.\n"
                               "Runs the full pipeline: lexer -> parser -> semantic analysis.\n"
                               "Errors are displayed with error codes (e.g. T0001).\n"
                               "Use `xs --explain <code>` to learn more about an error.\n")
                H("build",     "Usage: xs build <file.xs>\n\n"
                               "Compile a file to bytecode and dump the instruction listing.\n"
                               "Useful for debugging the VM compiler.\n")
                H("lint",      "Usage: xs lint [file|dir] [--fix]\n\n"
                               "Lint XS source files for style and correctness issues.\n\n"
                               "Checks:\n"
                               "  - Unused variables\n"
                               "  - Variable shadowing\n"
                               "  - Naming conventions (snake_case fns, PascalCase types)\n"
                               "  - Unreachable code after return/break/continue\n"
                               "  - Empty block bodies\n"
                               "  - Constant conditions in if/while\n"
                               "  - Comparison to null (suggest `is null`)\n"
                               "  - Redundant else after return\n\n"
                               "With --fix: auto-fix simple issues where possible.\n")
                H("fmt",       "Usage: xs fmt <file.xs> [--check]\n\n"
                               "Format an XS source file in canonical style.\n"
                               "  - 4-space indentation\n"
                               "  - Consistent brace placement\n"
                               "  - Preserves comments\n\n"
                               "With --check: verify formatting without modifying the file.\n"
                               "Exit code 0 = already formatted, 1 = would be reformatted.\n")
                H("doc",       "Usage: xs doc [file|dir]\n\n"
                               "Generate documentation from source files.\n"
                               "Extracts: function signatures, struct fields, enum variants,\n"
                               "trait methods, doc comments.\n"
                               "Outputs Markdown to stdout.\n")
                H("transpile", "Usage: xs transpile --target <js|c|wasm32|wasi> <file.xs>\n\n"
                               "Transpile an XS source file to another language.\n\n"
                               "Targets:\n"
                               "  js       JavaScript (ES2020+)\n"
                               "  c        C11 with XS runtime\n"
                               "  wasm32   WebAssembly binary\n"
                               "  wasi     WebAssembly with WASI interface\n")
                H("replay",    "Usage: xs replay <trace.xst> [--dump]\n\n"
                               "Replay a recorded execution trace interactively.\n\n"
                               "Options:\n"
                               "  --dump        Print all events and exit (non-interactive)\n\n"
                               "Commands during replay:\n"
                               "  n / next      Step forward\n"
                               "  p / prev      Step backward\n"
                               "  c / continue  Run to end\n"
                               "  g <n>         Go to event number\n"
                               "  w <var>       Show provenance for variable\n"
                               "  q / quit      Exit replay\n")
                H("new",       "Usage: xs new <name>\n\n"
                               "Scaffold a new XS project.\n\n"
                               "Creates:\n"
                               "  <name>/xs.toml             Project configuration\n"
                               "  <name>/src/main.xs         Entry point\n"
                               "  <name>/tests/test_main.xs  Initial test file\n"
                               "  <name>/README.md           Project readme\n"
                               "  <name>/.gitignore          Git ignore rules\n")
                H("install",   "Usage: xs install [pkg]\n\n"
                               "Install dependencies from xs.toml, or a specific package.\n"
                               "Packages are installed to xs_lib/.\n")
                H("remove",    "Usage: xs remove <pkg>\n\n"
                               "Remove an installed package from xs_lib/.\n")
                H("update",    "Usage: xs update [pkg]\n\n"
                               "Update all dependencies or a specific package to latest version.\n")
                H("publish",   "Usage: xs publish\n\n"
                               "Publish the current package to the XS registry.\n"
                               "Reads token from XS_REGISTRY_TOKEN env var or ~/.xs/credentials.\n"
                               "Run `xs login` once to store the token persistently.\n")
                H("login",     "Usage: xs login\n\n"
                               "Store registry credentials at ~/.xs/credentials (chmod 600).\n"
                               "Reads XS_REGISTRY_TOKEN if set, otherwise prompts on stdin.\n"
                               "Get the token from the registry web UI account page.\n")
                H("logout",    "Usage: xs logout\n\n"
                               "Delete ~/.xs/credentials.\n")
                H("whoami",    "Usage: xs whoami\n\n"
                               "Print the username/email associated with the stored token.\n")
                H("search",    "Usage: xs search <query>\n\n"
                               "Search the XS package registry.\n")
                H("lsp",       "Usage: xs lsp\n\n"
                               "Start the Language Server Protocol server on stdin/stdout.\n"
                               "Supports: diagnostics, hover, completion, go-to-definition.\n"
                               "Configure your editor to use `xs lsp` as the language server.\n")
                H("dap",       "Usage: xs dap\n\n"
                               "Start the Debug Adapter Protocol server.\n"
                               "Used by editors/IDEs for step debugging.\n"
                               "Or use: xs --debug <file.xs> to debug a specific file.\n")
                H("explain",   "Usage: xs --explain <error-code>\n\n"
                               "Show a detailed explanation of an error code.\n"
                               "Error codes are shown in diagnostic output, e.g. error[T0001].\n\n"
                               "Code prefixes:\n"
                               "  L0xxx  Lexer errors (unterminated strings, bad escapes)\n"
                               "  P0xxx  Parser errors (unexpected tokens, missing delimiters)\n"
                               "  T0xxx  Type errors (mismatches, undefined names, wrong args)\n"
                               "  S0xxx  Semantic errors (exhaustiveness, purity, orphan impls)\n\n"
                               "Example: xs --explain T0001\n")
                H("coverage",  "Usage: xs coverage <file.xs>\n\n"
                               "Run a script with line coverage tracking.\n"
                               "Prints a coverage report showing which lines were executed.\n")
                H("profile",   "Usage: xs profile <file.xs>\n\n"
                               "Run a script with the sampling profiler enabled.\n"
                               "Prints a hotspot report showing where time was spent.\n")
                H("pkg",       "Usage: xs pkg <subcommand> [args]\n\n"
                               "Package management commands:\n"
                               "  xs pkg install [name]  Install a package\n"
                               "  xs pkg remove <name>   Remove a package\n"
                               "  xs pkg update [name]   Update packages\n"
                               "  xs pkg list            List installed packages\n")
                /* Flags */
                H("--explain",  "Usage: xs --explain <error-code>\n\n"
                                "Show detailed explanation of an error code.\n"
                                "Example: xs --explain T0001\n")
                H("--vm",       "Flag: --vm\n\n"
                                "Use the bytecode VM backend instead of the tree-walking interpreter.\n"
                                "Alias for: --backend vm\n\n"
                                "Example: xs --vm script.xs\n")
                H("--jit",      "Flag: --jit\n\n"
                                "Tier-2 register-allocating native JIT (x86-64, arm64).\n"
                                "Compiles hot protos past a threshold; ops outside the\n"
                                "supported subset bail back to the VM transparently.\n"
                                "Alias for: --backend jit\n\n"
                                "Example: xs --jit script.xs\n")
                H("--backend",  "Flag: --backend <interp|vm|jit>\n\n"
                                "Select the execution backend.\n"
                                "  interp  Tree-walking interpreter (REPL / debugging)\n"
                                "  vm      Bytecode virtual machine (default)\n"
                                "  jit     Tier-2 register-allocating native JIT\n")
                H("--emit",     "Flag: --emit <format>\n\n"
                                "Dump internal representation instead of running.\n"
                                "  ast        Abstract syntax tree\n"
                                "  ir         SSA intermediate representation\n"
                                "  bytecode   VM bytecode listing\n"
                                "  jit-ir     Tier-2 JIT IR per proto (post-lowering)\n"
                                "  js         Transpiled JavaScript\n"
                                "  c          Transpiled C\n"
                                "  wasm       WebAssembly binary\n")
                H("--optimize", "Flag: --optimize\n\n"
                                "Run the AST optimizer before execution.\n"
                                "Passes: constant folding, dead code elimination,\n"
                                "strength reduction, inlining, CSE, constant propagation,\n"
                                "loop invariant code motion, algebraic simplification.\n")
                H("--watch",    "Flag: --watch\n\n"
                                "Watch the source file and re-run on changes.\n"
                                "Polls every second for modifications.\n"
                                "Press Ctrl+C to stop.\n")
                H("--check",    "Flag: --check\n\n"
                                "Run semantic analysis without executing.\n"
                                "Same as: xs check <file>\n")
                H("--lenient",  "Flag: --lenient\n\n"
                                "Downgrade semantic errors to warnings.\n"
                                "Code will still execute even if the type checker flags issues.\n")
                H("--strict",   "Flag: --strict\n\n"
                                "Enable strict typing mode.\n"
                                "All variable declarations and function parameters/returns must have type annotations.\n")
                H("--no-color", "Flag: --no-color\n\n"
                                "Disable ANSI color codes in all output.\n"
                                "Useful for piping output or terminals without color support.\n")
                H("--record",   "Flag: --record <file.xst>\n\n"
                                "Record an execution trace to a binary .xst file.\n"
                                "Traces function calls, returns, and variable stores.\n\n"
                                "Usage:\n"
                                "  xs --record trace.xst script.xs\n"
                                "  xs script.xs --record trace.xst\n\n"
                                "Options:\n"
                                "  --trace-deep    Serialize complex values (arrays, maps) as JSON\n\n"
                                "Replay:\n"
                                "  xs replay trace.xst\n\n"
                                "The trace file uses a binary format (header: XST1).\n"
                                "Events: CALL (function entry), RETURN (function exit),\n"
                                "        STORE (variable assignment). Each event has a\n"
                                "        nanosecond timestamp relative to program start.\n")
                H("--plugin",   "Flag: --plugin <path.so>\n\n"
                                "Load a native plugin (.so/.dll) before execution.\n"
                                "The plugin must export xs_plugin_init().\n")
                H("--sandbox",  "Flag: --sandbox\n\n"
                                "Restrict plugin capabilities (subprocess isolation).\n"
                                "Plugins run in a sandboxed environment with limited I/O.\n")
                H("--debug",    "Flag: --debug <file.xs>\n\n"
                                "Run a file with the DAP debug server attached.\n"
                                "Connect your editor's debugger to step through code.\n")
                H("-e",         "Flag: -e <code>  (alias: --eval)\n\n"
                                "Evaluate an inline expression and exit.\n\n"
                                "Examples:\n"
                                "  xs -e 'println(42)'\n"
                                "  xs -e 'let x = 2 ** 10; println(x)'\n")
                H("--eval",     "Flag: --eval <code>\n\n"
                                "Evaluate an inline expression and exit.\n"
                                "Alias for: -e\n")
                #undef H
            }
            usage();
            fflush(stdout);
            return 0;
        }
    }

    if (argc == 1) {
        usage();
        fflush(stdout);
        cache_free(g_sema_cache);
        return 0;
    }

    /* Find subcommand: first non-flag argument */
    const char *sub = NULL;
    int sub_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if ((strcmp(argv[i], "--backend") == 0 ||
                 strcmp(argv[i], "--emit") == 0 ||
                 strcmp(argv[i], "--debug") == 0 ||
                 strcmp(argv[i], "--record") == 0 ||
                 strcmp(argv[i], "--trace-sample") == 0 ||
                 strcmp(argv[i], "--plugin") == 0 ||
                 strcmp(argv[i], "--replay") == 0 ||
                 strcmp(argv[i], "-e") == 0 ||
                 strcmp(argv[i], "--eval") == 0 ||
                 strcmp(argv[i], "--explain") == 0) && i+1 < argc) {
                i++; /* skip the value too */
            }
            continue;
        }
        sub = argv[i];
        sub_idx = i;
        break;
    }

    #define sub_arg(n) (argv[sub_idx + 1 + (n)])
    #define sub_argc   (argc - sub_idx - 1)

    if (sub) {

        if (strcmp(sub, "--explain") == 0 || strcmp(sub, "explain") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "usage: xs --explain <error-code>\n"); return 1; }
            return diag_explain(sub_arg(0));

        } else if (strcmp(sub, "check") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs check: missing file\n"); return 1; }
            do_check = 1;
            filename = sub_arg(0);
            file_arg = sub_idx + 1;
            goto run_file;

        } else if (strcmp(sub, "build") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs build: missing file\n"); return 1; }
#ifdef XSC_ENABLE_VM
            {
                const char *src = sub_arg(0);
                const char *out = NULL;
                for (int k = 1; k < sub_argc; k++)
                    if (strcmp(sub_arg(k), "-o") == 0 && k + 1 < sub_argc) out = sub_arg(++k);
                char out_buf[4096];
                if (!out) {
                    snprintf(out_buf, sizeof out_buf, "%s", src);
                    char *dot = strrchr(out_buf, '.');
                    if (dot) strcpy(dot, ".xsc");
                    else strcat(out_buf, ".xsc");
                    out = out_buf;
                }
                Node *prog = parse_file(src, NULL);
                if (!prog) return 1;
                XSProto *proto = compile_program(prog);
                if (proto_write_file(proto, out) != 0) {
                    fprintf(stderr, "xs build: failed to write %s\n", out);
                    proto_free(proto); node_free(prog);
                    return 1;
                }
                fprintf(stderr, "compiled %s -> %s\n", src, out);
                proto_free(proto); node_free(prog);
                cache_free(g_sema_cache);
                return 0;
            }
#else
            fprintf(stderr, "xs build: VM not available (rebuild with XSC_ENABLE_VM=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "run") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs run: missing file\n"); return 1; }
            {
                const char *run_path = sub_arg(0);
                size_t rplen = strlen(run_path);
                /* .xsc files go through the VM, .xs files through the interpreter */
                if (rplen > 4 && strcmp(run_path + rplen - 4, ".xsc") == 0) {
#ifdef XSC_ENABLE_VM
                    XSProto *proto = proto_read_file(run_path);
                    if (!proto) {
                        fprintf(stderr, "xs run: failed to load %s\n", run_path);
                        return 1;
                    }
                    VM *vm = vm_new();
                    vm_run(vm, proto);
                    vm_drain_tasks();
                    vm_free(vm);
                    proto_free(proto);
                    return 0;
#else
                    fprintf(stderr, "xs run: VM not available (rebuild with XSC_ENABLE_VM=1)\n");
                    return 1;
#endif
                } else {
                    /* treat as .xs source file, run via interpreter */
                    filename = run_path;
                    file_arg = sub_idx + 1;
                    goto run_file;
                }
            }

        } else if (strcmp(sub, "dap") == 0) {
#ifdef _WIN32
            _setmode(_fileno(stdin), 0x8000);
            _setmode(_fileno(stdout), 0x8000);
#endif
#ifdef XSC_ENABLE_DAP
            int rc = dap_run();
            cache_free(g_sema_cache);
            return rc;
#else
            fprintf(stderr, "xs dap: not enabled in this build (rebuild with XSC_ENABLE_DAP=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "lsp") == 0) {
#ifdef _WIN32
            _setmode(_fileno(stdin), 0x8000);
            _setmode(_fileno(stdout), 0x8000);
#endif
            {
                /* check for -s / --source flag */
                const char *explicit_source = NULL;
                for (int k = 0; k < sub_argc; k++) {
                    if ((strcmp(sub_arg(k), "-s") == 0 || strcmp(sub_arg(k), "--source") == 0) && k + 1 < sub_argc)
                        explicit_source = sub_arg(++k);
                }

                if (explicit_source) {
                    FILE *f = fopen(explicit_source, "r");
                    if (f) {
                        fclose(f);
                        filename = explicit_source;
                        goto run_file;
                    }
                    fprintf(stderr, "xs lsp: source file not found: %s\n", explicit_source);
                    return 1;
                }

                /* auto-discover lsp.xs relative to executable. exe_dir
                 * deliberately stays at PATH_MAX/2 so the "%s/<rel>"
                 * concat below never overflows lsp_path[PATH_MAX]. */
                static char lsp_path[PATH_MAX];
                static char exe_dir[PATH_MAX/2];
                int found_xs_lsp = 0;

                exe_dir[0] = '\0';
#ifdef _WIN32
                GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
#elif defined(__linux__)
                { ssize_t r = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir)-1);
                  if (r > 0) exe_dir[r] = '\0'; }
#elif defined(__APPLE__)
                { uint32_t sz = sizeof(exe_dir);
                  _NSGetExecutablePath(exe_dir, &sz); }
#endif
                if (exe_dir[0] == '\0') snprintf(exe_dir, sizeof(exe_dir), "%s", argv[0]);

                char *last_sep = strrchr(exe_dir, '/');
                if (!last_sep) last_sep = strrchr(exe_dir, '\\');
                if (last_sep) *last_sep = '\0';

                const char *rel_paths[] = {
                    "src/lsp/lsp.xs",
                    "lsp.xs",
                    "../lib/xs/lsp.xs",
                    NULL
                };
                for (int pi = 0; rel_paths[pi] && !found_xs_lsp; pi++) {
                    snprintf(lsp_path, sizeof(lsp_path), "%s/%s", exe_dir, rel_paths[pi]);
                    FILE *f = fopen(lsp_path, "r");
                    if (f) { fclose(f); found_xs_lsp = 1; }
                }
                if (!found_xs_lsp) {
                    const char *cwd_paths[] = { "src/lsp/lsp.xs", "lsp.xs", NULL };
                    for (int pi = 0; cwd_paths[pi] && !found_xs_lsp; pi++) {
                        FILE *f = fopen(cwd_paths[pi], "r");
                        if (f) { fclose(f); snprintf(lsp_path, sizeof(lsp_path), "%s", cwd_paths[pi]); found_xs_lsp = 1; }
                    }
                }

                if (found_xs_lsp) {
                    filename = lsp_path;
                    goto run_file;
                }
            }
            /* fall back to C LSP */
#ifdef XSC_ENABLE_LSP
            int rc = lsp_run();
            cache_free(g_sema_cache);
            return rc;
#else
            fprintf(stderr, "xs lsp: not enabled in this build (rebuild with XSC_ENABLE_LSP=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "replay") == 0) {
#ifdef XSC_ENABLE_TRACER
            if (sub_argc < 1) { fprintf(stderr, "xs replay: missing trace file\n"); return 1; }
            int dump_mode = (sub_argc >= 2 && strcmp(sub_arg(1), "--dump") == 0);
            int rc = dump_mode ? replay_dump(sub_arg(0)) : replay_run(sub_arg(0));
            cache_free(g_sema_cache);
            return rc;
#else
            fprintf(stderr, "xs replay: not enabled in this build (rebuild with XSC_ENABLE_TRACER=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "fmt") == 0) {
#ifdef XSC_ENABLE_FMT
            if (sub_argc < 1) { fprintf(stderr, "xs fmt: missing file\n"); return 1; }
            /* find --check flag and file arg in any order */
            int check_only = 0;
            const char *fmt_path = NULL;
            for (int fi = 0; fi < sub_argc; fi++) {
                if (strcmp(sub_arg(fi), "--check") == 0) check_only = 1;
                else if (!fmt_path) fmt_path = sub_arg(fi);
            }
            if (!fmt_path) { fprintf(stderr, "xs fmt: missing file\n"); return 1; }
            if (check_only)
                return fmt_file_check(fmt_path);
            return fmt_file(fmt_path);
#else
            fprintf(stderr, "xs fmt: not enabled in this build (rebuild with XSC_ENABLE_FMT=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "lint") == 0) {
            const char *target = sub_argc >= 1 ? sub_arg(0) : ".";
            int auto_fix = 0;
            for (int i = sub_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--fix") == 0) auto_fix = 1;
            }
            return lint_file(target, auto_fix);

        } else if (strcmp(sub, "doc") == 0) {
#ifdef XSC_ENABLE_DOC
            const char *target = sub_argc >= 1 ? sub_arg(0) : ".";
            struct stat st;
            int is_dir = (stat(target, &st) == 0 && S_ISDIR(st.st_mode));
            if (is_dir) {
                DIR *d = opendir(target);
                if (!d) {
                    fprintf(stderr, "xs doc: cannot open dir '%s'\n", target);
                    return 1;
                }
                int rc = 0, found = 0;
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    const char *n = ent->d_name;
                    int nl = (int)strlen(n);
                    if (nl < 4 || strcmp(n + nl - 3, ".xs") != 0) continue;
                    char path[1024];
                    snprintf(path, sizeof path, "%s/%s", target, n);
                    Node *prog = parse_file(path, NULL);
                    if (!prog) { rc = 1; continue; }
                    char *out = docgen_generate(prog, path, "markdown");
                    if (out) { printf("%s", out); free(out); }
                    node_free(prog);
                    found = 1;
                }
                closedir(d);
                if (!found) {
                    fprintf(stderr, "xs doc: no .xs files in '%s'\n", target);
                }
                cache_free(g_sema_cache);
                return rc;
            }
            Node *prog = parse_file(target, NULL);
            if (!prog) return 1;
            char *out = docgen_generate(prog, target, "markdown");
            if (out) { printf("%s", out); free(out); }
            node_free(prog);
            cache_free(g_sema_cache);
            return 0;
#else
            fprintf(stderr, "xs doc: not enabled in this build (rebuild with XSC_ENABLE_DOC=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "test") == 0) {
            int test_watch = 0;
            const char *pattern = "test";
            for (int si = 0; si < sub_argc; si++) {
                if (strcmp(sub_arg(si), "--watch") == 0) test_watch = 1;
                else pattern = sub_arg(si);
            }
test_again: ;
            int passed = 0, failed = 0, total = 0;
            double total_elapsed = 0.0;

            printf("Running tests...\n");

            int is_specific_file = 0;
            int is_directory = 0;
            if (sub_argc >= 1) {
                size_t alen = strlen(sub_arg(0));
                if (alen > 3 && strcmp(sub_arg(0) + alen - 3, ".xs") == 0) {
                    FILE *fp = fopen(sub_arg(0), "r");
                    if (fp) { fclose(fp); is_specific_file = 1; }
                }
                if (!is_specific_file) {
                    DIR *dtest = opendir(sub_arg(0));
                    if (dtest) { closedir(dtest); is_directory = 1; }
                }
            }

            if (is_specific_file) {
                const char *tpath = sub_arg(0);
                Node *prog = parse_file(tpath, NULL);
                if (!prog) {
                    fprintf(stderr, "  FAIL  %s (parse error)\n", tpath);
                    cache_free(g_sema_cache);
                    return 1;
                }

                typedef struct { char *names[1024]; int count; } TestList;
                TestList tl; tl.count = 0;
                if (VAL_TAG(prog) == NODE_PROGRAM) {
                    for (int j = 0; j < prog->program.stmts.len; j++) {
                        Node *s = prog->program.stmts.items[j];
                        if (s && VAL_TAG(s) == NODE_FN_DECL && s->fn_decl.is_test && s->fn_decl.name) {
                            if (tl.count < 1024) {
                                tl.names[tl.count++] = s->fn_decl.name;
                            }
                        }
                    }
                }

                if (tl.count == 0) {
                    printf("  No #[test] functions found in %s\n", tpath);
                    node_free(prog);
                    cache_free(g_sema_cache);
                    return 0;
                }

                Interp *interp = interp_new(tpath);
                interp_run(interp, prog);

                for (int ti = 0; ti < tl.count; ti++) {
                    const char *fname = tl.names[ti];
                    clock_t t_start = clock();
                    int test_failed = 0;
                    const char *err_msg = NULL;

                    char err_buf[512] = {0};
                    Value *fn_val = env_get(interp->globals, fname);
                    if (!fn_val || (VAL_TAG(fn_val) != XS_FUNC &&
                                    VAL_TAG(fn_val) != XS_NATIVE &&
                                    VAL_TAG(fn_val) != XS_CLOSURE &&
                                    VAL_TAG(fn_val) != XS_OVERLOAD)) {
                        test_failed = 1;
                        err_msg = "function not found";
                    } else {
                        interp->cf.signal = 0;
                        Value *res = call_value(interp, fn_val, NULL, 0, fname);
                        if (interp->cf.signal == CF_ERROR || interp->cf.signal == CF_PANIC ||
                            interp->cf.signal == CF_THROW) {
                            test_failed = 1;
                            if (interp->cf.value) {
                                char *vs = value_str(interp->cf.value);
                                snprintf(err_buf, sizeof err_buf, "%s", vs);
                                free(vs);
                                err_msg = err_buf;
                            } else {
                                err_msg = interp->cf.signal == CF_PANIC ? "panic" : "runtime error";
                            }
                        }
                        if (res) value_decref(res);
                        interp->cf.signal = 0;
                    }

                    clock_t t_end = clock();
                    double elapsed = (double)(t_end - t_start) / CLOCKS_PER_SEC;
                    total_elapsed += elapsed;
                    total++;

                    if (test_failed) {
                        printf("  FAIL  %s (%.3fs)\n", fname, elapsed);
                        if (err_msg) printf("    Error: %s\n", err_msg);
                        failed++;
                    } else {
                        printf("  PASS  %s (%.3fs)\n", fname, elapsed);
                        passed++;
                    }
                }

                interp_free(interp);
                node_free(prog);

            } else {
                typedef struct { char paths[4096][PATH_MAX]; int count; } FileList;
                FileList *fl = (FileList *)xs_calloc(1, sizeof(FileList));

                struct { char dir[PATH_MAX]; } stack[256];
                int sp = 0;
                const char *scan_root = is_directory ? sub_arg(0) : ".";
                snprintf(stack[sp].dir, sizeof(stack[sp].dir), "%s", scan_root);
                sp++;

                while (sp > 0 && fl->count < 4096) {
                    sp--;
                    char cur_dir[PATH_MAX];
                    snprintf(cur_dir, sizeof(cur_dir), "%s", stack[sp].dir);
                    DIR *d = opendir(cur_dir);
                    if (!d) continue;
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL && fl->count < 4096) {
                        if (ent->d_name[0] == '.') continue;
                        char fullpath[PATH_MAX];
                        /* hard-bound both halves so even worst-case
                         * cur_dir + name + sep + nul fits fullpath. */
                        snprintf(fullpath, sizeof(fullpath), "%.*s/%.*s",
                                 (int)(sizeof(fullpath) / 2 - 1), cur_dir,
                                 (int)(sizeof(fullpath) / 2 - 1), ent->d_name);

                        DIR *sub_d = opendir(fullpath);
                        if (sub_d) {
                            closedir(sub_d);
                            if (sp < 256) {
                                snprintf(stack[sp].dir, sizeof(stack[sp].dir), "%s", fullpath);
                                sp++;
                            }
                            continue;
                        }

                        size_t nlen = strlen(ent->d_name);
                        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".xs") != 0) continue;

                        int match = 0;
                        if (strncmp(ent->d_name, "test_", 5) == 0) match = 1;
                        else if (nlen >= 8 && strcmp(ent->d_name + nlen - 8, "_test.xs") == 0) match = 1;
                        else if (strstr(ent->d_name, pattern) != NULL) match = 1;

                        if (match) {
                            snprintf(fl->paths[fl->count], sizeof(fl->paths[fl->count]), "%s", fullpath);
                            fl->count++;
                        }
                    }
                    closedir(d);
                }

                for (int i = 0; i < fl->count - 1; i++)
                    for (int j = i + 1; j < fl->count; j++)
                        if (strcmp(fl->paths[i], fl->paths[j]) > 0) {
                            char tmp[PATH_MAX];
                            memcpy(tmp, fl->paths[i], PATH_MAX);
                            memcpy(fl->paths[i], fl->paths[j], PATH_MAX);
                            memcpy(fl->paths[j], tmp, PATH_MAX);
                        }

                for (int ti = 0; ti < fl->count; ti++) {
                    const char *tpath = fl->paths[ti];
                    clock_t t_start = clock();

                    Node *prog = parse_file(tpath, NULL);
                    int test_failed = 0;
                    const char *err_msg = NULL;

                    if (!prog) {
                        test_failed = 1;
                        err_msg = "parse error";
                    } else {
                        int has_test_attrs = 0;
                        typedef struct { char *names[1024]; int cnt; } TL2;
                        TL2 tl2; tl2.cnt = 0;
                        if (VAL_TAG(prog) == NODE_PROGRAM) {
                            for (int j = 0; j < prog->program.stmts.len; j++) {
                                Node *s = prog->program.stmts.items[j];
                                if (s && VAL_TAG(s) == NODE_FN_DECL && s->fn_decl.is_test && s->fn_decl.name) {
                                    has_test_attrs = 1;
                                    if (tl2.cnt < 1024) tl2.names[tl2.cnt++] = s->fn_decl.name;
                                }
                            }
                        }

                        if (has_test_attrs) {
                            Interp *interp = interp_new(tpath);
                            interp_run(interp, prog);

                            const char *display = strrchr(tpath, '/');
                            display = display ? display + 1 : tpath;

                            printf("  %s (%d test functions)\n", display, tl2.cnt);

                            for (int k = 0; k < tl2.cnt; k++) {
                                clock_t tf_start = clock();
                                const char *fname = tl2.names[k];
                                int tf_failed = 0;
                                const char *tf_err = NULL;

                                Value *fn_val = env_get(interp->globals, fname);
                                if (!fn_val || VAL_TAG(fn_val) != XS_FUNC) {
                                    tf_failed = 1;
                                    tf_err = "function not found";
                                } else {
                                    interp->cf.signal = 0;
                                    Value *res = call_value(interp, fn_val, NULL, 0, fname);
                                    if (interp->cf.signal == CF_ERROR || interp->cf.signal == CF_PANIC ||
                                        interp->cf.signal == CF_THROW) {
                                        tf_failed = 1;
                                        tf_err = "assertion failed or runtime error";
                                    }
                                    if (res) value_decref(res);
                                    interp->cf.signal = 0;
                                }

                                clock_t tf_end = clock();
                                double tf_elapsed = (double)(tf_end - tf_start) / CLOCKS_PER_SEC;
                                total_elapsed += tf_elapsed;
                                total++;

                                if (tf_failed) {
                                    printf("    FAIL  %s (%.3fs)\n", fname, tf_elapsed);
                                    if (tf_err) printf("      Error: %s\n", tf_err);
                                    failed++;
                                } else {
                                    printf("    PASS  %s (%.3fs)\n", fname, tf_elapsed);
                                    passed++;
                                }
                            }

                            interp_free(interp);
                            node_free(prog);
                            continue; /* skip the whole-file pass/fail below */
                        }

                        /* No #[test] functions: run each file in a fresh
                           subprocess so global plugin/async/hook state
                           from one test cannot taint the next. */
                        node_free(prog);
                        char cmd[PATH_MAX + 64];
                        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"",
                                 argv[0], tpath);
                        int rc_sys = system(cmd);
                        if (rc_sys != 0) {
                            test_failed = 1;
                            err_msg = "runtime error";
                        }
                    }

                    clock_t t_end = clock();
                    double elapsed = (double)(t_end - t_start) / CLOCKS_PER_SEC;
                    total_elapsed += elapsed;
                    total++;

                    const char *display = strrchr(tpath, '/');
                    display = display ? display + 1 : tpath;

                    if (test_failed) {
                        printf("  FAIL  %s (%.3fs)\n", display, elapsed);
                        if (err_msg) printf("    Error: %s\n", err_msg);
                        failed++;
                    } else {
                        printf("  PASS  %s (%.3fs)\n", display, elapsed);
                        passed++;
                    }
                }

                free(fl);
            }

            printf("\nResults: %d passed, %d failed, %d total (%.3fs)\n",
                   passed, failed, total, total_elapsed);
            cache_free(g_sema_cache);
            g_sema_cache = NULL;
            if (test_watch) {
                printf("\nWatching for changes... (Ctrl+C to stop)\n");
                struct timespec ts = {0, 500000000}; /* 500ms */
                nanosleep(&ts, NULL);
                printf("\033[2J\033[H"); /* clear screen */
                goto test_again;
            }
            return (failed > 0) ? 1 : 0;

        } else if (strcmp(sub, "bench") == 0) {
            const char *pattern = (sub_argc >= 1) ? sub_arg(0) : "bench";
            int total = 0;

            printf("Running benchmarks...\n\n");

            {
                typedef struct { char paths[4096][PATH_MAX]; int count; } FileList;
                FileList *fl = (FileList *)xs_calloc(1, sizeof(FileList));

                struct { char dir[PATH_MAX]; } stack[256];
                int sp = 0;
                snprintf(stack[sp].dir, sizeof(stack[sp].dir), "%s", ".");
                sp++;

                while (sp > 0 && fl->count < 4096) {
                    sp--;
                    char cur_dir[PATH_MAX];
                    snprintf(cur_dir, sizeof(cur_dir), "%s", stack[sp].dir);
                    DIR *d = opendir(cur_dir);
                    if (!d) continue;
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL && fl->count < 4096) {
                        if (ent->d_name[0] == '.') continue;
                        char fullpath[PATH_MAX];
                        /* hard-bound both halves so even worst-case
                         * cur_dir + name + sep + nul fits fullpath. */
                        snprintf(fullpath, sizeof(fullpath), "%.*s/%.*s",
                                 (int)(sizeof(fullpath) / 2 - 1), cur_dir,
                                 (int)(sizeof(fullpath) / 2 - 1), ent->d_name);

                        DIR *sub_d = opendir(fullpath);
                        if (sub_d) {
                            closedir(sub_d);
                            if (sp < 256) {
                                snprintf(stack[sp].dir, sizeof(stack[sp].dir), "%s", fullpath);
                                sp++;
                            }
                            continue;
                        }

                        size_t nlen = strlen(ent->d_name);
                        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".xs") != 0) continue;

                        int match = 0;
                        if (strncmp(ent->d_name, "bench_", 6) == 0) match = 1;
                        else if (nlen >= 9 && strcmp(ent->d_name + nlen - 9, "_bench.xs") == 0) match = 1;
                        else if (strstr(ent->d_name, pattern) != NULL) match = 1;

                        if (match) {
                            snprintf(fl->paths[fl->count], sizeof(fl->paths[fl->count]), "%s", fullpath);
                            fl->count++;
                        }
                    }
                    closedir(d);
                }

                for (int bi = 0; bi < fl->count; bi++) {
                    const char *bpath = fl->paths[bi];
                    Node *prog = parse_file(bpath, NULL);
                    if (!prog) {
                        printf("  SKIP  %s (parse error)\n", bpath);
                        continue;
                    }

                    const char *display = strrchr(bpath, '/');
                    display = display ? display + 1 : bpath;

                    double times[10];
                    double min_t = 1e30, max_t = 0.0, sum_t = 0.0;

                    for (int run = 0; run < 10; run++) {
                        clock_t t_start = clock();
                        Interp *interp = interp_new(bpath);
                        interp_run(interp, prog);
                        interp_free(interp);
                        clock_t t_end = clock();

                        times[run] = (double)(t_end - t_start) / CLOCKS_PER_SEC;
                        sum_t += times[run];
                        if (times[run] < min_t) min_t = times[run];
                        if (times[run] > max_t) max_t = times[run];
                    }

                    printf("  BENCH %s  min=%.3fs  max=%.3fs  avg=%.3fs  (10 runs)\n",
                           display, min_t, max_t, sum_t / 10.0);
                    node_free(prog);
                    total++;
                }

                free(fl);
            }

            printf("\n%d benchmarks completed.\n", total);
            cache_free(g_sema_cache);
            return 0;

        } else if (strcmp(sub, "coverage") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs coverage: missing file\n"); return 1; }
#ifdef XSC_ENABLE_COVERAGE
            do_coverage = 1;
#endif
            filename = sub_arg(0);
            file_arg = sub_idx + 1;
            goto run_file;

        } else if (strcmp(sub, "profile") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs profile: missing file\n"); return 1; }
#ifdef XSC_ENABLE_PROFILER
            do_profile = 1;
#endif
            filename = sub_arg(0);
            file_arg = sub_idx + 1;
            goto run_file;

        } else if (strcmp(sub, "transpile") == 0) {
            const char *target_lang = NULL;
            const char *src_file    = NULL;
            for (int i = sub_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--target") == 0 && i+1 < argc) { target_lang = argv[++i]; }
                else if (argv[i][0] != '-') src_file = argv[i];
            }
            if (!target_lang || !src_file) {
                fprintf(stderr, "xs transpile: usage: xs transpile --target <js|c|wasm32|wasi> <file>\n");
                return 1;
            }
#ifdef XSC_ENABLE_TRANSPILER
            Node *prog = parse_file(src_file, NULL);
            if (!prog) return 1;
            char *out = NULL;
            int wasm_mode = 0;
            if      (strcmp(target_lang, "js")     == 0) out = transpile_js(prog, src_file);
            else if (strcmp(target_lang, "c")      == 0) out = transpile_c(prog, src_file);
            else if (strcmp(target_lang, "wasm32") == 0 ||
                     strcmp(target_lang, "wasi")   == 0) wasm_mode = 1;
            else { fprintf(stderr, "xs transpile: unknown target '%s'\n", target_lang); node_free(prog); return 1; }
            if (wasm_mode) { transpile_wasm(prog, src_file, "out.wasm"); }
            else if (out)  { printf("%s", out); free(out); }
            node_free(prog);
            cache_free(g_sema_cache);
            return 0;
#else
            fprintf(stderr, "xs transpile: not enabled in this build (rebuild with XSC_ENABLE_TRANSPILER=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "new") == 0) {
            if (sub_argc < 1) { fprintf(stderr, "xs new: missing project name\n"); return 1; }
#ifdef XSC_ENABLE_PKG
            return pkg_new(sub_arg(0));
#else
            {
                const char *pname = sub_arg(0);
                char pathbuf[1024];
                if (mkdir(pname, 0755) != 0) { fprintf(stderr, "xs new: cannot create directory '%s'\n", pname); return 1; }
                snprintf(pathbuf, sizeof pathbuf, "%s/src", pname);
                if (mkdir(pathbuf, 0755) != 0) { fprintf(stderr, "xs new: cannot create '%s'\n", pathbuf); return 1; }
                FILE *f;
                snprintf(pathbuf, sizeof pathbuf, "%s/xs.toml", pname);
                f = fopen(pathbuf, "w");
                if (!f) { fprintf(stderr, "xs new: cannot write '%s'\n", pathbuf); return 1; }
                fprintf(f, "[package]\nname = \"%s\"\nversion = \"0.1.0\"\nxs_version = \">=1.0\"\n\n[dependencies]\n\n[build]\nentry = \"src/main.xs\"\n", pname);
                fclose(f);
                snprintf(pathbuf, sizeof pathbuf, "%s/src/main.xs", pname);
                f = fopen(pathbuf, "w");
                if (!f) { fprintf(stderr, "xs new: cannot write '%s'\n", pathbuf); return 1; }
                fprintf(f, "fn main() {\n    println(\"Hello from %s!\")\n}\n", pname);
                fclose(f);
                snprintf(pathbuf, sizeof pathbuf, "%s/.gitignore", pname);
                f = fopen(pathbuf, "w");
                if (!f) { fprintf(stderr, "xs new: cannot write '%s'\n", pathbuf); return 1; }
                fprintf(f, "xs_lib/\n*.xsc\n.xs_cache/\nbuild/\n");
                fclose(f);
                snprintf(pathbuf, sizeof pathbuf, "%s/tests", pname);
                mkdir(pathbuf, 0755);
                snprintf(pathbuf, sizeof pathbuf, "%s/tests/test_main.xs", pname);
                f = fopen(pathbuf, "w");
                if (f) {
                    fprintf(f, "#[test]\nfn test_hello() {\n    assert(true)\n}\n");
                    fclose(f);
                }
                snprintf(pathbuf, sizeof pathbuf, "%s/README.md", pname);
                f = fopen(pathbuf, "w");
                if (f) {
                    fprintf(f, "# %s\n\nAn XS project.\n\n## Getting Started\n\n```sh\nxs run src/main.xs\nxs test\n```\n", pname);
                    fclose(f);
                }
                printf("Created project '%s'\n  %s/xs.toml\n  %s/src/main.xs\n  %s/tests/test_main.xs\n  %s/README.md\n  %s/.gitignore\n", pname, pname, pname, pname, pname, pname);
                return 0;
            }
#endif

        } else if (strcmp(sub, "init") == 0) {
            /* init project in current directory */
            {
                const char *dirname = ".";
                char cwd[1024];
                if (getcwd(cwd, sizeof cwd)) {
                    const char *slash = strrchr(cwd, '/');
                    dirname = slash ? slash + 1 : cwd;
                }
                FILE *f;
                struct stat st;
                if (stat("xs.toml", &st) == 0) {
                    fprintf(stderr, "xs init: xs.toml already exists\n");
                    return 1;
                }
                f = fopen("xs.toml", "w");
                if (!f) { fprintf(stderr, "xs init: cannot write xs.toml\n"); return 1; }
                fprintf(f, "[package]\nname = \"%s\"\nversion = \"0.1.0\"\nxs_version = \">=1.0\"\n\n[dependencies]\n\n[build]\nentry = \"src/main.xs\"\n", dirname);
                fclose(f);
                if (stat("src", &st) != 0) mkdir("src", 0755);
                if (stat("src/main.xs", &st) != 0) {
                    f = fopen("src/main.xs", "w");
                    if (f) {
                        fprintf(f, "fn main() {\n    println(\"Hello from %s!\")\n}\n", dirname);
                        fclose(f);
                    }
                }
                if (stat("tests", &st) != 0) mkdir("tests", 0755);
                if (stat(".gitignore", &st) != 0) {
                    f = fopen(".gitignore", "w");
                    if (f) { fprintf(f, "xs_lib/\n*.xsc\n.xs_cache/\nbuild/\n"); fclose(f); }
                }
                printf("Initialized XS project in current directory\n");
                return 0;
            }

        } else if (strcmp(sub, "install") == 0) {
#ifdef XSC_ENABLE_PKG
            return sub_argc >= 1 ? pkg_install(sub_arg(0)) : pkg_install(NULL);
#else
            fprintf(stderr, "xs install: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "add") == 0) {
#ifdef XSC_ENABLE_PKG
            if (sub_argc < 1) { fprintf(stderr, "xs add: missing package name\n  usage: xs add <name>  or  xs add user/repo\n"); return 1; }
            return pkg_add(sub_arg(0));
#else
            fprintf(stderr, "xs add: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "remove") == 0) {
#ifdef XSC_ENABLE_PKG
            if (sub_argc < 1) { fprintf(stderr, "xs remove: missing package\n"); return 1; }
            return pkg_remove(sub_arg(0));
#else
            fprintf(stderr, "xs remove: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "update") == 0) {
#ifdef XSC_ENABLE_PKG
            return sub_argc >= 1 ? pkg_update(sub_arg(0)) : pkg_update(NULL);
#else
            fprintf(stderr, "xs update: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "list") == 0) {
#ifdef XSC_ENABLE_PKG
            return pkg_list();
#else
            fprintf(stderr, "xs list: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "publish") == 0) {
#ifdef XSC_ENABLE_PKG
            return pkg_publish(sub_argc >= 1 ? sub_arg(0) : ".");
#else
            fprintf(stderr, "xs publish: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "login") == 0) {
#ifdef XSC_ENABLE_PKG
            return pkg_login();
#else
            fprintf(stderr, "xs login: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "logout") == 0) {
#ifdef XSC_ENABLE_PKG
            return pkg_logout();
#else
            fprintf(stderr, "xs logout: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "whoami") == 0) {
#ifdef XSC_ENABLE_PKG
            return pkg_whoami();
#else
            fprintf(stderr, "xs whoami: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "search") == 0) {
#ifdef XSC_ENABLE_PKG
            if (sub_argc < 1) { fprintf(stderr, "xs search: missing query\n"); return 1; }
            return pkg_search(sub_arg(0));
#else
            fprintf(stderr, "xs search: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif

        } else if (strcmp(sub, "pkg") == 0) {
#ifdef XSC_ENABLE_PKG
            if (sub_argc < 1) { fprintf(stderr, "xs pkg: missing subcommand (install|remove|update|list|publish|search)\n"); return 1; }
            if      (strcmp(sub_arg(0), "list")    == 0) return pkg_list();
            else if (strcmp(sub_arg(0), "install") == 0) return sub_argc >= 2 ? pkg_install(sub_arg(1)) : pkg_install(NULL);
            else if (strcmp(sub_arg(0), "remove")  == 0) { if (sub_argc < 2) { fprintf(stderr, "xs pkg remove: missing package\n"); return 1; } return pkg_remove(sub_arg(1)); }
            else if (strcmp(sub_arg(0), "update")  == 0) return sub_argc >= 2 ? pkg_update(sub_arg(1)) : pkg_update(NULL);
            else if (strcmp(sub_arg(0), "add")     == 0) { if (sub_argc < 2) { fprintf(stderr, "xs pkg add: missing package\n"); return 1; } return pkg_add(sub_arg(1)); }
            else if (strcmp(sub_arg(0), "search")  == 0) { if (sub_argc < 2) { fprintf(stderr, "xs pkg search: missing query\n"); return 1; } return pkg_search(sub_arg(1)); }
            else if (strcmp(sub_arg(0), "publish") == 0) return pkg_publish(sub_argc >= 2 ? sub_arg(1) : ".");
            else { fprintf(stderr, "xs pkg: unknown subcommand '%s'\n", sub_arg(0)); return 1; }
#else
            fprintf(stderr, "xs pkg: not enabled in this build (rebuild with XSC_ENABLE_PKG=1)\n");
            return 1;
#endif
        }
    }

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--check")    == 0) do_check = 1;
        else if (strcmp(argv[i], "--lenient")  == 0) lenient  = 1;
        else if (strcmp(argv[i], "--strict")   == 0) strict   = 1;
        else if (strcmp(argv[i], "--no-color") == 0) g_no_color = 1;
        else if (strcmp(argv[i], "--gc-debug") == 0) gc_set_debug(1);
        else if (strcmp(argv[i], "--optimize") == 0) do_optimize = 1;
        else if (strcmp(argv[i], "--watch")    == 0) do_watch = 1;
        else if (strcmp(argv[i], "--instr-limit") == 0 && i+1 < argc) {
            xs_limits_set_instructions(strtoull(argv[++i], NULL, 10));
        }
        else if (strcmp(argv[i], "--time-limit") == 0 && i+1 < argc) {
            xs_limits_set_wall_time_ms(strtoull(argv[++i], NULL, 10));
        }
        else if (strcmp(argv[i], "--memory-limit") == 0 && i+1 < argc) {
            /* Parse with optional K/M/G suffix. */
            char *end = NULL;
            unsigned long long v = strtoull(argv[++i], &end, 10);
            if (end && *end) {
                switch (*end) {
                    case 'k': case 'K': v *= 1024ULL; break;
                    case 'm': case 'M': v *= 1024ULL*1024ULL; break;
                    case 'g': case 'G': v *= 1024ULL*1024ULL*1024ULL; break;
                    default: break;
                }
            }
            xs_limits_set_memory_bytes((size_t)v);
        }
        else if (strcmp(argv[i], "run")        == 0) { /* skip */ }
        else if (strcmp(argv[i], "--backend")  == 0 && i+1 < argc) {
            const char *be = argv[++i];
            if (strcmp(be, "vm") == 0) {
#ifdef XSC_ENABLE_VM
                do_vm = 1;
#else
                fprintf(stderr, "xs: VM backend not built (rebuild with XSC_ENABLE_VM=1)\n");
                return 1;
#endif
            } else if (strcmp(be, "jit") == 0) {
#ifdef XSC_ENABLE_JIT
                do_jit = 1;
#else
                fprintf(stderr, "xs: JIT backend not available (rebuild with XSC_ENABLE_JIT=1)\n");
                return 1;
#endif
            } else if (strcmp(be, "interp") == 0) {
#ifdef XSC_ENABLE_VM
                do_vm = 0;
#endif
            }
        }
        else if (strcmp(argv[i], "--vm")       == 0) {
#ifdef XSC_ENABLE_VM
            do_vm = 1;
#else
            fprintf(stderr, "xs: VM not built (rebuild with XSC_ENABLE_VM=1)\n"); return 1;
#endif
        }
        else if (strcmp(argv[i], "--interp")   == 0) {
            /* Explicit opt-in to the tree-walking interpreter. */
#ifdef XSC_ENABLE_VM
            do_vm = 0;
#endif
        }
        else if (strcmp(argv[i], "--jit")       == 0) {
#ifdef XSC_ENABLE_JIT
            do_jit = 1;
#else
            fprintf(stderr, "xs: JIT not built (rebuild with XSC_ENABLE_JIT=1)\n"); return 1;
#endif
        }
        else if (strcmp(argv[i], "--debug")    == 0 && i+1 < argc) {
            filename = argv[++i]; file_arg = i;
#ifdef XSC_ENABLE_DAP
            {
                int rc = dap_run();
                cache_free(g_sema_cache);
                return rc;
            }
#else
            fprintf(stderr, "xs: DAP not built (rebuild with XSC_ENABLE_DAP=1)\n");
            return 1;
#endif
        }
        else if (strcmp(argv[i], "--lsp")      == 0) {
#ifdef _WIN32
            _setmode(_fileno(stdin), 0x8000);
            _setmode(_fileno(stdout), 0x8000);
#endif
#ifdef XSC_ENABLE_LSP
            int rc = lsp_run(); cache_free(g_sema_cache); return rc;
#else
            fprintf(stderr, "xs: LSP not built (rebuild with XSC_ENABLE_LSP=1)\n"); return 1;
#endif
        }
        else if (strcmp(argv[i], "--dap")      == 0) {
#ifdef _WIN32
            _setmode(_fileno(stdin), 0x8000);
            _setmode(_fileno(stdout), 0x8000);
#endif
#ifdef XSC_ENABLE_DAP
            int rc = dap_run(); cache_free(g_sema_cache); return rc;
#else
            fprintf(stderr, "xs: DAP not built (rebuild with XSC_ENABLE_DAP=1)\n"); return 1;
#endif
        }
        else if (strcmp(argv[i], "--record")   == 0 && i+1 < argc) {
#ifdef XSC_ENABLE_TRACER
            record_path = argv[++i];
            /* The tracer needs AST-level hooks that only the interpreter
               fires, so --record implies --interp. */
#ifdef XSC_ENABLE_VM
            do_vm = 0;
#endif
#else
            fprintf(stderr, "xs: tracer not built (rebuild with XSC_ENABLE_TRACER=1)\n"); i++; return 1;
#endif
        }
        else if (strcmp(argv[i], "--replay")   == 0 && i+1 < argc) {
#ifdef XSC_ENABLE_TRACER
            int rc = replay_run(argv[++i]); cache_free(g_sema_cache); return rc;
#else
            fprintf(stderr, "xs: tracer not built (rebuild with XSC_ENABLE_TRACER=1)\n"); return 1;
#endif
        }
        else if (strcmp(argv[i], "--trace-sample") == 0 && i+1 < argc) {
#ifdef XSC_ENABLE_TRACER
            trace_sample = atof(argv[++i]);
#else
            i++; /* skip value */
#endif
        }
        else if (strcmp(argv[i], "--trace-deep") == 0) {
#ifdef XSC_ENABLE_TRACER
            trace_deep = 1;
#endif
        }
        else if (strcmp(argv[i], "--plugin")   == 0 && i+1 < argc) {
#ifdef XSC_ENABLE_PLUGINS
            plugin_path = argv[++i];
#else
            i++; /* skip argument */
            fprintf(stderr, "xs: --plugin not available (rebuild with XSC_ENABLE_PLUGINS=1)\n");
            return 1;
#endif
        }
        else if (strcmp(argv[i], "--sandbox")  == 0) {
            fprintf(stderr, "xs: sandbox mode enabled\n");
        }
        else if (strcmp(argv[i], "--profile")  == 0) {
#ifdef XSC_ENABLE_PROFILER
            do_profile = 1;
#endif
        }
        else if (strcmp(argv[i], "--coverage") == 0) {
#ifdef XSC_ENABLE_COVERAGE
            do_coverage = 1;
#endif
        }
        else if (strcmp(argv[i], "--trace") == 0) {
            do_trace = 1;
        }
#ifdef XSC_ENABLE_VM
        else if (strcmp(argv[i], "--emit")     == 0 && i+1 < argc) {
            const char *what = argv[++i];
            if      (strcmp(what, "bytecode")  == 0) emit_bytecode = 1;
            else if (strcmp(what, "ast")       == 0) emit_ast = 1;
            else if (strcmp(what, "ir")        == 0) emit_ir_ssa = 1;
            else if (strcmp(what, "bytecode-ir") == 0) emit_bytecode = 1;
            else if (strcmp(what, "jit-ir")    == 0) emit_jit_ir = 1;
            else if (strcmp(what, "llvm")      == 0) { fprintf(stderr, "xs --emit llvm: requires XSC_ENABLE_JIT\n"); return 1; }
#ifdef XSC_ENABLE_TRANSPILER
            else if (strcmp(what, "js")        == 0) emit_js   = 1;
            else if (strcmp(what, "c")         == 0) emit_c    = 1;
            else if (strcmp(what, "wasm")      == 0) emit_wasm = 1;
#endif
            else { fprintf(stderr, "xs: unknown emit target '%s'\n", what); return 1; }
        }
#else
        else if (strcmp(argv[i], "--emit")     == 0 && i+1 < argc) {
            const char *what = argv[++i];
            if (strcmp(what, "ast") == 0) { emit_ast = 1; }
            else if (strcmp(what, "ir") == 0) { emit_ir_ssa = 1; }
#ifdef XSC_ENABLE_TRANSPILER
            else if (strcmp(what, "js")   == 0) emit_js   = 1;
            else if (strcmp(what, "c")    == 0) emit_c    = 1;
            else if (strcmp(what, "wasm") == 0) emit_wasm = 1;
#endif
            else { fprintf(stderr, "xs: unknown emit target '%s' (requires XSC_ENABLE_VM or XSC_ENABLE_TRANSPILER)\n", what); return 1; }
        }
#endif
        else if ((strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--eval") == 0) && i+1 < argc) {
            const char *expr_src = argv[++i];
            DiagContext *dctx = diag_context_new();
            diag_context_add_source(dctx, "<eval>", expr_src);
            Lexer lex; lexer_init(&lex, expr_src, "<eval>");
            TokenArray ta = lexer_tokenize(&lex);
            Parser psr; parser_init(&psr, &ta, "<eval>");
            psr.diag = dctx;
            Node *prog = parser_parse(&psr);
            if (!prog || psr.had_error) {
                diag_render_all(dctx);
                diag_context_free(dctx);
                token_array_free(&ta);
                comment_list_free(&lex.comments);
                cache_free(g_sema_cache);
                return 1;
            }
            if (strict || do_check) {
                if (lenient) diag_context_set_lenient(dctx, 1);
                SemaCtx sema;
                sema_init(&sema, lenient, strict);
                sema.diag = dctx;
                int sema_errors = sema_analyze(&sema, prog, "<eval>");
                diag_render_all(dctx);
                sema_free(&sema);
                if (sema_errors > 0) {
                    fprintf(stderr, "\n%d error%s found.\n", sema_errors, sema_errors > 1 ? "s" : "");
                    diag_context_free(dctx);
                    node_free(prog);
                    token_array_free(&ta);
                    comment_list_free(&lex.comments);
                    cache_free(g_sema_cache);
                    return 1;
                }
            }
            diag_context_free(dctx);
            if (do_check || strict) {
                InferResult *ir = infer_types(prog);
                int infer_errs = infer_error_count(ir);
                for (int ie = 0; ie < infer_errs; ie++) {
                    const char *msg = infer_error_at(ir, ie);
                    if (!msg) continue;
                    if (strstr(msg, "with unit") || strstr(msg, "with ?") ||
                    strstr(msg, "i64 with int") || strstr(msg, "int with i64") ||
                    strstr(msg, "f64 with float") || strstr(msg, "float with f64") ||
                    strstr(msg, "str with string") || strstr(msg, "string with str"))
                        continue;
                    fprintf(stderr, "  %sinference: %s\n",
                            strict ? "error: " : "warning: ", msg);
                }
                infer_result_free(ir);
            }
            if (do_check) {
                printf("No errors found in <eval>\n");
                node_free(prog);
                token_array_free(&ta);
                comment_list_free(&lex.comments);
                cache_free(g_sema_cache);
                return 0;
            }
            Interp *interp = interp_new("<eval>");
            interp_run(interp, prog);
            /* Drain unjoined spawn workers before tearing the interp
               down -- they share its Interp* and would deref freed
               state if drained later. */
            xs_drain_interp_tasks();
            int had_err = (interp->cf.signal == CF_ERROR || interp->cf.signal == CF_PANIC)
                          || interp->had_unhandled_exception
                          || g_xs_runtime_error_count > 0;
            interp_free(interp);
            node_free(prog);
            token_array_free(&ta);
            comment_list_free(&lex.comments);
            cache_free(g_sema_cache);
            return had_err ? 1 : 0;
        }
        else if ((strcmp(argv[i], "--explain") == 0 || strcmp(argv[i], "explain") == 0) && i+1 < argc) {
            cache_free(g_sema_cache);
            return diag_explain(argv[++i]);
        }
        /* once the filename is locked in, anything that follows is the
         * script's own argv (handled at line 2567); refuse unknown flags
         * before the filename so typos like 'xs --test foo.xs' fail loudly
         * instead of silently doing nothing. */
        else if (argv[i][0] == '-' && argv[i][1] != '\0' && !filename) {
            fprintf(stderr, "xs: unknown flag '%s' (run 'xs --help' for usage)\n", argv[i]);
            return 1;
        }
        else if (argv[i][0] != '-' && !filename) { filename = argv[i]; file_arg = i; }
    }

    if (!filename) { usage(); return 1; }

    if (file_arg >= 0 && file_arg + 1 <= argc)
        xs_set_user_args(argc - file_arg - 1, argv + file_arg + 1);

    if (do_watch) {
        int rc = watch_and_run(filename, file_arg, argc, argv);
        cache_free(g_sema_cache);
        return rc;
    }

run_file:;
    char *src = read_file(filename);
    if (!src) return 1;

    /* --trace: prepend provenance plugin load (skip if already loaded) */
    if (do_trace && !strstr(src, "load \"plugins/provenance") &&
                    !strstr(src, "load \"provenance") &&
                    !strstr(src, "provenance.xs")) {
        /* resolve provenance plugin relative to executable */
        char trace_prefix[PATH_MAX + 128];
        char exe_path[PATH_MAX];
        exe_path[0] = '\0';
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
#elif defined(__linux__)
        { ssize_t r = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
          if (r > 0) exe_path[r] = '\0'; }
#elif defined(__APPLE__)
        { uint32_t sz = sizeof(exe_path);
          _NSGetExecutablePath(exe_path, &sz); }
#endif
        if (exe_path[0] == '\0') snprintf(exe_path, sizeof(exe_path), "%s", argv[0]);
        char *last_sep = strrchr(exe_path, '/');
        if (!last_sep) last_sep = strrchr(exe_path, '\\');
        if (last_sep) *last_sep = '\0';
        /* try tests/plugins/provenance.xs relative to binary dir.
         * bound exe_path so a worst-case PATH_MAX dir + the suffix
         * still fits prov_path. */
        char prov_path[PATH_MAX];
        snprintf(prov_path, sizeof(prov_path),
                 "%.*s/tests/plugins/provenance.xs",
                 (int)(sizeof(prov_path) - 32), exe_path);
        struct stat prov_st;
        if (stat(prov_path, &prov_st) != 0) {
            /* try plugins/provenance.xs relative to script dir */
            const char *last_slash = strrchr(filename, '/');
            if (last_slash) {
                int dirlen = (int)(last_slash - filename);
                snprintf(prov_path, sizeof(prov_path), "%.*s/plugins/provenance.xs", dirlen, filename);
            } else {
                snprintf(prov_path, sizeof(prov_path), "plugins/provenance.xs");
            }
        }
        snprintf(trace_prefix, sizeof(trace_prefix), "load \"%s\"\n", prov_path);
        size_t plen = strlen(trace_prefix);
        size_t slen = strlen(src);
        char *new_src = malloc(plen + slen + 1);
        memcpy(new_src, trace_prefix, plen);
        memcpy(new_src + plen, src, slen + 1);
        free(src);
        src = new_src;
    }

    char *src_for_cache = xs_strdup(src);

    DiagContext *dctx = diag_context_new();
    diag_context_add_source(dctx, filename, src);

    Lexer lex;
    lexer_init(&lex, src, filename);
    TokenArray ta = lexer_tokenize(&lex);
    free(src);

    Parser parser;
    parser_init(&parser, &ta, filename);
    parser.diag = dctx;
    Node *program = parser_parse(&parser);
    token_array_free(&ta);

    /* A lexer error (e.g. unterminated string) reports to stderr directly
       but may still leave enough tokens for the parser to succeed. Treat
       any lexer or parser error as fatal so we never run a source file
       that failed to tokenize or parse cleanly. */
    if (!program || parser.had_error ||
        diag_context_error_count(dctx) > 0 || lex.n_errors > 0) {
        diag_render_all(dctx);
        diag_context_free(dctx);
        if (program) node_free(program);
        free(src_for_cache);
        return 1;
    }
    diag_context_free(dctx);

    if (emit_ast) {
        ast_dump(program, 0);
        node_free(program);
        free(src_for_cache);
        cache_free(g_sema_cache);
        return 0;
    }

    if (do_optimize) {
        OptStats stats = {0};
        program = optimize(program, &stats);
        if (stats.constants_folded + stats.dead_code_removed +
            stats.functions_inlined + stats.strengths_reduced +
            stats.cses_eliminated + stats.algebraic_simplified +
            stats.constants_propagated + stats.loop_invariants_hoisted +
            stats.unused_vars_eliminated > 0) {
            fprintf(stderr, "xs: optimizer: %d folded, %d dce, %d inlined, %d strength, "
                    "%d cse, %d algebraic, %d propagated, %d licm, %d unused\n",
                    stats.constants_folded, stats.dead_code_removed,
                    stats.functions_inlined, stats.strengths_reduced,
                    stats.cses_eliminated, stats.algebraic_simplified,
                    stats.constants_propagated, stats.loop_invariants_hoisted,
                    stats.unused_vars_eliminated);
        }
    }

    if (emit_ir_ssa) {
        IRModule *ir = ir_lower(program);
        if (ir) {
            ir_dump(ir, stdout);
            ir_module_free(ir);
        }
        node_free(program);
        free(src_for_cache);
        cache_free(g_sema_cache);
        return 0;
    }

#ifdef XSC_ENABLE_TRANSPILER
    if (emit_js || emit_c || emit_wasm) {
        if (emit_js || emit_c) {
            char *out = NULL;
            if      (emit_js) out = transpile_js(program, filename);
            else if (emit_c)  out = transpile_c(program, filename);
            if (out) { printf("%s", out); free(out); }
        } else if (emit_wasm) {
            transpile_wasm(program, filename, "out.wasm");
        }
        node_free(program);
        free(src_for_cache);
        cache_free(g_sema_cache);
        return 0;
    }
#endif

#ifdef XSC_ENABLE_VM
    if ((do_vm || emit_bytecode
#ifdef XSC_ENABLE_JIT
        || do_jit
#endif
        ) && program_uses_plugin_runtime_hook(program)) {
        /* AST-level runtime hooks require the tree-walk interpreter. Fall
           back silently so the program still runs. */
        do_vm = 0;
#ifdef XSC_ENABLE_JIT
        do_jit = 0;
#endif
        emit_bytecode = 0;
    }
    if (do_vm || emit_bytecode
#ifdef XSC_ENABLE_JIT
        || do_jit
#endif
    ) {
        {
            int has_plugins_vm = program_has_plugin_use(program);
            int cached_errors = 0;
            int sema_errors = 0;
            if (cache_lookup(g_sema_cache, filename, src_for_cache, &cached_errors)) {
                sema_errors = cached_errors;
                free(src_for_cache); src_for_cache = NULL;
            } else {
                DiagContext *dctx = diag_context_new();
                diag_context_add_source(dctx, filename, src_for_cache);
                if (lenient || has_plugins_vm) diag_context_set_lenient(dctx, 1);
                SemaCtx sema;
                sema_init(&sema, lenient || has_plugins_vm, strict);
                sema.diag = dctx;
                sema_errors = sema_analyze(&sema, program, filename);
                diag_render_all(dctx);
                cache_store(g_sema_cache, filename, src_for_cache, sema_errors);
                free(src_for_cache); src_for_cache = NULL;
                sema_free(&sema);
                diag_context_free(dctx);
            }
            if (sema_errors > 0 && !has_plugins_vm) {
                fprintf(stderr, "\n%d error%s found.\n", sema_errors, sema_errors > 1 ? "s" : "");
                node_free(program);
                cache_free(g_sema_cache);
                return 1;
            }
            if (do_check) {
                printf("No errors found in %s\n", filename);
                node_free(program);
                free(src_for_cache);
                cache_free(g_sema_cache);
                return 0;
            }
        }
        XSProto *proto = compile_program(program);
        if (emit_bytecode) {
            proto_dump(proto);
            proto_free(proto);
            node_free(program);
            free(src_for_cache);
            cache_free(g_sema_cache);
            return 0;
        }
        if (emit_jit_ir) {
#ifdef XSC_ENABLE_JIT
            jit_dump_ir(proto);
            proto_free(proto);
            node_free(program);
            free(src_for_cache);
            cache_free(g_sema_cache);
            return 0;
#else
            fprintf(stderr, "xs --emit jit-ir: requires XSC_ENABLE_JIT\n");
            proto_free(proto);
            node_free(program);
            free(src_for_cache);
            cache_free(g_sema_cache);
            return 1;
#endif
        }
#ifdef XSC_ENABLE_JIT
        if (do_jit) {
            /* In-process machine-code JIT.
             *
             * jit_compile returns a function `int(*)(VM*)` whose body
             * is real x86-64 emitted into mmap(PROT_EXEC) memory. The
             * body drives one opcode per native call (via vm_step) so
             * output is byte-identical with --vm. The same mmap'd
             * function is reused for the program's lifetime.
             *
             * Future tiers (per-opcode inlining, hot-function recompile)
             * stack on top of this entry without changing the contract:
             * a native function pointer that takes the VM and returns
             * an exit code. */
            XSJIT *jit = jit_new();
            if (jit && jit->available) {
                int (*fn)(VM *) = (int (*)(VM *))jit_compile(jit, proto);
                if (fn) {
                    VM *vm = vm_new();
                    int jit_rc = vm_run_with(vm, proto, fn);
                    trigger_fire_on_start(NULL);
                    trigger_run_event_loop(NULL);
                    trigger_fire_on_exit(NULL);
                    vm_drain_tasks();
                    vm_free(vm);
                    jit_free(jit);
                    proto_free(proto);
                    node_free(program);
                    free(src_for_cache);
                    cache_free(g_sema_cache);
                    if (jit_rc == 0 && g_xs_runtime_error_count > 0) jit_rc = 1;
                    return jit_rc;
                }
                jit_free(jit);
                fprintf(stderr, "xs: --jit: native code emit failed, falling back to VM\n");
            } else {
                if (jit) jit_free(jit);
                fprintf(stderr, "xs: --jit: native JIT unavailable on this build, falling back to VM\n");
            }
        }
#endif
        VM *vm = vm_new();
        int rc = vm_run(vm, proto);
        trigger_fire_on_start(NULL);
        trigger_run_event_loop(NULL);
        trigger_fire_on_exit(NULL);
        /* Drain unjoined spawn workers before freeing the parent VM,
           otherwise their borrowed globals pointer becomes dangling. */
        vm_drain_tasks();
        vm_free(vm);
        proto_free(proto);
        node_free(program);
        free(src_for_cache);
        cache_free(g_sema_cache);
        /* Any xs_runtime_error() call during execution must surface as a
           non-zero exit even if the VM chose to continue with a null. */
        if (rc == 0 && g_xs_runtime_error_count > 0) rc = 1;
        if (rc != 0 && getenv("XS_VERBOSE_EXIT")) {
            fprintf(stderr, "xs: exit %d (runtime_errors=%d)\n",
                    rc, g_xs_runtime_error_count);
        }
        return rc;
    }
#endif

    /* sema -> run (or just sema for --check) */
    {
        int has_plugins = program_has_plugin_use(program);
        int cached_errors = 0;
        int sema_errors = 0;
        if (cache_lookup(g_sema_cache, filename, src_for_cache, &cached_errors)) {
            sema_errors = cached_errors;
            free(src_for_cache);
        } else {
            DiagContext *dctx = diag_context_new();
            diag_context_add_source(dctx, filename, src_for_cache);
            if (lenient || has_plugins) diag_context_set_lenient(dctx, 1);
            SemaCtx sema;
            sema_init(&sema, lenient || has_plugins, strict);
            sema.diag = dctx;
            sema_errors = sema_analyze(&sema, program, filename);
            diag_render_all(dctx);
            cache_store(g_sema_cache, filename, src_for_cache, sema_errors);
            free(src_for_cache);
            sema_free(&sema);
            diag_context_free(dctx);
        }
        if (sema_errors > 0 && !has_plugins) {
            fprintf(stderr, "\n%d error%s found.\n", sema_errors, sema_errors > 1 ? "s" : "");
            node_free(program);
            cache_free(g_sema_cache);
            return 1;
        }
        /* HM type inference: runs after sema to catch additional type errors
           in unannotated code (e.g. passing wrong types through variables) */
        if (do_check || strict) {
            InferResult *ir = infer_types(program);
            int infer_errs = infer_error_count(ir);
            int printed = 0;
            for (int ie = 0; ie < infer_errs; ie++) {
                const char *msg = infer_error_at(ir, ie);
                if (!msg) continue;
                /* skip false positives from implicit returns and int/unit mismatches */
                if (strstr(msg, "with unit") || strstr(msg, "with ?") ||
                    strstr(msg, "i64 with int") || strstr(msg, "int with i64") ||
                    strstr(msg, "f64 with float") || strstr(msg, "float with f64") ||
                    strstr(msg, "str with string") || strstr(msg, "string with str"))
                    continue;
                if (!printed) { fprintf(stderr, "\n"); printed = 1; }
                fprintf(stderr, "  %sinference: %s\n",
                        strict ? "error: " : "warning: ", msg);
            }
            if (printed && strict) {
                fprintf(stderr, "\n");
                infer_result_free(ir);
                node_free(program);
                cache_free(g_sema_cache);
                return 1;
            }
            infer_result_free(ir);
        }
        if (do_check) {
            printf("No errors found in %s\n", filename);
            node_free(program);
            cache_free(g_sema_cache);
            return 0;
        }
    }

    if (!do_check) {
#ifdef XSC_ENABLE_PROFILER
        XSProfiler *prof = do_profile ? profiler_new() : NULL;
#ifdef XSC_ENABLE_TRACER
        if (prof && trace_sample > 0.0) profiler_set_sample_rate(prof, trace_sample);
#endif
        if (prof) profiler_start(prof);
#endif
#ifdef XSC_ENABLE_COVERAGE
        XSCoverage *cov = do_coverage ? coverage_new(filename) : NULL;
        if (cov) cov_register_node(cov, program);
#endif
        char *err_src = read_file(filename);
        if (err_src) xs_error_set_source(err_src);

        Interp *interp = interp_new(filename);
        interp->source = err_src; /* keep source for plugin re-parse */
        DiagContext *dctx = diag_context_new();
        if (err_src) diag_context_add_source(dctx, filename, err_src);
        interp->diag = dctx;
#ifdef XSC_ENABLE_TRACER
        XSTracer *tracer = record_path ? tracer_new(record_path) : NULL;
        if (tracer && trace_deep) tracer_set_deep(tracer, 1);
        interp->tracer = tracer;
        interp_setup_tracer_suppress(interp);
#endif
#ifdef XSC_ENABLE_COVERAGE
        interp->coverage = cov;
#endif
        {
            int nargs = argc - file_arg - 1;
            Value *argv_val = xs_array_new();
            for (int i = 0; i < nargs; i++) {
                Value *sv = xs_str(argv[file_arg + 1 + i]);
                array_push(argv_val->arr, sv);
                value_decref(sv);
            }
            env_define(interp->globals, "argv", argv_val, 0);
            value_decref(argv_val);
        }
#ifdef XSC_ENABLE_PLUGINS
        if (plugin_path) {
#ifdef _WIN32
            void *dl = (void *)LoadLibraryA(plugin_path);
#else
            void *dl = dlopen(plugin_path, RTLD_NOW);
#endif
            if (!dl) {
#ifdef _WIN32
                fprintf(stderr, "xs: failed to load plugin '%s'\n", plugin_path);
#else
                fprintf(stderr, "xs: failed to load plugin '%s': %s\n",
                        plugin_path, dlerror());
#endif
                interp_free(interp);
                node_free(program);
                cache_free(g_sema_cache);
                return 1;
            }
#ifdef _WIN32
            xs_plugin_init_fn init_fn =
                (xs_plugin_init_fn)(void *)GetProcAddress((HMODULE)dl, "xs_plugin_init");
#else
            xs_plugin_init_fn init_fn =
                (xs_plugin_init_fn)dlsym(dl, "xs_plugin_init");
#endif
            if (!init_fn) {
                fprintf(stderr, "xs: plugin '%s' missing xs_plugin_init symbol\n",
                        plugin_path);
#ifdef _WIN32
                FreeLibrary((HMODULE)dl);
#else
                dlclose(dl);
#endif
                interp_free(interp);
                node_free(program);
                cache_free(g_sema_cache);
                return 1;
            }
            int prc = init_fn(interp, XS_PLUGIN_API_VERSION);
            if (prc != 0) {
                fprintf(stderr, "xs: plugin '%s' init returned error %d\n",
                        plugin_path, prc);
#ifdef _WIN32
                FreeLibrary((HMODULE)dl);
#else
                dlclose(dl);
#endif
                interp_free(interp);
                node_free(program);
                cache_free(g_sema_cache);
                return 1;
            }
            /* Note: we intentionally do NOT dlclose here so the plugin stays loaded */
        }
#endif
        interp_run(interp, program);
#ifdef XSC_ENABLE_PLUGINS
        /* run custom passes after program execution.
           closure environments survive because env refcounting keeps them alive
           even after the temp plugin interpreter is freed. the callbacks are
           called through the main interpreter so native fns like
           plugin.runtime.global.set work correctly via s_plugin_host_globals. */
        if (interp->pipeline && !interp->cf.signal) {
            PluginPipeline *pp = (PluginPipeline *)interp->pipeline;
            pipeline_run_passes(pp, program, "parser", 0, interp);
            pipeline_run_passes(pp, program, "parser", 1, interp);
            pipeline_run_passes(pp, program, "sema", 0, interp);
            pipeline_run_passes(pp, program, "sema", 1, interp);
            pipeline_dispatch_sema(pp, program, interp);
        }
#endif
        /* Drain unjoined spawn workers before tearing the interp down. */
        xs_drain_interp_tasks();
        int had_error = (interp->cf.signal == CF_ERROR || interp->cf.signal == CF_PANIC)
                        || interp->had_unhandled_exception
                        || g_xs_runtime_error_count > 0;
        if (had_error && getenv("XS_VERBOSE_EXIT")) {
            fprintf(stderr,
                    "xs: exit 1 (cf.signal=%d unhandled=%d runtime_errors=%d)\n",
                    interp->cf.signal, interp->had_unhandled_exception,
                    g_xs_runtime_error_count);
        }
        interp_free(interp);
        diag_context_free(dctx);
        xs_error_set_source(NULL);
        free(err_src);
#ifdef XSC_ENABLE_TRACER
        if (tracer) { tracer_flush(tracer); tracer_free(tracer); }
#endif
#ifdef XSC_ENABLE_PROFILER
        if (prof) { profiler_stop(prof); profiler_report(prof); profiler_free(prof); }
#endif
#ifdef XSC_ENABLE_COVERAGE
        if (cov) { coverage_report(cov); coverage_free(cov); }
#endif
        node_free(program);
        return had_error ? 1 : 0;
    }

    node_free(program);
    cache_free(g_sema_cache);
    return 0;
}
