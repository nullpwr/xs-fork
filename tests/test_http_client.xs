-- test http client surface (the server code in src/net/http_server.c has
-- routing/middleware/static-file support but no XS bindings yet, so this
-- only checks the client methods are reachable)

import http

assert(http != null, "http module should exist")

assert(http.get != null, "http.get should exist")
assert(http.post != null, "http.post should exist")
assert(http.put != null, "http.put should exist")
assert(http.delete != null, "http.delete should exist")
assert(http.patch != null, "http.patch should exist")
assert(http.request != null, "http.request should exist")

print("test_http_client: all passed")
