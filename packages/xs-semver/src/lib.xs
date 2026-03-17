-- xs-semver: SemVer 2.0.0 parsing, comparison, range matching.

fn parse(s) {
    if typeof(s) != "str" { return null }
    var v = if s.startswith("v") { s.slice(1, s.len()) } else { s }
    var pre = ""
    var build = ""
    let plus = v.find("+")
    if plus >= 0 { build = v.slice(plus + 1, v.len()); v = v.slice(0, plus) }
    let dash = v.find("-")
    if dash >= 0 { pre = v.slice(dash + 1, v.len()); v = v.slice(0, dash) }
    let parts = v.split(".")
    if parts.len() != 3 { return null }
    var nums = []
    for p in parts {
        let n = p.to_int()
        if n < 0 { return null }
        nums.push(n)
    }
    return #{
        major: nums[0], minor: nums[1], patch: nums[2],
        prerelease: pre, build: build,
    }
}

fn _cmp_ids(a, b) {
    -- prerelease identifier compare (numeric or string)
    let an = a.to_int()
    let bn = b.to_int()
    let a_is_num = (an > 0 or a == "0")
    let b_is_num = (bn > 0 or b == "0")
    if a_is_num and b_is_num {
        if an < bn { return -1 }
        if an > bn { return 1 }
        return 0
    }
    if a_is_num { return -1 }
    if b_is_num { return 1 }
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
}

fn compare(a_str, b_str) {
    let a = parse(a_str)
    let b = parse(b_str)
    if a == null or b == null { throw "bad semver: {a_str} or {b_str}" }
    if a.major != b.major { return if a.major < b.major { -1 } else { 1 } }
    if a.minor != b.minor { return if a.minor < b.minor { -1 } else { 1 } }
    if a.patch != b.patch { return if a.patch < b.patch { -1 } else { 1 } }
    -- a release version always beats a prerelease version of the same triple
    if a.prerelease == "" and b.prerelease != "" { return  1 }
    if a.prerelease != "" and b.prerelease == "" { return -1 }
    if a.prerelease == b.prerelease { return 0 }
    let ap = a.prerelease.split(".")
    let bp = b.prerelease.split(".")
    var i = 0
    while i < ap.len() and i < bp.len() {
        let c = _cmp_ids(ap[i], bp[i])
        if c != 0 { return c }
        i = i + 1
    }
    if ap.len() < bp.len() { return -1 }
    if ap.len() > bp.len() { return  1 }
    return 0
}

fn lt(a, b) { return compare(a, b) < 0 }
fn lte(a, b) { return compare(a, b) <= 0 }
fn eq(a, b) { return compare(a, b) == 0 }
fn gt(a, b) { return compare(a, b) > 0 }
fn gte(a, b) { return compare(a, b) >= 0 }

-- "^1.2.3"  -> >=1.2.3 <2.0.0
-- "~1.2.3"  -> >=1.2.3 <1.3.0
-- "1.2.3"   -> exact
-- ">=1.2.3" -> open lower bound
fn satisfies(version, range) {
    let v = parse(version)
    if v == null { return false }
    let r = range.trim()
    if r.startswith("^") {
        let base = parse(r.slice(1, r.len()))
        if base == null { return false }
        if v.major != base.major { return false }
        return compare(version, r.slice(1, r.len())) >= 0
    }
    if r.startswith("~") {
        let base = parse(r.slice(1, r.len()))
        if base == null { return false }
        if v.major != base.major or v.minor != base.minor { return false }
        return compare(version, r.slice(1, r.len())) >= 0
    }
    if r.startswith(">=") { return compare(version, r.slice(2, r.len()).trim()) >= 0 }
    if r.startswith("<=") { return compare(version, r.slice(2, r.len()).trim()) <= 0 }
    if r.startswith(">") { return compare(version, r.slice(1, r.len()).trim()) > 0 }
    if r.startswith("<") { return compare(version, r.slice(1, r.len()).trim()) < 0 }
    if r.startswith("=") { return compare(version, r.slice(1, r.len()).trim()) == 0 }
    return compare(version, r) == 0
}
