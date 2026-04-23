#include "net/sock_compat.h"

#if defined(_WIN32) || defined(__MINGW32__)
#include <stdatomic.h>

static atomic_int g_init = 0;

void xs_sock_init(void) {
    int prev = atomic_exchange(&g_init, 1);
    if (prev) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
#endif
