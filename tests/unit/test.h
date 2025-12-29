/* Tiny unit-test harness for the compiler internals. One TEST(name) per
   function. RUN_TEST(name) registers and runs it. ASSERT family bails
   fast on failure. Kept header-only so each test file is its own .c. */
#ifndef XS_UNIT_TEST_H
#define XS_UNIT_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;
static const char *g_current = "?";

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
        g_current = #name; \
        test_##name(); \
        g_tests_run++; \
    } while (0)

#define FAIL(fmt, ...) do { \
        fprintf(stderr, "  FAIL  %s:%d %s: " fmt "\n", \
                __FILE__, __LINE__, g_current, ##__VA_ARGS__); \
        g_tests_failed++; \
        return; \
    } while (0)

#define ASSERT(cond) \
    do { if (!(cond)) FAIL("expected %s", #cond); } while (0)
#define ASSERT_EQ_INT(a, b) \
    do { long _a = (long)(a), _b = (long)(b); \
         if (_a != _b) FAIL("%s = %ld, expected %ld", #a, _a, _b); \
    } while (0)
#define ASSERT_EQ_STR(a, b) \
    do { const char *_a = (a), *_b = (b); \
         if (!_a || !_b || strcmp(_a, _b) != 0) \
            FAIL("%s = %s, expected %s", #a, _a ? _a : "(null)", _b ? _b : "(null)"); \
    } while (0)
#define ASSERT_NOT_NULL(p) \
    do { if (!(p)) FAIL("%s is null", #p); } while (0)
#define ASSERT_NULL(p) \
    do { if ((p)) FAIL("%s is not null", #p); } while (0)

#define REPORT_AND_EXIT(suite) do { \
    if (g_tests_failed) { \
        fprintf(stderr, "\n[unit:%s] %d/%d failed\n", suite, \
                g_tests_failed, g_tests_run); \
        return 1; \
    } \
    printf("[unit:%s] %d passed\n", suite, g_tests_run); \
    return 0; \
} while (0)

#endif
