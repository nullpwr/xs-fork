#define _POSIX_C_SOURCE 200809L
#include "tracer/replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int use_colors = 0;

#define C_BOLD  "\033[1m"
#define C_CYAN  "\033[36m"
#define C_YEL   "\033[33m"
#define C_GRN   "\033[32m"
#define C_RST   "\033[0m"

static const char *c_bold(void) { return use_colors ? C_BOLD : ""; }
static const char *c_cyan(void) { return use_colors ? C_CYAN : ""; }
static const char *c_yel(void)  { return use_colors ? C_YEL  : ""; }
static const char *c_grn(void)  { return use_colors ? C_GRN  : ""; }
static const char *c_rst(void)  { return use_colors ? C_RST  : ""; }

/* --- minimal JSON helpers for well-structured provenance objects --- */

/* skip whitespace */
static const char *json_skip(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* find value for a key in a JSON object; returns pointer to start of value */
static const char *json_find_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    json = json_skip(json);
    if (*json != '{') return NULL;
    json++;
    int depth = 0;
    while (*json) {
        json = json_skip(json);
        if (*json == '}') return NULL;
        if (*json == ',') { json++; continue; }
        /* expect a quoted key */
        if (*json != '"') return NULL;
        json++;
        const char *ks = json;
        while (*json && *json != '"') json++;
        int klen = (int)(json - ks);
        if (*json == '"') json++;
        json = json_skip(json);
        if (*json == ':') json++;
        json = json_skip(json);
        int keylen = (int)strlen(key);
        if (klen == keylen && strncmp(ks, key, (size_t)klen) == 0 && depth == 0)
            return json;
        /* skip value */
        if (*json == '"') {
            json++;
            while (*json && !(*json == '"' && *(json-1) != '\\')) json++;
            if (*json == '"') json++;
        } else if (*json == '{') {
            int d = 1;
            json++;
            while (*json && d > 0) {
                if (*json == '{') d++;
                else if (*json == '}') d--;
                else if (*json == '"') {
                    json++;
                    while (*json && !(*json == '"' && *(json-1) != '\\')) json++;
                }
                if (d > 0) json++;
            }
            if (*json == '}') json++;
        } else {
            /* number, bool, null */
            while (*json && *json != ',' && *json != '}') json++;
        }
        if (*json == '}') return NULL;
    }
    return NULL;
}

static const char *json_find_str(const char *json, const char *key, char *buf, int bufsize) {
    const char *v = json_find_key(json, key);
    if (!v || *v != '"') return NULL;
    v++;
    int i = 0;
    while (*v && *v != '"' && i < bufsize - 1) {
        if (*v == '\\' && *(v+1)) { v++; }
        buf[i++] = *v++;
    }
    buf[i] = '\0';
    return buf;
}

static int json_find_int(const char *json, const char *key, int *out) {
    const char *v = json_find_key(json, key);
    if (!v) return 0;
    if (*v == '-' || (*v >= '0' && *v <= '9')) {
        *out = atoi(v);
        return 1;
    }
    return 0;
}

/* find a nested object; copies it into buf and returns buf, or NULL */
static const char *json_find_obj(const char *json, const char *key, char *buf, int bufsize) {
    const char *v = json_find_key(json, key);
    if (!v || *v != '{') return NULL;
    int d = 1;
    const char *start = v;
    v++;
    while (*v && d > 0) {
        if (*v == '{') d++;
        else if (*v == '}') d--;
        else if (*v == '"') {
            v++;
            while (*v && !(*v == '"' && *(v-1) != '\\')) v++;
        }
        v++;
    }
    int len = (int)(v - start);
    if (len >= bufsize) len = bufsize - 1;
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* --- tree printing --- */

/* prefix_stack tracks whether each depth level has more siblings coming */
static void print_prov_node(const char *json, const char *prefix, int is_last, int is_root, const char *root_var);

static void print_child_nodes(const char *json, const char *prefix) {
    char parent_buf[2048], parent2_buf[2048], chain_buf[2048];
    const char *parent = json_find_obj(json, "parent", parent_buf, sizeof(parent_buf));
    const char *parent2 = json_find_obj(json, "parent2", parent2_buf, sizeof(parent2_buf));
    const char *chain = json_find_obj(json, "return_chain", chain_buf, sizeof(chain_buf));

    /* count children for is_last tracking */
    int n_children = 0;
    if (parent) n_children++;
    if (parent2) n_children++;
    if (chain) n_children++;

    int child_idx = 0;

    if (chain) {
        child_idx++;
        print_prov_node(chain, prefix, child_idx == n_children, 0, NULL);
    }

    if (parent) {
        child_idx++;
        print_prov_node(parent, prefix, child_idx == n_children && !parent2, 0, NULL);
    }

    if (parent2) {
        child_idx++;
        print_prov_node(parent2, prefix, 1, 0, NULL);
    }
}

static void print_prov_node(const char *json, const char *prefix, int is_last, int is_root, const char *root_var) {
    if (!json) return;

    char origin[64] = {0}, detail[128] = {0};
    int value = 0, line = 0;
    int has_value = 0, has_line = 0;

    json_find_str(json, "origin", origin, sizeof(origin));
    json_find_str(json, "detail", detail, sizeof(detail));
    has_value = json_find_int(json, "value", &value);
    has_line = json_find_int(json, "line", &line);

    /* build the new prefix for children */
    char new_prefix[512];
    if (is_root) {
        /* root node: show "var = value (line N)" header */
        if (root_var) {
            printf("%s%s%s = %s%d%s", c_bold(), root_var, c_rst(), c_grn(), value, c_rst());
        } else if (has_value) {
            printf("%s%s%s = %s%d%s", c_bold(), detail, c_rst(), c_grn(), value, c_rst());
        }
        if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
        printf("\n");

        /* print origin as first indented line */
        printf("%s  %s%s%s", prefix, c_cyan(), origin, c_rst());
        if (detail[0]) printf(": %s%s%s", c_bold(), detail, c_rst());
        printf("\n");

        snprintf(new_prefix, sizeof(new_prefix), "%s  ", prefix);
        print_child_nodes(json, new_prefix);
        return;
    }

    /* non-root: tree connector */
    const char *connector = is_last ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ";
    const char *extension = is_last ? "   " : "\xe2\x94\x82  ";

    printf("%s%s", prefix, connector);

    /* format based on origin type */
    if (strcmp(origin, "return") == 0 || strcmp(origin, "fn_return") == 0) {
        printf("%s%s%s: %s%s%s", c_cyan(), origin, c_rst(), c_bold(), detail, c_rst());
        if (has_value) printf(" = %s%d%s", c_grn(), value, c_rst());
        if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
    } else if (strcmp(origin, "literal") == 0) {
        printf("%s%s%s: %s%s%s", c_cyan(), origin, c_rst(), c_bold(), detail, c_rst());
        if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
    } else if (strcmp(origin, "variable") == 0 || strcmp(origin, "param") == 0) {
        printf("%s%s%s: %s%s%s", c_cyan(), origin, c_rst(), c_bold(), detail, c_rst());
        if (has_value) printf(" = %s%d%s", c_grn(), value, c_rst());
        if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
    } else {
        /* binop, unop, etc */
        printf("%s%s%s: %s%s%s", c_cyan(), origin, c_rst(), c_bold(), detail, c_rst());
        if (has_value) printf(" = %s%d%s", c_grn(), value, c_rst());
        if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
    }
    printf("\n");

    snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, extension);
    print_child_nodes(json, new_prefix);
}

static void print_rich_provenance(const char *json, const char *indent, const char *var) {
    if (!json) return;
    print_prov_node(json, indent, 1, 1, var);
}

/* print rich provenance tree for STORE context (indented under store line) */
static void print_rich_prov_inline(const char *json, const char *indent) {
    if (!json) return;

    char origin[64] = {0}, detail[128] = {0};
    int line = 0;
    int has_line = 0;

    json_find_str(json, "origin", origin, sizeof(origin));
    json_find_str(json, "detail", detail, sizeof(detail));
    has_line = json_find_int(json, "line", &line);

    printf("%s%s%s%s: %s%s%s", indent, c_cyan(), origin, c_rst(), c_bold(), detail, c_rst());
    if (has_line) printf(" %s(line %d)%s", c_yel(), line, c_rst());
    printf("\n");

    char new_prefix[512];
    snprintf(new_prefix, sizeof(new_prefix), "%s", indent);
    print_child_nodes(json, new_prefix);
}

typedef enum {
    TRACE_CALL   = 0,
    TRACE_RETURN = 1,
    TRACE_STORE  = 2,
    TRACE_IO     = 3,
    TRACE_BRANCH     = 4,
    TRACE_PROVENANCE = 5
} TraceEventKind;

typedef struct {
    TraceEventKind kind;
    int64_t timestamp;
    union {
        struct { char *fn; int line; } call;
        struct { char *fn; char *deep_json; } ret;
        struct { char *var; int64_t ival; double fval; char *sval; int tag; char *deep_json; } store;
        struct { char *op; int len; } io;
        struct { int line; int taken; } branch;
        struct { char *var; char *origin; char *detail; int line; } prov;
    };
} ReplayEvent;

struct XSReplay {
    char *trace_path;
    ReplayEvent *events;
    int n_events;
    int position;
};

static char *read_string(FILE *f) {
    uint32_t len = 0;
    if (fread(&len, 4, 1, f) != 1) return NULL;
    if (len == 0) return NULL;
    char *s = malloc((size_t)len + 1);
    if (!s) return NULL;
    if (fread(s, 1, len, f) != len) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}

static void print_store_value(ReplayEvent *e) {
    if (e->store.deep_json) {
        printf("%s", e->store.deep_json);
    } else if (e->store.sval) {
        printf("\"%s\"", e->store.sval);
    } else if (e->store.fval != 0.0) {
        printf("%g", e->store.fval);
    } else {
        printf("%lld", (long long)e->store.ival);
    }
}

static void print_event_ctx(int index, ReplayEvent *events, int n_events) {
    ReplayEvent *e = &events[index];
    printf("[%d] ", index);
    switch (e->kind) {
    case TRACE_CALL:
        printf("CALL %s line %d", e->call.fn ? e->call.fn : "?", e->call.line);
        break;
    case TRACE_RETURN:
        printf("RETURN %s", e->ret.fn ? e->ret.fn : "?");
        if (e->ret.deep_json) printf(" => %s", e->ret.deep_json);
        break;
    case TRACE_STORE:
        printf("STORE %s = ", e->store.var ? e->store.var : "?");
        print_store_value(e);
        /* look ahead for provenance on same var */
        if (index + 1 < n_events && events[index + 1].kind == TRACE_PROVENANCE &&
            e->store.var && events[index + 1].prov.var &&
            strcmp(e->store.var, events[index + 1].prov.var) == 0) {
            ReplayEvent *p = &events[index + 1];
            if (p->prov.origin && strcmp(p->prov.origin, "plugin") == 0) {
                printf("\n");
                print_rich_prov_inline(p->prov.detail, "    ");
            } else {
                printf("  (from: %s", p->prov.detail ? p->prov.detail : "?");
                printf(", %s at line %d)", p->prov.origin ? p->prov.origin : "?", p->prov.line);
            }
        }
        break;
    case TRACE_IO:
        printf("IO %s (%d bytes)", e->io.op ? e->io.op : "?", e->io.len);
        break;
    case TRACE_BRANCH:
        printf("BRANCH line %d %s", e->branch.line, e->branch.taken ? "taken" : "not taken");
        break;
    case TRACE_PROVENANCE:
        if (e->prov.origin && strcmp(e->prov.origin, "plugin") == 0) {
            printf("PROVENANCE ");
            print_rich_provenance(e->prov.detail,
                                 "      ",
                                 e->prov.var ? e->prov.var : "?");
            return; /* tree already printed newlines */
        } else {
            printf("PROVENANCE %s from %s %s at line %d",
                   e->prov.var ? e->prov.var : "?",
                   e->prov.origin ? e->prov.origin : "?",
                   e->prov.detail ? e->prov.detail : "",
                   e->prov.line);
        }
        break;
    }
    printf("\n");
}

XSReplay *replay_new(const char *trace_path) {
    if (!trace_path) return NULL;

    FILE *f = fopen(trace_path, "rb");
    if (!f) {
        fprintf(stderr, "xs replay: cannot open '%s'\n", trace_path);
        return NULL;
    }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "XST1", 4) != 0) {
        fprintf(stderr, "xs replay: '%s' is not a valid .xst trace file\n", trace_path);
        fclose(f);
        return NULL;
    }

    uint32_t version = 0;
    if (fread(&version, 4, 1, f) != 1 || version != 1) {
        fprintf(stderr, "xs replay: unsupported trace version %u\n", version);
        fclose(f);
        return NULL;
    }

    uint32_t count = 0;
    if (fread(&count, 4, 1, f) != 1) {
        fprintf(stderr, "xs replay: truncated header\n");
        fclose(f);
        return NULL;
    }

    XSReplay *r = calloc(1, sizeof(XSReplay));
    if (!r) { fclose(f); return NULL; }

    r->trace_path = strdup(trace_path);
    r->n_events = (int)count;
    r->events = calloc((size_t)count, sizeof(ReplayEvent));
    r->position = 0;

    if (!r->events) {
        free(r->trace_path);
        free(r);
        fclose(f);
        return NULL;
    }

    for (int i = 0; i < r->n_events; i++) {
        ReplayEvent *e = &r->events[i];
        uint8_t kind = 0;
        if (fread(&kind, 1, 1, f) != 1) { r->n_events = i; break; }
        e->kind = (TraceEventKind)kind;
        if (fread(&e->timestamp, 8, 1, f) != 1) { r->n_events = i; break; }

        switch (e->kind) {
        case TRACE_CALL:
            e->call.fn = read_string(f);
            if (fread(&e->call.line, 4, 1, f) != 1) { r->n_events = i; break; }
            break;
        case TRACE_RETURN: {
            e->ret.fn = read_string(f);
            uint8_t rtag = 0;
            if (fread(&rtag, 1, 1, f) != 1) { r->n_events = i; break; }
            int64_t skip_ival; double skip_fval;
            if (fread(&skip_ival, 8, 1, f) != 1) { r->n_events = i; break; }
            if (fread(&skip_fval, 8, 1, f) != 1) { r->n_events = i; break; }
            char *skip_sval = read_string(f); free(skip_sval);
            e->ret.deep_json = read_string(f);
            break;
        }
        case TRACE_STORE: {
            e->store.var = read_string(f);
            uint8_t tag = 0;
            if (fread(&tag, 1, 1, f) != 1) { r->n_events = i; break; }
            e->store.tag = tag;
            if (fread(&e->store.ival, 8, 1, f) != 1) { r->n_events = i; break; }
            if (fread(&e->store.fval, 8, 1, f) != 1) { r->n_events = i; break; }
            e->store.sval = read_string(f);
            e->store.deep_json = read_string(f);
            break;
        }
        case TRACE_IO:
            e->io.op = read_string(f);
            if (fread(&e->io.len, 4, 1, f) != 1) { r->n_events = i; break; }
            break;
        case TRACE_BRANCH:
            if (fread(&e->branch.line, 4, 1, f) != 1) { r->n_events = i; break; }
            if (fread(&e->branch.taken, 4, 1, f) != 1) { r->n_events = i; break; }
            break;
        case TRACE_PROVENANCE:
            e->prov.var = read_string(f);
            e->prov.origin = read_string(f);
            e->prov.detail = read_string(f);
            if (fread(&e->prov.line, 4, 1, f) != 1) { r->n_events = i; break; }
            break;
        }
    }

    fclose(f);
    return r;
}

void replay_free(XSReplay *r) {
    if (!r) return;
    for (int i = 0; i < r->n_events; i++) {
        ReplayEvent *e = &r->events[i];
        switch (e->kind) {
        case TRACE_CALL:   free(e->call.fn); break;
        case TRACE_RETURN: free(e->ret.fn); free(e->ret.deep_json); break;
        case TRACE_STORE:  free(e->store.var); free(e->store.sval); free(e->store.deep_json); break;
        case TRACE_IO:     free(e->io.op); break;
        case TRACE_BRANCH: break;
        case TRACE_PROVENANCE: free(e->prov.var); free(e->prov.origin); free(e->prov.detail); break;
        }
    }
    free(r->events);
    free(r->trace_path);
    free(r);
}

int replay_step_forward(XSReplay *r) {
    if (!r || r->position >= r->n_events) return 0;
    print_event_ctx(r->position, r->events, r->n_events);
    r->position++;
    return (r->position < r->n_events) ? 1 : 0;
}

int replay_step_backward(XSReplay *r) {
    if (!r || r->position <= 0) return 0;
    r->position--;
    print_event_ctx(r->position, r->events, r->n_events);
    return 1;
}

int replay_run(const char *trace_path) {
    XSReplay *r = replay_new(trace_path);
    if (!r) return 1;

    use_colors = isatty(fileno(stdout));

    printf("XS Replay -- %s (%d events)\n", trace_path, r->n_events);
    printf("Commands: n(ext), p(rev), c(ontinue), q(uit), g <n> (goto), w <var> (where)\n\n");

    if (r->n_events > 0) {
        print_event_ctx(r->position, r->events, r->n_events);
    }

    char line[256];
    for (;;) {
        printf("replay> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (len == 0) continue;

        if (line[0] == 'q') {
            break;
        } else if (line[0] == 'n') {
            if (!replay_step_forward(r)) {
                printf("(end of trace)\n");
            }
        } else if (line[0] == 'p') {
            if (!replay_step_backward(r)) {
                printf("(beginning of trace)\n");
            }
        } else if (line[0] == 'c') {
            while (replay_step_forward(r)) {}
            printf("(end of trace)\n");
        } else if (line[0] == 'g') {
            int target = 0;
            if (sscanf(line + 1, "%d", &target) == 1) {
                if (target < 0) target = 0;
                if (target >= r->n_events) target = r->n_events - 1;
                r->position = target;
                print_event_ctx(r->position, r->events, r->n_events);
            } else {
                printf("Usage: g <event_number>\n");
            }
        } else if (line[0] == 'w') {
            /* where command: show provenance for a variable */
            char varname[128] = {0};
            if (sscanf(line + 1, " %127s", varname) != 1) {
                printf("Usage: w <variable_name>\n");
            } else {
                /* search backward from current position for most recent provenance */
                int found = 0;
                for (int j = r->position - 1; j >= 0; j--) {
                    if (r->events[j].kind == TRACE_PROVENANCE &&
                        r->events[j].prov.var &&
                        strcmp(r->events[j].prov.var, varname) == 0) {
                        if (r->events[j].prov.origin &&
                            strcmp(r->events[j].prov.origin, "plugin") == 0) {
                            print_rich_provenance(r->events[j].prov.detail,
                                                  "  ", varname);
                        } else {
                            /* find matching store just before */
                            for (int k = j - 1; k >= 0; k--) {
                                if (r->events[k].kind == TRACE_STORE &&
                                    r->events[k].store.var &&
                                    strcmp(r->events[k].store.var, varname) == 0) {
                                    printf("%s = ", varname);
                                    print_store_value(&r->events[k]);
                                    printf("\n");
                                    break;
                                }
                            }
                            printf("  from %s %s at line %d\n",
                                   r->events[j].prov.origin ? r->events[j].prov.origin : "?",
                                   r->events[j].prov.detail ? r->events[j].prov.detail : "",
                                   r->events[j].prov.line);
                        }
                        found = 1;
                        break;
                    }
                }
                if (!found) printf("no provenance found for '%s'\n", varname);
            }
        } else {
            printf("Unknown command. Use n(ext), p(rev), c(ontinue), q(uit), g <n>, w <var>\n");
        }
    }

    replay_free(r);
    return 0;
}

/* check if event at idx is a rich plugin provenance for the given var */
static int is_rich_prov_for(ReplayEvent *events, int n, int idx, const char *var) {
    if (idx < 0 || idx >= n) return 0;
    ReplayEvent *e = &events[idx];
    if (e->kind != TRACE_PROVENANCE) return 0;
    if (!e->prov.origin || strcmp(e->prov.origin, "plugin") != 0) return 0;
    if (!var || !e->prov.var) return 0;
    return strcmp(e->prov.var, var) == 0;
}

/* check if event at idx is a basic (non-plugin) provenance for the given var */
static int is_basic_prov_for(ReplayEvent *events, int n, int idx, const char *var) {
    if (idx < 0 || idx >= n) return 0;
    ReplayEvent *e = &events[idx];
    if (e->kind != TRACE_PROVENANCE) return 0;
    if (e->prov.origin && strcmp(e->prov.origin, "plugin") == 0) return 0;
    if (!var || !e->prov.var) return 0;
    return strcmp(e->prov.var, var) == 0;
}

static void print_merged_event(int display_idx, ReplayEvent *e, const char *rich_json) {
    printf("[%d] ", display_idx);
    switch (e->kind) {
    case TRACE_CALL:
        printf("CALL %s line %d\n", e->call.fn ? e->call.fn : "?", e->call.line);
        break;
    case TRACE_RETURN:
        printf("RETURN %s", e->ret.fn ? e->ret.fn : "?");
        if (e->ret.deep_json) printf(" => %s", e->ret.deep_json);
        printf("\n");
        break;
    case TRACE_STORE:
        printf("STORE %s = ", e->store.var ? e->store.var : "?");
        print_store_value(e);
        printf("\n");
        if (rich_json) {
            print_rich_prov_inline(rich_json, "    ");
        }
        break;
    case TRACE_IO:
        printf("IO %s (%d bytes)\n", e->io.op ? e->io.op : "?", e->io.len);
        break;
    case TRACE_BRANCH:
        printf("BRANCH line %d %s\n", e->branch.line, e->branch.taken ? "taken" : "not taken");
        break;
    case TRACE_PROVENANCE:
        if (e->prov.origin && strcmp(e->prov.origin, "plugin") == 0) {
            printf("PROVENANCE ");
            print_rich_provenance(e->prov.detail, "      ", e->prov.var ? e->prov.var : "?");
        } else {
            printf("PROVENANCE %s from %s %s at line %d\n",
                   e->prov.var ? e->prov.var : "?",
                   e->prov.origin ? e->prov.origin : "?",
                   e->prov.detail ? e->prov.detail : "",
                   e->prov.line);
        }
        break;
    }
}

int replay_dump(const char *trace_path) {
    XSReplay *r = replay_new(trace_path);
    if (!r) return 1;

    use_colors = isatty(fileno(stdout));

    int display_idx = 0;
    for (int j = 0; j < r->n_events; j++) {
        ReplayEvent *e = &r->events[j];

        /* STORE event: merge with following provenance events */
        if (e->kind == TRACE_STORE && e->store.var) {
            const char *rich_json = NULL;
            int skip = 0;

            /* scan ahead for provenance events belonging to this store */
            int k = j + 1;
            while (k < r->n_events) {
                if (is_basic_prov_for(r->events, r->n_events, k, e->store.var)) {
                    skip++;
                    k++;
                } else if (is_rich_prov_for(r->events, r->n_events, k, e->store.var)) {
                    rich_json = r->events[k].prov.detail;
                    skip++;
                    k++;
                } else {
                    break;
                }
            }
            print_merged_event(display_idx++, e, rich_json);
            j += skip;
            continue;
        }

        /* skip standalone provenance events that are basic (already merged above) */
        /* standalone rich provenance (e.g. for params) still shown */
        if (e->kind == TRACE_PROVENANCE) {
            if (e->prov.origin && strcmp(e->prov.origin, "plugin") == 0) {
                print_merged_event(display_idx++, e, NULL);
            }
            /* skip basic standalone provenance */
            continue;
        }

        print_merged_event(display_idx++, e, NULL);
    }
    printf("(%d events)\n", display_idx);

    replay_free(r);
    return 0;
}
