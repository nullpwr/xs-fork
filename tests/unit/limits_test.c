#include "test.h"
#include "core/limits.h"

TEST(default_is_no_limit) {
    xs_limits_clear();
    ASSERT(!xs_limits_tick());
    ASSERT(!xs_limits_check());
}

TEST(instruction_budget_trips) {
    xs_limits_clear();
    xs_limits_set_instructions(5);
    xs_limits_reset();
    ASSERT(!xs_limits_tick());
    ASSERT(!xs_limits_tick());
    ASSERT(!xs_limits_tick());
    ASSERT(!xs_limits_tick());
    /* 5th tick crosses the budget */
    int tripped = 0;
    for (int i = 0; i < 20 && !tripped; i++) {
        tripped = xs_limits_tick();
    }
    ASSERT(tripped);
    ASSERT_EQ_INT(xs_limits_exceeded(), 1);
}

TEST(reset_clears_counters_but_keeps_cap) {
    xs_limits_clear();
    xs_limits_set_instructions(3);
    xs_limits_reset();
    while (!xs_limits_tick()) { /* drain */ }
    ASSERT(xs_limits_exceeded());
    xs_limits_reset();
    ASSERT(!xs_limits_exceeded());
    ASSERT_EQ_INT(xs_limits_get_instructions_budget(), 3);
}

TEST(clear_removes_cap) {
    xs_limits_clear();
    xs_limits_set_instructions(3);
    xs_limits_clear();
    ASSERT_EQ_INT(xs_limits_get_instructions_budget(), 0);
    for (int i = 0; i < 100; i++) ASSERT(!xs_limits_tick());
}

TEST(exceeded_name_human_readable) {
    xs_limits_clear();
    xs_limits_set_instructions(1);
    xs_limits_reset();
    while (!xs_limits_tick()) {}
    const char *name = xs_limits_exceeded_name();
    ASSERT_NOT_NULL(name);
    ASSERT(name[0] != '\0');
}

int main(void) {
    RUN_TEST(default_is_no_limit);
    RUN_TEST(instruction_budget_trips);
    RUN_TEST(reset_clears_counters_but_keeps_cap);
    RUN_TEST(clear_removes_cap);
    RUN_TEST(exceeded_name_human_readable);
    REPORT_AND_EXIT("limits");
}
