/* http_tls.c -- BearSSL termination for the HTTP/1.1 server.
 *
 * The HTTPServer has two opaque slots for TLS state: server->tls_ctx
 * is the per-listener parsed cert + key (XsHttpTlsCtx below), and
 * each HTTPConnection has its own engine + io buffer when accepted
 * by a TLS listener (XsHttpTlsConn).
 *
 * Network I/O on a TLS connection goes through the engine: the
 * existing http_server.c calls into http_tls_recv / http_tls_send
 * via the conn_recv / conn_send macros that branch on tls_state.
 * The engine is drained eagerly after every recv so the HTTP
 * parser sees plaintext; pending encrypted bytes are flushed back
 * to the socket on the same callback.
 */
#define _POSIX_C_SOURCE 200809L
#include "net/http_server.h"
#include "bearssl_ssl.h"
#include "bearssl_pem.h"
#include "bearssl_x509.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#define SOCK_RW_CAST(buf) ((const char *)(buf))
#define SOCK_R_CAST(buf)  ((char *)(buf))
#else
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#define SOCK_RW_CAST(buf) (buf)
#define SOCK_R_CAST(buf)  (buf)
#endif

#ifndef XS_NO_BEARSSL

/* ----------------------------------------------------------------
 *  PEM parser thin wrapper around br_pem_decoder.
 *
 *  Walks a PEM buffer and invokes the callback once per decoded
 *  block, with the block name (CERTIFICATE / RSA PRIVATE KEY /
 *  PRIVATE KEY / EC PRIVATE KEY ...) and the DER bytes.
 * ---------------------------------------------------------------- */

typedef struct {
    void (*on_block)(void *ud, const char *name,
                     const uint8_t *data, size_t len);
    void          *userdata;
    /* growable scratch buffer that pem decoder writes into */
    unsigned char *buf;
    size_t         len;
    size_t         cap;
    char           name[64];
} PemCtx;

static void pem_dest(void *ctx_v, const void *data, size_t len) {
    PemCtx *p = (PemCtx *)ctx_v;
    if (p->len + len > p->cap) {
        size_t newcap = p->cap ? p->cap * 2 : 4096;
        while (newcap < p->len + len) newcap *= 2;
        unsigned char *nb = (unsigned char *)realloc(p->buf, newcap);
        if (!nb) return;
        p->buf = nb; p->cap = newcap;
    }
    memcpy(p->buf + p->len, data, len);
    p->len += len;
}

static int parse_pem_blocks(const uint8_t *src, size_t src_len,
                            void (*on_block)(void *ud, const char *name,
                                             const uint8_t *data, size_t len),
                            void *userdata) {
    br_pem_decoder_context dec;
    br_pem_decoder_init(&dec);
    PemCtx ctx; memset(&ctx, 0, sizeof ctx);
    ctx.on_block = on_block; ctx.userdata = userdata;
    br_pem_decoder_setdest(&dec, pem_dest, &ctx);

    size_t pos = 0;
    int blocks = 0;
    while (pos < src_len) {
        size_t pushed = br_pem_decoder_push(&dec, src + pos, src_len - pos);
        pos += pushed;
        int ev = br_pem_decoder_event(&dec);
        if (ev == BR_PEM_BEGIN_OBJ) {
            const char *n = br_pem_decoder_name(&dec);
            snprintf(ctx.name, sizeof ctx.name, "%.*s",
                     (int)(sizeof ctx.name - 1), n ? n : "");
            ctx.len = 0;
        } else if (ev == BR_PEM_END_OBJ) {
            on_block(userdata, ctx.name, ctx.buf, ctx.len);
            blocks++;
            ctx.len = 0;
        } else if (ev == BR_PEM_ERROR) {
            free(ctx.buf);
            return -1;
        } else if (pushed == 0) {
            /* no progress: trailing whitespace / non-PEM content; bail */
            break;
        }
    }
    free(ctx.buf);
    return blocks;
}

/* ----------------------------------------------------------------
 *  Per-listener TLS context: parsed cert chain + private key.
 * ---------------------------------------------------------------- */

typedef struct {
    br_x509_certificate *chain;
    size_t               chain_len;
    int                  key_type;        /* BR_KEYTYPE_RSA / BR_KEYTYPE_EC */
    br_rsa_private_key   rsa;             /* valid when key_type == RSA */
    br_ec_private_key    ec;              /* valid when key_type == EC */
    /* Backing storage so the bigint pointers inside the rsa/ec
     * structs stay alive for the listener's lifetime. */
    unsigned char       *key_blob;
    size_t               key_blob_len;
} XsHttpTlsCtx;

static void cert_block_cb(void *ud, const char *name,
                          const uint8_t *data, size_t len) {
    XsHttpTlsCtx *ctx = (XsHttpTlsCtx *)ud;
    if (strcmp(name, "CERTIFICATE") != 0) return;
    ctx->chain = (br_x509_certificate *)realloc(
        ctx->chain, sizeof(br_x509_certificate) * (ctx->chain_len + 1));
    unsigned char *copy = (unsigned char *)malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    ctx->chain[ctx->chain_len].data     = copy;
    ctx->chain[ctx->chain_len].data_len = len;
    ctx->chain_len++;
}

static void key_block_cb(void *ud, const char *name,
                         const uint8_t *data, size_t len) {
    XsHttpTlsCtx *ctx = (XsHttpTlsCtx *)ud;
    /* Accept any of the three common labels; br_skey_decoder figures
     * out the actual algorithm. */
    if (strcmp(name, "RSA PRIVATE KEY")  != 0 &&
        strcmp(name, "EC PRIVATE KEY")   != 0 &&
        strcmp(name, "PRIVATE KEY")      != 0) return;
    if (ctx->key_blob) return;  /* first block wins */

    br_skey_decoder_context dec;
    br_skey_decoder_init(&dec);
    br_skey_decoder_push(&dec, data, len);
    if (br_skey_decoder_last_error(&dec) != 0) return;

    int kt = br_skey_decoder_key_type(&dec);
    if (kt == BR_KEYTYPE_RSA) {
        const br_rsa_private_key *src = br_skey_decoder_get_rsa(&dec);
        size_t blob_size = src->plen + src->qlen + src->dplen +
                           src->dqlen + src->iqlen;
        unsigned char *blob = (unsigned char *)malloc(blob_size);
        if (!blob) return;
        ctx->key_blob = blob; ctx->key_blob_len = blob_size;
        ctx->rsa.n_bitlen = src->n_bitlen;
        size_t off = 0;
        memcpy(blob + off, src->p,  src->plen);  ctx->rsa.p  = blob + off; ctx->rsa.plen  = src->plen;  off += src->plen;
        memcpy(blob + off, src->q,  src->qlen);  ctx->rsa.q  = blob + off; ctx->rsa.qlen  = src->qlen;  off += src->qlen;
        memcpy(blob + off, src->dp, src->dplen); ctx->rsa.dp = blob + off; ctx->rsa.dplen = src->dplen; off += src->dplen;
        memcpy(blob + off, src->dq, src->dqlen); ctx->rsa.dq = blob + off; ctx->rsa.dqlen = src->dqlen; off += src->dqlen;
        memcpy(blob + off, src->iq, src->iqlen); ctx->rsa.iq = blob + off; ctx->rsa.iqlen = src->iqlen;
        ctx->key_type = BR_KEYTYPE_RSA;
    } else if (kt == BR_KEYTYPE_EC) {
        const br_ec_private_key *src = br_skey_decoder_get_ec(&dec);
        unsigned char *blob = (unsigned char *)malloc(src->xlen);
        if (!blob) return;
        memcpy(blob, src->x, src->xlen);
        ctx->ec.curve = src->curve;
        ctx->ec.x     = blob;
        ctx->ec.xlen  = src->xlen;
        ctx->key_blob = blob; ctx->key_blob_len = src->xlen;
        ctx->key_type = BR_KEYTYPE_EC;
    }
}

static int read_file_all(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf; *out_len = (size_t)sz;
    return 0;
}

int http_tls_attach(HTTPServer *server, const char *cert_pem, const char *key_pem) {
    if (!server || !cert_pem || !key_pem) return -1;

    uint8_t *cert_buf = NULL, *key_buf = NULL;
    size_t cert_len = 0, key_len = 0;
    if (read_file_all(cert_pem, &cert_buf, &cert_len) < 0) {
        fprintf(stderr, "http: cannot read cert '%s'\n", cert_pem);
        return -1;
    }
    if (read_file_all(key_pem, &key_buf, &key_len) < 0) {
        free(cert_buf);
        fprintf(stderr, "http: cannot read key '%s'\n", key_pem);
        return -1;
    }

    XsHttpTlsCtx *ctx = (XsHttpTlsCtx *)calloc(1, sizeof(XsHttpTlsCtx));
    if (!ctx) { free(cert_buf); free(key_buf); return -1; }

    parse_pem_blocks(cert_buf, cert_len, cert_block_cb, ctx);
    parse_pem_blocks(key_buf,  key_len,  key_block_cb,  ctx);
    free(cert_buf);
    free(key_buf);

    if (ctx->chain_len == 0 || ctx->key_blob == NULL) {
        for (size_t i = 0; i < ctx->chain_len; i++) free(ctx->chain[i].data);
        free(ctx->chain);
        free(ctx->key_blob);
        free(ctx);
        fprintf(stderr, "http: TLS attach failed: missing CERTIFICATE / private key\n");
        return -1;
    }
    server->tls_ctx = ctx;
    return 0;
}

/* ----------------------------------------------------------------
 *  Per-connection engine state + raw read / write bridge.
 * ---------------------------------------------------------------- */

typedef struct {
    br_ssl_server_context engine;
    unsigned char         iobuf[BR_SSL_BUFSIZE_BIDI];
} XsHttpTlsConn;

void http_tls_conn_init(HTTPConnection *c) {
    if (!c || !c->server || !c->server->tls_ctx) return;
    XsHttpTlsCtx *cx = (XsHttpTlsCtx *)c->server->tls_ctx;
    XsHttpTlsConn *st = (XsHttpTlsConn *)calloc(1, sizeof(XsHttpTlsConn));
    if (!st) return;
    if (cx->key_type == BR_KEYTYPE_RSA) {
        br_ssl_server_init_full_rsa(&st->engine, cx->chain, cx->chain_len, &cx->rsa);
    } else {
        br_ssl_server_init_full_ec(&st->engine, cx->chain, cx->chain_len,
                                   BR_KEYTYPE_RSA, &cx->ec);
    }
    br_ssl_engine_set_buffer(&st->engine.eng, st->iobuf, sizeof st->iobuf, 1);
    br_ssl_server_reset(&st->engine);
    c->tls_state = st;
}

void http_tls_conn_free(HTTPConnection *c) {
    if (!c || !c->tls_state) return;
    free(c->tls_state);
    c->tls_state = NULL;
}

/* Drive the engine. Drains queued send-rec to the socket and
 * pushes any pending recv-rec into the engine; on app-data ready,
 * appends the decrypted bytes to *out_decrypted.
 *
 * Returns:
 *   1  on progress (decrypted bytes appended OR engine state advanced)
 *   0  on EAGAIN / no data
 *  -1  on hard error / closed
 */
static int tls_pump(HTTPConnection *c, unsigned char *decrypted_dst,
                    size_t decrypted_max, size_t *decrypted_out) {
    XsHttpTlsConn *st = (XsHttpTlsConn *)c->tls_state;
    *decrypted_out = 0;
    if (!st) return -1;

    for (int spin = 0; spin < 16; spin++) {
        unsigned state = br_ssl_engine_current_state(&st->engine.eng);
        if (state & BR_SSL_CLOSED) {
            return -1;
        }
        int progressed = 0;

        /* drain any encrypted bytes the engine has queued */
        if (state & BR_SSL_SENDREC) {
            size_t avail = 0;
            unsigned char *out = br_ssl_engine_sendrec_buf(&st->engine.eng, &avail);
            if (out && avail > 0) {
                int sent = (int)send(c->fd, SOCK_RW_CAST(out), (int)avail, 0);
                if (sent > 0) {
                    br_ssl_engine_sendrec_ack(&st->engine.eng, (size_t)sent);
                    progressed = 1;
                    continue;
                } else {
                    /* would block / fatal -- caller can retry on next event */
                    break;
                }
            }
        }

        /* pull plaintext out for the http parser */
        if ((state & BR_SSL_RECVAPP) && decrypted_dst) {
            size_t avail = 0;
            unsigned char *in = br_ssl_engine_recvapp_buf(&st->engine.eng, &avail);
            if (in && avail > 0) {
                size_t take = avail;
                if (take > decrypted_max - *decrypted_out)
                    take = decrypted_max - *decrypted_out;
                if (take == 0) return 1;
                memcpy(decrypted_dst + *decrypted_out, in, take);
                *decrypted_out += take;
                br_ssl_engine_recvapp_ack(&st->engine.eng, take);
                progressed = 1;
                continue;
            }
        }

        /* feed encrypted bytes from the socket into the engine */
        if (state & BR_SSL_RECVREC) {
            size_t cap = 0;
            unsigned char *into = br_ssl_engine_recvrec_buf(&st->engine.eng, &cap);
            if (into && cap > 0) {
                int got = (int)recv(c->fd, SOCK_R_CAST(into), (int)cap, 0);
                if (got > 0) {
                    br_ssl_engine_recvrec_ack(&st->engine.eng, (size_t)got);
                    progressed = 1;
                    continue;
                } else if (got == 0) {
                    /* peer closed */
                    return -1;
                } else {
                    /* would block: ok */
                    break;
                }
            }
        }

        if (!progressed) break;
    }
    return *decrypted_out > 0 ? 1 : 0;
}

ssize_t http_tls_read(HTTPConnection *c, void *buf, size_t len) {
    size_t out = 0;
    int rc = tls_pump(c, (unsigned char *)buf, len, &out);
    if (rc < 0 && out == 0) return -1;
    if (out > 0) return (ssize_t)out;
    return rc < 0 ? -1 : 0;
}

ssize_t http_tls_write(HTTPConnection *c, const void *buf, size_t len) {
    XsHttpTlsConn *st = (XsHttpTlsConn *)c->tls_state;
    if (!st) return -1;
    size_t total = 0;
    while (total < len) {
        unsigned state = br_ssl_engine_current_state(&st->engine.eng);
        if (state & BR_SSL_CLOSED) return -1;
        if (state & BR_SSL_SENDAPP) {
            size_t cap = 0;
            unsigned char *into = br_ssl_engine_sendapp_buf(&st->engine.eng, &cap);
            if (!into || cap == 0) break;
            size_t take = len - total;
            if (take > cap) take = cap;
            memcpy(into, (const unsigned char *)buf + total, take);
            br_ssl_engine_sendapp_ack(&st->engine.eng, take);
            br_ssl_engine_flush(&st->engine.eng, 0);
            total += take;
        }
        /* push out any encrypted bytes the engine produced */
        size_t junk = 0;
        if (tls_pump(c, NULL, 0, &junk) < 0) return total ? (ssize_t)total : -1;
    }
    return (ssize_t)total;
}

/* Public entry point exposed in http_server.h. Defined here so the
 * symbol is provided when this TU is linked; http_server.c keeps
 * only a fallback weak definition for builds that strip BearSSL. */
int http_server_use_tls(HTTPServer *s, const char *cert, const char *key) {
    return http_tls_attach(s, cert, key);
}

#endif /* !XS_NO_BEARSSL */
