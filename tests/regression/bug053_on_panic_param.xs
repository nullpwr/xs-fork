-- bug053: @on_panic accepts a single parameter (the exception). The
-- parser used to lump it in with the no-arg lifecycle hooks and
-- reject any params, which was inconsistent with the runtime: the
-- runtime calls the @on_panic handler with the exception as arg[0].
-- This test just checks that the parser accepts it.
@on_panic fn report(err) {
    println("would get:", err)
}

println("bug053: ok")
