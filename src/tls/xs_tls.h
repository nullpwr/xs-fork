#ifndef XS_TLS_H
#define XS_TLS_H

typedef struct xs_tls_conn xs_tls_conn;

/* connect TLS over an existing socket fd. returns NULL on failure */
xs_tls_conn *xs_tls_connect(int fd, const char *hostname);

/* read/write through TLS. returns bytes transferred or -1 */
int xs_tls_read(xs_tls_conn *conn, void *buf, int len);
int xs_tls_write(xs_tls_conn *conn, const void *buf, int len);

/* close and free */
void xs_tls_close(xs_tls_conn *conn);

/* verification policy: 0 (default) = verify against bundled Mozilla CAs,
   1 = skip trust-chain check (legacy behaviour, for pinning or testing).
   Override via env XS_TLS_INSECURE=1 or by calling xs_tls_set_insecure(1). */
void xs_tls_set_insecure(int on);
int  xs_tls_is_insecure(void);

/* how many bundled trust anchors are loaded; -1 if bundle parse failed */
int  xs_tls_anchor_count(void);

#endif
