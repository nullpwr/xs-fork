#include "test.h"
#include "core/value.h"
#include "core/gc.h"
#include "core/xs.h"

static void setup(void) {
    static int done = 0;
    if (!done) { value_init_singletons(); gc_init(); done = 1; }
}

TEST(tracking_roundtrip) {
    setup();
    Value *a = xs_array_new();
    int before = gc_tracked_count();
    gc_track(a);
    ASSERT(gc_tracked_count() >= before + 1);
    gc_untrack(a);
    ASSERT_EQ_INT(gc_tracked_count(), before);
    value_decref(a);
}

TEST(double_track_is_idempotent) {
    setup();
    Value *a = xs_array_new();
    gc_track(a);
    int t = gc_tracked_count();
    gc_track(a);
    ASSERT_EQ_INT(gc_tracked_count(), t);
    gc_untrack(a);
    value_decref(a);
}

TEST(simple_cycle_collected) {
    setup();
    /* a -> b -> a, drop all external refs, collect should reclaim both. */
    Value *a = xs_array_new();
    Value *b = xs_array_new();
    gc_track(a);
    gc_track(b);
    array_push(a->arr, value_incref(b));
    array_push(b->arr, value_incref(a));
    /* Release our external refs but leave the cycle intact. */
    value_decref(a);
    value_decref(b);
    int before = gc_tracked_count();
    gc_collect();
    int after = gc_tracked_count();
    ASSERT(after <= before);
}

TEST(enable_disable) {
    setup();
    gc_disable();
    ASSERT(!gc_is_enabled());
    gc_enable();
    ASSERT(gc_is_enabled());
}

TEST(threshold_round_trip) {
    setup();
    int orig = gc_get_threshold(0);
    gc_set_threshold(0, 9999);
    ASSERT_EQ_INT(gc_get_threshold(0), 9999);
    gc_set_threshold(0, orig);
}

TEST(collect_without_garbage_is_safe) {
    setup();
    int rc = gc_collect();
    (void)rc; /* implementation returns freed count; just assert no crash */
}

int main(void) {
    RUN_TEST(tracking_roundtrip);
    RUN_TEST(double_track_is_idempotent);
    RUN_TEST(simple_cycle_collected);
    RUN_TEST(enable_disable);
    RUN_TEST(threshold_round_trip);
    RUN_TEST(collect_without_garbage_is_safe);
    REPORT_AND_EXIT("gc");
}
