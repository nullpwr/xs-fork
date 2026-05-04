#ifndef XS_SOCK_COMPAT_H
#define XS_SOCK_COMPAT_H

/* Tiny shim so the BSD-socket calls in pkg.c, builtins_net.c and
 * builtins_http.c compile and run under mingw / MSVC. Posix targets
 * keep their normal headers; on Windows we map close()->closesocket()
 * etc. and ensure WSAStartup ran once before any socket call. */

#if defined(_WIN32) || defined(__MINGW32__)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef errno
#    undef errno
#  endif
#  define errno          (WSAGetLastError())
#  ifdef EWOULDBLOCK  /* mingw's errno.h supplies a value for posix code */
#    undef EWOULDBLOCK
#  endif
#  define EWOULDBLOCK    WSAEWOULDBLOCK
#  define EINTR_LIKE(e)  ((e) == WSAEINTR)
#  ifndef ssize_t
typedef long long ssize_t;
#  endif
static inline int sock_close(int fd) { return closesocket((SOCKET)fd); }
#  define close          sock_close
void xs_sock_init(void);
#elif !defined(__wasi__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/tcp.h>
#  define EINTR_LIKE(e)  ((e) == EINTR)
static inline void xs_sock_init(void) { }
#endif

#endif /* XS_SOCK_COMPAT_H */
