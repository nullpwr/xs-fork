#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "pkg/self.h"
#include "pkg/pkg_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

#elif defined(_WIN32)

int xs_self_upgrade(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
        "xs upgrade: self-upgrade is not supported on Windows in this build.\n"
        "Re-run the install script to get the latest version:\n"
        "    irm xslang.org/install.ps1 | iex\n");
    return 1;
}
int xs_self_uninstall(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
        "xs uninstall: not yet implemented on Windows.\n"
        "Remove C:\\xs\\bin\\xs.exe manually and remove that path from your system PATH.\n");
    return 1;
}

#else /* Linux / macOS */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ftw.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "tls/bearssl/bearssl_hash.h"

/* ------------------------------------------------------------------ */

static int detect_platform(const char **os_out, const char **arch_out) {
#if defined(__linux__)
    *os_out = "linux";
#elif defined(__APPLE__)
    *os_out = "macos";
#else
    fprintf(stderr, "xs: unsupported platform for self-management\n");
    return -1;
#endif
    /* only x86_64 for now; arm64 binaries aren't published yet */
    *arch_out = "x86_64";
    return 0;
}

static int self_path(char *buf, size_t buflen) {
#if defined(__linux__)
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

/* ------------------------------------------------------------------ */

int xs_self_upgrade(int argc, char **argv) {
    (void)argv;
    const char *os = NULL, *arch = NULL;
    if (detect_platform(&os, &arch) != 0) return 1;

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
    /* argc here is the count of args after "upgrade" */
    if (!confirm_or_yes_flag(argc, argv, "replace the current binary?"))
        return 0;

    /* build download URL */
    char bin_url[256];
    char sha_url[272];
    snprintf(bin_url, sizeof(bin_url),
        "https://github.com/xs-lang0/xs/releases/latest/download/xs-%s-%s",
        os, arch);
    snprintf(sha_url, sizeof(sha_url), "%s.sha256", bin_url);

    /* temp file for new binary */
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/xs.upgrade.XXXXXX");
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        fprintf(stderr, "xs upgrade: mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    close(tmp_fd);

    printf("downloading %s ...\n", bin_url);
    if (download_to(bin_url, tmp_path) != 0) {
        unlink(tmp_path);
        return 1;
    }

    /* download sha256 */
    char sha_tmp[PATH_MAX];
    snprintf(sha_tmp, sizeof(sha_tmp), "/tmp/xs.upgrade.sha256.XXXXXX");
    int sha_fd = mkstemp(sha_tmp);
    if (sha_fd < 0) {
        fprintf(stderr, "xs upgrade: mkstemp (sha256) failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return 1;
    }
    close(sha_fd);

    if (download_to(sha_url, sha_tmp) != 0) {
        unlink(tmp_path);
        unlink(sha_tmp);
        return 1;
    }

    /* read expected digest from sha256 file (first whitespace-delimited token) */
    char expected_hex[65] = {0};
    FILE *sf = fopen(sha_tmp, "r");
    if (!sf) {
        fprintf(stderr, "xs upgrade: cannot read sha256 file\n");
        unlink(tmp_path); unlink(sha_tmp);
        return 1;
    }
    if (fscanf(sf, "%64s", expected_hex) != 1) {
        fprintf(stderr, "xs upgrade: sha256 file appears empty or malformed\n");
        fclose(sf);
        unlink(tmp_path); unlink(sha_tmp);
        return 1;
    }
    fclose(sf);
    unlink(sha_tmp);

    /* verify */
    char actual_hex[65];
    if (sha256_hex_of_file(tmp_path, actual_hex) != 0) {
        unlink(tmp_path);
        return 1;
    }
    if (strcmp(expected_hex, actual_hex) != 0) {
        fprintf(stderr, "xs upgrade: SHA-256 mismatch - download may be corrupt\n");
        fprintf(stderr, "  expected: %s\n  got:      %s\n", expected_hex, actual_hex);
        unlink(tmp_path);
        return 1;
    }

    if (chmod(tmp_path, 0755) != 0) {
        fprintf(stderr, "xs upgrade: chmod failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return 1;
    }

    /* atomic replace */
    if (rename(tmp_path, bin_path) != 0) {
        if (errno == EXDEV) {
            /* cross-device: copy then unlink */
            FILE *src = fopen(tmp_path, "rb");
            FILE *dst = fopen(bin_path, "wb");
            if (!src || !dst) {
                fprintf(stderr, "xs upgrade: cross-device copy failed: %s\n", strerror(errno));
                if (src) fclose(src);
                if (dst) fclose(dst);
                unlink(tmp_path);
                return 1;
            }
            char cpbuf[65536];
            size_t nr;
            int copy_ok = 1;
            while ((nr = fread(cpbuf, 1, sizeof(cpbuf), src)) > 0) {
                if (fwrite(cpbuf, 1, nr, dst) != nr) { copy_ok = 0; break; }
            }
            fclose(src);
            fclose(dst);
            unlink(tmp_path);
            if (!copy_ok) {
                fprintf(stderr, "xs upgrade: write failed during cross-device copy\n");
                return 1;
            }
            chmod(bin_path, 0755);
        } else {
            fprintf(stderr, "xs upgrade: rename failed: %s\n", strerror(errno));
            unlink(tmp_path);
            return 1;
        }
    }

    printf("upgraded xs to %s\n", tag);
    return 0;
}

int xs_self_uninstall(int argc, char **argv) {
    const char *os = NULL, *arch = NULL;
    if (detect_platform(&os, &arch) != 0) return 1;
    (void)os; (void)arch;

    char bin_path[PATH_MAX];
    if (self_path(bin_path, sizeof(bin_path)) != 0) return 1;

    int with_data = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--with-data") == 0) with_data = 1;
    }

    printf("this will remove %s\n", bin_path);

    const char *home = getenv("HOME");
    char xs_dir[PATH_MAX] = {0};
    char xs_cache[PATH_MAX] = {0};
    if (with_data && home) {
        snprintf(xs_dir,   sizeof(xs_dir),   "%s/.xs", home);
        snprintf(xs_cache, sizeof(xs_cache), "%s/.xs_cache", home);
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

    if (unlink(bin_path) != 0) {
        fprintf(stderr, "xs uninstall: could not remove %s: %s\n",
                bin_path, strerror(errno));
        return 1;
    }

    printf("removed xs\n");
    return 0;
}

#endif /* !_WIN32 && !__wasi__ */
