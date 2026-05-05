-- bug042: http.get on a failed connect / TLS handshake / URL parse
-- silently returned null (or worse, an object with null fields when
-- the bytes-coming-back-after-half-failed-handshake parser produced
-- a partial response). Either way callers had no clean signal — they
-- couldn't distinguish "site returned 200 with empty body" from
-- "TLS handshake failed."
--
-- Fix: http_do_request now throws HttpError via xs_runtime_error on
-- connect failure, TLS failure, and bad URL. Try/catch recovers
-- cleanly; uncaught errors print the message and exit non-zero.

import http

-- bad URL throws
var caught = false
try {
    http.get("not a url")
} catch e {
    caught = true
    assert_eq(e.kind, "HttpError")
}
assert(caught, "bad URL should throw HttpError")

-- unreachable host throws
caught = false
try {
    http.get("http://no.such.host.example.invalid")
} catch e {
    caught = true
    assert_eq(e.kind, "HttpError")
}
assert(caught, "unreachable host should throw HttpError")

-- the error has a usable message field
caught = false
try {
    http.get("http://no.such.host.example.invalid")
} catch e {
    caught = true
    assert(e.message != null and e.message.len() > 0,
           "HttpError should carry a non-empty message")
}
assert(caught, "second probe should also throw")

println("bug042: ok")
