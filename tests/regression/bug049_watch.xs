-- bug049: @watch fires when the watched path changes. Linux uses
-- inotify, macOS / other unixes fall back to a stat poll, Windows
-- uses ReadDirectoryChangesW (TODO). The test creates a temp file,
-- modifies it from inside @delayed, and watches for the resulting
-- callback. parity covers interp / vm / jit.

import fs

fs.write("/tmp/xs_bug049.txt", "before")
var fired = false

@watch("/tmp/xs_bug049.txt") fn changed() {
    fired = true
    exit(0)
}

@delayed(100ms) fn modify() {
    fs.write("/tmp/xs_bug049.txt", "after")
}

@delayed(2s) fn give_up() {
    if !fired { exit(2) }
    exit(0)
}

println("bug049: ok")
