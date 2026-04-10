---
rfc: 0003
title: Higher-rank polymorphism
author: xs-lang0
status: draft
created: 2026-04-22
---

# 0003: Higher-rank polymorphism

## Summary

Allow `for<T>` quantifiers inside function types so callers can pass
generic functions as values. Today XS infers and constrains type
parameters at call sites; a generic function passed as an argument
loses its polymorphism. This RFC adds the explicit higher-rank form.

## Motivation

The book and migration chapters call this out:

> No higher-rank polymorphism today (`for<T>`-style). RFC open.

The use case shows up the moment you try to write a generic
combinator that takes another generic function:

```xs
fn each_collection(f) {
    f([1, 2, 3])
    f(["a", "b"])
    f([1.0, 2.5])
}

each_collection(|xs| xs.len())
```

This works dynamically. With `--check`, it does not: `f` is inferred
to take `[int]` from the first call, then fails on the second.

The workaround today is to forgo the type check for `f`, or to
duplicate `each_collection` for each shape. Neither scales.

## Guide-level explanation

A function parameter can carry its own quantifier:

```xs
fn each_collection(f: for<T> fn([T]) -> int) -> int {
    f([1, 2, 3]) + f(["a", "b"]) + f([1.0, 2.5])
}

each_collection(|xs| xs.len())
```

Reads: "`f` is a function that, for any choice of `T`, takes `[T]`
and returns `int`." The caller passes a closure that is itself
polymorphic; the type checker confirms it can specialise to each
shape used inside the body.

The same syntax works on multiple type parameters:

```xs
fn fold_pair(f: for<A, B> fn(A, B) -> A, init, items) { ... }
```

And on trait bounds:

```xs
fn collect(g: for<T> fn() -> T where T: Default) -> [(int, T)] { ... }
```

## Reference-level explanation

### Grammar

```
FnType    := "fn" "(" ArgTypes ")" "->" Type
HrtFnType := "for" "<" TypeParams ">" FnType WhereClause?
```

`HrtFnType` may appear anywhere a `FnType` is currently legal:
parameter types, return types, type aliases, trait method signatures.

### Type checking

A higher-rank type is checked by *generalisation*: when a closure is
matched against a `for<T> fn(T) -> T` annotation, the checker:

1. Introduces fresh skolem constants for each quantified variable.
2. Substitutes them into the function type.
3. Checks the closure body under the substitution.
4. Verifies no skolem escapes through the result type.

Skolem escape (a skolem appearing in the body's outer environment)
is the canonical higher-rank error:

```xs
let mut leak: int? = null

let ok: for<T> fn(T) -> T = |x| { leak = x; x }
                                  -- error: T escapes via `leak`
```

Diagnostic code: `T0042`, "skolem `T` escapes its scope".

### Inference

XS already infers monomorphic type parameters at call sites. This RFC
does *not* infer higher-rank types: the `for<T>` annotation must be
written. Inferring HRT is undecidable in the general case (System F);
we keep inference at rank-1 and require annotation for anything
higher.

A common ergonomic improvement: when a function-typed parameter has
no annotation, the checker can suggest `for<T>` if the body uses the
parameter at multiple incompatible types. The suggestion appears as a
`did you mean` hint on the existing T0008 error.

### Variance

Higher-rank types interact with variance in the standard way:

```xs
for<T> fn(T) -> T          -- invariant in `T`
for<T> fn(T) -> int        -- contravariant in `T`
for<T> fn() -> T            -- covariant in `T`
```

The RFC defers explicit `+T` / `-T` / `*T` markers to a follow-up RFC
("Variance annotations on type parameters"). Until then, all
user-defined generic types are invariant; built-in types
(`fn`, `[T]`, `T?`) keep their existing variance.

### Trait objects

`trait MyTrait { ... }` continues to type-erase via vtable; passing a
trait-object value as `for<T> fn(T) -> int` is fine, the polymorphism
is on the *function*, not on the implementation. No change here.

### Implementation

The semantic-analysis pass (`src/sema/typeck.c`) already has a
notion of *quantified types*; today they are introduced only at the
top of a function declaration. The RFC extends `Type` with a
`TY_FORALL` constructor that nests:

```c
typedef struct Type {
    TypeKind kind;
    union {
        ...
        struct { TypeParam *params; int n; struct Type *inner; } forall;
    };
} Type;
```

Generalisation introduces a `TY_FORALL`; instantiation (at a call
site) replaces the bound variables with fresh metavariables; skolem
introduction (at HRT-annotated sites) replaces them with skolem
constants. Standard System F, no surprises.

The bytecode and JIT do not change: types are erased before lowering.

## Drawbacks

- Annotation burden: `for<T>` is verbose. Most code does not need it,
  but the libraries that benefit will lean on it heavily.
- HRT diagnostics are notoriously hard to read. The "skolem escapes"
  message is not friendly. We will write a dedicated explanation in
  the error catalogue (`xs --explain T0042`).
- One more thing to teach. The book chapter on gradual typing grows.

## Rationale and alternatives

**Status quo (rank-1 only).** Forces duplication of generic
combinators. Painful for library authors.

**Implicit HRT inference.** Undecidable in general. Limited variants
exist (e.g. GHC's `RankNTypes` requires annotation, `HigherRankTypes`
infers in some cases) but the heuristics are subtle and error
messages get worse. Not worth the complexity.

**Trait-objects as the only polymorphism story.** Forces dynamic
dispatch. Costs perf; obscures the static guarantee that a parameter
really is generic.

## Prior art

- Haskell: `RankNTypes`, the canonical reference. Annotation-only.
- Rust: `for<'a>` for lifetimes; the type-level generalisation is
  internal. Partially what we want.
- OCaml: explicit polymorphism via `'a.` quantifiers in type
  annotations.
- Scala 3: polymorphic function types, exactly this feature, syntax
  `[T] => (T => T)`.

We follow the Haskell / Scala precedent: annotation-required, fully
checked, no inference of the quantifier itself.

## Unresolved questions

- Trait method signatures with HRT bodies: legal? Probably yes, but
  needs more thought on dispatch rules. Out of scope for this RFC if
  it complicates the first ship; can be a Tier 2 follow-up.
- Interaction with `where` constraints when a constraint refers to
  the higher-rank variable. Should be straightforward since we
  already substitute on instantiation, but worth a test pass.
- Should `for<T>` allow constraint `where T: Default` *outside* the
  function arrow, applying to all instantiations? Yes, and the syntax
  is unambiguous. Documented in the guide-level section.

## Future possibilities

- Variance annotations (`+T`, `-T`, `*T`) on user-defined generics.
- Higher-kinded types (rank-2 of *types*, not just functions). Bigger
  jump; separate RFC.
- Existential types: `exists<T> fn() -> T`, useful for type-erased
  iterators. Out of scope.

## Impact on stability

Pure addition. No existing program changes meaning. Lands behind
`@unstable` at first because the diagnostic story needs polish; once
the error messages and `xs --explain` entries are in place, promotes
to Tier 1.

Compatibility note: a future variance RFC may change the default
variance of user-defined generics from invariant to inferred. That is
explicitly out of scope here, and any change there will go through its
own deprecation cycle.
