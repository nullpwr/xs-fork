# xs-markdown

CommonMark-ish renderer. Covers what 95% of READMEs use: headers,
paragraphs, code fences, inline code, emphasis/strong, links, images,
lists, blockquotes, hr.

```xs
import markdown
let html = markdown.render("
# hello
*world* of `code` and [links](https://xslang.org).
- one
- two
")
```

Not yet: tables, footnotes, definition lists, nested lists.
