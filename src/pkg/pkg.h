#ifndef XS_PKG_H
#define XS_PKG_H

#include <stddef.h>

int pkg_new(const char *name);
int pkg_install(const char *package_name);
int pkg_add(const char *package_name);
int pkg_remove(const char *package_name);
int pkg_update(const char *package_name);
int pkg_list(void);
int pkg_publish(const char *path);
int pkg_search(const char *query);
int pkg_login(void);
int pkg_logout(void);
int pkg_whoami(void);
int pkg_creds_read(char *out, size_t cap);

#endif
