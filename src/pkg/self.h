#ifndef XS_PKG_SELF_H
#define XS_PKG_SELF_H

#include <stddef.h>

int xs_self_upgrade(int argc, char **argv);
int xs_self_uninstall(int argc, char **argv);

/* Best-effort sweep of <self>.old left behind by a previous Windows
   upgrade. Called once at startup; safe to call on any platform. */
void xs_self_cleanup_stale_old(void);

/* Move the running install_path aside so the slot is free for a new
   binary. Required on Windows because the running .exe is locked;
   no-op on POSIX where rename(new, dst) handles a busy destination.
   Exposed so the unit tests can exercise the rename trick directly. */
int xs_self_make_room_for_replace(const char *install_path);

/* Compose install_path + suffix into out. Returns 0 on success,
   -1 if out_len is too small. Exposed for tests. */
int xs_self_sibling_path(const char *install_path, const char *suffix,
                         char *out, size_t out_len);

#endif
