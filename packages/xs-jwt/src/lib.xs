-- xs-jwt: HS256 (and HS384/HS512 if the crypto module supports them).

import json
import crypto
import encode

fn _b64u(s) {
    -- base64url: standard b64 with -_ instead of +/, no padding
    let b64 = encode.base64_encode(s)
    return b64.replace("+", "-").replace("/", "_").trim_end("=")
}

fn _b64u_decode(s) {
    var p = s.replace("-", "+").replace("_", "/")
    while p.len() % 4 != 0 { p = p + "=" }
    return encode.base64_decode(p)
}

-- Signs a JSON-serialisable payload with the given secret. alg is HS256 by
-- default; HS384/HS512 require the crypto module to expose hmac_sha384/512.
fn sign(payload, secret, alg) {
    let a = alg ?? "HS256"
    let header = #{alg: a, typ: "JWT"}
    let h = _b64u(json.stringify(header))
    let p = _b64u(json.stringify(payload))
    let signing_input = h + "." + p
    let mac = match a {
        "HS256" -> crypto.hmac_sha256(secret, signing_input),
        "HS384" -> crypto.hmac_sha384(secret, signing_input),
        "HS512" -> crypto.hmac_sha512(secret, signing_input),
        _       -> throw "unsupported alg: {a}",
    }
    return signing_input + "." + _b64u(crypto.hex_decode(mac))
}

fn _verify_sig(signing_input, sig_b64, secret, alg) {
    let mac = match alg {
        "HS256" -> crypto.hmac_sha256(secret, signing_input),
        "HS384" -> crypto.hmac_sha384(secret, signing_input),
        "HS512" -> crypto.hmac_sha512(secret, signing_input),
        _       -> return false,
    }
    let want = _b64u(crypto.hex_decode(mac))
    return crypto.constant_time_eq(want, sig_b64)
}

-- Returns the decoded payload (as a Map) or throws on bad signature / shape.
fn verify(token, secret) {
    let parts = token.split(".")
    if parts.len() != 3 { throw "jwt: token must have 3 parts" }
    let header = json.parse(_b64u_decode(parts[0]))
    let alg = header.get("alg")
    if alg == null { throw "jwt: missing alg" }
    if not _verify_sig(parts[0] + "." + parts[1], parts[2], secret, alg) {
        throw "jwt: signature mismatch"
    }
    let payload = json.parse(_b64u_decode(parts[1]))
    return payload
}

-- decode without verifying (don't trust the result; useful for inspection)
fn decode_unsafe(token) {
    let parts = token.split(".")
    if parts.len() != 3 { return null }
    return #{
        header:  json.parse(_b64u_decode(parts[0])),
        payload: json.parse(_b64u_decode(parts[1])),
    }
}
