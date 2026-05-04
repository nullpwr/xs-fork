#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "net/http_server.h"
#include "runtime/concurrent.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#if !defined(_WIN32) && !defined(__wasi__)
#include <unistd.h>
#endif

extern Value *http_do_request(const char *method, const char *url,
                              XSMap *extra_headers, const char *body,
                              size_t body_len);

/* http high-level module */
static Value *native_http_get(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *hdrs=NULL;
    if (n>=2&&VAL_TAG(a[1])==XS_MAP) {
        Value *hv=map_get(a[1]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map;
    }
    return http_do_request("GET",a[0]->s,hdrs,NULL,0);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_post(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("POST",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_put(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("PUT",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_delete(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *hdrs=NULL;
    if (n>=2&&VAL_TAG(a[1])==XS_MAP) {
        Value *hv=map_get(a[1]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map;
    }
    return http_do_request("DELETE",a[0]->s,hdrs,NULL,0);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_patch(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("PATCH",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}

/* http.request(method, url, opts) - full request with options map */
static Value *native_http_request(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *method = a[0]->s;
    const char *url = a[1]->s;
    XSMap *hdrs = NULL;
    const char *body = NULL;
    size_t body_len = 0;
    if (n >= 3 && VAL_TAG(a[2]) == XS_MAP) {
        Value *hv = map_get(a[2]->map, "headers");
        if (hv && VAL_TAG(hv) == XS_MAP) hdrs = hv->map;
        Value *bv = map_get(a[2]->map, "body");
        if (bv && VAL_TAG(bv) == XS_STR) { body = bv->s; body_len = strlen(bv->s); }
    }
    Value *result = http_do_request(method, url, hdrs, body, body_len);
    /* add 'ok' field for convenience */
    if (result && VAL_TAG(result) == XS_MAP) {
        Value *sv = map_get(result->map, "status");
        if (sv && VAL_TAG(sv) == XS_INT) {
            int ok = (VAL_INT(sv) >= 200 && VAL_INT(sv) < 300) ? 1 : 0;
            map_set(result->map, "ok", ok ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
        }
    }
    return result;
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}

/* Match `pattern` against `path` and populate params. Pattern segments
 * starting with ':' capture the corresponding path segment by name.
 * Returns 1 on match, 0 on miss. params is allocated even on miss so
 * the caller's only job is to value_decref it.
 * Only used by native_http_serve below; MinGW + wasi route the call
 * through the stub branch so the helper would otherwise be flagged
 * unused there. */
#if !defined(__MINGW32__) && !defined(__wasi__)
static int route_match(const char *pattern, const char *path, Value **params_out) {
    Value *params = xs_map_new();
    *params_out = params;
    const char *pp = pattern, *qp = path;
    while (*pp || *qp) {
        if (*pp == '/') pp++;
        if (*qp == '/') qp++;
        if (!*pp && !*qp) return 1;
        if (!*pp || !*qp) return 0;
        const char *pe = strchr(pp, '/'); if (!pe) pe = pp + strlen(pp);
        const char *qe = strchr(qp, '/'); if (!qe) qe = qp + strlen(qp);
        size_t plen = (size_t)(pe - pp);
        size_t qlen = (size_t)(qe - qp);
        if (plen > 0 && pp[0] == ':') {
            char name[128]; size_t nlen = plen - 1;
            if (nlen >= sizeof name) nlen = sizeof name - 1;
            memcpy(name, pp + 1, nlen); name[nlen] = 0;
            char val[1024]; size_t vlen = qlen;
            if (vlen >= sizeof val) vlen = sizeof val - 1;
            memcpy(val, qp, vlen); val[vlen] = 0;
            Value *vv = xs_str(val);
            map_set(params->map, name, vv);
            value_decref(vv);
        } else if (plen == 1 && pp[0] == '*') {
            /* trailing wildcard */
            return 1;
        } else if (plen != qlen || memcmp(pp, qp, plen) != 0) {
            return 0;
        }
        pp = pe; qp = qe;
    }
    return 1;
}
#endif /* !MINGW32 && !wasi */

/* http.serve(port, handler)
   HTTP/1.1 server. `handler` is either:
     - an XS function taking a request map {method, path, query, headers,
       body} and returning a response map {status?, headers?, body}; or
     - a router map {routes: [...], middleware?: [...], not_found?: fn}
       where each route is {method, pattern, handler}. Patterns may
       include `:name` captures (populating req.params) and `*` as a
       trailing wildcard. Method matches case-insensitively; "ANY" or
       "*" matches every method.
   Each accepted connection blocks the GIL during handler execution but
   releases it during socket I/O. Blocks the calling thread until killed. */
#if !defined(__MINGW32__) && !defined(__wasi__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

static Value *native_http_serve(Interp *ig, Value **a, int n) {
#if defined(__MINGW32__) || defined(__wasi__)
    (void)ig; (void)a; (void)n;
    fprintf(stderr, "http.serve: not available on this platform\n");
    return value_incref(XS_NULL_VAL);
#else
    if (n < 2 || VAL_TAG(a[0]) != XS_INT) {
        fprintf(stderr, "http.serve: expected (port: int, handler|router)\n");
        return value_incref(XS_NULL_VAL);
    }
    int port = (int)VAL_INT(a[0]);
    Value *handler = a[1];
    Value *router = NULL;
    int htag = VAL_TAG(handler);
    if (htag == XS_MAP || htag == XS_MODULE) {
        /* router-shaped second arg */
        if (handler->map && map_get(handler->map, "routes")) {
            router = handler;
            handler = NULL;
        } else {
            fprintf(stderr, "http.serve: map argument missing 'routes'\n");
            return value_incref(XS_NULL_VAL);
        }
    } else if (!value_is_callable(handler)) {
        fprintf(stderr, "http.serve: expected (port: int, handler|router)\n");
        return value_incref(XS_NULL_VAL);
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("http.serve: socket"); return value_incref(XS_NULL_VAL); }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("http.serve: bind"); close(lfd);
        return value_incref(XS_NULL_VAL);
    }
    if (listen(lfd, 16) < 0) {
        perror("http.serve: listen"); close(lfd);
        return value_incref(XS_NULL_VAL);
    }
    fprintf(stderr, "http.serve: listening on :%d\n", port);

    for (;;) {
        struct sockaddr_in cli = {0};
        socklen_t clen = sizeof cli;
        /* Release the GIL while waiting for a connection so spawned
           XS tasks can run; reacquire on accept return. */
        xs_gil_release();
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clen);
        xs_gil_acquire();
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char buf[16384];
        /* Release the GIL while reading from the socket. */
        xs_gil_release();
        int got = (int)recv(cfd, buf, sizeof(buf) - 1, 0);
        xs_gil_acquire();
        if (got <= 0) { close(cfd); continue; }
        buf[got] = 0;

        /* Parse request line: METHOD PATH VERSION\r\n */
        char method[16] = "GET", url[2048] = "/", version[16] = "HTTP/1.1";
        sscanf(buf, "%15s %2047s %15s", method, url, version);

        /* Split path/query */
        char path[2048] = {0}, query[2048] = {0};
        const char *qmark = strchr(url, '?');
        if (qmark) {
            size_t plen = (size_t)(qmark - url);
            if (plen >= sizeof path) plen = sizeof path - 1;
            memcpy(path, url, plen); path[plen] = 0;
            snprintf(query, sizeof query, "%s", qmark + 1);
        } else {
            snprintf(path, sizeof path, "%s", url);
        }

        /* Collect headers until blank line */
        Value *hmap = xs_map_new();
        char *p = strstr(buf, "\r\n");
        if (p) p += 2;
        while (p && *p && !(p[0] == '\r' && p[1] == '\n')) {
            char *eol = strstr(p, "\r\n");
            if (!eol) break;
            char *colon = memchr(p, ':', (size_t)(eol - p));
            if (colon) {
                size_t nlen = (size_t)(colon - p);
                char name[256]; if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, p, nlen); name[nlen] = 0;
                char *v = colon + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)(eol - v);
                char val[4096]; if (vlen >= sizeof val) vlen = sizeof val - 1;
                memcpy(val, v, vlen); val[vlen] = 0;
                Value *sv = xs_str(val);
                map_set(hmap->map, name, sv);
                value_decref(sv);
            }
            p = eol + 2;
        }

        /* Body starts after \r\n\r\n */
        const char *body = "";
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) body = body_start + 4;

        /* Build request map */
        Value *req = xs_map_new();
        Value *mv = xs_str(method); map_set(req->map, "method", mv); value_decref(mv);
        Value *pv = xs_str(path);   map_set(req->map, "path",   pv); value_decref(pv);
        Value *qv = xs_str(query);  map_set(req->map, "query",  qv); value_decref(qv);
        map_set(req->map, "headers", hmap); value_decref(hmap);
        Value *bv = xs_str(body);   map_set(req->map, "body",   bv); value_decref(bv);

        Value *res = NULL;
        Value *chosen_handler = handler;
        if (router) {
            /* Run middleware first; if any returns a non-null map it
             * short-circuits the router. */
            Value *mws = map_get(router->map, "middleware");
            if (mws && VAL_TAG(mws) == XS_ARRAY && mws->arr) {
                for (int mi = 0; mi < mws->arr->len && !res; mi++) {
                    Value *mw = mws->arr->items[mi];
                    if (!mw) continue;
                    if (!value_is_callable(mw)) continue;
                    Value *margs[1] = { req };
                    Value *mres = call_value(ig, mw, margs, 1, "http.serve middleware");
                    if (mres && (VAL_TAG(mres) == XS_MAP || VAL_TAG(mres) == XS_MODULE) && mres->map) {
                        res = mres;
                    } else if (mres) {
                        value_decref(mres);
                    }
                }
            }
            if (!res) {
                Value *routes = map_get(router->map, "routes");
                if (routes && VAL_TAG(routes) == XS_ARRAY && routes->arr) {
                    for (int ri = 0; ri < routes->arr->len; ri++) {
                        Value *route = routes->arr->items[ri];
                        if (!route || (VAL_TAG(route) != XS_MAP && VAL_TAG(route) != XS_MODULE) || !route->map) continue;
                        Value *rmv = map_get(route->map, "method");
                        Value *rpv = map_get(route->map, "pattern");
                        Value *rhv = map_get(route->map, "handler");
                        if (!rpv || VAL_TAG(rpv) != XS_STR) continue;
                        if (!rhv) continue;
                        const char *rm = (rmv && VAL_TAG(rmv) == XS_STR) ? rmv->s : "GET";
                        if (strcmp(rm, "*") != 0 && strcasecmp(rm, "ANY") != 0 &&
                            strcasecmp(rm, method) != 0) continue;
                        Value *params = NULL;
                        if (route_match(rpv->s, path, &params)) {
                            map_set(req->map, "params", params);
                            value_decref(params);
                            chosen_handler = rhv;
                            Value *args[1] = { req };
                            res = call_value(ig, chosen_handler, args, 1, "http.serve route");
                            break;
                        }
                        value_decref(params);
                    }
                }
                if (!res) {
                    Value *nf = map_get(router->map, "not_found");
                    if (value_is_callable(nf)) {
                        Value *args[1] = { req };
                        res = call_value(ig, nf, args, 1, "http.serve not_found");
                    } else {
                        res = xs_map_new();
                        Value *sv = xs_int(404); map_set(res->map, "status", sv); value_decref(sv);
                        Value *bvv = xs_str("not found"); map_set(res->map, "body", bvv); value_decref(bvv);
                    }
                }
            }
        } else {
            Value *args[1] = { req };
            res = call_value(ig, chosen_handler, args, 1, "http.serve handler");
        }
        value_decref(req);

        int status = 200;
        const char *rbody = "";
        XSMap *rheaders = NULL;
        if (res && (VAL_TAG(res) == XS_MAP || VAL_TAG(res) == XS_MODULE) && res->map) {
            Value *sv = map_get(res->map, "status");
            if (sv && VAL_TAG(sv) == XS_INT) status = (int)VAL_INT(sv);
            Value *bv2 = map_get(res->map, "body");
            if (bv2 && VAL_TAG(bv2) == XS_STR) rbody = bv2->s;
            Value *hv = map_get(res->map, "headers");
            if (hv && VAL_TAG(hv) == XS_MAP && hv->map) rheaders = hv->map;
        } else if (res && VAL_TAG(res) == XS_STR) {
            rbody = res->s;
        }

        char resp_hdr[1024];
        int rbody_len = (int)strlen(rbody);
        int hlen = snprintf(resp_hdr, sizeof resp_hdr,
            "HTTP/1.1 %d OK\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n",
            status, rbody_len);
        if (rheaders) {
            int nk = 0; char **ks = map_keys(rheaders, &nk);
            for (int k = 0; k < nk && hlen < (int)sizeof resp_hdr - 64; k++) {
                Value *vv = map_get(rheaders, ks[k]);
                if (vv && VAL_TAG(vv) == XS_STR) {
                    hlen += snprintf(resp_hdr + hlen, sizeof resp_hdr - hlen,
                                     "%s: %s\r\n", ks[k], vv->s);
                }
            }
            if (ks) {
                for (int k = 0; k < nk; k++) free(ks[k]);
                free(ks);
            }
        }
        hlen += snprintf(resp_hdr + hlen, sizeof resp_hdr - hlen, "\r\n");
        /* Release the GIL during the write phase too. */
        xs_gil_release();
        if (send(cfd, resp_hdr, hlen, 0) < 0) { /* ignore */ }
        if (rbody_len > 0) {
            if (send(cfd, rbody, rbody_len, 0) < 0) { /* ignore */ }
        }
        close(cfd);
        xs_gil_acquire();
        if (res) value_decref(res);
    }
    close(lfd);
    return value_incref(XS_NULL_VAL);
#endif
}

Value *make_http_module(void) {
    XSMap *m=map_new();
    map_take(m,"get",     xs_native(native_http_get));
    map_take(m,"post",    xs_native(native_http_post));
    map_take(m,"put",     xs_native(native_http_put));
    map_take(m,"delete",  xs_native(native_http_delete));
    map_take(m,"patch",   xs_native(native_http_patch));
    map_take(m,"request", xs_native(native_http_request));
    map_take(m,"serve",   xs_native(native_http_serve));
    return xs_module(m);
}
