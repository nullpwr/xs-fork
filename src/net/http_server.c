#define _GNU_SOURCE
#include <strings.h>
/* http_server.c - HTTP/1.1 server with routing, middleware, and static files
 *
 * Features:
 *   - HTTP/1.1 request parsing (methods, headers, body)
 *   - Keep-alive connection support
 *   - URL-pattern router with :param captures
 *   - Middleware chain
 *   - Static file serving with MIME detection
 *   - Non-blocking I/O via event loop
 *   - Chunked transfer encoding (reading)
 *   - Query string parsing with URL decoding
 */
#define _POSIX_C_SOURCE 200809L

#include "net/http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>

/* ================================================================
 *  Status code table
 * ================================================================ */

typedef struct {
    int code;
    const char *text;
} StatusEntry;

static const StatusEntry status_table[] = {
    { 100, "Continue" },
    { 101, "Switching Protocols" },
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 206, "Partial Content" },
    { 301, "Moved Permanently" },
    { 302, "Found" },
    { 303, "See Other" },
    { 304, "Not Modified" },
    { 307, "Temporary Redirect" },
    { 308, "Permanent Redirect" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 406, "Not Acceptable" },
    { 408, "Request Timeout" },
    { 409, "Conflict" },
    { 410, "Gone" },
    { 411, "Length Required" },
    { 413, "Payload Too Large" },
    { 414, "URI Too Long" },
    { 415, "Unsupported Media Type" },
    { 416, "Range Not Satisfiable" },
    { 418, "I'm a Teapot" },
    { 422, "Unprocessable Entity" },
    { 429, "Too Many Requests" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 504, "Gateway Timeout" },
    { 505, "HTTP Version Not Supported" },
    { 0, NULL }
};

const char *http_status_text(int code) {
    for (int i = 0; status_table[i].text; i++) {
        if (status_table[i].code == code)
            return status_table[i].text;
    }
    return "Unknown";
}

/* ================================================================
 *  MIME type detection
 * ================================================================ */

typedef struct {
    const char *ext;
    const char *mime;
} MimeEntry;

static const MimeEntry mime_table[] = {
    { ".html",  "text/html; charset=utf-8" },
    { ".htm",   "text/html; charset=utf-8" },
    { ".css",   "text/css; charset=utf-8" },
    { ".js",    "application/javascript; charset=utf-8" },
    { ".mjs",   "application/javascript; charset=utf-8" },
    { ".json",  "application/json; charset=utf-8" },
    { ".xml",   "application/xml; charset=utf-8" },
    { ".txt",   "text/plain; charset=utf-8" },
    { ".md",    "text/markdown; charset=utf-8" },
    { ".csv",   "text/csv; charset=utf-8" },
    { ".svg",   "image/svg+xml" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".ico",   "image/x-icon" },
    { ".webp",  "image/webp" },
    { ".avif",  "image/avif" },
    { ".bmp",   "image/bmp" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf",   "font/ttf" },
    { ".otf",   "font/otf" },
    { ".eot",   "application/vnd.ms-fontobject" },
    { ".pdf",   "application/pdf" },
    { ".zip",   "application/zip" },
    { ".gz",    "application/gzip" },
    { ".tar",   "application/x-tar" },
    { ".mp3",   "audio/mpeg" },
    { ".mp4",   "video/mp4" },
    { ".webm",  "video/webm" },
    { ".ogg",   "audio/ogg" },
    { ".wav",   "audio/wav" },
    { ".wasm",  "application/wasm" },
    { ".map",   "application/json" },
    { ".yaml",  "text/yaml; charset=utf-8" },
    { ".yml",   "text/yaml; charset=utf-8" },
    { ".toml",  "text/toml; charset=utf-8" },
    { ".rs",    "text/plain; charset=utf-8" },
    { ".c",     "text/plain; charset=utf-8" },
    { ".h",     "text/plain; charset=utf-8" },
    { ".py",    "text/plain; charset=utf-8" },
    { ".rb",    "text/plain; charset=utf-8" },
    { ".go",    "text/plain; charset=utf-8" },
    { ".xs",    "text/plain; charset=utf-8" },
    { NULL, NULL }
};

const char *mime_type_for_ext(const char *filename) {
    if (!filename) return "application/octet-stream";

    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";

    for (int i = 0; mime_table[i].ext; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0)
            return mime_table[i].mime;
    }
    return "application/octet-stream";
}

/* ================================================================
 *  URL decode
 * ================================================================ */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(const char *src, char *dst, int dst_size) {
    int di = 0;
    for (int i = 0; src[i] && di < dst_size - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            int h = hex_val(src[i+1]);
            int l = hex_val(src[i+2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)(h * 16 + l);
                i += 2;
                continue;
            }
        }
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ================================================================
 *  Query string parsing
 * ================================================================ */

int parse_query_string(const char *qs, HTTPRequest *req) {
    if (!qs || !*qs) return 0;

    req->nquery_params = 0;
    char buf[HTTP_MAX_QUERY];
    strncpy(buf, qs, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, "&", &saveptr);

    while (token && req->nquery_params < 32) {
        char *eq = strchr(token, '=');
        int idx = req->nquery_params;

        if (eq) {
            *eq = '\0';
            url_decode(token, req->query_params[idx].name,
                       sizeof(req->query_params[idx].name));
            url_decode(eq + 1, req->query_params[idx].value,
                       sizeof(req->query_params[idx].value));
        } else {
            url_decode(token, req->query_params[idx].name,
                       sizeof(req->query_params[idx].name));
            req->query_params[idx].value[0] = '\0';
        }
        req->nquery_params++;
        token = strtok_r(NULL, "&", &saveptr);
    }

    return req->nquery_params;
}

/* ================================================================
 *  HTTP Request
 * ================================================================ */

void http_request_init(HTTPRequest *req) {
    memset(req, 0, sizeof(HTTPRequest));
    req->keep_alive = 1;  /* HTTP/1.1 default */
}

void http_request_free(HTTPRequest *req) {
    if (req->body) {
        free(req->body);
        req->body = NULL;
    }
}

/* case-insensitive header lookup */
static const char *find_header(HTTPRequest *req, const char *name) {
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* parse "Transfer-Encoding: chunked" body */
static int parse_chunked_body(const char *data, int len, HTTPRequest *req) {
    int body_cap = 4096;
    char *body = malloc(body_cap);
    int body_len = 0;
    int pos = 0;

    while (pos < len) {
        /* read chunk size (hex) */
        int chunk_start = pos;
        while (pos < len && data[pos] != '\r' && data[pos] != '\n')
            pos++;
        if (pos >= len) break;

        char size_buf[32];
        int slen = pos - chunk_start;
        if (slen >= (int)sizeof(size_buf)) slen = sizeof(size_buf) - 1;
        memcpy(size_buf, data + chunk_start, slen);
        size_buf[slen] = '\0';

        long chunk_size = strtol(size_buf, NULL, 16);
        if (chunk_size == 0) break;  /* final chunk */

        /* skip CRLF after size */
        if (pos < len && data[pos] == '\r') pos++;
        if (pos < len && data[pos] == '\n') pos++;

        /* read chunk data */
        if (pos + chunk_size > len) break;

        while (body_len + (int)chunk_size >= body_cap) {
            body_cap *= 2;
            body = realloc(body, body_cap);
        }
        memcpy(body + body_len, data + pos, chunk_size);
        body_len += (int)chunk_size;
        pos += (int)chunk_size;

        /* skip trailing CRLF */
        if (pos < len && data[pos] == '\r') pos++;
        if (pos < len && data[pos] == '\n') pos++;
    }

    req->body = body;
    req->body_len = body_len;
    return body_len;
}

int http_parse_request(const char *data, int len, HTTPRequest *req) {
    if (!data || len <= 0) return -1;

    http_request_init(req);

    /* find end of headers */
    const char *header_end = NULL;
    for (int i = 0; i < len - 3; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            header_end = data + i;
            break;
        }
    }
    if (!header_end) return -1;  /* incomplete headers */

    int header_len = (int)(header_end - data);
    int body_start = header_len + 4;

    /* parse request line */
    int pos = 0;

    /* method */
    int mi = 0;
    while (pos < header_len && data[pos] != ' ' &&
           mi < HTTP_MAX_METHOD - 1) {
        req->method[mi++] = data[pos++];
    }
    req->method[mi] = '\0';
    if (pos < header_len) pos++;  /* skip space */

    /* URI (path + query) */
    int url_start = pos;
    int pi = 0;
    while (pos < header_len && data[pos] != ' ' && data[pos] != '?') {
        if (pi < HTTP_MAX_PATH - 1)
            req->path[pi++] = data[pos];
        pos++;
    }
    req->path[pi] = '\0';

    /* query string */
    if (pos < header_len && data[pos] == '?') {
        pos++;  /* skip ? */
        int qi = 0;
        while (pos < header_len && data[pos] != ' ') {
            if (qi < HTTP_MAX_QUERY - 1)
                req->query[qi++] = data[pos];
            pos++;
        }
        req->query[qi] = '\0';
        parse_query_string(req->query, req);
    }

    /* build full URL */
    int url_end = pos;
    int url_len = url_end - url_start;
    if (url_len >= (int)sizeof(req->url))
        url_len = sizeof(req->url) - 1;
    memcpy(req->url, data + url_start, url_len);
    req->url[url_len] = '\0';

    /* skip to HTTP version */
    while (pos < header_len && data[pos] == ' ') pos++;
    int vi = 0;
    while (pos < header_len && data[pos] != '\r' && data[pos] != '\n') {
        if (vi < HTTP_MAX_VERSION - 1)
            req->version[vi++] = data[pos];
        pos++;
    }
    req->version[vi] = '\0';

    /* skip CRLF */
    if (pos < header_len && data[pos] == '\r') pos++;
    if (pos < header_len && data[pos] == '\n') pos++;

    /* parse headers */
    req->nheaders = 0;
    while (pos < header_len && req->nheaders < HTTP_MAX_HEADERS) {
        /* end of headers? */
        if (data[pos] == '\r' || data[pos] == '\n') break;

        /* header name */
        int ni = 0;
        while (pos < header_len && data[pos] != ':') {
            if (ni < HTTP_MAX_HDR_NAME - 1)
                req->headers[req->nheaders].name[ni++] = data[pos];
            pos++;
        }
        req->headers[req->nheaders].name[ni] = '\0';

        if (pos < header_len) pos++;  /* skip : */
        while (pos < header_len && data[pos] == ' ') pos++;  /* skip OWS */

        /* header value */
        int hvi = 0;
        while (pos < header_len && data[pos] != '\r' && data[pos] != '\n') {
            if (hvi < HTTP_MAX_HDR_VALUE - 1)
                req->headers[req->nheaders].value[hvi++] = data[pos];
            pos++;
        }
        req->headers[req->nheaders].value[hvi] = '\0';

        /* trim trailing whitespace from value */
        while (hvi > 0 && req->headers[req->nheaders].value[hvi-1] == ' ') {
            req->headers[req->nheaders].value[--hvi] = '\0';
        }

        req->nheaders++;

        /* skip CRLF */
        if (pos < header_len && data[pos] == '\r') pos++;
        if (pos < header_len && data[pos] == '\n') pos++;
    }

    /* process special headers */
    const char *cl = find_header(req, "Content-Length");
    if (cl) {
        req->content_length = atoi(cl);
    }

    const char *te = find_header(req, "Transfer-Encoding");
    if (te && strcasestr(te, "chunked")) {
        req->chunked = 1;
    }

    const char *conn = find_header(req, "Connection");
    if (conn) {
        if (strcasecmp(conn, "close") == 0)
            req->keep_alive = 0;
        else if (strcasecmp(conn, "keep-alive") == 0)
            req->keep_alive = 1;
    }

    /* HTTP/1.0 defaults to close */
    if (strcmp(req->version, "HTTP/1.0") == 0 && !conn) {
        req->keep_alive = 0;
    }

    /* parse body */
    int remaining = len - body_start;
    if (req->chunked && remaining > 0) {
        parse_chunked_body(data + body_start, remaining, req);
    } else if (req->content_length > 0) {
        if (remaining < req->content_length) {
            return -2;  /* incomplete body, need more data */
        }
        int blen = req->content_length;
        if (blen > HTTP_MAX_BODY) blen = HTTP_MAX_BODY;
        req->body = malloc(blen + 1);
        memcpy(req->body, data + body_start, blen);
        req->body[blen] = '\0';
        req->body_len = blen;
    }

    /* URL-decode the path in-place */
    char decoded_path[HTTP_MAX_PATH];
    url_decode(req->path, decoded_path, sizeof(decoded_path));
    strncpy(req->path, decoded_path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    /* normalize: strip trailing slash (except root) */
    int plen = (int)strlen(req->path);
    if (plen > 1 && req->path[plen - 1] == '/') {
        req->path[plen - 1] = '\0';
    }

    return body_start + (req->body_len > 0 ? req->body_len : 0);
}

/* ================================================================
 *  HTTP Response
 * ================================================================ */

void http_response_init(HTTPResponse *res) {
    memset(res, 0, sizeof(HTTPResponse));
    res->status = 200;
    strcpy(res->status_text, "OK");
    strcpy(res->content_type, "text/plain; charset=utf-8");
}

void http_response_free(HTTPResponse *res) {
    if (res->body) {
        free(res->body);
        res->body = NULL;
    }
}

void http_response_status(HTTPResponse *res, int code) {
    res->status = code;
    const char *text = http_status_text(code);
    strncpy(res->status_text, text, sizeof(res->status_text) - 1);
}

void http_response_header(HTTPResponse *res, const char *name,
                          const char *value) {
    if (res->nheaders >= 32) return;

    /* check for existing header, overwrite if found */
    for (int i = 0; i < res->nheaders; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0) {
            strncpy(res->headers[i].value, value,
                    sizeof(res->headers[i].value) - 1);
            return;
        }
    }

    int idx = res->nheaders++;
    strncpy(res->headers[idx].name, name,
            sizeof(res->headers[idx].name) - 1);
    strncpy(res->headers[idx].value, value,
            sizeof(res->headers[idx].value) - 1);
}

void http_response_body(HTTPResponse *res, const char *data, int len) {
    if (res->body) free(res->body);
    res->body = malloc(len + 1);
    memcpy(res->body, data, len);
    res->body[len] = '\0';
    res->body_len = len;
}

void http_response_body_str(HTTPResponse *res, const char *str) {
    http_response_body(res, str, (int)strlen(str));
}

void http_response_content_type(HTTPResponse *res, const char *ct) {
    strncpy(res->content_type, ct, sizeof(res->content_type) - 1);
    res->content_type[sizeof(res->content_type) - 1] = '\0';
}

/* format date header (RFC 7231) */
static void format_date(char *buf, int buf_size) {
    time_t now = time(NULL);
    struct tm gm;
    gmtime_r(&now, &gm);
    strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &gm);
}

int http_format_response(HTTPResponse *res, char *buf, int buf_size) {
    int off = 0;

    /* status line */
    off += snprintf(buf + off, buf_size - off,
                    "HTTP/1.1 %d %s\r\n",
                    res->status, res->status_text);

    /* date header */
    char date_buf[64];
    format_date(date_buf, sizeof(date_buf));
    off += snprintf(buf + off, buf_size - off,
                    "Date: %s\r\n", date_buf);

    /* server header */
    off += snprintf(buf + off, buf_size - off,
                    "Server: xs-http/1.0\r\n");

    /* content-type */
    off += snprintf(buf + off, buf_size - off,
                    "Content-Type: %s\r\n", res->content_type);

    /* content-length */
    off += snprintf(buf + off, buf_size - off,
                    "Content-Length: %d\r\n", res->body_len);

    /* custom headers */
    for (int i = 0; i < res->nheaders; i++) {
        /* skip if we already set these */
        if (strcasecmp(res->headers[i].name, "Content-Type") == 0) continue;
        if (strcasecmp(res->headers[i].name, "Content-Length") == 0) continue;
        if (strcasecmp(res->headers[i].name, "Date") == 0) continue;
        if (strcasecmp(res->headers[i].name, "Server") == 0) continue;

        off += snprintf(buf + off, buf_size - off,
                        "%s: %s\r\n",
                        res->headers[i].name, res->headers[i].value);
    }

    /* end of headers */
    off += snprintf(buf + off, buf_size - off, "\r\n");

    /* body */
    if (res->body && res->body_len > 0 &&
        off + res->body_len < buf_size) {
        memcpy(buf + off, res->body, res->body_len);
        off += res->body_len;
    }

    return off;
}

/* ================================================================
 *  Router
 * ================================================================ */

/* split a path or pattern by '/' */
static char **split_path(const char *path, int *count) {
    if (!path || !*path) {
        *count = 0;
        return NULL;
    }

    /* skip leading slash */
    const char *p = path;
    if (*p == '/') p++;
    if (!*p) {
        *count = 0;
        return NULL;
    }

    /* count segments */
    int n = 1;
    for (const char *s = p; *s; s++) {
        if (*s == '/') n++;
    }

    char **segs = calloc(n, sizeof(char *));
    char buf[HTTP_MAX_PATH];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int idx = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "/", &saveptr);
    while (tok && idx < n) {
        segs[idx++] = strdup(tok);
        tok = strtok_r(NULL, "/", &saveptr);
    }

    *count = idx;
    return segs;
}

static void free_segments(char **segs, int count) {
    if (!segs) return;
    for (int i = 0; i < count; i++) free(segs[i]);
    free(segs);
}

Router *router_new(void) {
    Router *r = calloc(1, sizeof(Router));
    r->cap = 32;
    r->routes = calloc(r->cap, sizeof(Route));
    return r;
}

void router_free(Router *r) {
    if (!r) return;
    for (int i = 0; i < r->nroutes; i++) {
        free_segments(r->routes[i].segments, r->routes[i].nsegments);
        for (int j = 0; j < r->routes[i].nparams; j++)
            free(r->routes[i].param_names[j]);
    }
    free(r->routes);
    free(r);
}

int router_add(Router *r, const char *method, const char *pattern,
               HTTPHandler handler, void *ctx) {
    if (!r || !method || !pattern || !handler) return -1;

    if (r->nroutes >= r->cap) {
        r->cap *= 2;
        r->routes = realloc(r->routes, sizeof(Route) * r->cap);
    }

    Route *route = &r->routes[r->nroutes];
    memset(route, 0, sizeof(Route));
    strncpy(route->method, method, sizeof(route->method) - 1);
    strncpy(route->pattern, pattern, sizeof(route->pattern) - 1);
    route->handler = handler;
    route->handler_ctx = ctx;

    /* check for wildcard */
    int plen = (int)strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '*') {
        route->is_wildcard = 1;
    }

    /* compile segments and extract param names */
    route->segments = split_path(pattern, &route->nsegments);
    route->nparams = 0;

    for (int i = 0; i < route->nsegments; i++) {
        if (route->segments[i][0] == ':') {
            if (route->nparams < HTTP_MAX_PARAMS) {
                route->param_names[route->nparams++] =
                    strdup(route->segments[i] + 1);
            }
        }
    }

    r->nroutes++;
    return 0;
}

Route *router_match(Router *r, const char *method, const char *path,
                    HTTPRequest *req) {
    if (!r || !method || !path) return NULL;

    int path_nseg;
    char **path_segs = split_path(path, &path_nseg);

    Route *best = NULL;

    for (int i = 0; i < r->nroutes; i++) {
        Route *route = &r->routes[i];

        /* method must match (or route is "*") */
        if (strcmp(route->method, "*") != 0 &&
            strcasecmp(route->method, method) != 0)
            continue;

        /* wildcard route: prefix match */
        if (route->is_wildcard) {
            int prefix_segs = route->nsegments;
            /* the last segment is "*", so compare up to nsegments-1 */
            if (prefix_segs > 0 &&
                strcmp(route->segments[prefix_segs - 1], "*") == 0)
                prefix_segs--;

            if (path_nseg < prefix_segs) continue;

            int match = 1;
            int param_idx = 0;
            for (int s = 0; s < prefix_segs; s++) {
                if (route->segments[s][0] == ':') {
                    /* param segment always matches */
                    if (req && param_idx < HTTP_MAX_PARAMS) {
                        strncpy(req->params[param_idx].name,
                                route->segments[s] + 1,
                                sizeof(req->params[0].name) - 1);
                        strncpy(req->params[param_idx].value,
                                path_segs[s],
                                sizeof(req->params[0].value) - 1);
                        param_idx++;
                    }
                } else if (strcmp(route->segments[s], path_segs[s]) != 0) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if (req) req->nparams = param_idx;
                best = route;
                break;
            }
            continue;
        }

        /* exact segment count must match */
        if (route->nsegments != path_nseg) {
            /* special case: root path "/" matches route "/" */
            if (route->nsegments == 0 && path_nseg == 0) {
                best = route;
                if (req) req->nparams = 0;
                break;
            }
            continue;
        }

        /* compare each segment */
        int match = 1;
        int param_idx = 0;
        for (int s = 0; s < route->nsegments; s++) {
            if (route->segments[s][0] == ':') {
                /* param: store value */
                if (req && param_idx < HTTP_MAX_PARAMS) {
                    strncpy(req->params[param_idx].name,
                            route->segments[s] + 1,
                            sizeof(req->params[0].name) - 1);
                    strncpy(req->params[param_idx].value,
                            path_segs[s],
                            sizeof(req->params[0].value) - 1);
                    param_idx++;
                }
            } else if (strcmp(route->segments[s], path_segs[s]) != 0) {
                match = 0;
                break;
            }
        }

        if (match) {
            if (req) req->nparams = param_idx;
            best = route;
            break;
        }
    }

    free_segments(path_segs, path_nseg);
    return best;
}

void router_set_not_found(Router *r, HTTPHandler handler, void *ctx) {
    r->not_found_handler = handler;
    r->not_found_ctx = ctx;
}

void router_add_middleware(Router *r, MiddlewareFunc fn, void *ctx) {
    if (r->nmiddlewares >= 32) return;
    r->middlewares[r->nmiddlewares].fn = fn;
    r->middlewares[r->nmiddlewares].ctx = ctx;
    r->nmiddlewares++;
}

void router_add_static(Router *r, const char *prefix, const char *dir) {
    if (r->nstatics >= 8) return;
    strncpy(r->statics[r->nstatics].prefix, prefix,
            sizeof(r->statics[0].prefix) - 1);
    strncpy(r->statics[r->nstatics].dir, dir,
            sizeof(r->statics[0].dir) - 1);
    r->nstatics++;
}

/* ================================================================
 *  Static file serving
 * ================================================================ */

/* security: check for path traversal */
static int is_safe_path(const char *path) {
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    if (path[0] == '/' && path[1] == '.') return 0;
    return 1;
}

static int serve_directory_listing(const char *dir_path,
                                   const char *url_path,
                                   HTTPResponse *res) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    int cap = 8192;
    char *html = malloc(cap);
    int len = 0;

    len += snprintf(html + len, cap - len,
        "<!DOCTYPE html>\n<html>\n<head>\n"
        "<title>Directory: %s</title>\n"
        "<style>\n"
        "body { font-family: monospace; margin: 2em; }\n"
        "a { text-decoration: none; }\n"
        "a:hover { text-decoration: underline; }\n"
        ".dir { color: #2196F3; }\n"
        ".file { color: #333; }\n"
        ".size { color: #999; margin-left: 2em; }\n"
        "</style>\n</head>\n<body>\n"
        "<h2>Directory: %s</h2>\n<hr>\n<pre>\n",
        url_path, url_path);

    /* parent directory link */
    if (strcmp(url_path, "/") != 0) {
        len += snprintf(html + len, cap - len,
            "<a class=\"dir\" href=\"..\">..</a>\n");
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);

        /* grow buffer if needed */
        if (len + 512 >= cap) {
            cap *= 2;
            html = realloc(html, cap);
        }

        if (is_dir) {
            len += snprintf(html + len, cap - len,
                "<a class=\"dir\" href=\"%s/\">%s/</a>\n",
                entry->d_name, entry->d_name);
        } else {
            char size_str[32];
            if (st.st_size < 1024)
                snprintf(size_str, sizeof(size_str), "%ldB",
                         (long)st.st_size);
            else if (st.st_size < 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%.1fK",
                         st.st_size / 1024.0);
            else
                snprintf(size_str, sizeof(size_str), "%.1fM",
                         st.st_size / (1024.0 * 1024.0));

            len += snprintf(html + len, cap - len,
                "<a class=\"file\" href=\"%s\">%s</a>"
                "<span class=\"size\">%s</span>\n",
                entry->d_name, entry->d_name, size_str);
        }
    }
    closedir(d);

    len += snprintf(html + len, cap - len,
        "</pre>\n<hr>\n<small>xs-http/1.0</small>\n"
        "</body>\n</html>\n");

    http_response_content_type(res, "text/html; charset=utf-8");
    http_response_body(res, html, len);
    free(html);
    return 0;
}

static int serve_static_file(const char *base_dir, const char *rel_path,
                             HTTPRequest *req, HTTPResponse *res) {
    (void)req;

    if (!is_safe_path(rel_path)) {
        http_response_status(res, 403);
        http_response_body_str(res, "403 Forbidden");
        return 0;
    }

    char filepath[2048];
    if (rel_path[0] == '/')
        snprintf(filepath, sizeof(filepath), "%s%s", base_dir, rel_path);
    else
        snprintf(filepath, sizeof(filepath), "%s/%s", base_dir, rel_path);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        return -1;  /* file not found */
    }

    /* directory? */
    if (S_ISDIR(st.st_mode)) {
        /* try index.html first */
        char index_path[2048];
        snprintf(index_path, sizeof(index_path), "%s/index.html", filepath);
        if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
            strcpy(filepath, index_path);
        } else {
            return serve_directory_listing(filepath, rel_path, res);
        }
    }

    /* read file */
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        http_response_status(res, 500);
        http_response_body_str(res, "500 Internal Server Error");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size > HTTP_MAX_BODY) {
        fclose(f);
        http_response_status(res, 413);
        http_response_body_str(res, "413 Payload Too Large");
        return 0;
    }

    char *file_data = malloc(file_size + 1);
    size_t nread = fread(file_data, 1, file_size, f);
    fclose(f);
    file_data[nread] = '\0';

    const char *mime = mime_type_for_ext(filepath);
    http_response_content_type(res, mime);
    http_response_body(res, file_data, (int)nread);
    free(file_data);

    /* cache control for static files */
    http_response_header(res, "Cache-Control", "public, max-age=3600");

    return 0;
}

/* check all static mount points */
static int try_serve_static(Router *r, HTTPRequest *req, HTTPResponse *res) {
    for (int i = 0; i < r->nstatics; i++) {
        const char *prefix = r->statics[i].prefix;
        const char *dir = r->statics[i].dir;

        int prefix_len = (int)strlen(prefix);
        if (strncmp(req->path, prefix, prefix_len) != 0)
            continue;

        const char *rel = req->path + prefix_len;
        if (*rel == '\0') rel = "/";

        if (serve_static_file(dir, rel, req, res) == 0)
            return 1;  /* served */
    }
    return 0;
}

/* ================================================================
 *  Default 404 handler
 * ================================================================ */

static void default_not_found(HTTPRequest *req, HTTPResponse *res,
                              void *ctx) {
    (void)ctx;
    http_response_status(res, 404);
    http_response_content_type(res, "text/html; charset=utf-8");

    char body[512];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n<html><head><title>404</title></head>\n"
        "<body><h1>404 Not Found</h1>\n"
        "<p>The requested URL <code>%s</code> was not found.</p>\n"
        "<hr><small>xs-http/1.0</small></body></html>\n",
        req->path);
    http_response_body_str(res, body);
}

/* ================================================================
 *  Connection handling
 * ================================================================ */

static HTTPConnection *conn_new(int fd, HTTPServer *server) {
    HTTPConnection *c = calloc(1, sizeof(HTTPConnection));
    c->fd = fd;
    c->server = server;
    c->read_cap = 8192;
    c->read_buf = malloc(c->read_cap);
    c->read_len = 0;
    c->state = 0;
    c->connected_at = evloop_now_ms();
    return c;
}

static void conn_free(HTTPConnection *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->read_buf);
    free(c);
}

/* remove connection from server's list */
static void server_remove_conn(HTTPServer *s, HTTPConnection *c) {
    for (int i = 0; i < s->nconns; i++) {
        if (s->conns[i] == c) {
            s->conns[i] = s->conns[s->nconns - 1];
            s->nconns--;
            break;
        }
    }
}

/* process a complete HTTP request on a connection */
static void process_request(HTTPConnection *c) {
    HTTPServer *s = c->server;
    HTTPRequest req;
    HTTPResponse res;

    int parsed = http_parse_request(c->read_buf, c->read_len, &req);
    if (parsed < 0) {
        /* bad request */
        http_response_init(&res);
        http_response_status(&res, 400);
        http_response_body_str(&res, "400 Bad Request");

        char out_buf[4096];
        int out_len = http_format_response(&res, out_buf, sizeof(out_buf));
        (void)write(c->fd, out_buf, out_len);

        http_response_free(&res);
        return;
    }

    http_response_init(&res);

    /* run middleware chain */
    int proceed = 1;
    for (int i = 0; i < s->router->nmiddlewares; i++) {
        int result = s->router->middlewares[i].fn(
            &req, &res, s->router->middlewares[i].ctx);
        if (result != 0) {
            proceed = 0;
            break;
        }
    }

    if (proceed) {
        /* try static file serving first */
        if (!try_serve_static(s->router, &req, &res)) {
            /* match a route */
            Route *route = router_match(s->router, req.method,
                                        req.path, &req);
            if (route) {
                route->handler(&req, &res, route->handler_ctx);
            } else if (s->router->not_found_handler) {
                s->router->not_found_handler(
                    &req, &res, s->router->not_found_ctx);
            } else {
                default_not_found(&req, &res, NULL);
            }
        }
    }

    /* add connection header */
    if (req.keep_alive) {
        http_response_header(&res, "Connection", "keep-alive");
    } else {
        http_response_header(&res, "Connection", "close");
    }

    /* format and send response */
    int response_size = res.body_len + 2048;
    char *out_buf = malloc(response_size);
    int out_len = http_format_response(&res, out_buf, response_size);

    /* write response (may need multiple writes for large responses) */
    int total_written = 0;
    while (total_written < out_len) {
        int n = (int)write(c->fd, out_buf + total_written,
                           out_len - total_written);
        if (n <= 0) break;
        total_written += n;
    }

    free(out_buf);
    s->request_count++;

    /* access log */
    if (s->access_log) {
        char date_buf[64];
        format_date(date_buf, sizeof(date_buf));
        fprintf(stderr, "%s %s %s %d %d\n",
                c->remote_addr, req.method, req.path,
                res.status, res.body_len);
    }

    /* consume processed data from buffer */
    if (parsed > 0 && parsed < c->read_len) {
        memmove(c->read_buf, c->read_buf + parsed,
                c->read_len - parsed);
        c->read_len -= parsed;
    } else {
        c->read_len = 0;
    }

    http_request_free(&req);
    http_response_free(&res);
}

/* I/O callback for client connections */
static void on_client_readable(int fd, EventType ev, void *ctx) {
    HTTPConnection *c = (HTTPConnection *)ctx;
    HTTPServer *s = c->server;
    (void)ev;

    /* grow buffer if needed */
    if (c->read_len >= c->read_cap - 1) {
        c->read_cap *= 2;
        if (c->read_cap > HTTP_MAX_BODY + 8192)
            c->read_cap = HTTP_MAX_BODY + 8192;
        c->read_buf = realloc(c->read_buf, c->read_cap);
    }

    int n = (int)read(fd, c->read_buf + c->read_len,
                      c->read_cap - c->read_len - 1);
    if (n <= 0) {
        /* client disconnected or error */
        evloop_remove_fd(s->evloop, fd);
        server_remove_conn(s, c);
        conn_free(c);
        return;
    }

    c->read_len += n;
    c->read_buf[c->read_len] = '\0';

    /* check if we have complete headers */
    if (strstr(c->read_buf, "\r\n\r\n")) {
        /* check if we need to wait for body */
        const char *cl_str = NULL;
        const char *hdr = c->read_buf;
        const char *hdr_end = strstr(hdr, "\r\n\r\n");

        /* quick scan for Content-Length */
        const char *cl_pos = strcasestr(hdr, "Content-Length:");
        if (cl_pos && cl_pos < hdr_end) {
            cl_str = cl_pos + 15;
            while (*cl_str == ' ') cl_str++;
            int content_len = atoi(cl_str);
            int header_size = (int)(hdr_end - hdr) + 4;
            int total_needed = header_size + content_len;
            if (c->read_len < total_needed) {
                return;  /* wait for more body data */
            }
        }

        process_request(c);

        /* if not keep-alive, close */
        /* check the request's Connection header quickly */
        const char *conn_hdr = strcasestr(c->read_buf, "Connection:");
        int should_close = 0;
        if (conn_hdr) {
            const char *val = conn_hdr + 11;
            while (*val == ' ') val++;
            if (strncasecmp(val, "close", 5) == 0)
                should_close = 1;
        }

        if (should_close || c->read_len == 0) {
            evloop_remove_fd(s->evloop, fd);
            server_remove_conn(s, c);
            conn_free(c);
        }
    }
}

/* accept callback */
static void on_accept(int listen_fd, EventType ev, void *ctx) {
    HTTPServer *s = (HTTPServer *)ctx;
    (void)ev;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }

    /* set non-blocking */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    /* disable Nagle's algorithm */
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* create connection */
    HTTPConnection *c = conn_new(client_fd, s);
    inet_ntop(AF_INET, &addr.sin_addr, c->remote_addr,
              sizeof(c->remote_addr));
    c->remote_port = ntohs(addr.sin_port);

    /* add to server's connection list */
    if (s->nconns >= s->conn_cap) {
        s->conn_cap = s->conn_cap ? s->conn_cap * 2 : 64;
        s->conns = realloc(s->conns,
                           sizeof(HTTPConnection *) * s->conn_cap);
    }
    s->conns[s->nconns++] = c;

    /* register with event loop */
    evloop_add_fd(s->evloop, client_fd, EV_READ, on_client_readable, c);
}

/* ================================================================
 *  Server API
 * ================================================================ */

HTTPServer *http_server_new(int port) {
    HTTPServer *s = calloc(1, sizeof(HTTPServer));
    s->port = port;
    s->listen_fd = -1;
    s->running = 0;
    s->max_connections = 1024;
    s->access_log = 1;

    s->evloop = evloop_new();
    s->router = router_new();

    return s;
}

void http_server_free(HTTPServer *s) {
    if (!s) return;

    /* close all connections */
    for (int i = 0; i < s->nconns; i++) {
        conn_free(s->conns[i]);
    }
    free(s->conns);

    if (s->listen_fd >= 0) close(s->listen_fd);

    router_free(s->router);
    evloop_free(s->evloop);
    free(s);
}

void http_server_route(HTTPServer *s, const char *method,
                       const char *pattern, HTTPHandler handler,
                       void *ctx) {
    router_add(s->router, method, pattern, handler, ctx);
}

void http_server_middleware(HTTPServer *s, MiddlewareFunc fn, void *ctx) {
    router_add_middleware(s->router, fn, ctx);
}

void http_server_static(HTTPServer *s, const char *prefix,
                        const char *dir) {
    router_add_static(s->router, prefix, dir);
}

static void on_sigint(int fd, EventType ev, void *ctx) {
    (void)fd; (void)ev;
    HTTPServer *s = (HTTPServer *)ctx;
    fprintf(stderr, "\nShutting down...\n");
    http_server_stop(s);
}

int http_server_start(HTTPServer *s) {
    if (!s) return -1;

    /* port 0 means don't actually bind (for testing) */
    if (s->port == 0) return 0;

    /* create listening socket */
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /* set socket options */
    int opt = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    /* set non-blocking */
    int flags = fcntl(s->listen_fd, F_GETFL, 0);
    fcntl(s->listen_fd, F_SETFL, flags | O_NONBLOCK);

    /* bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(s->port);

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    /* listen */
    if (listen(s->listen_fd, 128) < 0) {
        perror("listen");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    /* get actual port (in case of ephemeral) */
    socklen_t alen = sizeof(addr);
    getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen);
    s->port = ntohs(addr.sin_port);

    fprintf(stderr, "xs-http listening on http://0.0.0.0:%d\n", s->port);

    /* register with event loop */
    evloop_add_fd(s->evloop, s->listen_fd, EV_READ, on_accept, s);

    /* handle SIGINT for graceful shutdown */
    evloop_add_signal(s->evloop, SIGINT, on_sigint, s);

    /* ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* run event loop */
    s->running = 1;
    evloop_run(s->evloop);

    return 0;
}

void http_server_stop(HTTPServer *s) {
    if (!s) return;
    s->running = 0;
    evloop_stop(s->evloop);
}


/* ================================================================
 *  JSON encoding helper (for res.json)
 * ================================================================ */

/* minimal JSON string escaper */
static void json_escape_str(const char *s, char *out, int out_size) {
    int o = 0;
    out[o++] = '"';
    for (int i = 0; s[i] && o < out_size - 3; i++) {
        char c = s[i];
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b'; break;
            case '\f': out[o++] = '\\'; out[o++] = 'f'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if ((unsigned char)c < 0x20) {
                    o += snprintf(out + o, out_size - o, "\\u%04x", c);
                } else {
                    out[o++] = c;
                }
                break;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* ================================================================
 *  XS language bindings (called from builtins.c)
 * ================================================================ */

/*
 * These are C functions used by the XS http module bindings.
 * The actual NativeFn wrappers are in builtins.c, which calls
 * into these functions.
 */

/* convenience: send a JSON-encoded string (for XS res.json) */
void http_response_json_str(HTTPResponse *res, const char *json) {
    http_response_content_type(res, "application/json; charset=utf-8");
    http_response_body_str(res, json);
}

/* convenience: redirect */
void http_response_redirect(HTTPResponse *res, const char *url, int code) {
    if (code == 0) code = 302;
    http_response_status(res, code);
    http_response_header(res, "Location", url);

    char body[512];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
        "</head><body>Redirecting to <a href=\"%s\">%s</a></body></html>",
        url, url, url);
    http_response_content_type(res, "text/html; charset=utf-8");
    http_response_body_str(res, body);
}

/* convenience: send a file */
int http_response_send_file(HTTPResponse *res, const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    if (st.st_size > HTTP_MAX_BODY)
        return -2;

    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    char *data = malloc(st.st_size + 1);
    size_t nread = fread(data, 1, st.st_size, f);
    fclose(f);
    data[nread] = '\0';

    const char *mime = mime_type_for_ext(filepath);
    http_response_content_type(res, mime);
    http_response_body(res, data, (int)nread);
    free(data);

    return 0;
}

/* ================================================================
 *  Utility: simple form data parser (application/x-www-form-urlencoded)
 * ================================================================ */

typedef struct {
    char name[128];
    char value[2048];
} FormField;

typedef struct {
    FormField *fields;
    int nfields;
    int cap;
} FormData;

FormData *parse_form_data(const char *body) {
    if (!body || !*body) return NULL;

    FormData *fd = calloc(1, sizeof(FormData));
    fd->cap = 16;
    fd->fields = calloc(fd->cap, sizeof(FormField));

    char buf[HTTP_MAX_BODY];
    strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, "&", &saveptr);

    while (token && fd->nfields < fd->cap) {
        char *eq = strchr(token, '=');
        int idx = fd->nfields;

        if (eq) {
            *eq = '\0';
            url_decode(token, fd->fields[idx].name,
                       sizeof(fd->fields[idx].name));
            url_decode(eq + 1, fd->fields[idx].value,
                       sizeof(fd->fields[idx].value));
        } else {
            url_decode(token, fd->fields[idx].name,
                       sizeof(fd->fields[idx].name));
            fd->fields[idx].value[0] = '\0';
        }
        fd->nfields++;
        token = strtok_r(NULL, "&", &saveptr);
    }

    return fd;
}

void form_data_free(FormData *fd) {
    if (!fd) return;
    free(fd->fields);
    free(fd);
}

const char *form_data_get(FormData *fd, const char *name) {
    if (!fd) return NULL;
    for (int i = 0; i < fd->nfields; i++) {
        if (strcmp(fd->fields[i].name, name) == 0)
            return fd->fields[i].value;
    }
    return NULL;
}

/* ================================================================
 *  Cookie parsing and building
 * ================================================================ */

typedef struct {
    char name[128];
    char value[1024];
    char path[256];
    char domain[256];
    int max_age;
    int secure;
    int http_only;
    char same_site[16];
} HTTPCookie;

int http_parse_cookies(const HTTPRequest *req, HTTPCookie *cookies, int max_cookies) {
    int count = 0;
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, "Cookie") != 0) continue;

        const char *p = req->headers[i].value;
        while (*p && count < max_cookies) {
            while (*p == ' ' || *p == ';') p++;
            if (!*p) break;

            const char *eq = strchr(p, '=');
            if (!eq) break;

            int nlen = (int)(eq - p);
            if (nlen <= 0 || nlen >= (int)sizeof(cookies[count].name))
                nlen = (int)sizeof(cookies[count].name) - 1;
            memcpy(cookies[count].name, p, nlen);
            cookies[count].name[nlen] = '\0';

            p = eq + 1;
            const char *end = strchr(p, ';');
            int vlen;
            if (end) {
                vlen = (int)(end - p);
            } else {
                vlen = (int)strlen(p);
            }
            if (vlen >= (int)sizeof(cookies[count].value))
                vlen = (int)sizeof(cookies[count].value) - 1;
            memcpy(cookies[count].value, p, vlen);
            cookies[count].value[vlen] = '\0';

            cookies[count].path[0] = '\0';
            cookies[count].domain[0] = '\0';
            cookies[count].max_age = -1;
            cookies[count].secure = 0;
            cookies[count].http_only = 0;
            cookies[count].same_site[0] = '\0';

            count++;
            p += vlen;
        }
    }
    return count;
}

int http_set_cookie(HTTPResponse *res, const HTTPCookie *cookie) {
    if (!res || !cookie || !cookie->name[0]) return -1;

    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "%s=%s", cookie->name, cookie->value);

    if (cookie->path[0]) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; Path=%s", cookie->path);
    }
    if (cookie->domain[0]) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; Domain=%s", cookie->domain);
    }
    if (cookie->max_age >= 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; Max-Age=%d", cookie->max_age);
    }
    if (cookie->secure) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; Secure");
    }
    if (cookie->http_only) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; HttpOnly");
    }
    if (cookie->same_site[0]) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "; SameSite=%s", cookie->same_site);
    }
    (void)pos;

    http_response_header(res, "Set-Cookie", buf);
    return 0;
}

const char *http_get_cookie(const HTTPRequest *req, const char *name) {
    static char cookie_buf[1024];
    HTTPCookie cookies[32];
    int n = http_parse_cookies(req, cookies, 32);
    for (int i = 0; i < n; i++) {
        if (strcmp(cookies[i].name, name) == 0) {
            strncpy(cookie_buf, cookies[i].value, sizeof(cookie_buf) - 1);
            cookie_buf[sizeof(cookie_buf) - 1] = '\0';
            return cookie_buf;
        }
    }
    return NULL;
}

/* ================================================================
 *  CORS (Cross-Origin Resource Sharing) helpers
 * ================================================================ */

typedef struct {
    char allowed_origins[1024];
    char allowed_methods[256];
    char allowed_headers[512];
    int allow_credentials;
    int max_age;
    char exposed_headers[512];
} CORSConfig;

static CORSConfig default_cors = {
    .allowed_origins = "*",
    .allowed_methods = "GET, POST, PUT, DELETE, PATCH, OPTIONS",
    .allowed_headers = "Content-Type, Authorization, X-Requested-With",
    .allow_credentials = 0,
    .max_age = 86400,
    .exposed_headers = ""
};

void http_cors_init(CORSConfig *config) {
    *config = default_cors;
}

void http_cors_allow_origin(CORSConfig *config, const char *origin) {
    strncpy(config->allowed_origins, origin, sizeof(config->allowed_origins) - 1);
}

void http_cors_allow_methods(CORSConfig *config, const char *methods) {
    strncpy(config->allowed_methods, methods, sizeof(config->allowed_methods) - 1);
}

void http_cors_allow_headers(CORSConfig *config, const char *headers) {
    strncpy(config->allowed_headers, headers, sizeof(config->allowed_headers) - 1);
}

int http_cors_apply(const CORSConfig *config, const HTTPRequest *req,
                     HTTPResponse *res)
{
    if (!config || !req || !res) return -1;

    const char *origin = NULL;
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, "Origin") == 0) {
            origin = req->headers[i].value;
            break;
        }
    }

    if (!origin) return 0;

    if (strcmp(config->allowed_origins, "*") == 0) {
        http_response_header(res, "Access-Control-Allow-Origin", "*");
    } else {
        if (strstr(config->allowed_origins, origin)) {
            http_response_header(res, "Access-Control-Allow-Origin", origin);
            http_response_header(res, "Vary", "Origin");
        } else {
            return 0;
        }
    }

    if (config->allow_credentials) {
        http_response_header(res, "Access-Control-Allow-Credentials", "true");
    }

    if (config->exposed_headers[0]) {
        http_response_header(res, "Access-Control-Expose-Headers",
                           config->exposed_headers);
    }

    if (strcasecmp(req->method, "OPTIONS") == 0) {
        http_response_header(res, "Access-Control-Allow-Methods",
                           config->allowed_methods);
        http_response_header(res, "Access-Control-Allow-Headers",
                           config->allowed_headers);
        char age_buf[32];
        snprintf(age_buf, sizeof(age_buf), "%d", config->max_age);
        http_response_header(res, "Access-Control-Max-Age", age_buf);
        http_response_status(res, 204);
        return 1;
    }

    return 0;
}

/* ================================================================
 *  JSON response helpers
 * ================================================================ */

static void json_write_escaped(char *out, int *pos, int cap, const char *s) {
    for (; *s && *pos < cap - 6; s++) {
        switch (*s) {
            case '"':  out[(*pos)++] = '\\'; out[(*pos)++] = '"'; break;
            case '\\': out[(*pos)++] = '\\'; out[(*pos)++] = '\\'; break;
            case '\n': out[(*pos)++] = '\\'; out[(*pos)++] = 'n'; break;
            case '\r': out[(*pos)++] = '\\'; out[(*pos)++] = 'r'; break;
            case '\t': out[(*pos)++] = '\\'; out[(*pos)++] = 't'; break;
            default:
                if ((unsigned char)*s < 0x20) {
                    *pos += snprintf(out + *pos, cap - *pos, "\\u%04x",
                                    (unsigned char)*s);
                } else {
                    out[(*pos)++] = *s;
                }
                break;
        }
    }
}

void http_response_json(HTTPResponse *res, const char *json_str) {
    http_response_content_type(res, "application/json; charset=utf-8");
    http_response_body_str(res, json_str);
}

void http_response_json_error(HTTPResponse *res, int status, const char *message) {
    http_response_status(res, status);
    http_response_content_type(res, "application/json; charset=utf-8");

    int cap = (int)strlen(message) * 2 + 128;
    char *buf = malloc(cap);
    int pos = 0;
    pos += snprintf(buf, cap, "{\"error\":");

    buf[pos++] = '"';
    json_write_escaped(buf, &pos, cap, message);
    buf[pos++] = '"';

    pos += snprintf(buf + pos, cap - pos, ",\"status\":%d}", status);
    http_response_body(res, buf, pos);
    free(buf);
}

void http_response_redirect_perm(HTTPResponse *res, const char *url, int permanent) {
    http_response_status(res, permanent ? 301 : 302);
    http_response_header(res, "Location", url);
    http_response_body_str(res, "");
}

/* ================================================================
 *  Request body content type detection
 * ================================================================ */

typedef enum {
    CT_UNKNOWN = 0,
    CT_JSON,
    CT_FORM_URLENCODED,
    CT_MULTIPART,
    CT_TEXT_PLAIN,
    CT_TEXT_HTML,
    CT_XML,
    CT_OCTET_STREAM
} ContentType;

ContentType http_request_content_type(const HTTPRequest *req) {
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, "Content-Type") != 0) continue;
        const char *ct = req->headers[i].value;
        if (strstr(ct, "application/json")) return CT_JSON;
        if (strstr(ct, "application/x-www-form-urlencoded")) return CT_FORM_URLENCODED;
        if (strstr(ct, "multipart/form-data")) return CT_MULTIPART;
        if (strstr(ct, "text/plain")) return CT_TEXT_PLAIN;
        if (strstr(ct, "text/html")) return CT_TEXT_HTML;
        if (strstr(ct, "application/xml") || strstr(ct, "text/xml")) return CT_XML;
        if (strstr(ct, "application/octet-stream")) return CT_OCTET_STREAM;
    }
    return CT_UNKNOWN;
}

/* ================================================================
 *  Rate limiter (token bucket)
 * ================================================================ */

#define RATE_LIMIT_BUCKETS 256

typedef struct {
    uint32_t ip_hash;
    double tokens;
    int64_t last_refill;
    int active;
} RateBucket;

typedef struct {
    RateBucket buckets[RATE_LIMIT_BUCKETS];
    double rate;
    double burst;
} RateLimiter;

void rate_limiter_init(RateLimiter *rl, double requests_per_sec, double burst) {
    memset(rl, 0, sizeof(RateLimiter));
    rl->rate = requests_per_sec;
    rl->burst = burst;
    for (int i = 0; i < RATE_LIMIT_BUCKETS; i++) {
        rl->buckets[i].tokens = burst;
    }
}

static uint32_t ip_hash(const char *addr) {
    uint32_t h = 0;
    while (*addr) {
        h = h * 31 + (unsigned char)*addr;
        addr++;
    }
    return h;
}

int rate_limiter_allow(RateLimiter *rl, const char *remote_addr, int64_t now_ms) {
    uint32_t h = ip_hash(remote_addr);
    int idx = (int)(h % RATE_LIMIT_BUCKETS);
    RateBucket *b = &rl->buckets[idx];

    if (!b->active || b->ip_hash != h) {
        b->ip_hash = h;
        b->tokens = rl->burst;
        b->last_refill = now_ms;
        b->active = 1;
    }

    double elapsed = (double)(now_ms - b->last_refill) / 1000.0;
    b->tokens += elapsed * rl->rate;
    if (b->tokens > rl->burst) b->tokens = rl->burst;
    b->last_refill = now_ms;

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        return 1;
    }
    return 0;
}

/* ================================================================
 *  Request header helpers
 * ================================================================ */

const char *http_request_header(const HTTPRequest *req, const char *name) {
    for (int i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

int http_request_accepts(const HTTPRequest *req, const char *mime) {
    const char *accept = http_request_header(req, "Accept");
    if (!accept) return 0;
    if (strstr(accept, mime)) return 1;
    if (strstr(accept, "*/*")) return 1;
    return 0;
}

int http_request_is_ajax(const HTTPRequest *req) {
    const char *xhr = http_request_header(req, "X-Requested-With");
    return xhr && strcasecmp(xhr, "XMLHttpRequest") == 0;
}

const char *http_request_param(const HTTPRequest *req, const char *name) {
    for (int i = 0; i < req->nparams; i++) {
        if (strcmp(req->params[i].name, name) == 0)
            return req->params[i].value;
    }
    return NULL;
}

const char *http_request_query_param(const HTTPRequest *req, const char *name) {
    for (int i = 0; i < req->nquery_params; i++) {
        if (strcmp(req->query_params[i].name, name) == 0)
            return req->query_params[i].value;
    }
    return NULL;
}

/* ================================================================
 *  Response caching headers
 * ================================================================ */

void http_response_cache_control(HTTPResponse *res, const char *directive) {
    http_response_header(res, "Cache-Control", directive);
}

void http_response_no_cache(HTTPResponse *res) {
    http_response_header(res, "Cache-Control", "no-store, no-cache, must-revalidate");
    http_response_header(res, "Pragma", "no-cache");
    http_response_header(res, "Expires", "0");
}

void http_response_etag(HTTPResponse *res, const char *etag) {
    char buf[256];
    snprintf(buf, sizeof(buf), "\"%s\"", etag);
    http_response_header(res, "ETag", buf);
}

int http_check_etag(const HTTPRequest *req, const char *etag) {
    const char *if_none = http_request_header(req, "If-None-Match");
    if (!if_none) return 0;
    char quoted[256];
    snprintf(quoted, sizeof(quoted), "\"%s\"", etag);
    return strstr(if_none, quoted) != NULL;
}

/* ================================================================
 *  Basic auth parsing
 * ================================================================ */

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *input, char *output, int out_size) {
    int len = (int)strlen(input);
    int i = 0, j = 0;

    while (i < len && j < out_size - 1) {
        int a = base64_decode_char(input[i++]);
        int b = (i < len) ? base64_decode_char(input[i++]) : 0;
        int c = (i < len) ? base64_decode_char(input[i++]) : 0;
        int d = (i < len) ? base64_decode_char(input[i++]) : 0;

        if (a < 0) a = 0;
        if (b < 0) b = 0;
        if (c < 0) c = 0;
        if (d < 0) d = 0;

        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                          ((uint32_t)c << 6) | (uint32_t)d;

        if (j < out_size - 1) output[j++] = (char)((triple >> 16) & 0xFF);
        if (j < out_size - 1) output[j++] = (char)((triple >> 8) & 0xFF);
        if (j < out_size - 1) output[j++] = (char)(triple & 0xFF);
    }

    int padding = 0;
    if (len > 0 && input[len - 1] == '=') padding++;
    if (len > 1 && input[len - 2] == '=') padding++;
    j -= padding;
    if (j < 0) j = 0;

    output[j] = '\0';
    return j;
}

int http_parse_basic_auth(const HTTPRequest *req, char *user, int user_size,
                           char *pass, int pass_size)
{
    const char *auth = http_request_header(req, "Authorization");
    if (!auth) return -1;

    if (strncasecmp(auth, "Basic ", 6) != 0) return -1;
    auth += 6;
    while (*auth == ' ') auth++;

    char decoded[512];
    int dlen = base64_decode(auth, decoded, sizeof(decoded));
    if (dlen <= 0) return -1;

    char *colon = strchr(decoded, ':');
    if (!colon) return -1;

    int ulen = (int)(colon - decoded);
    if (ulen >= user_size) ulen = user_size - 1;
    memcpy(user, decoded, ulen);
    user[ulen] = '\0';

    int plen = dlen - ulen - 1;
    if (plen >= pass_size) plen = pass_size - 1;
    if (plen < 0) plen = 0;
    memcpy(pass, colon + 1, plen);
    pass[plen] = '\0';

    return 0;
}

/* ================================================================
 *  IP address parsing and matching
 * ================================================================ */

typedef struct {
    uint32_t addr;
    uint32_t mask;
} IPRange;

static uint32_t parse_ipv4(const char *s) {
    uint32_t result = 0;
    int parts = 0;
    const char *p = s;

    while (*p && parts < 4) {
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val > 255) val = 255;
        result = (result << 8) | (uint32_t)val;
        parts++;
        if (*p == '.') p++;
    }

    while (parts < 4) {
        result <<= 8;
        parts++;
    }

    return result;
}

int parse_cidr(const char *cidr, IPRange *range) {
    char ip_part[64];
    const char *slash = strchr(cidr, '/');
    if (!slash) {
        range->addr = parse_ipv4(cidr);
        range->mask = 0xFFFFFFFF;
        return 0;
    }

    int iplen = (int)(slash - cidr);
    if (iplen >= (int)sizeof(ip_part)) return -1;
    memcpy(ip_part, cidr, iplen);
    ip_part[iplen] = '\0';

    range->addr = parse_ipv4(ip_part);
    int prefix = atoi(slash + 1);
    if (prefix < 0) prefix = 0;
    if (prefix > 32) prefix = 32;
    range->mask = prefix == 0 ? 0 : (0xFFFFFFFF << (32 - prefix));
    range->addr &= range->mask;

    return 0;
}

int ip_in_range(const char *ip_str, const IPRange *range) {
    uint32_t ip = parse_ipv4(ip_str);
    return (ip & range->mask) == range->addr;
}

/* ================================================================
 *  Chunked transfer encoding writer
 * ================================================================ */

typedef struct {
    int fd;
    int headers_written;
    HTTPResponse *res;
} ChunkedWriter;

void chunked_writer_init(ChunkedWriter *cw, int fd, HTTPResponse *res) {
    cw->fd = fd;
    cw->headers_written = 0;
    cw->res = res;
}

int chunked_writer_begin(ChunkedWriter *cw) {
    http_response_header(cw->res, "Transfer-Encoding", "chunked");
    char header_buf[4096];
    int hlen = http_format_response(cw->res, header_buf, sizeof(header_buf));
    if (hlen <= 0) return -1;
    ssize_t w = write(cw->fd, header_buf, hlen);
    cw->headers_written = 1;
    return (int)w;
}

int chunked_writer_write(ChunkedWriter *cw, const char *data, int len) {
    if (!cw->headers_written) chunked_writer_begin(cw);

    char size_line[32];
    int slen = snprintf(size_line, sizeof(size_line), "%x\r\n", len);
    ssize_t w1 = write(cw->fd, size_line, slen);
    ssize_t w2 = write(cw->fd, data, len);
    ssize_t w3 = write(cw->fd, "\r\n", 2);
    return (int)(w1 + w2 + w3);
}

int chunked_writer_end(ChunkedWriter *cw) {
    ssize_t w = write(cw->fd, "0\r\n\r\n", 5);
    return (int)w;
}

/* ================================================================
 *  Server-sent events (SSE) support
 * ================================================================ */

typedef struct {
    int fd;
    int active;
    int64_t last_id;
} SSEConnection;

int sse_begin(int fd) {
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    ssize_t w = write(fd, headers, strlen(headers));
    return (int)w;
}

int sse_send_event(int fd, const char *event, const char *data, int64_t id) {
    char buf[8192];
    int pos = 0;

    if (id >= 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "id: %lld\n",
                       (long long)id);
    }
    if (event && event[0]) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "event: %s\n", event);
    }

    const char *p = data;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (nl) {
            int llen = (int)(nl - p);
            if (pos + llen + 8 >= (int)sizeof(buf)) break;
            memcpy(buf + pos, "data: ", 6);
            pos += 6;
            memcpy(buf + pos, p, llen);
            pos += llen;
            buf[pos++] = '\n';
            p = nl + 1;
        } else {
            int llen = (int)strlen(p);
            if (pos + llen + 8 >= (int)sizeof(buf)) break;
            memcpy(buf + pos, "data: ", 6);
            pos += 6;
            memcpy(buf + pos, p, llen);
            pos += llen;
            buf[pos++] = '\n';
            break;
        }
    }

    buf[pos++] = '\n';
    ssize_t w = write(fd, buf, pos);
    return (int)w;
}

int sse_send_retry(int fd, int retry_ms) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "retry: %d\n\n", retry_ms);
    ssize_t w = write(fd, buf, len);
    return (int)w;
}

/* ================================================================
 *  WebSocket frame handling (basic)
 * ================================================================ */

#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

typedef struct {
    int fin;
    int opcode;
    int masked;
    uint64_t payload_len;
    uint8_t mask_key[4];
} WSFrame;

int ws_parse_frame(const uint8_t *data, int len, WSFrame *frame) {
    if (len < 2) return -1;

    frame->fin = (data[0] >> 7) & 1;
    frame->opcode = data[0] & 0x0F;
    frame->masked = (data[1] >> 7) & 1;
    frame->payload_len = data[1] & 0x7F;

    int offset = 2;

    if (frame->payload_len == 126) {
        if (len < 4) return -1;
        frame->payload_len = ((uint64_t)data[2] << 8) | data[3];
        offset = 4;
    } else if (frame->payload_len == 127) {
        if (len < 10) return -1;
        frame->payload_len = 0;
        for (int i = 0; i < 8; i++) {
            frame->payload_len = (frame->payload_len << 8) | data[2 + i];
        }
        offset = 10;
    }

    if (frame->masked) {
        if (len < offset + 4) return -1;
        memcpy(frame->mask_key, data + offset, 4);
        offset += 4;
    }

    return offset;
}

int ws_encode_frame(uint8_t *buf, int buf_size, int opcode,
                     const uint8_t *payload, int payload_len)
{
    int header_len = 2;
    if (payload_len >= 126 && payload_len < 65536) header_len = 4;
    else if (payload_len >= 65536) header_len = 10;

    if (header_len + payload_len > buf_size) return -1;

    buf[0] = 0x80 | (opcode & 0x0F);

    if (payload_len < 126) {
        buf[1] = (uint8_t)payload_len;
    } else if (payload_len < 65536) {
        buf[1] = 126;
        buf[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        buf[3] = (uint8_t)(payload_len & 0xFF);
    } else {
        buf[1] = 127;
        for (int i = 0; i < 8; i++) {
            buf[2 + i] = (uint8_t)((payload_len >> (56 - i * 8)) & 0xFF);
        }
    }

    memcpy(buf + header_len, payload, payload_len);
    return header_len + payload_len;
}

void ws_unmask(uint8_t *data, int len, const uint8_t *mask) {
    for (int i = 0; i < len; i++) {
        data[i] ^= mask[i & 3];
    }
}

/* ================================================================
 *  Path normalization and security
 * ================================================================ */

int http_normalize_path(const char *path, char *out, int out_size) {
    if (!path || !out || out_size <= 0) return -1;

    char parts[32][256];
    int nparts = 0;

    const char *p = path;
    while (*p == '/') p++;

    while (*p && nparts < 32) {
        const char *seg_end = strchr(p, '/');
        int slen;
        if (seg_end) {
            slen = (int)(seg_end - p);
        } else {
            slen = (int)strlen(p);
        }

        if (slen == 0 || (slen == 1 && p[0] == '.')) {
            /* skip empty and '.' */
        } else if (slen == 2 && p[0] == '.' && p[1] == '.') {
            if (nparts > 0) nparts--;
        } else {
            if (slen >= (int)sizeof(parts[0])) slen = (int)sizeof(parts[0]) - 1;
            memcpy(parts[nparts], p, slen);
            parts[nparts][slen] = '\0';
            nparts++;
        }

        if (seg_end) {
            p = seg_end + 1;
        } else {
            break;
        }
    }

    int pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < nparts && pos < out_size - 2; i++) {
        if (i > 0) out[pos++] = '/';
        int slen = (int)strlen(parts[i]);
        if (pos + slen >= out_size - 1) break;
        memcpy(out + pos, parts[i], slen);
        pos += slen;
    }
    out[pos] = '\0';
    return pos;
}

int http_path_is_safe(const char *path) {
    if (!path) return 0;
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    if (path[0] != '/') return 0;

    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) return 0;
        if (c == '\\') return 0;
    }
    return 1;
}
