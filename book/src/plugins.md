# Plugins

XS lets a program rewrite the compiler's parser, hook semantic
analysis, and inject runtime behaviour, all from a `.xs` file you
load with `use plugin "name"`.

This is a sharp tool. The chapter covers what plugins can do, the
sandbox flags that contain them, and when not to reach for one.

## Three phases

A plugin has access to three points in the compilation pipeline:

1. **Lexer / parser extension**: add new tokens, override an existing
   production, define an entirely new statement.
2. **Sema hooks**: register custom passes that run after parsing,
   inspect the AST, emit diagnostics.
3. **Runtime hooks**: `before_eval`/`after_eval` callbacks that fire
   per node, runtime methods injected onto types, an
   `on_error` chain for diagnostic transformation.

## Example: a `pipeline` keyword

```xs
-- in plugins/pipeline.xs
plugin.parser.on_unknown(|kw, parser| {
    if kw == "pipeline" {
        let name = parser.ident()
        parser.expect("{")
        let body = parser.block()
        return plugin.ast.fn_decl(
            name,
            [], -- no params
            body,
        )
    }
    return null  -- not ours, fall through
})
```

```xs
-- in your code
use plugin "pipeline"

pipeline build {
    let src = read_sources()
    let bin = compile(src)
    publish(bin)
}

build()
```

## Sandboxing

By default, plugins run with **no restrictions**: they can override
keywords, replace functions, hook every eval. That's powerful and
appropriate for first-party tooling; it's wrong for randomly imported
third-party code.

Plugins declare their needs in their manifest:

```toml
# plugins/pipeline/xs.toml
[plugin]
name = "pipeline"
sandbox = ["INJECT_ONLY"]    # only inject; cannot override existing
```

Sandbox flags:

| flag             | effect |
|------------------|--------|
| `INJECT_ONLY`    | new keywords/types only; no overrides |
| `NO_OVERRIDE`    | cannot replace existing parser productions |
| `NO_EVAL_HOOK`   | cannot register before/after_eval |

The compiler refuses to load a plugin that requests more than its
manifest declares.

## When *not* to write a plugin

- For a library: write a normal module.
- For a DSL nested inside XS: try a plain function with a closure
  builder first.
- For a syntax preference (custom operators etc.): please don't.
  Five plugins all redefining `+` is worse than no plugins.

## When plugins earn their keep

- Domain-specific languages embedded in the host (build configs,
  query languages, schema definitions).
- Compile-time codegen: `derive(Show, Eq)` style traits.
- Static analysis passes specific to your codebase (e.g. "every
  service must call `tracing.span` at the entry point").
- Project-local lints (override `xs lint` rules just for this repo).

## Plugin API surface

```xs
plugin.lexer.add_keyword(name)
plugin.lexer.transform(fn)             -- token-stream rewriter

plugin.parser.on_unknown(fn)           -- new statement keywords
plugin.parser.on_unknown_expr(fn)      -- new expression keywords
plugin.parser.on_postfix(fn)
plugin.parser.override(name, fn)       -- replace built-in production

plugin.sema.add_pass(fn)               -- runs over each function's AST
plugin.sema.before_eval(fn, tag)       -- pre-eval hook
plugin.sema.after_eval(fn, tag)
plugin.sema.on_error(fn)               -- transform diagnostics

plugin.runtime.global.set(k, v)
plugin.runtime.add_method(type, name, fn)
plugin.runtime.resolve_import(fn)      -- intercept imports
plugin.runtime.teardown(fn)            -- run on interpreter shutdown
```

`plugin.ast.*` builds AST fragments; combined with parser overrides,
that's enough to introduce arbitrary syntax.

See `examples/provenance-plugin/` for a worked-through example.
