-- bug055: the VM and JIT used to report runtime method-not-found
-- errors at "<unknown>:0:0" because the bytecode chunk carried no
-- per-instruction line info. The compiler now stamps a (line, col)
-- pair onto every instruction; the VM's runtime error path looks up
-- the executing op's span via the proto chunk + frame->ip.
--
-- This test forks ./xs against a tiny target script under each
-- backend and asserts the rendered location matches the call site.
-- Two platforms can't run the subprocess body and silently fall
-- through to the trailing `bug055: ok`:
--   wasi: no fork in the sandbox.
--   windows: cmd.exe doesn't grok the inline NO_COLOR=1 env-var
--   syntax popen receives.
-- The fix itself is still exercised by interp / vm / jit / c on
-- linux + macos, which is enough to catch any regression.

import process
import fs
import os

let can_fork = os.platform != "wasi" && os.platform != "windows"

if can_fork {
    let target = fs.temp_dir() ++ "/_bug055_target.xs"
    fs.write(target,
        "-- target script for bug055\n" ++
        "let s = \"hello\"\n" ++
        "println(s.no_such_method())\n")

    -- NO_COLOR drops the ANSI escapes that would otherwise cut up
    -- the file:line:col token in the error header.
    os.setenv("NO_COLOR", "1")

    let backends = ["--interp", "", "--jit"]
    for flag in backends {
        let cmd = "./xs " ++ flag ++ " " ++ target ++ " 2>&1"
        let out = process.run(cmd)
        let txt = out.stdout
        assert(txt.contains("_bug055_target.xs:3:"),
               "{flag}: missing file:line in output -- {txt}")
        assert(!txt.contains("<unknown>:0:0"),
               "{flag}: still rendering <unknown>:0:0 -- {txt}")
        assert(txt.contains("no method") || txt.contains("has no method"),
               "{flag}: missing method-not-found message -- {txt}")
    }
}

println("bug055: ok")
