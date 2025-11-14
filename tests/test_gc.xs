-- garbage collector module tests
import gc

-- gc functions exist and can be called
gc.disable()
gc.enable()
gc.collect()

-- gc.stats returns detailed info
let stats = gc.stats()
assert_eq(stats.strategy, "generational-refcount")
assert(stats.total_allocations >= 0)
assert(stats.gen0_collections >= 0)
assert(stats.gen1_collections >= 0)
assert(stats.gen2_collections >= 0)

-- collect returns number of freed objects
let freed = gc.collect()
assert(freed >= 0)

-- allocation still works after gc cycle
let arr = []
for i in range(100) {
    arr.push(i)
}
assert_eq(arr.len(), 100)
gc.collect()
assert_eq(arr.len(), 100)

-- disable/enable cycle
gc.disable()
let x = "hello"
gc.enable()
gc.collect()
assert_eq(x, "hello")

-- stats have more detail after operations
let s2 = gc.stats()
assert(s2.total_allocations >= 0)
assert(s2.tracked >= 0)

-- threshold tuning
gc.set_threshold(0, 500)
gc.set_threshold(1, 5)
gc.set_threshold(2, 5)

-- force a few collections to exercise generational promotion
for i in range(10) {
    gc.collect()
}
let s3 = gc.stats()
assert(s3.gen0_collections > 0)

-- cycles still collected: create a map cycle
let a = #{}
let b = #{}
a.set("ref", b)
b.set("ref", a)
gc.collect()

-- freeze prevents collection
let frozen_arr = [1, 2, 3]
gc.freeze(frozen_arr)
gc.collect()
assert_eq(frozen_arr.len(), 3)

print("test_gc: all passed")
