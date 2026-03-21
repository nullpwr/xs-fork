-- variance markers + forall<T> are accepted by the parser. runtime
-- behaviour is unchanged: type variables are placeholders that the
-- runtime treats as 'any'. exercises that the surface stays stable.

struct Box<+T> {
    inner
}

struct Sink<-T> {
    accept
}

fn first<+T>(xs) {
    if len(xs) > 0 { return xs[0] }
    return null
}

fn id_higher_rank(f: forall<T> fn(T) -> T, x) {
    return f(x)
}

fn id(x) { return x }

let b = Box{inner: 42}
println(b.inner)
println(first([1, 2, 3]))
println(id_higher_rank(id, 7))
