-- Runtime resource limits. Each case runs the interpreter or VM
-- against a configured cap; exceeding the cap should throw a
-- catchable ResourceLimit error. These are invoked by the harness
-- in tests/run.sh under both --interp and --vm.

-- Plain arithmetic under a huge budget: must succeed.
let sum = 0
var i = 0
while i < 10 {
  sum = sum + i
  i = i + 1
}
assert(sum == 45, "sum")

-- The rest of the hardening (hitting budget / time) is exercised from
-- the CLI in tests/test_limits.sh which runs the binary with
-- --instr-limit / --time-limit and checks the exit code.
println("ok")
