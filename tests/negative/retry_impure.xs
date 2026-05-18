-- EXPECT_RUNTIME_ERROR
-- @retry replays the body on each attempt, so an impure body would
-- multiply its side effects per failure. Refused at decoration.
@retry(3)
fn bad() {
    print("retry attempt\n")
    panic("oops")
}

bad()
