# The bytecode VM and JIT

XS ships three execution backends. This chapter covers what's under
the hood for the bytecode VM and the tier-2 JIT.

## Pipeline

```
.xs source
    │
    ▼
  lexer  ──tokens──▶  parser  ──AST──▶  semantic analysis
                                              │
                                              ▼
                                          tree-walking interpreter (default with --interp)
                                              │
                                              ▼
                                        bytecode compiler
                                              │
                                              ▼
                                          bytecode VM (default; --vm)
                                              │
                                              ▼
                                          tier-2 IR lowerer (--jit, hot protos only)
                                              │
                                              ▼
                                          register allocator + codegen
                                              │
                                              ▼
                                          native code in mmap'd RWX buffer
```

## Bytecode VM

96 opcodes covering arithmetic, control flow, closures, method
dispatch. Switch-based dispatch loop in `src/vm/vm.c`. Inline caches
on `OP_LOAD_GLOBAL`. Tail-call elision via `OP_TAIL_CALL` rewriting
the current frame in place.

Stack: dynamic array of tagged Values, grows on demand. Per-frame
locals are pre-allocated based on the proto's `nlocals`.

## Tier-2 JIT

`src/jit/` contains the lowering pipeline:

```
bytecode chunk ──▶ ralow_lower ──▶ IR
                                    │
                                    ▼
                          ralow_liveness  ──▶ liveness
                                    │
                                    ▼
                          ralow_alloc     ──▶ register alloc + spills
                                    │
                                    ▼
                          ralow_codegen   ──▶ native bytes (x86-64 or arm64)
                                    │
                                    ▼
                          proto.jit_entry = ptr_into_code_buffer
```

What's lowered:

- Arithmetic (int + float), comparisons, bitwise ops
- Control flow (jumps, calls, returns, tail calls)
- Loads / stores (locals, upvalues, globals)
- Closure construction
- Inline caches preserved

What isn't (yet):

- Effect machinery (`perform`, `handle`, `resume`) — falls back to
  the VM
- Some method dispatch paths
- Code-buffer overflow — proto stays bytecode; flagged `jit_tried`
  so we don't retry the whole pipeline next call

## Adaptive threshold

```c
int threshold_for(XSProto *proto) {
    int len = proto->chunk.len;
    if (len < 20)   return 25;
    if (len < 100)  return 50;
    if (len < 500)  return 100;
    return 200;
}
```

Tiny helpers reach native faster (compilation pays off quickly).
Big protos need more confidence before paying the codegen cost.

## Code buffer

4 MiB by default; tunable via `XS_JIT_CODE_SIZE_MB=N`. Allocated
once with `mmap(MAP_ANONYMOUS | PROT_RWX)` on POSIX, `VirtualAlloc`
on Windows, `mmap(MAP_JIT)` on Apple Silicon (with the
write-protect toggle dance).

When the buffer fills, subsequent `ralow_codegen` returns NULL and
the proto stays in bytecode. This is conservative — long-running
workloads with thousands of distinct closures may not all be JITted.
Future work: multi-region code buffer with eviction.

## Deoptimization

Not implemented. The lowerer is conservative — it rejects code
patterns whose runtime shapes it can't prove. If your code matches a
pattern the lowerer accepts, the JIT'd version is correct under all
inputs.

The cost of this conservatism: speculative inlining and shape-based
specialization are off the table. A proper deopt path is on the
roadmap (RFC pending).

## Forcing a backend

```sh
xs --interp file.xs       # tree-walk
xs --vm     file.xs       # bytecode VM, no JIT
xs --jit    file.xs       # JIT consideration enabled (the default)
```

For a single proto:

```xs
@no_jit                                 -- never JIT this function
fn cold_path() { ... }
```

`@no_jit` is rarely useful — usually the JIT was right not to compile
something cold. Reach for it when profiling shows JIT overhead
dominating a one-shot startup function.
