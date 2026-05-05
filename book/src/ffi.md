# Foreign function interface

`import ffi` lets XS call into a shared library at runtime.

```xs
import ffi

let lib = ffi.load("libm.so")
let cos = lib.sym("cos", "double", ["double"])
println(cos(0.0))           -- 1.0
```

`ffi.load(path)` returns a handle to the loaded library;
`handle.sym(name, return_type, [arg_types])` produces a callable XS
function that marshals values across the boundary.

## Type strings

| string         | C type           |
|----------------|------------------|
| `"void"`       | `void`           |
| `"int"`        | `int`            |
| `"long"`       | `long long`      |
| `"float"`      | `float`          |
| `"double"`     | `double`         |
| `"str"`        | `const char *`   |
| `"ptr"`        | `void *`         |
| `"bool"`       | `int` (0 or 1)   |

Multiple values, structs, callbacks: not yet supported. Workaround
for structs: use a thin C wrapper that takes/returns by-value.

## Platform paths

```xs
import ffi
import os

let lib = ffi.load(
    if os.name == "Darwin"  { "libfoo.dylib" }
    elif os.name == "Windows" { "foo.dll" }
    else { "libfoo.so" }
)
```

## Lifecycle

```xs
import ffi

ffi.close(lib)              -- explicit unload
```

Otherwise the library stays loaded until the process exits.

## Why FFI exists

For the things XS deliberately doesn't ship:
- **GPU compute** (call into CUDA/OpenCL via a C wrapper)
- **Mature C libraries you don't want to port** (libsqlite3,
  libgit2, libleveldb)
- **Performance hot spots** that the JIT can't reach (write the
  inner loop in C, call it once per outer iteration)

For everything else, write XS. A lot of "we need C for performance"
turns out, after benchmarking, to be a 5% gain at the cost of a
~20% maintenance tax.

## Safety

FFI calls bypass XS's GC. Passing a string through `"str"` works as
long as the C side reads the string and is done with the pointer
before the next allocation might move memory. For long-lived
borrows, allocate a stable buffer:

```xs
let buf = buf.new(1024)
let p = buf.as_ptr()        -- raw pointer
let n = read_from_lib(p, 1024)
```

The `buf` value pins memory; the pointer remains valid until `buf`
is garbage-collected.
