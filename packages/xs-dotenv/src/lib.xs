-- xs-dotenv: parse .env-style files, expand ${VAR}, optionally load
-- into the process env.
--
--   import dotenv
--   let cfg = dotenv.parse_file(".env")
--   dotenv.load(".env")          // also writes into os.env

import fs
import os

fn _strip_quotes(s) {
    if s.len() < 2 { return s }
    let f = s.slice(0, 1)
    let l = s.slice(s.len() - 1, s.len())
    if (f == "\"" and l == "\"") or (f == "'" and l == "'") {
        return s.slice(1, s.len() - 1)
    }
    return s
}

fn _expand(value, scope) {
    var out = ""
    var i = 0
    let n = value.len()
    while i < n {
        let c = value.slice(i, i + 1)
        if c == "$" and i + 1 < n and value.slice(i + 1, i + 2) == "{" {
            let close = value.find("}", i + 2)
            if close > 0 {
                let name = value.slice(i + 2, close)
                let v = scope.get(name) ?? os.env(name) ?? ""
                out = out + v
                i = close + 1
                continue
            }
        }
        out = out + c
        i = i + 1
    }
    return out
}

fn parse(text) {
    var result = #{}
    let lines = text.split("\n")
    for raw in lines {
        var line = raw.trim()
        if line.len() == 0 or line.startswith("#") { continue }
        if line.startswith("export ") { line = line.slice(7, line.len()).trim() }
        let eq = line.find("=")
        if eq <= 0 { continue }
        let key = line.slice(0, eq).trim()
        let val = _strip_quotes(line.slice(eq + 1, line.len()).trim())
        result.set(key, _expand(val, result))
    }
    return result
}

fn parse_file(path) {
    let text = fs.read(path)
    if text == null { throw "dotenv: cannot read {path}" }
    return parse(text)
}

fn load(path) {
    let kv = parse_file(path)
    for k in kv.keys() {
        if os.env(k) == null { os.env(k, kv.get(k)) }
    }
    return kv
}
