#ifndef XS_PKG_HTTP_H
#define XS_PKG_HTTP_H

#include <stddef.h>

typedef struct {
    int   status;
    char *body;
    size_t body_len;
} PkgHttpResponse;

int  pkg_http_request(const char *method, const char *url,
                      const char *const *headers, int n_headers,
                      const char *body, size_t body_len,
                      PkgHttpResponse *out);
void pkg_http_response_free(PkgHttpResponse *r);

#endif
