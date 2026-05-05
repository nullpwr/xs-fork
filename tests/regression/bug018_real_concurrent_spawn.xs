-- bug018: spawn used to run synchronously on the calling thread
-- ("Execute immediately (no real threads yet)"). Channels also did
-- not block. Fix: pthread-backed spawn with a recursive GIL, plus a
-- channel implementation backed by mutex+condvar. recv blocks until
-- a sender wakes it; sleep releases the GIL so peer threads run.

import time

let ch = channel()
let producer = spawn {
    var i = 0
    while i < 4 {
        ch.send(i)
        i = i + 1
    }
    ch.send("done")
}

var got = []
while true {
    let v = ch.recv()
    if v == "done" { break }
    got.push(v)
}
await producer
assert_eq(got, [0, 1, 2, 3])

-- spawn returns a future; await blocks until completion.
let t = spawn { 7 * 6 }
let r = await t
assert_eq(r, 42)

-- recv blocks until a value is available, even when the sender runs
-- in another thread that sleeps first.
let ch2 = channel()
let slow = spawn {
    time.sleep(0.05)
    ch2.send("late")
}
assert_eq(ch2.recv(), "late")
await slow

println("bug018: ok")
