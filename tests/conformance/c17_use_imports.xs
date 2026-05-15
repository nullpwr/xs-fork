-- Cross-file `use` semantics: pub-marked top-level decls are visible
-- through the imported namespace, unmarked ones are not.

use "./c17_use_lib/util.xs"

assert_eq(util.shout("world"), "HI world")
assert_eq(util.public_const, 99)
assert_eq(util.TAU > 6.0 and util.TAU < 7.0, true)

-- Imported struct constructor builds an instance through the namespace.
let P = util.Point
let p = P { x: 3, y: 4 }
assert_eq(p.x, 3)
assert_eq(p.y, 4)

-- @export("alias") exposes both the local name AND the alias
assert_eq(util.aliased(), "via alias")
assert_eq(util.local_only_name(), "via alias")

-- Private bindings are not visible
assert_eq(util.whisper, null)
assert_eq(util.private_const, null)
assert_eq(util.SECRET, null)

-- `as` rebinds the namespace
use "./c17_use_lib/util.xs" as u
assert_eq(u.shout("again"), "HI again")

-- Selective `{ name }` binding pulls just the listed pub names
use "./c17_use_lib/util.xs" { shout, public_const }
assert_eq(shout("local"), "HI local")
assert_eq(public_const, 99)

-- Renamed selective: `{ shout as bark }` (parser accepts; runtime binds)
use "./c17_use_lib/util.xs" { shout as bark }
assert_eq(bark("renamed"), "HI renamed")

println("CONFORMANCE OK")
