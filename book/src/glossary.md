# Glossary

**Actor.** A thread-isolated object whose only inbound surface is its
methods, serialised through a per-actor message queue.

**Adapt function.** A function tagged with multiple target backends
that XS picks at compile time. Used to provide platform-specific
implementations.

**Algebraic effect.** A named operation that a callee performs and
the caller's `handle` block fulfils. Like exceptions, but the
handler can resume the callee with a value.

**Bytecode VM.** The default execution backend. Compiles XS source
to a register-machine bytecode, runs it through a switch-dispatch
loop. ~4-9× faster than the tree-walking interpreter.

**Channel.** A typed, optionally-buffered queue used to move values
between threads or coroutines.

**Effect handler.** The dynamic counterpart to a try/catch: runs
when a `perform` happens inside the handler's scope.

**FFI.** Foreign Function Interface. Use `ffi.load("libfoo.so")` to
call into a shared library; values cross the boundary as raw bytes
or pointers.

**Generator.** A function declared `fn*` that yields values via
`yield`. Iterating a generator suspends it between yields.

**Gradual typing.** Type annotations are optional but enforced when
present. `xs --check` checks the file. `xs --strict` requires
annotations on every public surface.

**JIT.** Just-In-Time native code generation. The tier-2 JIT lowers
hot bytecode to x86-64 or arm64 machine code, called when a proto's
call count crosses an adaptive threshold.

**Nursery.** A structured-concurrency block that owns its child
tasks. Returns when all children finish; cancels children on
exception.

**Plugin.** Compile-time + runtime code that hooks the parser,
semantic analyser, or evaluator. See [plugins](./plugins.md).

**Proto.** A compiled function prototype: bytecode chunk, locals,
upvalue descriptors, embedded constants.

**Resume.** Inside an effect handler, calls back into the
`perform`-site with a return value. The handler chooses whether to
resume zero, one, or many times.

**Sema.** Semantic analysis. Runs after parsing, before evaluation.
Resolves names, checks types, runs registered passes.

**Span (tracing).** A timed unit of work with optional attributes.
Spans nest; child spans automatically inherit a `parent_id`.

**Trait.** A set of methods a type promises to implement. Like Rust
traits or Java interfaces, with structural dispatch.

**Tree-walking interpreter.** The original execution backend.
Reserved for debugging and plugins that rely on AST-level runtime
hooks. Slower than the VM; pass `--interp` to force.

**Universal literal.** A typed literal recognised by the lexer:
`500ms`, `#ff6600`, `2025-01-20`, `10MB`, `45deg`. Each compiles to
a typed runtime value, not a string.

**Upvalue.** A variable captured by a closure from an enclosing
scope. Lives in the heap so the closure can outlive the scope.

**WASI.** WebAssembly System Interface. The target XS uses for its
WebAssembly build, providing filesystem, clock, random, and stdin/
stdout to wasm.
