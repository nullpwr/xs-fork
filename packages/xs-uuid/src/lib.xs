-- xs-uuid: UUID v4 (random) and v7 (time-ordered) generation.

import random
import time

fn _hex(n, w) {
    let chars = "0123456789abcdef"
    var s = ""
    var v = n
    var i = 0
    while i < w {
        s = chars.slice(v % 16, v % 16 + 1) + s
        v = v / 16
        i = i + 1
    }
    return s
}

fn _rand_u32() {
    return random.int(0, 2147483647) * 2 + random.int(0, 1)
}

-- v4: 122 random bits + version 4 + variant RFC 4122
fn v4() {
    let a = _rand_u32()
    let b = _rand_u32() & 0xffff_ffff
    let c = (_rand_u32() & 0x0fff_ffff) | 0x4000_0000  -- set version 4 nibble
    let d = (_rand_u32() & 0x3fff_ffff) | 0x8000_0000  -- set variant nibble
    let e_hi = _rand_u32() & 0xffff_ffff
    let e_lo = _rand_u32() & 0xffff_ffff
    return _hex(a, 8) + "-"
         + _hex(b >> 16, 4) + "-"
         + _hex((c >> 16) & 0xffff, 4) + "-"
         + _hex((d >> 16) & 0xffff, 4) + "-"
         + _hex(e_hi & 0xffff, 4) + _hex(e_lo, 8)
}

-- v7: 48-bit unix-ms timestamp prefix + version 7 + 74 random bits
fn v7() {
    let ms = time.now_ms()
    let ts_hi = (ms / 65536) % 4294967296
    let ts_lo = ms % 65536
    let r1 = (_rand_u32() & 0x0fff) | 0x7000  -- version 7 nibble
    let r2 = (_rand_u32() & 0x3fff) | 0x8000  -- RFC variant
    let r3 = _rand_u32() & 0xffffffff
    let r4 = _rand_u32() & 0xffffffff
    return _hex(ts_hi, 8) + "-"
         + _hex(ts_lo, 4) + "-"
         + _hex(r1, 4) + "-"
         + _hex(r2, 4) + "-"
         + _hex(r3, 8) + _hex(r4 & 0xffff, 4)
}

fn is_valid(s) {
    if typeof(s) != "str" or s.len() != 36 { return false }
    var i = 0
    while i < 36 {
        let c = s.slice(i, i + 1)
        if i == 8 or i == 13 or i == 18 or i == 23 {
            if c != "-" { return false }
        } else {
            let lo = c.lower()
            if not "0123456789abcdef".contains(lo) { return false }
        }
        i = i + 1
    }
    return true
}

fn version(s) {
    if not is_valid(s) { return null }
    let c = s.slice(14, 15)
    return c.to_int()
}
