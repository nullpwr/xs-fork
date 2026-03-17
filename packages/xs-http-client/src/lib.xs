-- xs-http-client: ergonomic wrapper over net.http with retry, JSON
-- helpers, and a fluent builder.

import net
import json
import time

fn make(opts) {
    let o = opts ?? #{}
    let base = o.get("base_url") ?? ""
    let default_headers = o.get("headers") ?? #{}
    let timeout_ms = o.get("timeout_ms") ?? 30000
    let retries = o.get("retries") ?? 0
    let retry_backoff = o.get("retry_backoff") ?? 200
    let retry_on_status = o.get("retry_on_status") ?? [502, 503, 504]

    fn _full(url) {
        if url.startswith("http") { return url }
        return base + url
    }

    fn _do_request(method, url, body, headers) {
        let merged = #{}
        for k in default_headers.keys() { merged.set(k, default_headers.get(k)) }
        if headers != null { for k in headers.keys() { merged.set(k, headers.get(k)) } }
        var attempt = 0
        var last_resp = null
        while attempt <= retries {
            let resp = net.http(method, _full(url), merged, body)
            last_resp = resp
            if resp == null { return null }
            let status = resp.get("status") ?? 0
            if status >= 200 and status < 500 and not retry_on_status.contains(status) {
                return resp
            }
            if attempt < retries {
                time.sleep_ms(retry_backoff * (2 ** attempt))
            }
            attempt = attempt + 1
        }
        return last_resp
    }

    return #{
        get:    |url, headers| _do_request("GET", url, null, headers),
        post:   |url, body, headers| _do_request("POST", url, body, headers),
        put:    |url, body, headers| _do_request("PUT", url, body, headers),
        patch:  |url, body, headers| _do_request("PATCH", url, body, headers),
        delete: |url, headers| _do_request("DELETE", url, null, headers),

        get_json: |url, headers| {
            let r = _do_request("GET", url, null, headers)
            if r == null or (r.get("status") ?? 0) >= 400 { return null }
            return json.parse(r.get("body") ?? "")
        },
        post_json: |url, payload, headers| {
            var h = headers ?? #{}
            h.set("Content-Type", "application/json")
            let r = _do_request("POST", url, json.stringify(payload), h)
            if r == null or (r.get("status") ?? 0) >= 400 { return null }
            return json.parse(r.get("body") ?? "")
        },
    }
}
