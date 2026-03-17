-- xs-async-queue: bounded queue on top of channels.

fn make(capacity) {
    let cap = capacity ?? 64
    let ch = channel(cap)
    var closed = false

    return #{
        send: |v| {
            if closed { throw "queue closed" }
            ch.send(v)
        },
        recv: || {
            return ch.recv()
        },
        try_send: |v| {
            if closed or ch.is_full() { return false }
            ch.send(v)
            return true
        },
        try_recv: || {
            if ch.is_empty() { return null }
            return ch.recv()
        },
        len: || ch.len(),
        capacity: || cap,
        is_empty: || ch.is_empty(),
        is_full:  || ch.is_full(),
        close: || { closed = true },
    }
}
