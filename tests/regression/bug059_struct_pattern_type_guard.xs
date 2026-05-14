-- bug059: VM struct pattern matching wasn't checking the struct's
-- type name, so `Circle { radius }` would silently fire on any value
-- with a loadable `radius` field (or where the field returned null).
-- Output was "circle r=null" for a Rect input. Fixed by emitting an
-- OP_IS guard against pat_struct.path before the field iteration.

struct Circle { radius }
struct Rect { w, h }

fn describe(shape) {
  match shape {
    Circle { radius } => "circle r={radius}"
    Rect { w, h }     => "rect {w}x{h}"
    _                 => "?"
  }
}

assert_eq(describe(Circle { radius: 5 }), "circle r=5")
assert_eq(describe(Rect { w: 3, h: 4 }), "rect 3x4")

-- and the other direction: a Circle should not match a Rect arm
fn is_rect(s) {
  match s {
    Rect { w, h } => true
    _             => false
  }
}

assert_eq(is_rect(Rect { w: 1, h: 2 }), true)
assert_eq(is_rect(Circle { radius: 1 }), false)
