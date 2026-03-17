-- xs-yaml: parse a useful subset of YAML.
-- Supports: nested maps, sequences, scalars (string/int/float/bool/null),
-- # line comments, block-style indentation. Does not support anchors, tags,
-- multi-document streams, or flow-style nested maps.

fn _scalar(s) {
    let t = s.trim()
    if t == "" or t == "~" or t == "null" { return null }
    if t == "true"  or t == "True"  or t == "TRUE"  { return true }
    if t == "false" or t == "False" or t == "FALSE" { return false }
    let i = t.to_int()
    if str(i) == t { return i }
    let f = t.to_float()
    if str(f) == t or t.contains(".") { return f }
    if t.startswith("\"") and t.endswith("\"") {
        return t.slice(1, t.len() - 1)
    }
    if t.startswith("'") and t.endswith("'") {
        return t.slice(1, t.len() - 1)
    }
    return t
}

fn _indent(line) {
    var i = 0
    while i < line.len() and line.slice(i, i + 1) == " " { i = i + 1 }
    return i
}

fn parse(text) {
    let raw_lines = text.split("\n")
    var lines = []
    for ln in raw_lines {
        let trimmed = ln.trim()
        if trimmed == "" or trimmed.startswith("#") { continue }
        let pound = ln.find(" #")
        let body = if pound > 0 { ln.slice(0, pound) } else { ln }
        lines.push(body)
    }
    return _parse_block(lines, 0, 0).value
}

fn _parse_block(lines, idx, base_indent) {
    -- Returns #{value, next_idx}. If first line is "- ...", parse a list;
    -- if first line is "key:", parse a map; otherwise a scalar.
    if idx >= lines.len() { return #{value: null, next_idx: idx} }
    let first = lines[idx]
    let ind = _indent(first)
    if ind < base_indent { return #{value: null, next_idx: idx} }
    let body = first.slice(ind, first.len())
    if body.startswith("- ") or body == "-" {
        return _parse_list(lines, idx, ind)
    }
    return _parse_map(lines, idx, ind)
}

fn _parse_list(lines, idx, list_indent) {
    var arr = []
    var i = idx
    while i < lines.len() {
        let line = lines[i]
        let ind = _indent(line)
        if ind < list_indent { break }
        if ind > list_indent { i = i + 1; continue }
        let body = line.slice(ind, line.len())
        if not (body.startswith("- ") or body == "-") { break }
        let after_dash = if body == "-" { "" } else { body.slice(2, body.len()) }
        if after_dash.contains(":") and not after_dash.startswith("\"") {
            -- inline map element starting with `- key: value`
            let synthetic = []
            synthetic.push(" ".repeat(list_indent + 2) + after_dash)
            var j = i + 1
            while j < lines.len() and _indent(lines[j]) > list_indent {
                synthetic.push(lines[j])
                j = j + 1
            }
            let inner = _parse_map(synthetic, 0, list_indent + 2)
            arr.push(inner.value)
            i = j
        } else if after_dash == "" {
            -- nested block under the dash
            let inner = _parse_block(lines, i + 1, list_indent + 2)
            arr.push(inner.value)
            i = inner.next_idx
        } else {
            arr.push(_scalar(after_dash))
            i = i + 1
        }
    }
    return #{value: arr, next_idx: i}
}

fn _parse_map(lines, idx, map_indent) {
    var m = #{}
    var i = idx
    while i < lines.len() {
        let line = lines[i]
        let ind = _indent(line)
        if ind < map_indent { break }
        if ind > map_indent { i = i + 1; continue }
        let body = line.slice(ind, line.len())
        let colon = body.find(":")
        if colon < 0 { break }
        let key = body.slice(0, colon).trim()
        let val_text = body.slice(colon + 1, body.len()).trim()
        if val_text == "" {
            let inner = _parse_block(lines, i + 1, map_indent + 2)
            m.set(key, inner.value)
            i = inner.next_idx
        } else {
            m.set(key, _scalar(val_text))
            i = i + 1
        }
    }
    return #{value: m, next_idx: i}
}
