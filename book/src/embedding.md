# Embedding XS in C

`xs_embed.h` is the only header you need to embed the XS interpreter
in a C or C++ host application. Link with the xs object files (or
`libxs.a`) and `-lm`; everything else is bundled.

```c
#include "xs_embed.h"

int main(void) {
    XSContext *xs = xs_new();
    XSResult r = xs_eval(xs, "fn add(a, b) { a + b }; add(2, 3)");
    if (r.ok) {
        printf("%lld\n", xs_value_int(r.value));   // 5
    }
    xs_free(xs);
    return 0;
}
```

## API surface

```c
XSContext *xs_new(void);                     // create an interpreter
void       xs_free(XSContext *xs);

XSResult   xs_eval(XSContext *xs, const char *src);
XSResult   xs_call(XSContext *xs, const char *fn_name,
                   XSValue *args, int argc);
const char *xs_error(XSContext *xs);

void       xs_register_native(XSContext *xs, const char *name,
                              XSValue (*fn)(XSContext *, XSValue *, int));
```

Values cross the boundary as `XSValue` — a small tagged union the
host can build with `xs_int(...)`, `xs_str(...)`, `xs_array_new()`,
etc. The host owns the lifetime of XSContext but XS values are
GC-managed; rooting a value is `xs_root(xs, val)` / `xs_unroot`.

## Threading

`xs_new()` returns a context tied to one OS thread. To use XS from
multiple host threads, create one context per thread; they don't
share state. (For shared state across threads, use XS-side `actor`s
and pass message values across the C boundary.)

## Static link

```sh
ar rcs libxs.a $(find src -name '*.o')
gcc myapp.c -L. -lxs -lm -o myapp
```

The static library is around 2 MB. If you want to slim it down, the
`wasm-browser` make target shows which sources can be left out (TLS,
HTTP server, the doc generator, etc.) — the same trick works for
embedded native builds.

## Examples

`examples/embedding/` has a worked `hello.c`, plus the inverse —
calling C from XS via `ffi.load` — in `examples/ffi-demo/`.

## Sandboxing

A host that wants to run untrusted XS should:

1. Pass `xs_new_sandbox(&XS_SANDBOX_RO)` to disable filesystem,
   networking, and `os.exec`.
2. Install resource limits via `xs_set_limits(xs, &limits)` —
   max instructions, RSS bytes, wall seconds.
3. Watch for `XSResult.error` to surface as a host-level fault, not
   a panic.

Resource limits use the same machinery as the CLI's
`XS_LIMITS_INSTRUCTIONS=N` env var; details in
[performance](./performance.md).
