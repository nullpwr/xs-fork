#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "tls/xs_tls.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#if !defined(__MINGW32__) && !defined(__wasi__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/tcp.h>
#elif defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

/* net */
#if !defined(__MINGW32__) && !defined(__wasi__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

static Value *native_net_tcp_connect(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *host = a[0]->s;
    int port = (VAL_TAG(a[1]) == XS_INT) ? (int)VAL_INT(a[1]) : 0;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return value_incref(XS_NULL_VAL);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return value_incref(XS_NULL_VAL); }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return value_incref(XS_NULL_VAL);
    }
    freeaddrinfo(res);

    XSMap *conn = map_new();
    map_take(conn, "fd", xs_int(fd));
    return xs_module(conn);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

static Value *native_net_tcp_listen(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1) return value_incref(XS_NULL_VAL);
    int port = (VAL_TAG(a[0]) == XS_INT) ? (int)VAL_INT(a[0]) : 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return value_incref(XS_NULL_VAL);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }
    if (listen(fd, 128) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }

    XSMap *srv = map_new();
    map_take(srv, "fd", xs_int(fd));
    return xs_module(srv);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

static Value *native_net_resolve(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a[0]->s, NULL, &hints, &res) != 0)
        return xs_array_new();

    Value *arr = xs_array_new();
    for (p = res; p; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof ip);
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, ip, sizeof ip);
        }
        Value *s = xs_str(ip);
        array_push(arr->arr, s);
        value_decref(s);
    }
    freeaddrinfo(res);
    return arr;
#else
    (void)a; (void)n;
    return xs_array_new();
#endif
}

static Value *native_net_url_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_map_new();
    Value *m = xs_map_new();
    const char *url = a[0]->s;

    /* scheme */
    const char *p = strstr(url, "://");
    if (p) {
        Value *v = xs_str_n(url, (int)(p - url)); map_set(m->map, "scheme", v); value_decref(v);
        url = p + 3;
    } else {
        Value *v = xs_str(""); map_set(m->map, "scheme", v); value_decref(v);
    }

    /* host and port */
    const char *slash = strchr(url, '/');
    const char *host_end = slash ? slash : url + strlen(url);
    char host_buf[512];
    int hlen = (int)(host_end - url);
    if (hlen >= (int)sizeof(host_buf)) hlen = (int)sizeof(host_buf) - 1;
    memcpy(host_buf, url, hlen); host_buf[hlen] = '\0';

    const char *colon = strchr(host_buf, ':');
    if (colon) {
        Value *hv = xs_str_n(host_buf, (int)(colon - host_buf));
        map_set(m->map, "host", hv); value_decref(hv);
        Value *pv = xs_int(atoi(colon + 1));
        map_set(m->map, "port", pv); value_decref(pv);
    } else {
        Value *hv = xs_str(host_buf);
        map_set(m->map, "host", hv); value_decref(hv);
        Value *pv = xs_int(0);
        map_set(m->map, "port", pv); value_decref(pv);
    }

    url = host_end;

    /* path and query */
    const char *qm = strchr(url, '?');
    if (qm) {
        Value *pv = xs_str_n(url, (int)(qm - url)); map_set(m->map, "path", pv); value_decref(pv);
        Value *qv = xs_str(qm + 1); map_set(m->map, "query", qv); value_decref(qv);
    } else {
        Value *pv = xs_str(url); map_set(m->map, "path", pv); value_decref(pv);
        Value *qv = xs_str(""); map_set(m->map, "query", qv); value_decref(qv);
    }

    return m;
}

/* HTTP client helpers */

#if !defined(__MINGW32__) && !defined(__wasi__)

/* Parse a URL into host, port, path. Returns 0 on success, -1 on error. */
static int http_parse_url(const char *url, char *host, int hostlen,
                          int *port, char *path, int pathlen) {
    const char *start = url;
    if (strncmp(url, "https://", 8) == 0) { start = url + 8; *port = 443; }
    else if (strncmp(url, "http://", 7) == 0) { start = url + 7; *port = 80; }
    else { *port = 80; }

    const char *slash = strchr(start, '/');
    const char *host_end = slash ? slash : start + strlen(start);

    /* extract host:port */
    int hlen = (int)(host_end - start);
    char hbuf[512];
    if (hlen >= (int)sizeof(hbuf)) hlen = (int)sizeof(hbuf) - 1;
    memcpy(hbuf, start, hlen);
    hbuf[hlen] = '\0';

    const char *colon = strchr(hbuf, ':');
    if (colon) {
        int nlen = (int)(colon - hbuf);
        if (nlen >= hostlen) nlen = hostlen - 1;
        memcpy(host, hbuf, nlen);
        host[nlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        if (hlen >= hostlen) hlen = hostlen - 1;
        memcpy(host, hbuf, hlen);
        host[hlen] = '\0';
    }

    if (slash) {
        int plen = (int)strlen(slash);
        if (plen >= pathlen) plen = pathlen - 1;
        memcpy(path, slash, plen);
        path[plen] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* Connect to host:port via TCP. Returns fd or -1. */
static int http_connect(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Dynamic buffer for reading response */
typedef struct { char *data; size_t len; size_t cap; } HttpBuf;

static void httpbuf_init(HttpBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void httpbuf_append(HttpBuf *b, const char *src, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t newcap = (b->cap == 0) ? 4096 : b->cap * 2;
        while (newcap < b->len + n + 1) newcap *= 2;
        b->data = realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void httpbuf_free(HttpBuf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* Read entire response from fd into buf */
static void http_read_all(int fd, HttpBuf *buf) {
    char tmp[4096];
    ssize_t nr;
    while ((nr = read(fd, tmp, sizeof tmp)) > 0)
        httpbuf_append(buf, tmp, (size_t)nr);
}

/* Parse HTTP response and return XS map: #{ status, headers, body } */
static Value *http_parse_response(HttpBuf *buf) {
    Value *result = xs_map_new();
    if (!buf->data || buf->len == 0) {
        Value *sv = xs_int(0); map_set(result->map, "status", sv); value_decref(sv);
        Value *hv = xs_map_new(); map_set(result->map, "headers", hv); value_decref(hv);
        Value *bv = xs_str(""); map_set(result->map, "body", bv); value_decref(bv);
        return result;
    }

    /* find end of status line */
    char *p = buf->data;
    char *end = buf->data + buf->len;
    char *line_end = strstr(p, "\r\n");
    if (!line_end) line_end = strstr(p, "\n");
    if (!line_end) {
        Value *sv = xs_int(0); map_set(result->map, "status", sv); value_decref(sv);
        Value *hv = xs_map_new(); map_set(result->map, "headers", hv); value_decref(hv);
        Value *bv = xs_str(""); map_set(result->map, "body", bv); value_decref(bv);
        return result;
    }

    /* parse status code from "HTTP/1.x NNN ..." */
    int status = 0;
    {
        char *sp = strchr(p, ' ');
        if (sp && sp < line_end) status = atoi(sp + 1);
    }
    Value *sv = xs_int(status); map_set(result->map, "status", sv); value_decref(sv);

    /* advance past status line */
    p = line_end;
    if (*p == '\r') p++;
    if (*p == '\n') p++;

    /* parse headers */
    Value *headers = xs_map_new();
    int chunked = 0;
    long content_length = -1;
    while (p < end) {
        /* blank line = end of headers */
        if (*p == '\r' || *p == '\n') {
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            break;
        }
        char *hline_end = strstr(p, "\r\n");
        if (!hline_end) hline_end = strstr(p, "\n");
        if (!hline_end) hline_end = end;

        char *colon = memchr(p, ':', (size_t)(hline_end - p));
        if (colon) {
            int klen = (int)(colon - p);
            char key[256];
            if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
            memcpy(key, p, klen);
            key[klen] = '\0';
            /* lowercase the key for easier matching */
            char lkey[256];
            for (int i = 0; i <= klen; i++) lkey[i] = (char)tolower((unsigned char)key[i]);

            const char *val = colon + 1;
            while (val < hline_end && *val == ' ') val++;
            int vlen = (int)(hline_end - val);

            Value *vv = xs_str_n(val, vlen);
            map_set(headers->map, key, vv);
            value_decref(vv);

            /* detect chunked / content-length */
            if (strcmp(lkey, "transfer-encoding") == 0) {
                char vbuf[128];
                int cl = vlen < (int)sizeof(vbuf) - 1 ? vlen : (int)sizeof(vbuf) - 1;
                memcpy(vbuf, val, cl); vbuf[cl] = '\0';
                for (int i = 0; vbuf[i]; i++) vbuf[i] = (char)tolower((unsigned char)vbuf[i]);
                if (strstr(vbuf, "chunked")) chunked = 1;
            } else if (strcmp(lkey, "content-length") == 0) {
                content_length = atol(val);
            }
        }
        p = hline_end;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    map_set(result->map, "headers", headers);
    value_decref(headers);

    /* extract body */
    size_t body_offset = (size_t)(p - buf->data);
    size_t body_avail = (body_offset < buf->len) ? buf->len - body_offset : 0;

    if (chunked) {
        /* decode chunked transfer encoding */
        HttpBuf decoded;
        httpbuf_init(&decoded);
        char *cp = p;
        while (cp < end) {
            /* read chunk size (hex) */
            char *chunk_end = strstr(cp, "\r\n");
            if (!chunk_end) break;
            long chunk_size = strtol(cp, NULL, 16);
            if (chunk_size <= 0) break;
            cp = chunk_end + 2; /* skip \r\n after size */
            if (cp + chunk_size > end) chunk_size = (long)(end - cp);
            httpbuf_append(&decoded, cp, (size_t)chunk_size);
            cp += chunk_size;
            if (cp + 2 <= end && cp[0] == '\r' && cp[1] == '\n') cp += 2;
        }
        Value *bv = xs_str_n(decoded.data ? decoded.data : "", (int)decoded.len);
        map_set(result->map, "body", bv);
        value_decref(bv);
        httpbuf_free(&decoded);
    } else if (content_length >= 0) {
        size_t blen = (size_t)content_length;
        if (blen > body_avail) blen = body_avail;
        Value *bv = xs_str_n(p, (int)blen);
        map_set(result->map, "body", bv);
        value_decref(bv);
    } else {
        /* read until close */
        Value *bv = xs_str_n(p, (int)body_avail);
        map_set(result->map, "body", bv);
        value_decref(bv);
    }

    return result;
}

/* Core: perform an HTTP request. Returns XS map. */
Value *http_do_request(const char *method, const char *url,
                              XSMap *extra_headers, const char *body,
                              size_t body_len) {
    char host[512], path[2048];
    int port;
    if (http_parse_url(url, host, sizeof host, &port, path, sizeof path) < 0)
        return value_incref(XS_NULL_VAL);

    int use_tls = (strncmp(url, "https://", 8) == 0);
    int fd = http_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "error: could not connect to %s:%d\n", host, port);
        return value_incref(XS_NULL_VAL);
    }

    xs_tls_conn *tls = NULL;
    if (use_tls) {
        tls = xs_tls_connect(fd, host);
        if (!tls) {
            fprintf(stderr, "error: TLS handshake failed for %s\n", host);
            close(fd);
            return value_incref(XS_NULL_VAL);
        }
    }

    /* build request */
    HttpBuf req;
    httpbuf_init(&req);

    /* request line */
    httpbuf_append(&req, method, strlen(method));
    httpbuf_append(&req, " ", 1);
    httpbuf_append(&req, path, strlen(path));
    httpbuf_append(&req, " HTTP/1.1\r\n", 11);

    /* Host header */
    httpbuf_append(&req, "Host: ", 6);
    httpbuf_append(&req, host, strlen(host));
    if (port != 80) {
        char pbuf[16];
        snprintf(pbuf, sizeof pbuf, ":%d", port);
        httpbuf_append(&req, pbuf, strlen(pbuf));
    }
    httpbuf_append(&req, "\r\n", 2);

    /* Connection: close */
    httpbuf_append(&req, "Connection: close\r\n", 19);

    /* Content-Length if body present */
    if (body && body_len > 0) {
        char clbuf[64];
        snprintf(clbuf, sizeof clbuf, "Content-Length: %zu\r\n", body_len);
        httpbuf_append(&req, clbuf, strlen(clbuf));
    }

    /* extra headers from map */
    if (extra_headers) {
        for (int i = 0; i < extra_headers->cap; i++) {
            if (extra_headers->keys[i] && extra_headers->vals[i]) {
                const char *k = extra_headers->keys[i];
                Value *v = extra_headers->vals[i];
                if (VAL_TAG(v) == XS_STR) {
                    httpbuf_append(&req, k, strlen(k));
                    httpbuf_append(&req, ": ", 2);
                    httpbuf_append(&req, v->s, strlen(v->s));
                    httpbuf_append(&req, "\r\n", 2);
                }
            }
        }
    }

    /* end of headers */
    httpbuf_append(&req, "\r\n", 2);

    /* send request */
    if (tls) {
        if (xs_tls_write(tls, req.data, (int)req.len) < 0) {
            xs_tls_close(tls); httpbuf_free(&req); return value_incref(XS_NULL_VAL);
        }
    } else {
        size_t sent = 0;
        while (sent < req.len) {
            ssize_t w = write(fd, req.data + sent, req.len - sent);
            if (w <= 0) { close(fd); httpbuf_free(&req); return value_incref(XS_NULL_VAL); }
            sent += (size_t)w;
        }
    }
    httpbuf_free(&req);

    /* send body */
    if (body && body_len > 0) {
        if (tls) {
            if (xs_tls_write(tls, body, (int)body_len) < 0) {
                xs_tls_close(tls); return value_incref(XS_NULL_VAL);
            }
        } else {
            size_t sent = 0;
            while (sent < body_len) {
                ssize_t w = write(fd, body + sent, body_len - sent);
                if (w <= 0) { close(fd); return value_incref(XS_NULL_VAL); }
                sent += (size_t)w;
            }
        }
    }

    /* read response */
    HttpBuf resp;
    httpbuf_init(&resp);
    if (tls) {
        char tmp[4096];
        int nr;
        while ((nr = xs_tls_read(tls, tmp, sizeof tmp)) > 0)
            httpbuf_append(&resp, tmp, (size_t)nr);
        xs_tls_close(tls);
    } else {
        http_read_all(fd, &resp);
        close(fd);
    }

    Value *result = http_parse_response(&resp);
    httpbuf_free(&resp);
    return result;
}

#endif /* __MINGW32__ */

/* net.http_get(url) */
static Value *native_net_http_get(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    return http_do_request("GET", a[0]->s, NULL, NULL, 0);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.http_post(url, body, content_type) */
static Value *native_net_http_post(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 3 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR || VAL_TAG(a[2]) != XS_STR)
        return value_incref(XS_NULL_VAL);

    /* build a temporary headers map with Content-Type */
    XSMap *hdrs = map_new();
    Value *ct = xs_str(a[2]->s);
    map_set(hdrs, "Content-Type", ct);
    value_decref(ct);

    Value *result = http_do_request("POST", a[0]->s, hdrs, a[1]->s, strlen(a[1]->s));
    map_free(hdrs);
    return result;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.http(method, url, headers_map, body) */
static Value *native_net_http(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);

    XSMap *hdrs = NULL;
    if (n >= 3 && VAL_TAG(a[2]) == XS_MAP) hdrs = a[2]->map;
    const char *body = NULL;
    size_t body_len = 0;
    if (n >= 4 && VAL_TAG(a[3]) == XS_STR) {
        body = a[3]->s;
        body_len = strlen(a[3]->s);
    }

    return http_do_request(a[0]->s, a[1]->s, hdrs, body, body_len);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.udp_bind(port) */
static Value *native_net_udp_bind(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return value_incref(XS_NULL_VAL);
    int port = (int)VAL_INT(a[0]);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return value_incref(XS_NULL_VAL);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }
    XSMap *m = map_new();
    map_take(m, "fd", xs_int(fd));
    return xs_module(m);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.udp_send(sock, host, port, data) */
static Value *native_net_udp_send(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 4) return value_incref(XS_FALSE_VAL);
    if ((VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR
        || VAL_TAG(a[2]) != XS_INT || VAL_TAG(a[3]) != XS_STR)
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)VAL_INT(a[2]));
    inet_pton(AF_INET, a[1]->s, &dest.sin_addr);
    ssize_t sent = sendto(fd, a[3]->s, strlen(a[3]->s), 0,
                          (struct sockaddr*)&dest, sizeof(dest));
    return (sent >= 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.udp_recv(sock, max) */
static Value *native_net_udp_recv(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    int fd = (int)VAL_INT(fdv);
    int maxsz = 65536;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxsz = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxsz + 1);
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t nr = recvfrom(fd, buf, maxsz, 0, (struct sockaddr*)&from, &fromlen);
    if (nr < 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *result = xs_map_new();
    Value *dv = xs_str_n(buf, nr); map_set(result->map, "data", dv); value_decref(dv);
    free(buf);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    Value *hv = xs_str(ip); map_set(result->map, "host", hv); value_decref(hv);
    Value *pv = xs_int(ntohs(from.sin_port)); map_set(result->map, "port", pv); value_decref(pv);
    return result;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.set_timeout(conn, ms) */
static Value *native_net_set_timeout(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_INT)
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    int ms = (int)VAL_INT(a[1]);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.set_nodelay(conn, bool) */
static Value *native_net_set_nodelay(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    int flag = value_truthy(a[1]) ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.close(conn) */
static Value *native_net_close(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    close((int)VAL_INT(fdv));
    map_take(a[0]->map, "fd", xs_int(-1));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.send(conn, data) / net.recv(conn, max) */
static Value *native_net_send(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR)
        return xs_int(-1);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return xs_int(-1);
    ssize_t w = write((int)VAL_INT(fdv), a[1]->s, strlen(a[1]->s));
    return xs_int((int64_t)w);
#else
    (void)a; (void)n;
    return xs_int(-1);
#endif
}

static Value *native_net_recv(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    int maxsz = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxsz = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxsz + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxsz);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

Value *make_net_module(void) {
    XSMap *m = map_new();
    map_take(m, "tcp_connect", xs_native(native_net_tcp_connect));
    map_take(m, "tcp_listen",  xs_native(native_net_tcp_listen));
    map_take(m, "resolve",     xs_native(native_net_resolve));
    map_take(m, "url_parse",   xs_native(native_net_url_parse));
    map_take(m, "http_get",    xs_native(native_net_http_get));
    map_take(m, "http_post",   xs_native(native_net_http_post));
    map_take(m, "http",        xs_native(native_net_http));
    map_take(m, "udp_bind",    xs_native(native_net_udp_bind));
    map_take(m, "udp_send",    xs_native(native_net_udp_send));
    map_take(m, "udp_recv",    xs_native(native_net_udp_recv));
    map_take(m, "set_timeout", xs_native(native_net_set_timeout));
    map_take(m, "set_nodelay", xs_native(native_net_set_nodelay));
    map_take(m, "close",       xs_native(native_net_close));
    map_take(m, "send",        xs_native(native_net_send));
    map_take(m, "recv",        xs_native(native_net_recv));
    return xs_module(m);
}
