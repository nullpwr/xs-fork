-- hash throughput: sha256 a 100 kb buffer 50 times
import crypto

var buf = ""
for i in 0..1000 { buf = buf + "the quick brown fox jumps over the lazy dog 0123456789 " }
let total = 50

var last = ""
for _ in 0..total { last = crypto.sha256(buf) }

assert(last.len() == 64)
println("hash rounds =", total, "digest =", last)
