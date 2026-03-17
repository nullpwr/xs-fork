# xs-async-queue

Wraps a `channel(cap)` with a backpressure-aware API.

```xs
import async_queue
let q = async_queue.make(16)
spawn { while not q.is_empty() { println(q.recv()) } }
for i in 0..100 { q.send(i) }
q.close()
```
