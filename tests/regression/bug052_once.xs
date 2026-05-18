-- skip-emit: js, wasm (TODO: @once trigger composition not lowered by --emit js / --emit wasm)
-- bug052: @once turns a repeating trigger into a one-shot. without
-- the modifier @every(50ms) would fire ~4 times in 200ms; with it,
-- the entry quiesces after the first fire and the counter stays
-- at 1.

var ticks = 0

@once @every(50ms) fn one_shot() {
    ticks = ticks + 1
}

@delayed(200ms) fn check() {
    if ticks != 1 { exit(2) }
    exit(0)
}

println("bug052: ok")
