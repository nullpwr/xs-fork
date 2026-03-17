# xs-jwt

HS256 (and HS384/HS512) JWT sign and verify. RSA/ECDSA support
follows once `crypto` ships those primitives.

```xs
import jwt
let token = jwt.sign(#{sub: "alice", exp: 1735689600}, "secret")
let claims = jwt.verify(token, "secret")    // throws on bad sig
println(claims.get("sub"))
```
