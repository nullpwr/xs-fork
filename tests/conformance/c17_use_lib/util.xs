-- Library file for c17_use_imports.xs. Top-level statements declare
-- public and private bindings; the importer verifies which side of
-- the wall each one ends up on.

pub fn shout(s) { return "HI " + s }
fn whisper(s) { return "psst " + s }

pub let public_const = 99
let private_const = 7

pub const TAU = 6.2831
const SECRET = 42

pub struct Point { x: int, y: int }

@export("aliased")
fn local_only_name() { return "via alias" }
