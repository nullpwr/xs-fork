-- bug049: @watch fires when the watched path changes. Linux uses
-- inotify, macOS / other unixes fall back to a stat poll. parity
-- covers interp / vm / jit. Path is cwd-relative so the test runs
-- the same on Linux / macOS / Windows MinGW.

import fs

fs.write("xs_bug049_watch.txt", "before")
var fired = false

@watch("xs_bug049_watch.txt") fn changed() {
    fired = true
    exit(0)
}

@delayed(200ms) fn modify() {
    fs.write("xs_bug049_watch.txt", "after-write")
}

@delayed(3s) fn give_up() {
    if !fired { exit(2) }
    exit(0)
}

println("bug049: ok")
