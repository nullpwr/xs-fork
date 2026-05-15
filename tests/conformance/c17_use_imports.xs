-- Cross-file `use` semantics: the importer sees only what the trailing
-- `export { ... }` list names. Everything else stays private.

use "./c17_use_lib/util.xs"

assert_eq(util.shout("world"), "HI world")
assert_eq(util.public_const, 99)
assert_eq(util.TAU > 6.0 and util.TAU < 7.0, true)

-- Imported struct constructor builds an instance through the namespace.
let P = util.Point
let p = P { x: 3, y: 4 }
assert_eq(p.x, 3)
assert_eq(p.y, 4)

-- `export { local as public }` rebinds the public name.
assert_eq(util.rgbToHex(1, 2, 3), 6)
-- Local name is no longer visible under the original spelling.
assert_eq(util.rgb_to_hex, null)

-- Anything not listed in the export block is private.
assert_eq(util.whisper, null)
assert_eq(util.private_const, null)
assert_eq(util.SECRET, null)

-- `as` rebinds the namespace at the use site.
use "./c17_use_lib/util.xs" as u
assert_eq(u.shout("again"), "HI again")

-- Selective `{ name }` binding pulls the listed names into local scope.
use "./c17_use_lib/util.xs" { shout, public_const }
assert_eq(shout("local"), "HI local")
assert_eq(public_const, 99)

-- Renamed selective pull: `{ shout as bark }`.
use "./c17_use_lib/util.xs" { shout as bark }
assert_eq(bark("renamed"), "HI renamed")

println("CONFORMANCE OK")
