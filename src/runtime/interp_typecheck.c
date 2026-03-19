/* interp_typecheck.c -- runtime type-annotation checking helpers.
   value_matches_type / value_matches_typeexpr drive the runtime
   enforcement of `let x: int = ...` style annotations and the
   `--strict` mode. typeexpr_str / value_type_str produce the
   strings used in TypeMismatch error messages. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "core/value.h"
#include "core/ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int value_matches_type(Value *v, const char *type_name) {
    if (!type_name || !v) return 1;
    if (strcmp(type_name, "any") == 0) return 1;
    if (strcmp(type_name, "int") == 0 || strcmp(type_name, "i64") == 0)
        return VAL_TAG(v) == XS_INT;
    if (strcmp(type_name, "float") == 0 || strcmp(type_name, "f64") == 0)
        return VAL_TAG(v) == XS_FLOAT;
    if (strcmp(type_name, "str") == 0 || strcmp(type_name, "string") == 0)
        return VAL_TAG(v) == XS_STR;
    if (strcmp(type_name, "bool") == 0)
        return VAL_TAG(v) == XS_BOOL;
    if (strcmp(type_name, "array") == 0)
        return VAL_TAG(v) == XS_ARRAY;
    if (strcmp(type_name, "map") == 0)
        return VAL_TAG(v) == XS_MAP;
    if (strcmp(type_name, "null") == 0)
        return VAL_TAG(v) == XS_NULL;
    if (strcmp(type_name, "fn") == 0 || strcmp(type_name, "function") == 0)
        return VAL_TAG(v) == XS_FUNC || VAL_TAG(v) == XS_NATIVE;
    if (strcmp(type_name, "tuple") == 0)
        return VAL_TAG(v) == XS_TUPLE;
    if (VAL_TAG(v) == XS_STRUCT_VAL && v->st)
        return strcmp(v->st->type_name, type_name) == 0;
    if (VAL_TAG(v) == XS_INST && v->inst && v->inst->class_)
        return strcmp(v->inst->class_->name, type_name) == 0;
    if (VAL_TAG(v) == XS_ENUM_VAL && v->en)
        return strcmp(v->en->type_name, type_name) == 0;
    return 1; /* unknown type = pass */
}

int value_matches_typeexpr(Value *v, TypeExpr *te) {
    if (!te || !v) return 1;

    switch (te->kind) {
    case TEXPR_NAMED:
        if (!value_matches_type(v, te->name)) return 0;
        if (te->name && strcmp(te->name, "map") == 0 && te->nargs >= 2 &&
            VAL_TAG(v) == XS_MAP && v->map) {
            int nk = 0;
            char **keys = map_keys(v->map, &nk);
            for (int j = 0; j < nk; j++) {
                Value *kv = xs_str(keys[j]);
                if (!value_matches_typeexpr(kv, te->args[0])) {
                    value_decref(kv); free(keys[j]);
                    for (int k = j+1; k < nk; k++) free(keys[k]);
                    free(keys); return 0;
                }
                value_decref(kv);
                Value *val = map_get(v->map, keys[j]);
                if (val && !value_matches_typeexpr(val, te->args[1])) {
                    for (int k = j; k < nk; k++) free(keys[k]);
                    free(keys); return 0;
                }
                free(keys[j]);
            }
            free(keys);
        }
        return 1;

    case TEXPR_ARRAY:
        if (VAL_TAG(v) != XS_ARRAY) return 0;
        if (te->inner && v->arr) {
            for (int j = 0; j < v->arr->len; j++) {
                if (!value_matches_typeexpr(v->arr->items[j], te->inner))
                    return 0;
            }
        }
        return 1;

    case TEXPR_TUPLE:
        if (VAL_TAG(v) != XS_TUPLE) return 0;
        if (v->arr && te->nelems > 0) {
            if (v->arr->len != te->nelems) return 0;
            for (int j = 0; j < te->nelems; j++) {
                if (!value_matches_typeexpr(v->arr->items[j], te->elems[j]))
                    return 0;
            }
        }
        return 1;

    case TEXPR_FN:
        return VAL_TAG(v) == XS_FUNC || VAL_TAG(v) == XS_NATIVE || VAL_TAG(v) == XS_CLOSURE;

    case TEXPR_OPTION:
        if (VAL_TAG(v) == XS_NULL) return 1;
        return value_matches_typeexpr(v, te->inner);

    case TEXPR_INFER:
        return 1;

    default:
        return 1;
    }
}

const char *typeexpr_str(TypeExpr *te) {
    static char buf[256];
    if (!te) return "?";
    switch (te->kind) {
    case TEXPR_NAMED:
        if (te->nargs > 0) {
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s<", te->name ? te->name : "?");
            for (int j = 0; j < te->nargs; j++) {
                if (j) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", typeexpr_str(te->args[j]));
            }
            snprintf(buf + pos, sizeof(buf) - pos, ">");
            return buf;
        }
        return te->name ? te->name : "?";
    case TEXPR_ARRAY:
        snprintf(buf, sizeof buf, "[%s]", te->inner ? typeexpr_str(te->inner) : "?");
        return buf;
    case TEXPR_TUPLE: {
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "(");
        for (int j = 0; j < te->nelems; j++) {
            if (j) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", typeexpr_str(te->elems[j]));
        }
        snprintf(buf + pos, sizeof(buf) - pos, ")");
        return buf;
    }
    case TEXPR_OPTION:
        snprintf(buf, sizeof buf, "%s?", te->inner ? typeexpr_str(te->inner) : "?");
        return buf;
    case TEXPR_FN:
        return "fn";
    case TEXPR_INFER:
        return "_";
    default:
        return "?";
    }
}

const char *value_type_str(Value *v) {
    static char tbuf[256];
    if (!v) return "null";
    switch (VAL_TAG(v)) {
        case XS_INT:   return "int";
        case XS_FLOAT: return "float";
        case XS_STR:   return "str";
        case XS_BOOL:  return "bool";
        case XS_ARRAY: {
            if (!v->arr || v->arr->len == 0) return "[?]";
            const char *first = value_type_str(v->arr->items[0]);
            int all_same = 1;
            for (int j = 1; j < v->arr->len; j++) {
                if (strcmp(value_type_str(v->arr->items[j]), first) != 0) {
                    all_same = 0; break;
                }
            }
            if (all_same) snprintf(tbuf, sizeof tbuf, "[%s]", first);
            else snprintf(tbuf, sizeof tbuf, "[mixed]");
            return tbuf;
        }
        case XS_TUPLE: {
            if (!v->arr || v->arr->len == 0) return "()";
            int pos = 0;
            pos += snprintf(tbuf + pos, sizeof(tbuf) - pos, "(");
            for (int j = 0; j < v->arr->len && pos < 240; j++) {
                if (j) pos += snprintf(tbuf + pos, sizeof(tbuf) - pos, ", ");
                pos += snprintf(tbuf + pos, sizeof(tbuf) - pos, "%s", value_type_str(v->arr->items[j]));
            }
            snprintf(tbuf + pos, sizeof(tbuf) - pos, ")");
            return tbuf;
        }
        case XS_MAP: {
            if (!v->map || v->map->len == 0) return "map<?, ?>";
            int nk = 0;
            char **keys = map_keys(v->map, &nk);
            if (nk == 0) { free(keys); return "map<?, ?>"; }
            Value *first_val = map_get(v->map, keys[0]);
            const char *vtype = first_val ? value_type_str(first_val) : "?";
            int all_same = 1;
            for (int j = 1; j < nk; j++) {
                Value *vv = map_get(v->map, keys[j]);
                if (vv && strcmp(value_type_str(vv), vtype) != 0) {
                    all_same = 0; break;
                }
            }
            for (int j = 0; j < nk; j++) free(keys[j]);
            free(keys);
            if (all_same) snprintf(tbuf, sizeof tbuf, "map<str, %s>", vtype);
            else snprintf(tbuf, sizeof tbuf, "map<str, mixed>");
            return tbuf;
        }
        case XS_FUNC: case XS_NATIVE: case XS_OVERLOAD: return "fn";
        case XS_STRUCT_VAL: return v->st ? v->st->type_name : "struct";
        case XS_INST:  return (v->inst && v->inst->class_) ? v->inst->class_->name : "instance";
        case XS_ENUM_VAL: return v->en ? v->en->type_name : "enum";
        default: return "unknown";
    }
}
