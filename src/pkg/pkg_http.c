#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "pkg/pkg_http.h"
#include "net/sock_compat.h"

#if !defined(__wasi__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Tiny self-contained HTTP/1.1 client used by the registry CLI.
 * builtins_net.c has a much richer client but its current guards
 * exclude mingw entirely; rather than rewrite that whole module's
 * Windows story for v1.0, the pkg subcommands use this minimal one
 * so `xs install`, `xs publish`, `xs search`, `xs whoami` work on
 * every platform that has a TCP stack. TLS handshakes go through the
 * BearSSL plumbing in src/tls/. */

#include "tls/xs_tls.h"

typedef struct { char *data; size_t len, cap; } sbuf;

static void sbuf_init(sbuf *b) { b->data = NULL; b->len = b->cap = 0; }
static void sbuf_grow(sbuf *b, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap = nc;
    }
}
static void sbuf_append(sbuf *b, const char *s, size_t n) {
    sbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static int parse_url(const char *url, int *https, char *host, size_t hcap,
                     int *port, char *path, size_t pcap) {
    *https = 0;
    *port = 80;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { p += 8; *https = 1; *port = 443; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *hend = slash ? slash : p + strlen(p);
    size_t hl = (size_t)(hend - p);
    if (hl >= hcap) hl = hcap - 1;
    memcpy(host, p, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    if (slash) snprintf(path, pcap, "%s", slash);
    else snprintf(path, pcap, "/");
    return 0;
}

static int dial(const char *host, int port) {
    xs_sock_init();
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = (int)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

typedef struct {
    int fd;
    int is_tls;
    xs_tls_conn *tls;
} conn_t;

static int io_send(conn_t *c, const void *buf, size_t n) {
    if (c->is_tls) return xs_tls_write(c->tls, buf, (int)n);
    size_t sent = 0;
    while (sent < n) {
        int r = (int)send(c->fd, (const char *)buf + sent, (int)(n - sent), 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return (int)sent;
}

static int io_recv(conn_t *c, void *buf, size_t n) {
    if (c->is_tls) return xs_tls_read(c->tls, buf, (int)n);
    return (int)recv(c->fd, (char *)buf, (int)n, 0);
}

int pkg_http_request(const char *method, const char *url,
                     const char *const *headers, int n_headers,
                     const char *body, size_t body_len,
                     PkgHttpResponse *out) {
    if (!out || !method || !url) return -1;
    out->status = 0;
    out->body = NULL;
    out->body_len = 0;

    char host[512], path[2048];
    int port, https;
    if (parse_url(url, &https, host, sizeof host, &port, path, sizeof path) < 0) {
        return -1;
    }

    int fd = dial(host, port);
    if (fd < 0) return -1;

    conn_t c = { fd, 0, NULL };
    if (https) {
        c.tls = xs_tls_connect(fd, host);
        if (!c.tls) { close(fd); return -1; }
        c.is_tls = 1;
    }

    sbuf req; sbuf_init(&req);
    char line[4096];
    int L = snprintf(line, sizeof line,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: xs-pkg/1.0\r\n"
        "Connection: close\r\n", method, path, host);
    sbuf_append(&req, line, (size_t)L);
    for (int i = 0; i < n_headers; i++) {
        sbuf_append(&req, headers[i], strlen(headers[i]));
        sbuf_append(&req, "\r\n", 2);
    }
    if (body && body_len > 0) {
        L = snprintf(line, sizeof line, "Content-Length: %zu\r\n", body_len);
        sbuf_append(&req, line, (size_t)L);
    }
    sbuf_append(&req, "\r\n", 2);
    if (body && body_len > 0) sbuf_append(&req, body, body_len);

    if (io_send(&c, req.data, req.len) < 0) {
        free(req.data);
        if (c.is_tls) xs_tls_close(c.tls);
        close(fd);
        return -1;
    }
    free(req.data);

    sbuf resp; sbuf_init(&resp);
    char chunk[8192];
    for (;;) {
        int r = io_recv(&c, chunk, sizeof chunk);
        if (r <= 0) break;
        sbuf_append(&resp, chunk, (size_t)r);
    }
    if (c.is_tls) xs_tls_close(c.tls);
    close(fd);

    if (resp.len == 0) { free(resp.data); return -1; }

    /* Parse status line */
    char *hdr_end = strstr(resp.data, "\r\n\r\n");
    if (!hdr_end) { free(resp.data); return -1; }
    int code = 0;
    sscanf(resp.data, "HTTP/%*s %d", &code);
    out->status = code;

    char *body_start = hdr_end + 4;
    size_t body_off = (size_t)(body_start - resp.data);

    /* Look for Transfer-Encoding: chunked */
    int chunked = 0;
    char *p = resp.data;
    while (p < hdr_end) {
        char *eol = strstr(p, "\r\n");
        if (!eol || eol >= hdr_end) break;
        if (strncasecmp(p, "Transfer-Encoding:", 18) == 0 &&
            strstr(p, "chunked")) chunked = 1;
        p = eol + 2;
    }

    if (chunked) {
        sbuf decoded; sbuf_init(&decoded);
        char *q = body_start;
        char *qend = resp.data + resp.len;
        while (q < qend) {
            char *eol = strstr(q, "\r\n");
            if (!eol) break;
            *eol = 0;
            size_t clen = (size_t)strtoul(q, NULL, 16);
            *eol = '\r';
            if (clen == 0) break;
            char *cstart = eol + 2;
            if (cstart + clen > qend) break;
            sbuf_append(&decoded, cstart, clen);
            q = cstart + clen + 2;
        }
        out->body = decoded.data;
        out->body_len = decoded.len;
        free(resp.data);
    } else {
        size_t blen = resp.len > body_off ? resp.len - body_off : 0;
        char *bcopy = malloc(blen + 1);
        if (bcopy) {
            memcpy(bcopy, resp.data + body_off, blen);
            bcopy[blen] = 0;
            out->body = bcopy;
            out->body_len = blen;
        }
        free(resp.data);
    }
    return 0;
}

void pkg_http_response_free(PkgHttpResponse *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

#else /* __wasi__ */

int pkg_http_request(const char *m, const char *u, const char *const *h, int nh,
                     const char *b, size_t bl, PkgHttpResponse *out) {
    (void)m;(void)u;(void)h;(void)nh;(void)b;(void)bl;(void)out;
    return -1;
}
void pkg_http_response_free(PkgHttpResponse *r) { (void)r; }

#endif
