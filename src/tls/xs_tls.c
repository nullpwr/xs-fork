#include "xs_tls.h"
#include "bearssl/bearssl.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#define ssize_t int
#define read(fd, buf, len) recv(fd, (char*)(buf), len, 0)
#define write(fd, buf, len) send(fd, (const char*)(buf), len, 0)
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

extern const unsigned char xs_ca_pem[];
extern const size_t        xs_ca_pem_len;

struct xs_tls_conn {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    int fd;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = read(fd, buf, len);
    if (n <= 0) return -1;
    return (int)n;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = write(fd, buf, len);
    if (n <= 0) return -1;
    return (int)n;
}

/* ---- policy flags ---- */

static int g_insecure_mode = -1; /* -1 = not yet initialised */

static void init_policy_once(void) {
    if (g_insecure_mode != -1) return;
    const char *e = getenv("XS_TLS_INSECURE");
    g_insecure_mode = (e && *e && *e != '0') ? 1 : 0;
}

void xs_tls_set_insecure(int on) { g_insecure_mode = on ? 1 : 0; }
int  xs_tls_is_insecure(void)    { init_policy_once(); return g_insecure_mode; }

/* ---- trust anchor table (parsed once from xs_ca_pem) ---- */

typedef struct {
    br_x509_trust_anchor *anchors;
    int n;
    int load_failed;
} TrustTable;

static TrustTable g_ta = {0};

/* DN collector used during x509 decoding */
typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
} DnBuf;

static void dn_append(void *ctx, const void *data, size_t len) {
    DnBuf *d = (DnBuf *)ctx;
    if (d->len + len > d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 128;
        while (nc < d->len + len) nc *= 2;
        unsigned char *nb = (unsigned char *)realloc(d->buf, nc);
        if (!nb) return;
        d->buf = nb; d->cap = nc;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;
}

/* PEM decoder state: emits DER per BEGIN CERTIFICATE block */
typedef struct {
    unsigned char *der;
    size_t der_len;
    size_t der_cap;
} DerBuf;

static void der_append(void *ctx, const void *data, size_t len) {
    DerBuf *d = (DerBuf *)ctx;
    if (d->der_len + len > d->der_cap) {
        size_t nc = d->der_cap ? d->der_cap * 2 : 1024;
        while (nc < d->der_len + len) nc *= 2;
        unsigned char *nb = (unsigned char *)realloc(d->der, nc);
        if (!nb) return;
        d->der = nb; d->der_cap = nc;
    }
    memcpy(d->der + d->der_len, data, len);
    d->der_len += len;
}

/* Convert one DER-encoded cert into a trust anchor (CA flag set).
   Allocates DN bytes and pkey byte arrays with plain malloc -- they live
   for the process lifetime. Returns 0 on success, -1 on failure. */
static int cert_to_anchor(const unsigned char *der, size_t der_len,
                          br_x509_trust_anchor *out) {
    br_x509_decoder_context dc;
    DnBuf dn = {0};
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) { free(dn.buf); return -1; }

    memset(out, 0, sizeof(*out));
    out->dn.data = dn.buf;   /* transfer ownership */
    out->dn.len  = dn.len;
    out->flags   = BR_X509_TA_CA;

    if (pk->key_type == BR_KEYTYPE_RSA) {
        size_t nl = pk->key.rsa.nlen;
        size_t el = pk->key.rsa.elen;
        unsigned char *nb = (unsigned char *)malloc(nl ? nl : 1);
        unsigned char *eb = (unsigned char *)malloc(el ? el : 1);
        if (!nb || !eb) { free(nb); free(eb); free(dn.buf); return -1; }
        memcpy(nb, pk->key.rsa.n, nl);
        memcpy(eb, pk->key.rsa.e, el);
        out->pkey.key_type      = BR_KEYTYPE_RSA;
        out->pkey.key.rsa.n     = nb;
        out->pkey.key.rsa.nlen  = nl;
        out->pkey.key.rsa.e     = eb;
        out->pkey.key.rsa.elen  = el;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        size_t ql = pk->key.ec.qlen;
        unsigned char *qb = (unsigned char *)malloc(ql ? ql : 1);
        if (!qb) { free(dn.buf); return -1; }
        memcpy(qb, pk->key.ec.q, ql);
        out->pkey.key_type      = BR_KEYTYPE_EC;
        out->pkey.key.ec.curve  = pk->key.ec.curve;
        out->pkey.key.ec.q      = qb;
        out->pkey.key.ec.qlen   = ql;
    } else {
        free(dn.buf); return -1;
    }
    return 0;
}

/* PEM decoder walks the bundle text, splits into certs, calls cert_to_anchor */
static void load_anchors(void) {
    if (g_ta.anchors || g_ta.load_failed) return;
    if (!xs_ca_pem || !xs_ca_pem_len) { g_ta.load_failed = 1; return; }

    br_pem_decoder_context pem;
    br_pem_decoder_init(&pem);

    DerBuf der = {0};
    int in_cert = 0;

    /* rough upper bound on anchor count: trust cacert has ~150 entries */
    int cap = 256;
    br_x509_trust_anchor *arr = (br_x509_trust_anchor *)calloc(cap, sizeof(*arr));
    if (!arr) { g_ta.load_failed = 1; return; }
    int n = 0;

    size_t off = 0;
    while (off < xs_ca_pem_len) {
        size_t pushed = br_pem_decoder_push(&pem,
            xs_ca_pem + off, xs_ca_pem_len - off);
        off += pushed;

        int ev = br_pem_decoder_event(&pem);
        switch (ev) {
        case BR_PEM_BEGIN_OBJ: {
            const char *name = br_pem_decoder_name(&pem);
            if (name && strcmp(name, "CERTIFICATE") == 0) {
                der.der_len = 0;
                br_pem_decoder_setdest(&pem, der_append, &der);
                in_cert = 1;
            } else {
                br_pem_decoder_setdest(&pem, NULL, NULL);
                in_cert = 0;
            }
            break;
        }
        case BR_PEM_END_OBJ:
            if (in_cert && der.der_len > 0) {
                if (n == cap) {
                    int nc = cap * 2;
                    br_x509_trust_anchor *nb = (br_x509_trust_anchor *)
                        realloc(arr, (size_t)nc * sizeof(*arr));
                    if (!nb) break;
                    memset(nb + cap, 0, (size_t)(nc - cap) * sizeof(*nb));
                    arr = nb; cap = nc;
                }
                if (cert_to_anchor(der.der, der.der_len, &arr[n]) == 0) {
                    n++;
                }
            }
            in_cert = 0;
            break;
        case BR_PEM_ERROR:
            /* skip malformed blocks; keep going */
            in_cert = 0;
            break;
        default:
            break;
        }
    }

    free(der.der);
    g_ta.anchors = arr;
    g_ta.n = n;
    if (n == 0) g_ta.load_failed = 1;
}

int xs_tls_anchor_count(void) {
    load_anchors();
    return g_ta.load_failed ? -1 : g_ta.n;
}

/* ---- legacy insecure vtable (used only when insecure mode is on) ---- */

typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context inner;
} insecure_x509_ctx;

static void ix_start_chain(const br_x509_class **ctx, const char *name) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)ctx;
    xc->inner.vtable->start_chain((const br_x509_class **)&xc->inner, name);
}
static void ix_start_cert(const br_x509_class **ctx, uint32_t length) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)ctx;
    xc->inner.vtable->start_cert((const br_x509_class **)&xc->inner, length);
}
static void ix_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)ctx;
    xc->inner.vtable->append((const br_x509_class **)&xc->inner, buf, len);
}
static void ix_end_cert(const br_x509_class **ctx) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)ctx;
    xc->inner.vtable->end_cert((const br_x509_class **)&xc->inner);
}
static unsigned ix_end_chain(const br_x509_class **ctx) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)ctx;
    xc->inner.vtable->end_chain((const br_x509_class **)&xc->inner);
    return 0; /* always trust */
}
static const br_x509_pkey *ix_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    insecure_x509_ctx *xc = (insecure_x509_ctx *)(void *)ctx;
    return xc->inner.vtable->get_pkey((const br_x509_class *const *)&xc->inner, usages);
}
static const br_x509_class insecure_vtable = {
    sizeof(insecure_x509_ctx),
    ix_start_chain, ix_start_cert, ix_append, ix_end_cert, ix_end_chain, ix_get_pkey
};
static insecure_x509_ctx g_ix509;

xs_tls_conn *xs_tls_connect(int fd, const char *hostname) {
    xs_tls_conn *c = calloc(1, sizeof(xs_tls_conn));
    if (!c) return NULL;
    c->fd = fd;

    init_policy_once();
    load_anchors();

    br_ssl_client_init_full(&c->sc, &c->xc, g_ta.anchors, (size_t)g_ta.n);

    if (g_insecure_mode || g_ta.load_failed || g_ta.n == 0) {
        /* wrap the minimal engine with a vtable that accepts any chain */
        memset(&g_ix509, 0, sizeof(g_ix509));
        g_ix509.vtable = &insecure_vtable;
        memcpy(&g_ix509.inner, &c->xc, sizeof(br_x509_minimal_context));
        br_ssl_engine_set_x509(&c->sc.eng, (const br_x509_class **)&g_ix509);
    }

    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
    br_ssl_client_reset(&c->sc, hostname, 0);
    br_sslio_init(&c->ioc, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);

    br_sslio_flush(&c->ioc);
    if (br_ssl_engine_current_state(&c->sc.eng) == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&c->sc.eng);
        if (err != 0)
            fprintf(stderr, "xs: tls error %d for %s\n", err, hostname);
        free(c);
        return NULL;
    }

    return c;
}

int xs_tls_read(xs_tls_conn *conn, void *buf, int len) {
    return br_sslio_read(&conn->ioc, buf, len);
}

int xs_tls_write(xs_tls_conn *conn, const void *buf, int len) {
    int r = br_sslio_write_all(&conn->ioc, buf, len);
    if (r < 0) return -1;
    br_sslio_flush(&conn->ioc);
    return len;
}

void xs_tls_close(xs_tls_conn *conn) {
    if (!conn) return;
    br_sslio_close(&conn->ioc);
    close(conn->fd);
    free(conn);
}
