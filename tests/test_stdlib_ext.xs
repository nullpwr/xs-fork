-- extended stdlib tests: crypto, hash, uuid, base64, encode, time, process
import crypto

-- crypto.sha256 returns 64-char hex
let sha = crypto.sha256("hello")
assert_eq(sha.len(), 64)

-- crypto.md5 returns 32-char hex
let md = crypto.md5("hello")
assert_eq(md.len(), 32)

-- crypto.sha1 returns 40-char hex
let s1 = crypto.sha1("hello")
assert_eq(s1.len(), 40)
assert_eq(crypto.sha1("hello"), crypto.sha1("hello"))

-- deterministic hashes
assert_eq(crypto.sha256("test"), crypto.sha256("test"))
assert_eq(crypto.md5("test"), crypto.md5("test"))

-- random bytes (returns hex string, so 16 bytes = 32 hex chars)
let rb = crypto.random_bytes(16)
assert_eq(rb.len(), 32)

-- uuid
let id = crypto.uuid4()
assert_eq(id.len(), 36)
assert(id.contains("-"))

-- base64
let b64 = crypto.base64_encode("hello world")
assert(b64.len() > 0)
let decoded = crypto.base64_decode(b64)
assert_eq(decoded, "hello world")

-- hex encode/decode
let hex = crypto.hex_encode("AB")
assert_eq(hex, "4142")
let unhex = crypto.hex_decode("4142")
assert_eq(unhex, "AB")

-- hmac_sha256
let hmac = crypto.hmac_sha256("secret", "message")
assert_eq(hmac.len(), 64)
assert_eq(crypto.hmac_sha256("secret", "message"), crypto.hmac_sha256("secret", "message"))

-- pbkdf2
let dk = crypto.pbkdf2("password", "salt", 1000, 32)
assert_eq(dk.len(), 64)
assert_eq(crypto.pbkdf2("password", "salt", 1000, 32), dk)

-- hkdf
let hk = crypto.hkdf("input key", "salt", "info", 32)
assert_eq(hk.len(), 64)
assert_eq(crypto.hkdf("input key", "salt", "info", 32), hk)

-- aes encrypt/decrypt roundtrip (GCM)
let key_hex = crypto.hex_encode("0123456789abcdef")
let ct = crypto.aes_encrypt(key_hex, "secret data", "gcm")
assert(ct.len() > 0)
let pt = crypto.aes_decrypt(key_hex, ct, "gcm")
assert_eq(pt, "secret data")

-- aes CBC roundtrip
let ct_cbc = crypto.aes_encrypt(key_hex, "cbc test msg!", "cbc")
assert(ct_cbc.len() > 0)
let pt_cbc = crypto.aes_decrypt(key_hex, ct_cbc, "cbc")
assert_eq(pt_cbc, "cbc test msg!")

-- constant_time_eq
assert_eq(crypto.constant_time_eq("abc", "abc"), true)
assert_eq(crypto.constant_time_eq("abc", "abd"), false)
assert_eq(crypto.constant_time_eq("abc", "ab"), false)

-- hash module
import hash
assert_eq(hash.md5("hello").len(), 32)
assert_eq(hash.sha256("hello").len(), 64)

-- base64 module
import base64
let enc = base64.encode("hello")
assert_eq(base64.decode(enc), "hello")

-- uuid module
import uuid
let uid = uuid.v4()
assert_eq(uid.len(), 36)

-- time module extras
import time
let now = time.now_ms()
assert(now > 0)

let ns = time.now_ns()
assert(ns > 0)

let d = time.date(now / 1000)
assert(d.year >= 2026)
assert(d.month >= 1)
assert(d.month <= 12)

let iso = time.to_iso(now / 1000)
assert(iso.contains("20"))
assert(iso.contains("T"))

let mono1 = time.monotonic()
let mono2 = time.monotonic()
assert(mono2 >= mono1)

-- process module extras
import process
let pid = process.pid()
assert(pid > 0)

let cwd = process.cwd()
assert(cwd.len() > 0)

let rc = process.exec("echo .")
assert_eq(rc, 0)

-- process.spawn with pipes
let proc = process.spawn("echo", ["hello from spawn"])
let out = proc.stdout_read(proc)
assert(out.contains("hello from spawn"))
let code = proc.wait(proc)
assert_eq(code, 0)

-- process.spawn shell mode
let proc2 = process.spawn("echo pipe_test")
let out2 = proc2.stdout_read(proc2)
assert(out2.contains("pipe_test"))
proc2.wait(proc2)

-- process.env
let old_path = process.env("PATH")
assert(old_path != null)

print("test_stdlib_ext: all passed")
