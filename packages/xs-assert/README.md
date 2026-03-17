# xs-assert

Deep-equal assertions with informative failure messages. Pairs well
with `xs-test`.

```xs
import assert
assert.eq([1, 2, 3], [1, 2, 3])
assert.includes("hello world", "world")
assert.throws_with(|| panic("boom"), "boom")
assert.near(0.1 + 0.2, 0.3)
```
