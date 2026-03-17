# xs-test

Tiny BDD-style test runner. `describe` / `it` / `before_each` /
`after_each` plus a handful of asserters. No global mutation, no
network calls, deps only on `time`.

```xs
import test

test.describe("string", || {
    test.it("uppers", || test.eq("hi".upper(), "HI"))
    test.it("len",    || test.eq("abc".len(), 3))
})

test.run()
```
