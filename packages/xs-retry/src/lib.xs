-- xs-retry: exponential backoff with jitter.
--
--   import retry
--   let result = retry.run(|| http.get("https://api.example.com"),
--       #{tries: 5, base_ms: 100, max_ms: 5000, jitter: true})

import time
import random

fn _delay_ms(attempt, base, max, jitter) {
    var d = base * (2 ** attempt)
    if d > max { d = max }
    if jitter {
        d = d / 2 + random.int(0, d / 2)
    }
    return d
}

fn run(fn, opts) {
    let o = opts ?? #{}
    let tries = o.get("tries") ?? 3
    let base  = o.get("base_ms") ?? 100
    let max   = o.get("max_ms") ?? 30000
    let jitter = o.get("jitter") ?? true
    let should_retry = o.get("should_retry") ?? (|err| true)
    let on_retry = o.get("on_retry") ?? null

    var last_err = null
    var attempt = 0
    while attempt < tries {
        try {
            return fn()
        } catch e {
            last_err = e
            if not should_retry(e) { throw e }
            if attempt + 1 >= tries { break }
            let d = _delay_ms(attempt, base, max, jitter)
            if on_retry != null { on_retry(attempt + 1, d, e) }
            time.sleep_ms(d)
        }
        attempt = attempt + 1
    }
    throw last_err
}
