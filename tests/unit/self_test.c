#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "pkg/self.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #define unlink _unlink
#else
  #include <unistd.h>
#endif

/* Stubs: pkg/self.c references pkg_http for the upgrade path and
   bearssl SHA-256 for digest verification. The helpers we exercise
   here never reach either, so satisfy the linker with no-ops. */
#include "pkg/pkg_http.h"
#include "tls/bearssl/bearssl_hash.h"

int pkg_http_request(const char *method, const char *url,
                     const char *const *headers, int n_headers,
                     const char *body, size_t body_len,
                     PkgHttpResponse *out) {
    (void)method; (void)url; (void)headers; (void)n_headers;
    (void)body; (void)body_len;
    if (out) { out->status = 0; out->body = NULL; out->body_len = 0; }
    return -1;
}
void pkg_http_response_free(PkgHttpResponse *r) {
    if (r && r->body) { free(r->body); r->body = NULL; r->body_len = 0; }
}

void br_sha256_init(br_sha256_context *ctx) { (void)ctx; }
/* br_sha256_update is a #define for br_sha224_update in the bearssl
   header, so stub the actual symbol name. */
void br_sha224_update(br_sha224_context *ctx, const void *data, size_t len) {
    (void)ctx; (void)data; (void)len;
}
void br_sha256_out(const br_sha256_context *ctx, void *out) {
    (void)ctx;
    if (out) memset(out, 0, 32);
}

/* ------------------------------------------------------------------ */

TEST(sibling_path_basic) {
    char out[256];
    int rc = xs_self_sibling_path("/usr/local/xs/bin/xs", ".old",
                                  out, sizeof(out));
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_STR(out, "/usr/local/xs/bin/xs.old");
}

TEST(sibling_path_windows_style) {
    char out[256];
    int rc = xs_self_sibling_path("C:\\xs\\bin\\xs.exe", ".old",
                                  out, sizeof(out));
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_STR(out, "C:\\xs\\bin\\xs.exe.old");
}

TEST(sibling_path_too_small) {
    char out[8];
    int rc = xs_self_sibling_path("/usr/local/xs/bin/xs", ".old",
                                  out, sizeof(out));
    ASSERT_EQ_INT(rc, -1);
}

TEST(sibling_path_empty_suffix) {
    char out[64];
    int rc = xs_self_sibling_path("foo", "", out, sizeof(out));
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_STR(out, "foo");
}

/* ---- rename-trick integration ---- */

static const char *temp_dir_for_test(void) {
#if defined(_WIN32)
    static char buf[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(buf), buf);
    if (n == 0) return ".";
    if (buf[n - 1] == '\\' || buf[n - 1] == '/') buf[n - 1] = '\0';
    return buf;
#else
    const char *t = getenv("TMPDIR");
    return t && t[0] ? t : "/tmp";
#endif
}

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(contents);
    int ok = (fwrite(contents, 1, len, f) == len);
    fclose(f);
    return ok ? 0 : -1;
}

static int read_file_eq(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, expected) == 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Simulate the upgrade swap end-to-end:
   1. drop a fake "old" binary into a temp path
   2. call make_room_for_replace -> on Windows it renames to .old,
      on POSIX it's a no-op
   3. write a fake "new" binary to the install path
   4. assert install path now has the new bytes
   5. on Windows, assert the .old sibling exists and has the old bytes
   6. on POSIX, do the equivalent of the POSIX rename overwrite */
TEST(make_room_for_replace_swap) {
    char install[512];
    snprintf(install, sizeof(install), "%s%cxs_self_test_swap%c",
             temp_dir_for_test(),
#if defined(_WIN32)
             '\\', '\\');
#else
             '/', '/');
#endif
    /* turn the trailing separator into part of a filename */
    install[strlen(install) - 1] = '\0';
    strncat(install, "_bin", sizeof(install) - strlen(install) - 1);

    /* make sure we start from a clean slate */
    char old[600];
    xs_self_sibling_path(install, ".old", old, sizeof(old));
    unlink(install);
    unlink(old);

    ASSERT_EQ_INT(write_file(install, "OLD-BINARY"), 0);
    ASSERT(file_exists(install));

    int rc = xs_self_make_room_for_replace(install);
    ASSERT_EQ_INT(rc, 0);

#if defined(_WIN32)
    /* install path slot should be free, .old should hold the old bytes */
    ASSERT(!file_exists(install));
    ASSERT(file_exists(old));
    ASSERT(read_file_eq(old, "OLD-BINARY"));
#else
    /* POSIX: no-op, file is still where it was */
    ASSERT(file_exists(install));
    ASSERT(read_file_eq(install, "OLD-BINARY"));
    /* simulate what the upgrade actually does on POSIX: rename(new, install) */
#endif

    /* write the "new" binary */
    char tmp_new[600];
    snprintf(tmp_new, sizeof(tmp_new), "%s.new", install);
    ASSERT_EQ_INT(write_file(tmp_new, "NEW-BINARY"), 0);
    ASSERT_EQ_INT(rename(tmp_new, install), 0);

    ASSERT(file_exists(install));
    ASSERT(read_file_eq(install, "NEW-BINARY"));

    /* cleanup */
    unlink(install);
    unlink(old);
}

/* cleanup_stale_old must not blow up when no .old exists for the
   running binary. It's invoked from main() on every launch, so it
   has to be safe in the steady state. */
TEST(cleanup_stale_old_is_safe) {
    /* just make sure it returns without exploding */
    xs_self_cleanup_stale_old();
}

int main(void) {
    RUN_TEST(sibling_path_basic);
    RUN_TEST(sibling_path_windows_style);
    RUN_TEST(sibling_path_too_small);
    RUN_TEST(sibling_path_empty_suffix);
    RUN_TEST(make_room_for_replace_swap);
    RUN_TEST(cleanup_stale_old_is_safe);
    REPORT_AND_EXIT("self");
}
