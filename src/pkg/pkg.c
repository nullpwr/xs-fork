#define _POSIX_C_SOURCE 200809L
#include "pkg/pkg.h"
#include "core/xs_compat.h"
#include "core/xs.h"
#include "core/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#ifndef PKG_REGISTRY_URL
#define PKG_REGISTRY_URL "https://reg.xslang.org"
#endif

/* http_do_request lives in builtins_net.c behind the same guard
 * (no raw POSIX sockets in mingw / wasi); registry features are
 * skipped on those targets and surface a clear error instead. */
#if !defined(__MINGW32__) && !defined(__wasi__)
#define PKG_HAS_REGISTRY_HTTP 1
extern Value *http_do_request(const char *method, const char *url,
                              XSMap *extra_headers, const char *body,
                              size_t body_len);
#else
#define PKG_HAS_REGISTRY_HTTP 0
#endif

/* tiny JSON-ish field grabber for known-shape registry responses. The
 * registry returns
 *   {"package":{...}, "version":{"version":"0.2.1","tarball_url":"https://...", ...}}
 * so we walk char-by-char looking for "key":"value" with rudimentary
 * escape handling. avoids dragging the full json builtin into pkg.c. */
static char *json_field(const char *body, const char *key) {
    if (!body || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (*p == '"' && strncmp(p + 1, key, klen) == 0 &&
            p[1 + klen] == '"') {
            const char *q = p + 1 + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != ':') { p++; continue; }
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != '"') { p++; continue; }
            q++;
            const char *start = q;
            char *out = malloc(strlen(q) + 1);
            if (!out) return NULL;
            char *w = out;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1]) {
                    char c = q[1];
                    switch (c) {
                        case '"': *w++ = '"'; break;
                        case '\\': *w++ = '\\'; break;
                        case '/': *w++ = '/'; break;
                        case 'n': *w++ = '\n'; break;
                        case 't': *w++ = '\t'; break;
                        case 'r': *w++ = '\r'; break;
                        default: *w++ = c; break;
                    }
                    q += 2;
                } else {
                    *w++ = *q++;
                }
            }
            *w = '\0';
            (void)start;
            return out;
        }
        p++;
    }
    return NULL;
}

#if PKG_HAS_REGISTRY_HTTP
/* Pull body / status out of an http_do_request response map. */
static int registry_get(const char *url, char **body_out, int *status_out) {
    *body_out = NULL;
    if (status_out) *status_out = 0;
    Value *resp = http_do_request("GET", url, NULL, NULL, 0);
    if (!resp) return -1;
    if (VAL_TAG(resp) != XS_MAP || !resp->map) {
        value_decref(resp);
        return -1;
    }
    Value *st = map_get(resp->map, "status");
    Value *bd = map_get(resp->map, "body");
    int status = (st && VAL_TAG(st) == XS_INT) ? (int)VAL_INT(st) : 0;
    if (status_out) *status_out = status;
    if (bd && VAL_TAG(bd) == XS_STR && bd->s) {
        *body_out = strdup(bd->s);
    }
    value_decref(resp);
    return 0;
}
#endif /* PKG_HAS_REGISTRY_HTTP */

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "xs new: cannot write '%s'\n", path);
        return 1;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

static int read_pkg_version(const char *toml_path, char *version_out, size_t vlen) {
    FILE *f = fopen(toml_path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "version");
        if (!p) continue;
        p = strchr(p, '=');
        if (!p) continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            char *end = strchr(p, '"');
            if (end) {
                size_t len = (size_t)(end - p);
                if (len >= vlen) len = vlen - 1;
                memcpy(version_out, p, len);
                version_out[len] = '\0';
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}

static int read_pkg_source(const char *toml_path, char *source_out, size_t slen) {
    FILE *f = fopen(toml_path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "source");
        if (!p) continue;
        p = strchr(p, '=');
        if (!p) continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            char *end = strchr(p, '"');
            if (end) {
                size_t len = (size_t)(end - p);
                if (len >= slen) len = slen - 1;
                memcpy(source_out, p, len);
                source_out[len] = '\0';
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}

static int is_directory(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

static int count_pkg_files(const char *pkg_dir) {
    DIR *d = opendir(pkg_dir);
    if (!d) return 0;
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    return count;
}

static int copy_directory(const char *src, const char *dst) {
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    if (!d) {
        fprintf(stderr, "xs install: cannot open source directory '%s'\n", src);
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char srcpath[1024], dstpath[1024];
        snprintf(srcpath, sizeof(srcpath), "%s/%s", src, ent->d_name);
        snprintf(dstpath, sizeof(dstpath), "%s/%s", dst, ent->d_name);
        if (is_directory(srcpath)) {
            if (copy_directory(srcpath, dstpath) != 0) {
                closedir(d);
                return 1;
            }
        } else {
            FILE *in = fopen(srcpath, "rb");
            if (!in) { closedir(d); return 1; }
            FILE *out = fopen(dstpath, "wb");
            if (!out) { fclose(in); closedir(d); return 1; }
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                fwrite(buf, 1, n, out);
            }
            fclose(in);
            fclose(out);
        }
    }
    closedir(d);
    return 0;
}

static int remove_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    if (!S_ISDIR(st.st_mode)) return remove(path);

    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[2048];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        remove_recursive(child);
    }
    closedir(d);
    return rmdir(path);
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash) return slash + 1;
    return path;
}

/* strip .git suffix from a name, returns static buffer */
static const char *strip_git_suffix(const char *name) {
    static char buf[512];
    size_t len = strlen(name);
    if (len >= 4 && strcmp(name + len - 4, ".git") == 0) {
        if (len - 4 >= sizeof(buf)) len = sizeof(buf) - 1 + 4;
        memcpy(buf, name, len - 4);
        buf[len - 4] = '\0';
        return buf;
    }
    return name;
}


int pkg_new(const char *name) {
    if (!name || !name[0]) {
        fprintf(stderr, "xs new: missing project name\n");
        return 1;
    }

    if (mkdir(name, 0755) != 0) {
        fprintf(stderr, "xs new: cannot create directory '%s'\n", name);
        return 1;
    }

    char pathbuf[1024];
    snprintf(pathbuf, sizeof pathbuf, "%s/src", name);
    if (mkdir(pathbuf, 0755) != 0) {
        fprintf(stderr, "xs new: cannot create directory '%s'\n", pathbuf);
        return 1;
    }

    snprintf(pathbuf, sizeof pathbuf, "%s/xs.toml", name);
    char toml[1024];
    snprintf(toml, sizeof toml,
        "[package]\n"
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "xs_version = \">=1.0\"\n"
        "\n"
        "[dependencies]\n"
        "\n"
        "[build]\n"
        "entry = \"src/main.xs\"\n",
        name);
    if (write_file(pathbuf, toml)) return 1;

    snprintf(pathbuf, sizeof pathbuf, "%s/src/main.xs", name);
    char mainxs[512];
    snprintf(mainxs, sizeof mainxs,
        "fn main() {\n"
        "    println(\"Hello from %s!\")\n"
        "}\n",
        name);
    if (write_file(pathbuf, mainxs)) return 1;

    snprintf(pathbuf, sizeof pathbuf, "%s/.gitignore", name);
    if (write_file(pathbuf,
        ".xs_lib/\n"
        "*.xsc\n"
        ".xs_cache/\n")) return 1;

    printf("Created project '%s'\n", name);
    printf("  %s/xs.toml\n", name);
    printf("  %s/src/main.xs\n", name);
    printf("  %s/.gitignore\n", name);
    return 0;
}

// xs install

int pkg_install(const char *package_name) {
    if (!package_name) {
        FILE *f = fopen("xs.toml", "r");
        if (!f) {
            fprintf(stderr, "xs install: no xs.toml found in current directory\n");
            return 1;
        }
        char line[1024];
        int in_deps = 0;
        int count = 0;
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
            if (strcmp(line, "[dependencies]") == 0) { in_deps = 1; continue; }
            if (in_deps && line[0] == '[') break; /* next section */
            if (in_deps && len > 0 && line[0] != '#') {
                char *eq = strchr(line, '=');
                if (eq) {
                    *eq = '\0';
                    char *end = eq - 1;
                    while (end >= line && (*end == ' ' || *end == '\t')) *end-- = '\0';
                    char *val = eq + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    size_t vlen = strlen(val);
                    if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
                        val[vlen - 1] = '\0';
                        val++;
                    }
                    if (strlen(line) > 0) {
                        mkdir(".xs_lib", 0755);
                        char dirpath[2048];
                        snprintf(dirpath, sizeof(dirpath), ".xs_lib/%s", line);
                        if (strncmp(val, "file://", 7) == 0) {
                            const char *src = val + 7;
                            if (is_directory(src)) {
                                copy_directory(src, dirpath);
                                printf("  installed %s from %s\n", line, src);
                            } else {
                                fprintf(stderr, "  warning: source '%s' not found for %s\n", src, line);
                                mkdir(dirpath, 0755);
                            }
                        } else if (strstr(val, ".git") || strncmp(val, "git://", 6) == 0 ||
                                   strncmp(val, "https://", 8) == 0 || strncmp(val, "http://", 7) == 0) {
                            char cmd[4096];
                            snprintf(cmd, sizeof(cmd), "git clone --depth 1 %s %s 2>&1", val, dirpath);
                            int ret = system(cmd);
                            if (ret != 0) {
                                fprintf(stderr, "  warning: git clone failed for %s\n", line);
                                mkdir(dirpath, 0755);
                            } else {
                                printf("  installed %s from %s\n", line, val);
                            }
                        } else {
                            mkdir(dirpath, 0755);
                            /* Write xs.toml with version info */
                            char tomlpath[4096];
                            snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", dirpath);
                            FILE *tf = fopen(tomlpath, "w");
                            if (tf) {
                                fprintf(tf,
                                    "[package]\nname = \"%s\"\nversion = \"%s\"\n",
                                    line, val);
                                fclose(tf);
                            }
                            printf("  installed %s@%s\n", line, val);
                        }
                        count++;
                    }
                }
            }
        }
        fclose(f);
        printf("installed %d dependencies\n", count);
        return 0;
    }

    mkdir(".xs_lib", 0755);

    const char *pkg_name = package_name;
    const char *source = package_name;
    int is_local = 0;
    int is_git = 0;
    char expanded_url[2048];

    /* GitHub shorthand: "user/repo" -> "https://github.com/user/repo.git" */
    if (strchr(package_name, '/') && !strchr(package_name, ':') &&
        strncmp(package_name, ".", 1) != 0 && !is_directory(package_name)) {
        const char *slash = strchr(package_name, '/');
        /* must have exactly one slash and no dots before it (not a path) */
        if (slash && !strchr(slash + 1, '/')) {
            snprintf(expanded_url, sizeof(expanded_url),
                     "https://github.com/%s.git", package_name);
            source = expanded_url;
            is_git = 1;
            pkg_name = strip_git_suffix(slash + 1);
        }
    }

    if (!is_git && strncmp(package_name, "file://", 7) == 0) {
        is_local = 1;
        source = package_name + 7;
        pkg_name = basename_of(source);
    } else if (!is_git && is_directory(package_name)) {
        is_local = 1;
        source = package_name;
        pkg_name = basename_of(source);
    } else if (!is_git && (strstr(package_name, ".git") || strncmp(package_name, "git://", 6) == 0 ||
               strncmp(package_name, "https://", 8) == 0 || strncmp(package_name, "http://", 7) == 0)) {
        is_git = 1;
        pkg_name = strip_git_suffix(basename_of(package_name));
    }

    char dirpath[1024];
    snprintf(dirpath, sizeof(dirpath), ".xs_lib/%s", pkg_name);

    if (is_local) {
        if (!is_directory(source)) {
            fprintf(stderr, "xs install: source path '%s' not found\n", source);
            return 1;
        }
        if (copy_directory(source, dirpath) != 0) {
            fprintf(stderr, "xs install: failed to copy from '%s'\n", source);
            return 1;
        }
        char tomlpath[2048];
        snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", dirpath);
        if (!file_exists(tomlpath)) {
            FILE *f = fopen(tomlpath, "w");
            if (f) {
                fprintf(f,
                    "[package]\n"
                    "name = \"%s\"\n"
                    "version = \"0.1.0\"\n"
                    "source = \"file://%s\"\n",
                    pkg_name, source);
                fclose(f);
            }
        }
        printf("installed %s from %s\n", pkg_name, source);
    } else if (is_git) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "git clone --depth 1 %s %s 2>&1", package_name, dirpath);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "xs install: git clone failed for '%s'\n", package_name);
            return 1;
        }
        char tomlpath[2048];
        snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", dirpath);
        if (!file_exists(tomlpath)) {
            FILE *f = fopen(tomlpath, "w");
            if (f) {
                fprintf(f,
                    "[package]\n"
                    "name = \"%s\"\n"
                    "version = \"0.1.0\"\n"
                    "source = \"%s\"\n",
                    pkg_name, package_name);
                fclose(f);
            }
        }
        printf("installed %s from %s\n", pkg_name, package_name);
    } else {
#if !PKG_HAS_REGISTRY_HTTP
        fprintf(stderr,
            "xs install: registry fetch is not available on this build "
            "(no raw sockets); install from a git URL or local path "
            "instead.\n");
        return 1;
#else
        /* Plain "name" -> hosted registry at reg.xslang.org. Hit
         * /api/pkg/{name}/latest, fish the tarball URL out of the JSON
         * envelope, download, unpack into .xs_lib/{name}. */
        char meta_url[1024];
        snprintf(meta_url, sizeof(meta_url),
                 "%s/api/pkg/%s/latest", PKG_REGISTRY_URL, package_name);

        char *body = NULL;
        int status = 0;
        if (registry_get(meta_url, &body, &status) != 0 || !body) {
            fprintf(stderr,
                "xs install: could not reach registry at %s\n",
                PKG_REGISTRY_URL);
            free(body);
            return 1;
        }
        if (status != 200) {
            fprintf(stderr,
                "xs install: %s: registry returned %d\n",
                package_name, status);
            free(body);
            return 1;
        }
        char *tarball = json_field(body, "tarball_url");
        char *version = json_field(body, "version");
        free(body);
        if (!tarball || !*tarball) {
            fprintf(stderr,
                "xs install: %s: registry response missing tarball_url\n",
                package_name);
            free(tarball); free(version);
            return 1;
        }

        if (mkdir(dirpath, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "xs install: cannot create '%s'\n", dirpath);
            free(tarball); free(version);
            return 1;
        }

        /* download tarball: registry uploads land in supabase storage,
         * which ships an https url that http_do_request can fetch the
         * same way the metadata endpoint did. */
        Value *tar_resp = http_do_request("GET", tarball, NULL, NULL, 0);
        if (!tar_resp || VAL_TAG(tar_resp) != XS_MAP || !tar_resp->map) {
            fprintf(stderr, "xs install: tarball download failed for %s\n",
                    package_name);
            if (tar_resp) value_decref(tar_resp);
            free(tarball); free(version);
            return 1;
        }
        Value *tst = map_get(tar_resp->map, "status");
        Value *tbd = map_get(tar_resp->map, "body");
        int tstatus = (tst && VAL_TAG(tst) == XS_INT) ? (int)VAL_INT(tst) : 0;
        if (tstatus != 200 || !tbd || VAL_TAG(tbd) != XS_STR || !tbd->s) {
            fprintf(stderr,
                "xs install: tarball fetch returned %d for %s\n",
                tstatus, package_name);
            value_decref(tar_resp);
            free(tarball); free(version);
            return 1;
        }
        char tarpath[2048];
        snprintf(tarpath, sizeof(tarpath), "%s/.tarball.tgz", dirpath);
        FILE *tf = fopen(tarpath, "wb");
        if (!tf) {
            fprintf(stderr, "xs install: cannot write %s\n", tarpath);
            value_decref(tar_resp);
            free(tarball); free(version);
            return 1;
        }
        size_t blen = strlen(tbd->s);
        fwrite(tbd->s, 1, blen, tf);
        fclose(tf);
        value_decref(tar_resp);

        /* unpack with the system tar; bsd / gnu tar both accept these
         * args. strip-components=1 collapses the package-version
         * top-level dir. */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
                 "tar xzf %s -C %s --strip-components=1 2>&1", tarpath, dirpath);
        if (system(cmd) != 0) {
            fprintf(stderr, "xs install: tar extraction failed for %s\n",
                    package_name);
            free(tarball); free(version);
            return 1;
        }
        unlink(tarpath);

        /* If the package didn't include an xs.toml, leave one behind
         * pointing at the registry source. */
        char tomlpath[2048];
        snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", dirpath);
        if (!file_exists(tomlpath)) {
            FILE *f = fopen(tomlpath, "w");
            if (f) {
                fprintf(f,
                    "[package]\n"
                    "name = \"%s\"\n"
                    "version = \"%s\"\n"
                    "source = \"registry\"\n"
                    "\n"
                    "[dependencies]\n",
                    package_name, version ? version : "0.0.0");
                fclose(f);
            }
        }

        printf("installed %s%s%s from %s\n",
               package_name,
               version ? "@" : "",
               version ? version : "",
               PKG_REGISTRY_URL);
        free(tarball); free(version);
#endif /* PKG_HAS_REGISTRY_HTTP */
    }
    return 0;
}

/* xs search <query>: hits /api/search?q=... and prints names + descriptions. */
int pkg_search(const char *query) {
    if (!query || !*query) {
        fprintf(stderr, "xs search: missing query\n");
        return 1;
    }
#if !PKG_HAS_REGISTRY_HTTP
    fprintf(stderr,
        "xs search: registry queries are not available on this build "
        "(no raw sockets).\n");
    (void)query;
    return 1;
#else
    /* very small URL-escape: replace spaces with '+' and stop on quote
     * / backslash since the registry rejects those anyway. */
    char esc[512];
    size_t qi = 0;
    for (const char *p = query; *p && qi + 1 < sizeof(esc); p++) {
        if (*p == ' ') esc[qi++] = '+';
        else if (*p == '"' || *p == '\\') break;
        else esc[qi++] = *p;
    }
    esc[qi] = '\0';

    char url[1024];
    snprintf(url, sizeof(url),
             "%s/api/search?q=%s&limit=20", PKG_REGISTRY_URL, esc);

    char *body = NULL;
    int status = 0;
    if (registry_get(url, &body, &status) != 0 || !body) {
        fprintf(stderr, "xs search: could not reach %s\n", PKG_REGISTRY_URL);
        free(body);
        return 1;
    }
    if (status != 200) {
        fprintf(stderr, "xs search: registry returned %d\n", status);
        free(body);
        return 1;
    }

    /* Walk the body, picking out each results[].{name,description}.
     * The full-blown json builtin would be cleaner, but pkg.c is part
     * of the bootstrap and we'd rather not pull more of the runtime
     * surface in. */
    int found = 0;
    const char *p = body;
    while ((p = strstr(p, "\"name\""))) {
        const char *name_end;
        const char *name = NULL;
        const char *q = p + 6;
        while (*q == ' ' || *q == ':' || *q == '\t') q++;
        if (*q == '"') {
            name = ++q;
            while (*q && *q != '"') q++;
            name_end = q;
            int nlen = (int)(name_end - name);
            const char *desc_pos = strstr(q, "\"description\"");
            char *desc = json_field(p, "description");
            printf("  %.*s%s%s\n",
                   nlen, name,
                   desc && *desc ? " - " : "",
                   desc && *desc ? desc : "");
            free(desc);
            (void)desc_pos;
            found++;
        }
        p = q + 1;
    }
    free(body);
    if (!found) {
        printf("no packages match '%s'\n", query);
    } else {
        printf("(%d results from %s)\n", found, PKG_REGISTRY_URL);
    }
    return 0;
#endif /* PKG_HAS_REGISTRY_HTTP */
}

int pkg_remove(const char *package_name) {
    if (!package_name) {
        fprintf(stderr, "xs remove: missing package name\n");
        return 1;
    }
    char dirpath[1024];
    snprintf(dirpath, sizeof(dirpath), ".xs_lib/%s", package_name);

    if (!is_directory(dirpath)) {
        fprintf(stderr, "xs remove: package '%s' not installed\n", package_name);
        return 1;
    }
    if (remove_recursive(dirpath) != 0) {
        fprintf(stderr, "xs remove: failed to fully remove '%s'\n", package_name);
        return 1;
    }
    printf("removed %s\n", package_name);
    return 0;
}

/* xs add: install + write to xs.toml */
int pkg_add(const char *package_name) {
    if (!package_name || !package_name[0]) {
        fprintf(stderr, "xs add: missing package name\n");
        return 1;
    }

    /* figure out the actual package name and source URL */
    const char *pkg_name = package_name;
    char source_url[2048];
    source_url[0] = '\0';

    /* GitHub shorthand: user/repo */
    if (strchr(package_name, '/') && !strchr(package_name, ':') &&
        strncmp(package_name, ".", 1) != 0 && !is_directory(package_name)) {
        const char *slash = strchr(package_name, '/');
        if (slash && !strchr(slash + 1, '/')) {
            snprintf(source_url, sizeof(source_url),
                     "https://github.com/%s.git", package_name);
            pkg_name = strip_git_suffix(slash + 1);
        }
    } else if (strstr(package_name, ".git") || strncmp(package_name, "https://", 8) == 0 ||
               strncmp(package_name, "http://", 7) == 0 || strncmp(package_name, "git://", 6) == 0) {
        snprintf(source_url, sizeof(source_url), "%s", package_name);
        pkg_name = strip_git_suffix(basename_of(package_name));
    } else if (is_directory(package_name)) {
        snprintf(source_url, sizeof(source_url), "file://%s", package_name);
        pkg_name = basename_of(package_name);
    }

    /* install the package */
    int rc = pkg_install(package_name);
    if (rc != 0) return rc;

    /* write to xs.toml */
    FILE *f = fopen("xs.toml", "r");
    if (!f) {
        /* create xs.toml if it doesn't exist */
        f = fopen("xs.toml", "w");
        if (f) {
            fprintf(f, "[package]\nname = \"project\"\nversion = \"0.1.0\"\n\n[dependencies]\n%s = \"%s\"\n",
                    pkg_name, source_url[0] ? source_url : "0.1.0");
            fclose(f);
            printf("added %s to xs.toml\n", pkg_name);
            return 0;
        }
        fprintf(stderr, "xs add: cannot create xs.toml\n");
        return 1;
    }

    /* read existing xs.toml */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc((size_t)(sz + 1));
    if (fread(content, 1, (size_t)sz, f) != (size_t)sz) { free(content); fclose(f); return 1; }
    content[sz] = '\0';
    fclose(f);

    /* check if already in dependencies */
    char needle[512];
    snprintf(needle, sizeof(needle), "%s", pkg_name);
    if (strstr(content, needle)) {
        printf("%s already in xs.toml\n", pkg_name);
        free(content);
        return 0;
    }

    /* find [dependencies] section and append */
    char *deps = strstr(content, "[dependencies]");
    if (deps) {
        char *after = deps + strlen("[dependencies]");
        while (*after == '\n' || *after == '\r') after++;
        size_t before_len = (size_t)(after - content);
        f = fopen("xs.toml", "w");
        if (f) {
            fwrite(content, 1, before_len, f);
            fprintf(f, "%s = \"%s\"\n", pkg_name, source_url[0] ? source_url : "0.1.0");
            fprintf(f, "%s", after);
            fclose(f);
        }
    } else {
        /* no [dependencies] section, append one */
        f = fopen("xs.toml", "a");
        if (f) {
            fprintf(f, "\n[dependencies]\n%s = \"%s\"\n",
                    pkg_name, source_url[0] ? source_url : "0.1.0");
            fclose(f);
        }
    }
    free(content);
    printf("added %s to xs.toml\n", pkg_name);
    return 0;
}

/* xs update */
int pkg_update(const char *package_name) {
    DIR *d = opendir(".xs_lib");
    if (!d) {
        printf("no .xs_lib/ directory found: nothing to update\n");
        return 0;
    }

    struct dirent *ent;
    int checked = 0;
    int updated = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), ".xs_lib/%s", ent->d_name);
        if (!is_directory(fullpath)) continue;

        if (package_name && strcmp(ent->d_name, package_name) != 0) continue;

        char tomlpath[2048];
        snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", fullpath);

        char version[256] = "unknown";
        char source[1024] = "";
        read_pkg_version(tomlpath, version, sizeof(version));
        read_pkg_source(tomlpath, source, sizeof(source));

        int file_count = count_pkg_files(fullpath);
        int needs_update = 0;

        if (!file_exists(tomlpath)) {
            printf("  %s: missing xs.toml: needs reinstall\n", ent->d_name);
            needs_update = 1;
        } else if (file_count <= 1) {
            printf("  %s@%s: incomplete package (only %d file(s)) -- needs reinstall\n",
                   ent->d_name, version, file_count);
            needs_update = 1;
        }

        if (needs_update) {
            if (source[0] && strcmp(source, "registry") != 0) {
                printf("  %s: re-installing from %s\n", ent->d_name, source);
                pkg_remove(ent->d_name);
                if (strncmp(source, "file://", 7) == 0) {
                    pkg_install(source);
                } else if (strstr(source, ".git") || strncmp(source, "https://", 8) == 0 ||
                           strncmp(source, "http://", 7) == 0 || strncmp(source, "git://", 6) == 0) {
                    pkg_install(source);
                } else {
                    pkg_install(ent->d_name);
                }
                updated++;
            } else {
                printf("  %s@%s: no known source: reinstall manually\n",
                       ent->d_name, version);
            }
        } else {
            printf("  %s@%s: ok (%d files)\n", ent->d_name, version, file_count);
        }
        checked++;
    }
    closedir(d);

    if (package_name && checked == 0) {
        fprintf(stderr, "xs update: package '%s' not installed\n", package_name);
        return 1;
    }
    printf("checked %d package(s), %d updated\n", checked, updated);
    return 0;
}

int pkg_list(void) {
    DIR *d = opendir(".xs_lib");
    if (!d) {
        printf("no .xs_lib/ directory found\n");
        return 0;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), ".xs_lib/%s", ent->d_name);
        DIR *sub = opendir(fullpath);
        if (sub) {
            closedir(sub);
            char tomlpath[2048];
            snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", fullpath);
            char version[256] = "unknown";
            read_pkg_version(tomlpath, version, sizeof(version));
            printf("  %s@%s\n", ent->d_name, version);
            count++;
        }
    }
    closedir(d);
    if (count == 0) printf("no packages installed\n");
    return 0;
}

/* xs publish */
int pkg_publish(const char *path) {
    const char *pkg_dir = path ? path : ".";

    char tomlpath[1024];
    snprintf(tomlpath, sizeof(tomlpath), "%s/xs.toml", pkg_dir);
    if (!file_exists(tomlpath)) {
        fprintf(stderr, "xs publish: no xs.toml found in '%s'\n", pkg_dir);
        return 1;
    }

    char name[256] = "";
    char version[256] = "";

    FILE *f = fopen(tomlpath, "r");
    if (!f) {
        fprintf(stderr, "xs publish: cannot read '%s'\n", tomlpath);
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Parse name */
        char *p = strstr(line, "name");
        if (p && !name[0]) {
            p = strchr(p, '=');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len >= sizeof(name)) len = sizeof(name) - 1;
                        memcpy(name, p, len);
                        name[len] = '\0';
                    }
                }
            }
        }
        /* Parse version */
        p = strstr(line, "version");
        if (p && !version[0]) {
            p = strchr(p, '=');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len >= sizeof(version)) len = sizeof(version) - 1;
                        memcpy(version, p, len);
                        version[len] = '\0';
                    }
                }
            }
        }
    }
    fclose(f);

    /* Validate required fields */
    if (!name[0]) {
        fprintf(stderr, "xs publish: xs.toml missing 'name' field\n");
        return 1;
    }
    if (!version[0]) {
        fprintf(stderr, "xs publish: xs.toml missing 'version' field\n");
        return 1;
    }

    /* Check that the package has source files */
    int file_count = count_pkg_files(pkg_dir);
    if (file_count <= 1) {
        fprintf(stderr, "xs publish: package '%s' has no source files\n", name);
        return 1;
    }

    /* Create tarball */
    char tarball[1024];
    snprintf(tarball, sizeof(tarball), "%s-%s.tar.gz", name, version);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar czf %s -C %s . 2>&1", tarball, pkg_dir);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "xs publish: failed to create tarball '%s'\n", tarball);
        return 1;
    }

    printf("Package validated:\n");
    printf("  name:    %s\n", name);
    printf("  version: %s\n", version);
    printf("  files:   %d\n", file_count);
    printf("  tarball: %s\n", tarball);

#if !PKG_HAS_REGISTRY_HTTP
    printf("\nregistry uploads are not available on this build "
           "(no raw sockets); tarball '%s' kept locally.\n", tarball);
    return 0;
#else
    /* If a publish token is in the environment, hand the tarball off to
     * the registry. Token is expected to be a Supabase JWT scoped to the
     * caller's account; the registry validates it server-side and rejects
     * publishes that don't own the package name. Without a token we keep
     * the legacy "tarball lives next to xs.toml" behaviour so the local
     * step is still useful for testing or third-party hosting. */
    const char *token = getenv("XS_REGISTRY_TOKEN");
    if (!token || !*token) {
        printf("\nXS_REGISTRY_TOKEN not set: tarball '%s' kept locally.\n",
               tarball);
        printf("  export XS_REGISTRY_TOKEN=<jwt>  # then xs publish to send.\n");
        return 0;
    }

    /* Read the tarball back into memory and base64-encode it so it
     * fits the registry's JSON envelope. The registry server decodes
     * the base64 in route-helpers.ts (the existing publish handler
     * already accepts {tarball: <b64>} or {tarball_url: <https>}). */
    FILE *tf = fopen(tarball, "rb");
    if (!tf) {
        fprintf(stderr, "xs publish: cannot reopen '%s'\n", tarball);
        return 1;
    }
    fseek(tf, 0, SEEK_END);
    long tlen = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    if (tlen <= 0) {
        fclose(tf);
        fprintf(stderr, "xs publish: empty tarball\n");
        return 1;
    }
    unsigned char *raw = malloc((size_t)tlen);
    if (!raw) { fclose(tf); fprintf(stderr, "xs publish: oom\n"); return 1; }
    size_t got = fread(raw, 1, (size_t)tlen, tf);
    fclose(tf);
    if ((long)got != tlen) {
        fprintf(stderr, "xs publish: short read on '%s'\n", tarball);
        free(raw);
        return 1;
    }

    static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz"
                                   "0123456789+/";
    size_t b64len = 4 * ((size_t)tlen + 2) / 3;
    char *b64 = malloc(b64len + 1);
    if (!b64) { free(raw); fprintf(stderr, "xs publish: oom\n"); return 1; }
    size_t bi = 0;
    for (size_t i = 0; i < (size_t)tlen; i += 3) {
        unsigned int n = (unsigned int)raw[i] << 16;
        if (i + 1 < (size_t)tlen) n |= (unsigned int)raw[i + 1] << 8;
        if (i + 2 < (size_t)tlen) n |= (unsigned int)raw[i + 2];
        b64[bi++] = b64chars[(n >> 18) & 0x3F];
        b64[bi++] = b64chars[(n >> 12) & 0x3F];
        b64[bi++] = (i + 1 < (size_t)tlen) ? b64chars[(n >> 6) & 0x3F] : '=';
        b64[bi++] = (i + 2 < (size_t)tlen) ? b64chars[n & 0x3F]        : '=';
    }
    b64[bi] = '\0';
    free(raw);

    /* Build {"version":"...","tarball":"<b64>"}. Description / readme /
     * keywords come from xs.toml in a future pass; for now we ship the
     * minimum the registry accepts. */
    size_t json_cap = bi + 256;
    char *json = malloc(json_cap);
    if (!json) { free(b64); fprintf(stderr, "xs publish: oom\n"); return 1; }
    int json_len = snprintf(json, json_cap,
        "{\"version\":\"%s\",\"tarball\":\"%s\"}", version, b64);
    free(b64);

    char publish_url[1024];
    snprintf(publish_url, sizeof(publish_url),
             "%s/api/pkg/%s/publish", PKG_REGISTRY_URL, name);

    char auth_header[2048];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    XSMap *headers = map_new();
    Value *auth_v = xs_str(auth_header);
    map_set(headers, "Authorization", auth_v);
    value_decref(auth_v);
    Value *ct_v = xs_str("application/json");
    map_set(headers, "Content-Type", ct_v);
    value_decref(ct_v);

    Value *resp = http_do_request("POST", publish_url, headers, json, (size_t)json_len);
    map_free(headers);
    free(json);

    if (!resp || VAL_TAG(resp) != XS_MAP || !resp->map) {
        fprintf(stderr, "xs publish: could not reach %s\n", PKG_REGISTRY_URL);
        if (resp) value_decref(resp);
        return 1;
    }
    Value *st = map_get(resp->map, "status");
    Value *bd = map_get(resp->map, "body");
    int status = (st && VAL_TAG(st) == XS_INT) ? (int)VAL_INT(st) : 0;
    if (status >= 200 && status < 300) {
        printf("\npublished %s@%s -> %s\n", name, version, PKG_REGISTRY_URL);
        unlink(tarball);
        value_decref(resp);
        return 0;
    }
    fprintf(stderr, "\nxs publish: registry returned %d\n", status);
    if (bd && VAL_TAG(bd) == XS_STR && bd->s) {
        fprintf(stderr, "  %s\n", bd->s);
    }
    value_decref(resp);
    return 1;
#endif /* PKG_HAS_REGISTRY_HTTP */
}
