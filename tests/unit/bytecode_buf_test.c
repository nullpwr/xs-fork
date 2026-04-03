#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "vm/bytecode.h"
#include "core/parser.h"
#include "core/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Inline stubs for transitive deps that bytecode.c doesn't actually use
 * but the linker still resolves. Keeps this test independent of the
 * interpreter / plugin stacks. */
int g_no_color = 1;
Value *interp_eval(void *i, Node *e) { (void)i; (void)e; return NULL; }
Node *parse_plugin_decl(Parser *p)   { (void)p; return NULL; }

/* Verify proto_read_buf parses a .xsc image identically to
 * proto_read_file. The test depends on the host `xs` binary having
 * been built (the e2e suite does that), and uses it as a fixture
 * generator so this unit test stays free of compiler/runtime deps. */

static uint8_t *slurp(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

TEST(read_buf_round_trip) {
    /* Generate a minimal .xsc via the host binary. The temp-dir base
     * differs between platforms (mingw doesn't have /tmp); honor the
     * env variables that the host already uses for tmpfile placement. */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = ".";
    char src_path[256], xsc_path[256];
    snprintf(src_path, sizeof src_path, "%s/xs_bcbuf_src_XXXXXX", tmpdir);
    snprintf(xsc_path, sizeof xsc_path, "%s/xs_bcbuf_out_XXXXXX", tmpdir);
    int fd1 = mkstemp(src_path); if (fd1 < 0) FAIL("mkstemp src");
    int fd2 = mkstemp(xsc_path); if (fd2 < 0) FAIL("mkstemp xsc");
    close(fd2);
    FILE *f = fdopen(fd1, "w");
    fprintf(f, "let x = 1 + 1\n");
    fclose(f);

    /* Locate the xs binary: tests/run-all.sh exports XS, otherwise
     * fall back to the conventional repo-root location. */
    const char *xs_bin = getenv("XS");
    if (!xs_bin) xs_bin = "./xs";
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" build \"%s\" -o \"%s\" >/dev/null 2>&1",
             xs_bin, src_path, xsc_path);
    if (system(cmd) != 0) {
        unlink(src_path); unlink(xsc_path);
        FAIL("xs build failed (host binary not yet built?)");
    }

    long sz = 0;
    uint8_t *buf = slurp(xsc_path, &sz);
    ASSERT_NOT_NULL(buf);
    ASSERT(sz >= 6);

    /* Both readers should yield non-null protos. */
    XSProto *via_buf  = proto_read_buf(buf, (size_t)sz);
    XSProto *via_file = proto_read_file(xsc_path);
    ASSERT_NOT_NULL(via_buf);
    ASSERT_NOT_NULL(via_file);
    ASSERT_EQ_INT(via_buf->chunk.len, via_file->chunk.len);
    ASSERT_EQ_INT(via_buf->arity, via_file->arity);

    proto_free(via_buf);
    proto_free(via_file);
    free(buf);
    unlink(src_path); unlink(xsc_path);
}

TEST(read_buf_rejects_short) {
    uint8_t junk[3] = { 'X', 'S', 'C' };
    ASSERT_NULL(proto_read_buf(junk, sizeof junk));
    ASSERT_NULL(proto_read_buf(NULL, 0));
}

TEST(read_buf_rejects_bad_magic) {
    uint8_t bad[8] = { 'N', 'O', 'P', 'E', 0, 1, 0, 0 };
    ASSERT_NULL(proto_read_buf(bad, sizeof bad));
}

int main(void) {
    RUN_TEST(read_buf_round_trip);
    RUN_TEST(read_buf_rejects_short);
    RUN_TEST(read_buf_rejects_bad_magic);
    REPORT_AND_EXIT("bytecode_buf");
}
