-- Library file for c17_use_imports.xs. The trailing `export { ... }`
-- list declares the public surface; everything else stays file-local.

fn shout(s) { return "HI " + s }
fn whisper(s) { return "psst " + s }

let public_const = 99
let private_const = 7

const TAU = 6.2831
const SECRET = 42

struct Point { x: int, y: int }

fn rgb_to_hex(r, g, b) { return r + g + b }

export { shout, public_const, TAU, Point, rgb_to_hex as rgbToHex }
