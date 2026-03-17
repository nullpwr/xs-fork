# xs-diff

Line-level diff with unified-diff output. Backed by an LCS table; fine
for small/medium files, not optimised for gigabyte texts.

```xs
import diff

let ops = diff.lines("foo\nbar\nbaz", "foo\nqux\nbaz")
// [{op: "=", line: "foo"}, {op: "-", line: "bar"}, {op: "+", line: "qux"}, {op: "=", line: "baz"}]

let patch = diff.unified(old_text, new_text, #{a_path: "old.txt", b_path: "new.txt"})
println(patch)
```
