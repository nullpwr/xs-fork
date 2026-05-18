#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "pkg/self.h"
#include "pkg/pkg_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef XS_VERSION
#define XS_VERSION "dev"
#endif

#if defined(__wasi__)

int xs_self_upgrade(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "xs upgrade: not supported on WASM/WASI\n");
    return 1;
}
int xs_self_uninstall(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "xs uninstall: not supported on WASM/WASI\n");
    return 1;
}
void xs_self_cleanup_stale_old(void) { }

#else

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #define PATH_SEP_CHAR '\\'
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <ftw.h>
  #include <dirent.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #endif
  #define PATH_SEP_CHAR '/'
#endif

#include "tls/bearssl/bearssl_hash.h"

/* ------------------------------------------------------------------ */
/* platform helpers                                                    */
/* ------------------------------------------------------------------ */

static int detect_platform(const char **os_out, const char **arch_out,
                           const char **suffix_out) {
#if defined(_WIN32)
    *os_out = "windows";
    *suffix_out = ".exe";
#elif defined(__linux__)
    *os_out = "linux";
    *suffix_out = "";
#elif defined(__APPLE__)
    *os_out = "macos";
    *suffix_out = "";
#else
    fprintf(stderr, "xs: unsupported platform for self-management\n");
    return -1;
#endif
    /* only x86_64 published for now; arm64 will get its own asset later */
    *arch_out = "x86_64";
    return 0;
}

static int self_path(char *buf, size_t buflen) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buflen);
    if (n == 0 || n >= buflen) {
        fprintf(stderr, "xs: could not resolve own path (GetModuleFileName failed)\n");
        return -1;
    }
    return 0;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, buflen - 1);
    if (n <= 0) {
        fprintf(stderr, "xs: could not resolve own path: %s\n", strerror(errno));
        return -1;
    }
    buf[n] = '\0';
    return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)buflen;
    if (_NSGetExecutablePath(buf, &sz) != 0) {
        fprintf(stderr, "xs: could not resolve own path (buffer too small?)\n");
        return -1;
    }
    return 0;
#else
    (void)buf; (void)buflen;
    fprintf(stderr, "xs: cannot resolve own path on this platform\n");
    return -1;
#endif
}

/* Build a sibling temp path next to install_path: install_path + suffix.
   Used for the .new staging file and the .old fallback name. */
static int sibling_path(const char *install_path, const char *suffix,
                        char *out, size_t out_len) {
    size_t a = strlen(install_path);
    size_t b = strlen(suffix);
    if (a + b + 1 > out_len) return -1;
    memcpy(out, install_path, a);
    memcpy(out + a, suffix, b);
    out[a + b] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers exposed to the unit test (see tests/unit/self_test.c)       */
/* ------------------------------------------------------------------ */

int xs_self_sibling_path(const char *install_path, const char *suffix,
                         char *out, size_t out_len) {
    return sibling_path(install_path, suffix, out, out_len);
}

/* On Windows the running .exe is locked so the destination can't be
   overwritten. The trick: rename the running file out of the way, then
   write the new bytes to the original path. The renamed handle is still
   alive, but the slot is freed for a new file. The .old file gets
   removed lazily at the next `xs` startup (cleanup_stale_old below).
   On POSIX `rename(new, install_path)` already handles a busy
   destination atomically, so this fallback is Windows-only. */
int xs_self_make_room_for_replace(const char *install_path) {
#if defined(_WIN32)
    char old_path[PATH_MAX];
    if (sibling_path(install_path, ".old", old_path, sizeof(old_path)) != 0)
        return -1;

    /* If a stale .old already exists from a previous upgrade, try to
       evict it. We may be holding it open through a forked child;
       schedule a reboot-time delete as a last resort. */
    if (GetFileAttributesA(old_path) != INVALID_FILE_ATTRIBUTES) {
        if (!DeleteFileA(old_path)) {
            MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }

    if (GetFileAttributesA(install_path) == INVALID_FILE_ATTRIBUTES) {
        /* nothing at the install path yet, no rename needed */
        return 0;
    }

    if (!MoveFileExA(install_path, old_path, MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        fprintf(stderr,
            "xs upgrade: could not move running binary out of the way "
            "(error %lu)\n", (unsigned long)err);
        return -1;
    }
    return 0;
#else
    (void)install_path;
    return 0;
#endif
}

/* Best-effort removal of <install_path>.old left behind by a previous
   upgrade. Always succeeds from the caller's point of view: a stale
   .old that can't be removed (file still mapped, perms, etc.) just
   gets re-tried on the next launch. */
void xs_self_cleanup_stale_old(void) {
#if defined(_WIN32)
    char self[PATH_MAX];
    char old_path[PATH_MAX];
    if (self_path(self, sizeof(self)) != 0) return;
    if (sibling_path(self, ".old", old_path, sizeof(old_path)) != 0) return;
    if (GetFileAttributesA(old_path) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(old_path);
#endif
}

/* ------------------------------------------------------------------ */
/* GitHub release helpers                                              */
/* ------------------------------------------------------------------ */

/* extract "tag_name" value from GitHub releases JSON; simple substring scan */
static int parse_latest_tag(const char *json, char *tag_out, size_t tag_size) {
    const char *key = strstr(json, "\"tag_name\"");
    if (!key) return -1;
    const char *colon = strchr(key + 10, ':');
    if (!colon) return -1;
    const char *q1 = strchr(colon + 1, '"');
    if (!q1) return -1;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2) return -1;
    size_t len = (size_t)(q2 - q1);
    if (len >= tag_size) len = tag_size - 1;
    memcpy(tag_out, q1, len);
    tag_out[len] = '\0';
    return 0;
}

static int sha256_hex_of_file(const char *path, char hex_out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "xs: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        br_sha256_update(&ctx, buf, n);
    fclose(f);
    unsigned char digest[32];
    br_sha256_out(&ctx, digest);
    for (int i = 0; i < 32; i++)
        snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
    hex_out[64] = '\0';
    return 0;
}

static int download_to(const char *url, const char *dest_path) {
    PkgHttpResponse resp = {0, NULL, 0};
    int rc = pkg_http_request("GET", url, NULL, 0, NULL, 0, &resp);
    if (rc != 0 || resp.status != 200) {
        fprintf(stderr, "xs: download failed (HTTP %d): %s\n", resp.status, url);
        pkg_http_response_free(&resp);
        return -1;
    }
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        fprintf(stderr, "xs: cannot write to %s: %s\n", dest_path, strerror(errno));
        pkg_http_response_free(&resp);
        return -1;
    }
    size_t written = fwrite(resp.body, 1, resp.body_len, f);
    fclose(f);
    pkg_http_response_free(&resp);
    if (written != resp.body_len) {
        fprintf(stderr, "xs: short write to %s\n", dest_path);
        return -1;
    }
    return 0;
}

static int prompt_yes_no(const char *q, int default_yes) {
    printf("%s %s ", q, default_yes ? "[Y/n]" : "[y/N]");
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof(line), stdin)) return default_yes;
    char c = line[0];
    if (c == 'y' || c == 'Y') return 1;
    if (c == 'n' || c == 'N') return 0;
    return default_yes;
}

static int confirm_or_yes_flag(int argc, char **argv, const char *msg) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            return 1;
    }
    return prompt_yes_no(msg, 0);
}

/* Pick a writable temp directory. On Windows, ask the OS; on Unix
   stick to /tmp which is what the previous version did. The result
   is a directory path with no trailing separator. */
static int temp_dir(char *out, size_t out_len) {
#if defined(_WIN32)
    DWORD n = GetTempPathA((DWORD)out_len, out);
    if (n == 0 || n >= out_len) return -1;
    /* trim trailing backslash so the join below is consistent */
    if (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/'))
        out[n - 1] = '\0';
    return 0;
#else
    snprintf(out, out_len, "/tmp");
    return 0;
#endif
}

/* Make a unique temp file path. Caller must remove it. Pre-creates
   the file so the path is reserved. */
static int unique_temp_file(const char *stem, char *out, size_t out_len) {
    char dir[PATH_MAX];
    if (temp_dir(dir, sizeof(dir)) != 0) return -1;
#if defined(_WIN32)
    /* GetTempFileNameA writes a unique 0-byte file and returns its path */
    char tmp[PATH_MAX];
    if (GetTempFileNameA(dir, stem, 0, tmp) == 0) return -1;
    if (strlen(tmp) >= out_len) { DeleteFileA(tmp); return -1; }
    strcpy(out, tmp);
    return 0;
#else
    if (snprintf(out, out_len, "%s/%s.XXXXXX", dir, stem) >= (int)out_len)
        return -1;
    int fd = mkstemp(out);
    if (fd < 0) return -1;
    close(fd);
    return 0;
#endif
}

/* Atomic-ish replace: rename(src, dst). On POSIX rename() handles
   overwriting an existing dst. On Windows the destination might be
   the running .exe, in which case make_room_for_replace has already
   moved it aside, so MoveFileEx with REPLACE_EXISTING is safe. */
static int atomic_replace(const char *src, const char *dst) {
#if defined(_WIN32)
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING)) return 0;
    /* Fall back to copy + delete in case the temp file lives on a
       different volume from the install dir. */
    DWORD err = GetLastError();
    if (err == ERROR_NOT_SAME_DEVICE) {
        if (CopyFileA(src, dst, FALSE)) {
            DeleteFileA(src);
            return 0;
        }
    }
    fprintf(stderr, "xs upgrade: rename failed (error %lu)\n",
            (unsigned long)err);
    return -1;
#else
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) {
        fprintf(stderr, "xs upgrade: rename failed: %s\n", strerror(errno));
        return -1;
    }
    /* cross-device: copy then unlink */
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        fprintf(stderr, "xs upgrade: cross-device copy failed: %s\n",
                strerror(errno));
        return -1;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    fclose(in);
    fclose(out);
    unlink(src);
    return ok ? 0 : -1;
#endif
}

#if !defined(_WIN32)
/* simple recursive rm; used by uninstall --with-data */
static int rm_rf_cb(const char *path, const struct stat *sb,
                    int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_DP || typeflag == FTW_D)
        return rmdir(path);
    return unlink(path);
}

static int rm_rf(const char *path) {
    return nftw(path, rm_rf_cb, 64, FTW_DEPTH | FTW_PHYS);
}
#else
/* recursive rm for Windows: walk the tree manually and remove leaves
   first. Best-effort; failures bubble up as a non-zero return. */
static int rm_rf(const char *path) {
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int rc = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rm_rf(child) != 0) rc = -1;
            } else {
                /* clear read-only so DeleteFile doesn't refuse */
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(child)) rc = -1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path)) rc = -1;
    return rc;
}
#endif

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

int xs_self_upgrade(int argc, char **argv) {
    const char *os = NULL, *arch = NULL, *suffix = NULL;
    if (detect_platform(&os, &arch, &suffix) != 0) return 1;

    char bin_path[PATH_MAX];
    if (self_path(bin_path, sizeof(bin_path)) != 0) return 1;

    /* fetch latest release metadata */
    PkgHttpResponse meta = {0, NULL, 0};
    const char *api_hdrs[] = { "User-Agent: xs-cli", "Accept: application/json" };
    int mrc = pkg_http_request("GET",
        "https://api.github.com/repos/xs-lang0/xs/releases/latest",
        api_hdrs, 2, NULL, 0, &meta);
    if (mrc != 0 || meta.status != 200) {
        fprintf(stderr, "xs upgrade: could not reach GitHub API (HTTP %d)\n", meta.status);
        pkg_http_response_free(&meta);
        return 1;
    }

    char tag[64];
    if (parse_latest_tag(meta.body, tag, sizeof(tag)) != 0) {
        fprintf(stderr, "xs upgrade: could not parse release tag from API response\n");
        pkg_http_response_free(&meta);
        return 1;
    }
    pkg_http_response_free(&meta);

    /* compare to running version */
    char current_tag[64];
    snprintf(current_tag, sizeof(current_tag), "v%s", XS_VERSION);
    if (strcmp(tag, current_tag) == 0) {
        printf("xs is already up to date (%s)\n", XS_VERSION);
        return 0;
    }
    printf("current: %s, latest: %s\n", current_tag, tag);

    /* prompt unless --yes / -y */
    if (!confirm_or_yes_flag(argc, argv, "replace the current binary?"))
        return 0;

    /* build download URLs */
    char bin_url[256];
    char sha_url[272];
    snprintf(bin_url, sizeof(bin_url),
        "https://github.com/xs-lang0/xs/releases/latest/download/xs-%s-%s%s",
        os, arch, suffix);
    snprintf(sha_url, sizeof(sha_url), "%s.sha256", bin_url);

    /* temp file for new binary */
    char tmp_path[PATH_MAX];
    if (unique_temp_file("xsup", tmp_path, sizeof(tmp_path)) != 0) {
        fprintf(stderr, "xs upgrade: could not create temp file\n");
        return 1;
    }

    printf("downloading %s ...\n", bin_url);
    if (download_to(bin_url, tmp_path) != 0) {
        remove(tmp_path);
        return 1;
    }

    /* download sha256 */
    char sha_tmp[PATH_MAX];
    if (unique_temp_file("xssha", sha_tmp, sizeof(sha_tmp)) != 0) {
        fprintf(stderr, "xs upgrade: could not create temp file\n");
        remove(tmp_path);
        return 1;
    }

    if (download_to(sha_url, sha_tmp) != 0) {
        remove(tmp_path);
        remove(sha_tmp);
        return 1;
    }

    /* read expected digest from sha256 file (first whitespace-delimited token) */
    char expected_hex[65] = {0};
    FILE *sf = fopen(sha_tmp, "r");
    if (!sf) {
        fprintf(stderr, "xs upgrade: cannot read sha256 file\n");
        remove(tmp_path); remove(sha_tmp);
        return 1;
    }
    if (fscanf(sf, "%64s", expected_hex) != 1) {
        fprintf(stderr, "xs upgrade: sha256 file appears empty or malformed\n");
        fclose(sf);
        remove(tmp_path); remove(sha_tmp);
        return 1;
    }
    fclose(sf);
    remove(sha_tmp);

    /* verify */
    char actual_hex[65];
    if (sha256_hex_of_file(tmp_path, actual_hex) != 0) {
        remove(tmp_path);
        return 1;
    }
    if (strcmp(expected_hex, actual_hex) != 0) {
        fprintf(stderr, "xs upgrade: SHA-256 mismatch, download may be corrupt\n");
        fprintf(stderr, "  expected: %s\n  got:      %s\n", expected_hex, actual_hex);
        remove(tmp_path);
        return 1;
    }

#if !defined(_WIN32)
    if (chmod(tmp_path, 0755) != 0) {
        fprintf(stderr, "xs upgrade: chmod failed: %s\n", strerror(errno));
        remove(tmp_path);
        return 1;
    }
#endif

    /* On Windows, move the running binary out of the way before
       writing the new one. No-op on POSIX. */
    if (xs_self_make_room_for_replace(bin_path) != 0) {
        remove(tmp_path);
        return 1;
    }

    if (atomic_replace(tmp_path, bin_path) != 0) {
        remove(tmp_path);
        return 1;
    }

    printf("upgraded xs to %s\n", tag);
#if defined(_WIN32)
    printf("note: open shells are still using the old binary. "
           "start a new terminal to pick up %s.\n", tag);
#endif
    return 0;
}

int xs_self_uninstall(int argc, char **argv) {
    const char *os = NULL, *arch = NULL, *suffix = NULL;
    if (detect_platform(&os, &arch, &suffix) != 0) return 1;
    (void)os; (void)arch; (void)suffix;

    char bin_path[PATH_MAX];
    if (self_path(bin_path, sizeof(bin_path)) != 0) return 1;

    int with_data = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--with-data") == 0) with_data = 1;
    }

    printf("this will remove %s\n", bin_path);

#if defined(_WIN32)
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    char xs_dir[PATH_MAX] = {0};
    char xs_cache[PATH_MAX] = {0};
    if (with_data && home) {
        snprintf(xs_dir,   sizeof(xs_dir),   "%s%c.xs", home, PATH_SEP_CHAR);
        snprintf(xs_cache, sizeof(xs_cache), "%s%c.xs_cache", home, PATH_SEP_CHAR);
        struct stat st;
        if (stat(xs_dir, &st) == 0)
            printf("         %s\n", xs_dir);
        else
            xs_dir[0] = '\0';
        if (stat(xs_cache, &st) == 0)
            printf("         %s\n", xs_cache);
        else
            xs_cache[0] = '\0';
    }

    if (!confirm_or_yes_flag(argc, argv, "remove xs and these files?"))
        return 0;

    if (with_data) {
        if (xs_dir[0] && rm_rf(xs_dir) != 0)
            fprintf(stderr, "xs uninstall: warning: could not remove %s\n", xs_dir);
        if (xs_cache[0] && rm_rf(xs_cache) != 0)
            fprintf(stderr, "xs uninstall: warning: could not remove %s\n", xs_cache);
    }

#if defined(_WIN32)
    /* Windows: can't delete a running .exe. Use the same rename trick
       as upgrade: move it to .old, then schedule a reboot-time delete
       so it disappears even if no future xs run gets the chance to
       sweep it. The user can also delete xs.exe.old by hand. */
    char old_path[PATH_MAX];
    if (sibling_path(bin_path, ".old", old_path, sizeof(old_path)) != 0) {
        fprintf(stderr, "xs uninstall: path too long\n");
        return 1;
    }
    if (GetFileAttributesA(old_path) != INVALID_FILE_ATTRIBUTES) {
        if (!DeleteFileA(old_path))
            MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!MoveFileExA(bin_path, old_path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr,
            "xs uninstall: could not move %s aside (error %lu)\n",
            bin_path, (unsigned long)GetLastError());
        return 1;
    }
    /* Try an immediate delete; if Windows still considers the file
       open, fall back to a reboot-time delete. */
    if (!DeleteFileA(old_path))
        MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    printf("removed xs (close any open shells to release the file lock)\n");
    return 0;
#else
    if (unlink(bin_path) != 0) {
        fprintf(stderr, "xs uninstall: could not remove %s: %s\n",
                bin_path, strerror(errno));
        return 1;
    }
    printf("removed xs\n");
    return 0;
#endif
}

#endif /* !__wasi__ */
